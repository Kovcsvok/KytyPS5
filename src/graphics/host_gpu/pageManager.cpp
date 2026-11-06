#include "graphics/host_gpu/pageManager.h"

#include "common/alignment.h"
#include "common/liveSwitches.h"
#include "common/profiler.h"
#include "common/telemetry.h"
#include "common/spinBackoff.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fmt/format.h>
#include <string>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#else
#include <unistd.h>
#endif

namespace Libs::Graphics {
namespace {

constexpr uint64_t PAGE_SIZE    = TRACKER_PAGE_SIZE;
constexpr uint64_t REGION_SIZE  = TRACKER_REGION_SIZE;
constexpr uint64_t ADDRESS_SIZE = TRACKER_ADDRESS_SIZE;
constexpr uint64_t REGION_COUNT = ADDRESS_SIZE / REGION_SIZE;

constexpr uint64_t REGION_PAGES = REGION_SIZE / PAGE_SIZE;

[[noreturn]] void FailFast(const char* reason = nullptr) noexcept {
	std::fputs("PageManager fail-fast: ", stderr);
	std::fputs(reason != nullptr ? reason : "invalid page state", stderr);
	std::fputc('\n', stderr);
	std::fflush(stderr);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	TerminateProcess(GetCurrentProcess(), static_cast<UINT>(EXCEPTION_NONCONTINUABLE_EXCEPTION));
#endif
	std::_Exit(322);
}

[[noreturn]] void Fatal(const char* format, ...) {
	std::fputs("PageManager fatal: ", stderr);
	va_list args;
	va_start(args, format);
	std::vfprintf(stderr, format, args);
	va_end(args);
	std::fputc('\n', stderr);
	std::fflush(stderr);
	std::_Exit(322);
}

class SpinGuard final {
public:
	explicit SpinGuard(std::atomic_flag& lock): m_lock(lock) { Common::SpinAcquire(m_lock); }
	~SpinGuard() { m_lock.clear(std::memory_order_release); }
	KYTY_CLASS_NO_COPY(SpinGuard);

private:
	std::atomic_flag& m_lock;
};

void ValidateRange(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		Fatal("invalid range vaddr=0x%016" PRIx64 ", size=0x%016" PRIx64, vaddr, size);
	}
}

} // namespace

// KYTY_PROTECT_STATS=1 (live, default 0): every protection syscall counted by thread (the GPU
// thread or another), tracker (buffer regions through RegionManager, or images through the texture
// cache), watcher (write: read-only pages that catch CPU writes; access: no-access pages over
// GPU-written bytes) and direction (on, off), in calls and pages; also how many page-state updates
// made calls, and how many calls began where the same thread's previous call with the same
// protection ended (one call could have covered both). Combat 2026-11-06: ~1,940 calls a frame,
// and batching the GPU thread's upload re-protections (KYTY_REPROTECT_BATCH) did not reduce them;
// this says where they come from. Report: "Protect calls (5 s)".
namespace {
struct ProtectStats {
	std::array<std::atomic<uint64_t>, 16> calls {};
	std::array<std::atomic<uint64_t>, 16> pages {};
	std::atomic<uint64_t>                 updates {0};
	std::atomic<uint64_t>                 adjacent {0};
};
ProtectStats          g_protect_stats;
thread_local uint64_t t_last_protect_end  = 0;
thread_local uint32_t t_last_protect_mode = UINT32_MAX;

bool ProtectStatsEnabled() {
	static auto& enabled = Common::LiveSwitches::Get("KYTY_PROTECT_STATS", 0);
	return enabled.load(std::memory_order_relaxed) != 0;
}

// bucket: 8 GPU thread, 4 images (else buffer regions), 2 access watch (else write), 1 on.
void NoteProtect(size_t bucket, uint64_t begin, uint64_t bytes, Common::VirtualMemory::Mode mode,
                 uint64_t page_size) {
	auto& s = g_protect_stats;
	bucket |= Common::Telemetry::OnGpuThread() ? 8u : 0u;
	s.calls[bucket].fetch_add(1, std::memory_order_relaxed);
	s.pages[bucket].fetch_add(bytes / page_size, std::memory_order_relaxed);
	if (begin == t_last_protect_end && static_cast<uint32_t>(mode) == t_last_protect_mode) {
		s.adjacent.fetch_add(1, std::memory_order_relaxed);
	}
	t_last_protect_end  = begin + bytes;
	t_last_protect_mode = static_cast<uint32_t>(mode);
}
} // namespace

struct PageManager::Impl {
	struct PageState {
		uint8_t write_watchers  : 7 = 0;
		uint8_t access_watchers : 1 = 0;

		[[nodiscard]] Common::VirtualMemory::Mode Perms() const noexcept {
			if (access_watchers != 0) {
				return Common::VirtualMemory::Mode::NoAccess;
			}
			if (write_watchers != 0) {
				return Common::VirtualMemory::Mode::Read;
			}
			return Common::VirtualMemory::Mode::ReadWrite;
		}

		template <int delta, bool is_read>
		uint32_t AddDelta(uint64_t address) {
			static_assert(delta >= -1 && delta <= 1);
			if constexpr (is_read) {
				if constexpr (delta == 1) {
					if (access_watchers != 0) {
						Fatal("read-watcher overflow at 0x%016" PRIx64, address);
					}
					return ++access_watchers;
				} else if constexpr (delta == -1) {
					if (access_watchers == 0) {
						Fatal("read-watcher underflow at 0x%016" PRIx64, address);
					}
					return --access_watchers;
				} else {
					return access_watchers;
				}
			} else {
				if constexpr (delta == 1) {
					if (write_watchers == 0x7f) {
						Fatal("write-watcher overflow at 0x%016" PRIx64, address);
					}
					return ++write_watchers;
				} else if constexpr (delta == -1) {
					if (write_watchers == 0) {
						Fatal("write-watcher underflow at 0x%016" PRIx64, address);
					}
					return --write_watchers;
				} else {
					return write_watchers;
				}
			}
		}
	};
	static_assert(sizeof(PageState) == 1);

	struct Region {
		std::atomic_flag                    lock = ATOMIC_FLAG_INIT;
		std::array<PageState, REGION_PAGES> pages;
	};

	Impl() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		SYSTEM_INFO info {};
		GetSystemInfo(&info);
		if (info.dwPageSize != PAGE_SIZE) {
			Fatal("unsupported host page size 0x%08" PRIx32,
			      static_cast<uint32_t>(info.dwPageSize));
		}
#elif defined(__APPLE__)
		// Under Rosetta the host page size is 4 KB, matching TRACKER_PAGE_SIZE.
		if (static_cast<uint64_t>(getpagesize()) != PAGE_SIZE) {
			Fatal("unsupported host page size 0x%08" PRIx32, static_cast<uint32_t>(getpagesize()));
		}
#else
		const auto host_page_size = ::sysconf(_SC_PAGESIZE);
		if (host_page_size < 0 || static_cast<uint64_t>(host_page_size) != PAGE_SIZE) {
			Fatal("unsupported host page size %ld", static_cast<long>(host_page_size));
		}
#endif
		regions = std::make_unique<std::atomic<Region*>[]>(REGION_COUNT);
	}

	~Impl() {
		for (const auto& region: region_storage) {
			SpinGuard lock(region->lock);
			for (auto& page: region->pages) {
				if (page.write_watchers != 0 || page.access_watchers != 0) {
					FailFast("PageManager destroyed with live page state");
				}
			}
		}
	}

	Region* FindRegion(uint64_t vaddr) const noexcept {
		return vaddr < ADDRESS_SIZE ? regions[vaddr / REGION_SIZE].load(std::memory_order_acquire)
		                            : nullptr;
	}

	Region* GetOrCreateRegion(uint64_t vaddr) {
		const auto index = vaddr / REGION_SIZE;
		if (auto* region = regions[index].load(std::memory_order_acquire); region != nullptr) {
			return region;
		}
		std::lock_guard lock(region_mutex);
		if (auto* region = regions[index].load(std::memory_order_acquire); region != nullptr) {
			return region;
		}
		auto  region = std::make_unique<Region>();
		auto* ptr    = region.get();
		region_storage.push_back(std::move(region));
		regions[index].store(ptr, std::memory_order_release);
		return ptr;
	}

	void Protect(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode) noexcept {
		KYTY_PROFILER_BLOCK("PageManager::Protect");
		Common::Telemetry::Add(Common::Telemetry::Counter::ProtectCalls, 1);
		Common::Telemetry::ScopedGpuTime telemetry(Common::Telemetry::Counter::GpuProtect);
		if (!Libs::LibKernel::Memory::ProtectGuestHostMemory(vaddr, size, mode)) {
			Fatal("address-space protection failed at 0x%016" PRIx64 ", mode=0x%08" PRIx32, vaddr,
			      static_cast<uint32_t>(mode));
		}
	}

	template <bool track, bool is_read, bool masked>
	void UpdateRegionWatchers(Region& region, uint64_t base_addr, size_t first, size_t last,
	                          const RegionBits* mask = nullptr) {
		SpinGuard lock(region.lock);
		auto      perms                 = region.pages[first].Perms();
		uint64_t  range_begin           = 0;
		uint64_t  range_bytes           = 0;
		uint64_t  potential_range_bytes = 0;
		const bool stats                = ProtectStatsEnabled();
		uint32_t   calls                = 0;

		const auto release_pending = [&] {
			if (range_bytes != 0) {
				if (stats) [[unlikely]] {
					constexpr size_t Bucket = (masked ? 0u : 4u) | (is_read ? 2u : 0u) | (track ? 1u : 0u);
					NoteProtect(Bucket, base_addr + range_begin * PAGE_SIZE, range_bytes, perms, PAGE_SIZE);
					calls++;
				}
				Protect(base_addr + range_begin * PAGE_SIZE, range_bytes, perms);
				range_bytes           = 0;
				potential_range_bytes = 0;
			}
		};

		for (size_t page_index = first; page_index < last; page_index++) {
			auto&      page    = region.pages[page_index];
			const auto address = base_addr + page_index * PAGE_SIZE;
			const bool update  = !masked || mask->Get(page_index);

			const auto old_perms = page.Perms();
			const auto new_count = update ? page.AddDelta<track ? 1 : -1, is_read>(address)
			                              : page.AddDelta<0, is_read>(address);
			const auto new_perms = page.Perms();

			if (new_perms != perms) [[unlikely]] {
				release_pending();
				perms = new_perms;
			} else if (range_bytes != 0) {
				potential_range_bytes += PAGE_SIZE;
			}

			if (!update) {
				continue;
			}

			const bool watcher_edge = (track && new_count == 1) || (!track && new_count == 0);
			if (watcher_edge && old_perms != new_perms) {
				if (range_bytes == 0) {
					range_begin           = page_index;
					potential_range_bytes = PAGE_SIZE;
				}
				range_bytes = potential_range_bytes;
			}
		}

		release_pending();
		if (calls != 0) {
			g_protect_stats.updates.fetch_add(1, std::memory_order_relaxed);
		}
	}

	template <bool track, bool is_read>
	void UpdatePageWatchers(uint64_t vaddr, uint64_t size) {
		ValidateRange(vaddr, size);
		const auto begin = Common::AlignDown(vaddr, PAGE_SIZE);
		const auto end   = Common::AlignUp(vaddr + size, PAGE_SIZE);
		for (auto chunk_begin = begin; chunk_begin < end;) {
			const auto chunk_end = std::min(end, Common::AlignUp(chunk_begin + 1, REGION_SIZE));
			const auto region_base = Common::AlignDown(chunk_begin, REGION_SIZE);
			auto*      region = track ? GetOrCreateRegion(chunk_begin) : FindRegion(chunk_begin);
			if (region == nullptr) {
				Fatal("untracking unknown page 0x%016" PRIx64, chunk_begin);
			}
			const auto first = static_cast<size_t>((chunk_begin - region_base) / PAGE_SIZE);
			const auto last  = static_cast<size_t>((chunk_end - region_base) / PAGE_SIZE);
			UpdateRegionWatchers<track, is_read, false>(*region, region_base, first, last);
			chunk_begin = chunk_end;
		}
	}

	std::unique_ptr<std::atomic<Region*>[]> regions;
	std::vector<std::unique_ptr<Region>>    region_storage;
	std::mutex                              region_mutex;
};

static_assert(std::atomic<void*>::is_always_lock_free);

PageManager::PageManager(): m_impl(std::make_unique<Impl>()) {}

PageManager::~PageManager() = default;

uint64_t PageManager::GetPageSize() const {
	return PAGE_SIZE;
}

void PageManager::ReportProtectStats() {
	static auto report = std::chrono::steady_clock::now();
	const auto  now    = std::chrono::steady_clock::now();
	if (now - report < std::chrono::seconds(5)) {
		return;
	}
	report = now;
	auto& s = g_protect_stats;
	if (!ProtectStatsEnabled()) {
		return;
	}
	static constexpr const char* Threads[]  = {"other threads", "GPU thread"};
	static constexpr const char* Trackers[] = {"buffers", "images"};
	static constexpr const char* Watches[]  = {"write-watch", "access-watch"};
	std::string text;
	uint64_t    total = 0;
	for (size_t thread = 2; thread-- > 0;) {
		std::string part;
		for (size_t tracker = 0; tracker < 2; tracker++) {
			for (size_t watch = 0; watch < 2; watch++) {
				for (size_t on = 2; on-- > 0;) {
					const auto bucket = thread * 8 + tracker * 4 + watch * 2 + on;
					const auto calls  = s.calls[bucket].exchange(0, std::memory_order_relaxed);
					const auto pages  = s.pages[bucket].exchange(0, std::memory_order_relaxed);
					if (calls == 0) {
						continue;
					}
					total += calls;
					part += fmt::format("{}{} {} {} {} calls/{} pages", part.empty() ? "" : ", ",
					                    Trackers[tracker], Watches[watch], on != 0 ? "on" : "off", calls,
					                    pages);
				}
			}
		}
		text += fmt::format("{}{}: {}", text.empty() ? "" : "; ", Threads[thread],
		                    part.empty() ? "none" : part);
	}
	const auto updates  = s.updates.exchange(0, std::memory_order_relaxed);
	const auto adjacent = s.adjacent.exchange(0, std::memory_order_relaxed);
	::printf("Protect calls (5 s): %s; %llu calls from %llu page-state updates, %llu beginning where "
	         "the thread's previous call with the same protection ended\n",
	         text.c_str(), static_cast<unsigned long long>(total),
	         static_cast<unsigned long long>(updates), static_cast<unsigned long long>(adjacent));
	std::fflush(stdout);
}

template <bool track>
void PageManager::UpdatePageWatchers(uint64_t vaddr, uint64_t size) {
	m_impl->UpdatePageWatchers<track, false>(vaddr, size);
}

template void PageManager::UpdatePageWatchers<true>(uint64_t, uint64_t);
template void PageManager::UpdatePageWatchers<false>(uint64_t, uint64_t);

bool PageManager::IsReadWatched(uint64_t vaddr) const noexcept {
	const auto* region = m_impl->FindRegion(vaddr);
	if (region == nullptr) {
		return false;
	}
	const auto index = static_cast<size_t>((vaddr % REGION_SIZE) / PAGE_SIZE);
	return region->pages[index].access_watchers != 0;
}

void PageManager::Watchers(uint64_t vaddr, uint32_t& write, uint32_t& access) const noexcept {
	write  = 0;
	access = 0;
	const auto* region = m_impl->FindRegion(vaddr);
	if (region == nullptr) {
		return;
	}
	const auto& page = region->pages[static_cast<size_t>((vaddr % REGION_SIZE) / PAGE_SIZE)];
	write            = page.write_watchers;
	access           = page.access_watchers;
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(uint64_t base_addr, RegionBits& mask) {
	if (base_addr % REGION_SIZE != 0 || base_addr >= ADDRESS_SIZE ||
	    REGION_SIZE > ADDRESS_SIZE - base_addr) {
		Fatal("invalid tracking region base 0x%016" PRIx64, base_addr);
	}

	const auto start_range = mask.FirstRange();
	const auto end_range   = mask.LastRange();
	if (start_range.first == REGION_PAGES) {
		FailFast("empty region watcher mask");
	}
	const auto first = start_range.first;
	const auto last  = end_range.second;
	if (start_range.second == end_range.second) {
		m_impl->UpdatePageWatchers<track, is_read>(base_addr + first * PAGE_SIZE,
		                                           (last - first) * PAGE_SIZE);
		return;
	}

	auto* region = track ? m_impl->GetOrCreateRegion(base_addr) : m_impl->FindRegion(base_addr);
	if (region == nullptr) {
		Fatal("untracking unknown region 0x%016" PRIx64, base_addr);
	}
	m_impl->UpdateRegionWatchers<track, is_read, true>(*region, base_addr, first, last, &mask);
}

template void PageManager::UpdatePageWatchersForRegion<true, true>(uint64_t, RegionBits&);
template void PageManager::UpdatePageWatchersForRegion<true, false>(uint64_t, RegionBits&);
template void PageManager::UpdatePageWatchersForRegion<false, true>(uint64_t, RegionBits&);
template void PageManager::UpdatePageWatchersForRegion<false, false>(uint64_t, RegionBits&);

} // namespace Libs::Graphics
