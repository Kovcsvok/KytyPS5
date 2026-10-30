#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_

#include "common/assert.h"
#include "common/spinBackoff.h"
#include "common/telemetry.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <memory>
#include <mutex>
#include <utility>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#undef MemoryBarrier
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libs::Graphics {

class TrackingSpinLock final {
public:
	void lock() noexcept {
		const auto thread = CurrentThread();
		if (m_lock.test_and_set(std::memory_order_acquire)) {
			// A thread that already owns it would wait forever.
			EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) == thread);
			KYTY_PROFILER_BLOCK("LockWait(spin)", profiler::colors::RedA100);
			const auto start = Common::Telemetry::Enabled() ? Common::Telemetry::NowNs() : 0;
			Common::SpinAcquire(m_lock);
			if (start != 0) {
				Common::Telemetry::AddWait(Common::Telemetry::Counter::SpinLockWait,
				                           Common::Telemetry::NowNs() - start);
			}
		}
		m_owner.store(thread, std::memory_order_relaxed);
	}
	void unlock() noexcept {
		EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) != CurrentThread());
		m_owner.store(0, std::memory_order_relaxed);
		m_lock.clear(std::memory_order_release);
	}

private:
	static uint32_t CurrentThread() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return GetCurrentThreadId();
#elif defined(__APPLE__)
		// mach thread port is a nonzero per-thread id (0 is the "no owner" sentinel).
		return static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
#elif defined(__linux__)
		static thread_local const uint32_t tid = static_cast<uint32_t>(::syscall(SYS_gettid));
		return tid;
#else
		EXIT("region tracking thread identity is unsupported on this platform\n");
#endif
	}

	std::atomic_flag     m_lock = ATOMIC_FLAG_INIT;
	std::atomic_uint32_t m_owner {0};
};

static_assert(std::atomic_uint32_t::is_always_lock_free);

inline std::atomic<uint64_t> g_cpu_dirty_epoch {1};

// Bumped by the GPU thread when it starts a guest submission (its first slice).
inline std::atomic<uint64_t> g_guest_submission_seq {1};

// KYTY_WRITE_PREDICT: the guest pages tracked GPU writes are known to write (or the GPU thread
// reads itself), one bit per page below LOWER_ADDRESS_SIZE, the write bitmap's reach (32 MiB).
// Never cleared; any thread notes pages.
class WriteHistory final {
public:
	WriteHistory(): m_bits(std::make_unique<std::atomic<uint64_t>[]>(Words)) {}

	KYTY_CLASS_NO_COPY(WriteHistory);

	void Note(uint64_t vaddr, uint64_t size) noexcept {
		if (size == 0 || vaddr >= LOWER_ADDRESS_SIZE) {
			return;
		}
		const uint64_t last = (std::min(vaddr + size, LOWER_ADDRESS_SIZE) + TRACKER_PAGE_SIZE - 1) /
		                      TRACKER_PAGE_SIZE;
		for (uint64_t page = vaddr / TRACKER_PAGE_SIZE; page < last;) {
			const uint64_t begin = page % 64;
			const uint64_t end   = std::min<uint64_t>(64, begin + (last - page));
			const uint64_t mask  = (end == 64 ? ~uint64_t {0} : (uint64_t {1} << end) - 1) &
			                      (~uint64_t {0} << begin);
			const auto old = m_bits[page / 64].fetch_or(mask, std::memory_order_relaxed);
			m_pages.fetch_add(static_cast<uint64_t>(std::popcount(mask & ~old)),
			                  std::memory_order_relaxed);
			page += end - begin;
		}
	}

	// The pages of the tracker region at region_base predicted written: all pages of each block of
	// block_pages (a power of two, at most a region) that holds a noted page.
	void Guarded(uint64_t region_base, size_t block_pages, RegionBits& out) const noexcept {
		constexpr size_t RegionWords = TRACKER_REGION_PAGES / 64;
		out.Clear();
		if (region_base >= LOWER_ADDRESS_SIZE) {
			out.Fill();
			return;
		}
		const auto* words = &m_bits[region_base / TRACKER_PAGE_SIZE / 64];
		if (block_pages >= 64) {
			const size_t group = std::min(block_pages / 64, RegionWords);
			for (size_t word = 0; word < RegionWords; word += group) {
				uint64_t any = 0;
				for (size_t i = 0; i < group; i++) {
					any |= words[word + i].load(std::memory_order_relaxed);
				}
				if (any != 0) {
					out.SetRange(word * 64, (word + group) * 64);
				}
			}
			return;
		}
		const uint64_t lane_mask = (uint64_t {1} << block_pages) - 1;
		for (size_t word = 0; word < RegionWords; word++) {
			const auto bits = words[word].load(std::memory_order_relaxed);
			if (bits == 0) {
				continue;
			}
			for (size_t lane = 0; lane < 64; lane += block_pages) {
				if (((bits >> lane) & lane_mask) != 0) {
					out.SetRange(word * 64 + lane, word * 64 + lane + block_pages);
				}
			}
		}
	}

	[[nodiscard]] uint64_t Pages() const noexcept { return m_pages.load(std::memory_order_relaxed); }

private:
	static constexpr uint64_t Words = LOWER_ADDRESS_SIZE / TRACKER_PAGE_SIZE / 64;

	std::unique_ptr<std::atomic<uint64_t>[]> m_bits;
	std::atomic<uint64_t>                    m_pages {0};
};

class RegionManager final {
public:
	RegionManager(PageManager& page_manager, uint64_t cpu_addr)
	    : m_page_manager(page_manager), m_cpu_addr(cpu_addr) {
		if (m_cpu_addr % TRACKER_REGION_SIZE != 0) {
			EXIT("invalid region tracking manager construction\n");
		}
		m_cpu_dirty.Fill();
		m_writable.Fill();
		m_readable.Fill();
	}

	KYTY_CLASS_NO_COPY(RegionManager);

	[[nodiscard]] uint64_t GetCpuAddr() const { return m_cpu_addr; }

	template <DirtySource source>
	[[nodiscard]] bool MaybeModified() const noexcept {
		return GetSummary<source>().load(std::memory_order_acquire);
	}
	template <DirtySource source>
	[[nodiscard]] bool IsModified(uint64_t offset, uint64_t size) const {
		const auto [start, end] = GetPageRange(m_cpu_addr + offset, size);
		return GetBits<source>().FirstRangeFrom(start).first < end;
	}

	template <DirtySource source, bool enable>
	void ChangeState(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		if constexpr (source == DirtySource::Cpu && enable) {
			if (RegionBits(m_gpu_dirty, start, end).Any()) {
				EXIT("CPU dirty state conflicts with GPU dirty state\n");
			}
		}
		if constexpr (source == DirtySource::Gpu && enable) {
			if (RegionBits(m_cpu_dirty, start, end).Any()) {
				EXIT("GPU dirty state conflicts with CPU dirty state\n");
			}
		}
		auto& bits = GetBits<source>();
		if constexpr (enable) {
			bits.SetRange(start, end);
			SetSummary<source>(true);
			if constexpr (source == DirtySource::Cpu) {
				g_cpu_dirty_epoch.fetch_add(1, std::memory_order_release);
			}
		} else {
			bits.UnsetRange(start, end);
			if (bits.None()) {
				SetSummary<source>(false);
			}
		}
		if constexpr (source == DirtySource::Cpu) {
			UpdateProtection<!enable, false>();
		} else {
			m_gpu_open.UnsetRange(start, end);
			m_gpu_shadow.UnsetRange(start, end);
			UpdateProtection<enable, true>();
		}
	}

	// KYTY_WRITE_PREDICT: ChangeState<Gpu, true> for a tracked GPU write, except that the pages
	// readable now outside `guarded` stay readable: open (apply), or only recorded as shadow pages
	// for the completion check's counts. A readable page here is CPU-clean (just uploaded) and holds
	// what the GPU's copy holds; a protected one may wait for a download and is never opened (so
	// this only protects pages, as UpdateProtection<true, true> requires). Shadow pages count as
	// readable while measuring. Returns the pages left readable (or recorded).
	size_t MarkGpuPredicted(uint64_t vaddr, uint64_t size, const RegionBits& guarded, bool apply) {
		const auto [start, end] = GetPageRange(vaddr, size);
		if (RegionBits(m_cpu_dirty, start, end).Any()) {
			EXIT("GPU dirty state conflicts with CPU dirty state\n");
		}
		RegionBits open(m_readable, start, end);
		if (!apply) {
			open |= RegionBits(m_gpu_shadow, start, end);
		}
		open &= ~guarded;
		m_gpu_dirty.SetRange(start, end);
		SetSummary<DirtySource::Gpu>(true);
		m_gpu_open.UnsetRange(start, end);
		m_gpu_shadow.UnsetRange(start, end);
		if (apply) {
			m_gpu_open |= open;
		} else {
			m_gpu_shadow |= open;
		}
		UpdateProtection<true, true>();
		return open.Count();
	}

	// KYTY_WRITE_PREDICT: the range's open pages become unreadable GPU-written pages, its shadow
	// pages plain ones. Returns how many pages were open or shadow.
	size_t CloseOpen(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		const auto shadow       = RegionBits(m_gpu_shadow, start, end).Count();
		const auto open         = RegionBits(m_gpu_open, start, end).Count();
		m_gpu_shadow.UnsetRange(start, end);
		if (open != 0) {
			m_gpu_open.UnsetRange(start, end);
			UpdateProtection<true, true>();
		}
		return open + shadow;
	}

	// KYTY_WRITE_PREDICT: calls func(address, bytes) for each run of open or shadow pages.
	template <typename Func>
	void ForEachPredictedRun(uint64_t vaddr, uint64_t size, Func&& func) const {
		const auto [start, end] = GetPageRange(vaddr, size);
		const auto mask = RegionBits(m_gpu_open, start, end) | RegionBits(m_gpu_shadow, start, end);
		for (const auto [first, last]: mask) {
			func(m_cpu_addr + first * TRACKER_PAGE_SIZE, (last - first) * TRACKER_PAGE_SIZE);
		}
	}

	template <DirtySource source, bool clear, typename Func>
	void ForEachModifiedRange(uint64_t vaddr, uint64_t size, Func&& func) {
		const auto [start, end] = GetPageRange(vaddr, size);
		auto&      bits         = GetBits<source>();
		if (bits.FirstRangeFrom(start).first >= end) {
			return;
		}
		RegionBits mask(bits, start, end);
		if constexpr (clear) {
			bits.UnsetRange(start, end);
			if (bits.None()) {
				SetSummary<source>(false);
			}
			if constexpr (source == DirtySource::Cpu) {
				UpdateProtection<true, false>();
			} else {
				m_gpu_open.UnsetRange(start, end);
				m_gpu_shadow.UnsetRange(start, end);
				UpdateProtection<false, true>();
			}
		}
		for (const auto [first, last]: mask) {
			func(m_cpu_addr + first * TRACKER_PAGE_SIZE, (last - first) * TRACKER_PAGE_SIZE);
		}
	}

	// Mirrors the CPU summary into one bit of the tracker's region bitmap, so range walks can skip
	// clean regions without touching their managers (MemoryTracker, KYTY_REGION_BITMAP). Set
	// before the manager is published.
	void SetCpuSummaryBit(std::atomic<uint64_t>* word, uint64_t bit) noexcept {
		m_cpu_summary_word = word;
		m_cpu_summary_bit  = bit;
		SetSummary<DirtySource::Cpu>(m_cpu_maybe_dirty.load(std::memory_order_relaxed));
	}

	TrackingSpinLock lock;

private:
	template <DirtySource source>
	void SetSummary(bool value) noexcept {
		GetSummary<source>().store(value, std::memory_order_release);
		if constexpr (source == DirtySource::Cpu) {
			if (m_cpu_summary_word != nullptr) {
				if (value) {
					m_cpu_summary_word->fetch_or(m_cpu_summary_bit, std::memory_order_release);
				} else {
					m_cpu_summary_word->fetch_and(~m_cpu_summary_bit, std::memory_order_release);
				}
			}
		}
	}

	template <bool track, bool is_read>
	void UpdateProtection() {
		// Open pages (always GPU-dirty) stay readable.
		const auto protection = is_read ? ~(m_gpu_dirty ^ m_gpu_open) : m_cpu_dirty;
		auto&      previous   = is_read ? m_readable : m_writable;
		auto       mask       = protection ^ previous;
		if (mask.None()) {
			return;
		}
		previous = protection;
		m_page_manager.UpdatePageWatchersForRegion<track, is_read>(m_cpu_addr, mask);
	}

	template <DirtySource source>
	RegionBits& GetBits() {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	template <DirtySource source>
	const RegionBits& GetBits() const {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	template <DirtySource source>
	std::atomic<bool>& GetSummary() {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_maybe_dirty;
		} else {
			return m_gpu_maybe_dirty;
		}
	}

	template <DirtySource source>
	const std::atomic<bool>& GetSummary() const {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_maybe_dirty;
		} else {
			return m_gpu_maybe_dirty;
		}
	}

	[[nodiscard]] std::pair<size_t, size_t> GetPageRange(uint64_t vaddr, uint64_t size) const {
		if (size == 0 || vaddr < m_cpu_addr || vaddr >= m_cpu_addr + TRACKER_REGION_SIZE ||
		    size > m_cpu_addr + TRACKER_REGION_SIZE - vaddr) {
			EXIT("range lies outside its tracking region\n");
		}
		const auto offset = vaddr - m_cpu_addr;
		return {static_cast<size_t>(offset / TRACKER_PAGE_SIZE),
		        static_cast<size_t>((offset + size + TRACKER_PAGE_SIZE - 1) / TRACKER_PAGE_SIZE)};
	}

	PageManager& m_page_manager;
	uint64_t     m_cpu_addr = 0;
	RegionBits   m_cpu_dirty;
	RegionBits   m_gpu_dirty;
	std::atomic<bool> m_cpu_maybe_dirty {true};
	std::atomic<bool> m_gpu_maybe_dirty {false};
	std::atomic<uint64_t>* m_cpu_summary_word = nullptr;
	uint64_t               m_cpu_summary_bit  = 0;
	RegionBits   m_writable;
	RegionBits   m_readable;
	// KYTY_WRITE_PREDICT: GPU-dirty pages left readable (open) or that would be (shadow, measure
	// only); both always within m_gpu_dirty.
	RegionBits   m_gpu_open;
	RegionBits   m_gpu_shadow;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
