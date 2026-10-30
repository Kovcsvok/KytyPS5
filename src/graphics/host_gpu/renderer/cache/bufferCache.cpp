#include "graphics/host_gpu/renderer/cache/bufferCache.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/liveSwitches.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/telemetry.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/addressBindingReport.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;

// KYTY_READBACK_STATS=1: every 5 s, print the buffers whose guest page faults were handed to the
// GPU thread (BufferCache::ReadMemory) and how each one ended. For downloads it also reports the
// written binding that last marked the faulting bytes (shader, size, whether that GPU work had
// already finished, how long ago it was bound) and how many downloaded bytes differed from guest
// memory. Only the GPU thread records, except the byte comparison (download completions).
class ReadbackStats {
public:
	enum Outcome : uint32_t {
		Unregistered,
		Unmarked,
		Downloaded,
		NothingToDownload,
		DirectRead,
		OutcomeCount
	};
	static constexpr uint32_t AgeBuckets = 5; // <1 ms, <10 ms, <50 ms, <250 ms, older

	struct Writer {
		bool     found    = false;
		bool     finished = false;
		uint32_t age      = 0;
		uint64_t shader   = 0;
		uint64_t size     = 0;
	};

	class Scope {
	public:
		Scope(uint64_t vaddr, bool is_write)
		    : m_stats(Get()), m_vaddr(vaddr), m_is_write(is_write),
		      m_start(m_stats != nullptr ? std::chrono::steady_clock::now()
		                                 : std::chrono::steady_clock::time_point {}) {}
		~Scope() {
			if (m_stats != nullptr) {
				const auto elapsed = std::chrono::steady_clock::now() - m_start;
				m_stats->Record(m_buffer_begin, m_buffer_size, m_vaddr, m_is_write, m_outcome,
				                m_download_bytes, m_writer, m_own_dirty,
				                std::chrono::duration<double, std::micro>(elapsed).count());
			}
		}
		KYTY_CLASS_NO_COPY(Scope);

		[[nodiscard]] bool Enabled() const { return m_stats != nullptr; }
		void               SetBuffer(uint64_t begin, uint64_t size) {
            m_buffer_begin = begin;
            m_buffer_size  = size;
		}
		void SetOutcome(Outcome outcome) { m_outcome = outcome; }
		void SetDownloadBytes(uint64_t bytes) { m_download_bytes = bytes; }
		void SetOwnBytesDirty(bool dirty) { m_own_dirty = dirty ? 1 : 0; }
		// The latest written binding covering the faulting address; is_finished(tick) tells
		// whether that GPU work has completed.
		template <typename IsFinished>
		void FindWriter(IsFinished&& is_finished) {
			if (m_stats != nullptr) {
				m_writer = m_stats->FindWriter(m_vaddr, is_finished);
			}
		}

	private:
		ReadbackStats*                        m_stats;
		uint64_t                              m_vaddr;
		bool                                  m_is_write;
		std::chrono::steady_clock::time_point m_start;
		uint64_t                              m_buffer_begin   = 0;
		uint64_t                              m_buffer_size    = 0;
		Outcome                               m_outcome        = Unregistered;
		uint64_t                              m_download_bytes = 0;
		Writer                                m_writer;
		int                                   m_own_dirty      = -1; // -1: not looked at
	};

	// GPU thread: the shader whose resources are being bound next.
	static void SetShader(uint64_t hash) { s_shader = hash; }

	// GPU thread: a binding the GPU may write.
	static void NoteWrite(uint64_t begin, uint64_t size, uint64_t tick) {
		if (auto* stats = Get(); stats != nullptr) {
			auto& mark = stats->m_marks[stats->m_next_mark++ % stats->m_marks.size()];
			mark       = {begin, begin + size, tick, s_shader, std::chrono::steady_clock::now()};
		}
	}

	// Download completion: how many bytes the GPU copy changed in guest memory.
	static void NoteCompare(uint64_t changed, uint64_t total) {
		if (Get() != nullptr) {
			s_changed_bytes.fetch_add(changed, std::memory_order_relaxed);
			s_compared_bytes.fetch_add(total, std::memory_order_relaxed);
		}
	}

	[[nodiscard]] static bool On() { return Get() != nullptr; }

private:
	struct Mark {
		uint64_t                              begin  = 0;
		uint64_t                              end    = 0;
		uint64_t                              tick   = 0;
		uint64_t                              shader = 0;
		std::chrono::steady_clock::time_point time;
	};

	static ReadbackStats* Get() {
		static auto&         enabled = Common::LiveSwitches::Get("KYTY_READBACK_STATS", 0);
		static ReadbackStats stats;
		return enabled.load(std::memory_order_relaxed) != 0 ? &stats : nullptr;
	}

	template <typename IsFinished>
	Writer FindWriter(uint64_t vaddr, IsFinished&& is_finished) const {
		// Newest first.
		for (size_t i = 0; i < m_marks.size(); i++) {
			const auto& mark = m_marks[(m_next_mark + m_marks.size() - 1 - i) % m_marks.size()];
			if (mark.end == 0 || vaddr < mark.begin || vaddr >= mark.end) {
				continue;
			}
			const auto age_ms = std::chrono::duration<double, std::milli>(
			                        std::chrono::steady_clock::now() - mark.time)
			                        .count();
			const uint32_t age = age_ms < 1     ? 0
			                     : age_ms < 10  ? 1
			                     : age_ms < 50  ? 2
			                     : age_ms < 250 ? 3
			                                    : 4;
			return {true, is_finished(mark.tick), age, mark.shader, mark.end - mark.begin};
		}
		return {};
	}

	void Record(uint64_t buffer_begin, uint64_t buffer_size, uint64_t vaddr, bool is_write,
	            Outcome outcome, uint64_t download_bytes, const Writer& writer, int own_dirty,
	            double micros) {
		auto& entry = m_entries[buffer_begin];
		entry.size  = buffer_size;
		(is_write ? entry.writes : entry.reads)++;
		if (outcome == Downloaded && own_dirty >= 0) {
			(own_dirty != 0 ? entry.own_dirty : entry.other_only)++;
		}
		if (s_forwarded) {
			entry.forwarded++;
			entry.forwarded_micros += micros;
		}
		entry.outcomes[outcome]++;
		entry.download_bytes += download_bytes;
		entry.micros += micros;
		entry.pages.insert(vaddr >> 12u);
		if (writer.found) {
			(writer.finished ? entry.writer_finished : entry.writer_running)++;
			entry.writer_age[writer.age]++;
			entry.writer_shaders[writer.shader]++;
			entry.writer_size = std::max(entry.writer_size, writer.size);
		} else if (outcome == Downloaded) {
			entry.writer_unknown++;
		}
		const auto now = std::chrono::steady_clock::now();
		if (now - m_last >= std::chrono::seconds(5)) {
			Print();
			m_entries.clear();
			m_last = now;
		}
	}

	void Print() const {
		std::vector<std::pair<uint64_t, const Entry*>> sorted;
		double                                         total = 0;
		for (const auto& [begin, entry]: m_entries) {
			sorted.emplace_back(begin, &entry);
			total += entry.micros;
		}
		std::ranges::sort(sorted, [](const auto& a, const auto& b) {
			return a.second->micros > b.second->micros;
		});
		const auto changed  = s_changed_bytes.exchange(0, std::memory_order_relaxed);
		const auto compared = s_compared_bytes.exchange(0, std::memory_order_relaxed);
		::printf("Readback stats (5 s): %zu buffers, %.1f ms on the GPU thread; downloads changed "
		         "%" PRIu64 " of %" PRIu64 " KiB of guest memory\n",
		         sorted.size(), total / 1000.0, changed / 1024, compared / 1024);
		for (size_t i = 0; i < std::min<size_t>(sorted.size(), 8); i++) {
			const auto& [begin, e] = sorted[i];
			::printf("  buffer 0x%016" PRIx64 " size %" PRIu64 " KiB: writes %" PRIu64
			         " reads %" PRIu64 ", unregistered %" PRIu64 " unmarked %" PRIu64
			         " downloaded %" PRIu64 " (%" PRIu64 " KiB) nothing-to-download %" PRIu64
			         " direct-read %" PRIu64 ", %zu pages, %.1f ms (guest threads %" PRIu64
			         " / %.1f ms)\n",
			         begin, e->size / 1024, e->writes, e->reads, e->outcomes[Unregistered],
			         e->outcomes[Unmarked], e->outcomes[Downloaded], e->download_bytes / 1024,
			         e->outcomes[NothingToDownload], e->outcomes[DirectRead], e->pages.size(),
			         e->micros / 1000.0, e->forwarded, e->forwarded_micros / 1000.0);
			if (e->own_dirty + e->other_only != 0) {
				::printf("    downloads: faulting bytes GPU-written %" PRIu64
				         ", only other bytes of the window %" PRIu64 "\n",
				         e->own_dirty, e->other_only);
			}
			if (e->pages.size() <= 16) {
				std::vector<uint64_t> pages(e->pages.begin(), e->pages.end());
				std::ranges::sort(pages);
				::printf("    pages:");
				for (const auto page: pages) {
					::printf(" 0x%" PRIx64, page << 12u);
				}
				::printf("\n");
			}
			if (e->writer_finished + e->writer_running + e->writer_unknown == 0) {
				continue;
			}
			::printf("    writer: finished %" PRIu64 " still-running %" PRIu64 " unknown %" PRIu64
			         "; bound <1ms %" PRIu64 " <10ms %" PRIu64 " <50ms %" PRIu64 " <250ms %" PRIu64
			         " older %" PRIu64 "; largest binding %" PRIu64 " KiB; shaders",
			         e->writer_finished, e->writer_running, e->writer_unknown, e->writer_age[0],
			         e->writer_age[1], e->writer_age[2], e->writer_age[3], e->writer_age[4],
			         e->writer_size / 1024);
			std::vector<std::pair<uint64_t, uint64_t>> shaders(e->writer_shaders.begin(),
			                                                   e->writer_shaders.end());
			std::ranges::sort(shaders,
			                  [](const auto& a, const auto& b) { return a.second > b.second; });
			for (size_t s = 0; s < std::min<size_t>(shaders.size(), 4); s++) {
				::printf(" 0x%016" PRIx64 " x%" PRIu64, shaders[s].first, shaders[s].second);
			}
			::printf("\n");
		}
		std::fflush(stdout);
	}

	struct Entry {
		uint64_t                               size   = 0;
		uint64_t                               writes = 0;
		uint64_t                               reads  = 0;
		uint64_t                               outcomes[OutcomeCount] {};
		uint64_t                               download_bytes = 0;
		double                                 micros         = 0;
		std::unordered_set<uint64_t>           pages;
		uint64_t                               writer_finished = 0;
		uint64_t                               writer_running  = 0;
		uint64_t                               writer_unknown  = 0;
		uint64_t                               writer_age[AgeBuckets] {};
		uint64_t                               writer_size = 0;
		std::unordered_map<uint64_t, uint64_t> writer_shaders;
		// Faults of guest threads handed to the GPU thread (the rest are its own).
		uint64_t forwarded        = 0;
		double   forwarded_micros = 0;
		// Downloads: the faulting bytes themselves are GPU-written, or only other bytes of the
		// window (the page's or its neighbours').
		uint64_t own_dirty  = 0;
		uint64_t other_only = 0;
	};

public:
	// GPU thread: set while a guest thread's fault is handled (BufferCache::ReadMemory).
	inline static thread_local bool s_forwarded = false;

private:
	std::unordered_map<uint64_t, Entry>   m_entries;
	std::chrono::steady_clock::time_point m_last = std::chrono::steady_clock::now();
	// The newest written bindings. 1024 lost most writers of the 2.8 GB heap within a frame
	// (2026-10-25: 98% "unknown").
	std::array<Mark, 8192>                m_marks {};
	size_t                                m_next_mark = 0;

	inline static thread_local uint64_t s_shader = 0;
	inline static std::atomic<uint64_t> s_changed_bytes {0};
	inline static std::atomic<uint64_t> s_compared_bytes {0};
};

// KYTY_DIRECT_READBACK (live, default 0): see TryDirectReadback. The buffers must be created in
// host-visible device memory (KYTY_MAPPED_DEVICE_BUFFERS=1 at start-up; Resizable BAR). 1 also
// serves guest-thread faults (KYTY_ASYNC_READBACK) from the mapping once the buffer's writers have
// finished, with no download, submission or wait; 2 also waits for a running writer's tick only,
// not for everything recorded after it, and then copies from the mapping. Wolverine 2026-10-22
// (forward_2026-10-22_100601.tracy, 5-10 fps stretches): the main guest thread spent 70-90 % of
// each frame in ~40 readbacks, ~0.85 ms each, while the GPU thread was half idle.
int64_t DirectReadbackMode() {
	static auto& mode = Common::LiveSwitches::Get("KYTY_DIRECT_READBACK", 0);
	return mode.load(std::memory_order_relaxed);
}

// KYTY_READBACK_RANGE_TICK (live, default 0): a readback waits for the last GPU work that wrote
// the window's bytes (m_range_writes), not for the buffer's last writer. Wolverine combat
// 2026-10-22: the 2.8 GB heap 0x1210000000 always has a writer running, so each of its ~100
// readbacks per frame waited for all the work recorded before it. 1: a guest thread's readback
// whose bytes are final is downloaded on the readback queue at once (behind nothing) instead of
// on the main queue; the direct (KYTY_DIRECT_READBACK) and copy-queue paths use the range tick.
// 2: also when the range's writer still runs (the readback queue waits for that tick only).
// Report: "Range-tick readbacks (5 s)".
int64_t RangeTickMode() {
	static auto& mode = Common::LiveSwitches::Get("KYTY_READBACK_RANGE_TICK", 0);
	return mode.load(std::memory_order_relaxed);
}

// KYTY_READBACK_QUEUE_ASYNC (live, default 0): a guest thread's readback whose bytes are final
// (1), or whose writer the readback queue can wait for alone (2), is copied on the readback queue
// and only the faulting guest thread waits for that copy (BeginQueueReadback). The GPU thread
// submits it and goes on. KYTY_READBACK_RANGE_TICK=1/2 waited for the copy on the GPU thread
// instead: Wolverine combat 2026-10-23, 9-11 fps against 12-16 with 0, GPU 34% busy against
// 51-57%. With KYTY_READBACK_RANGE_TICK=0 the writer is the buffer's last writer. Report: "Queue
// readbacks (5 s)".
int64_t QueueReadbackMode() {
	static auto& mode = Common::LiveSwitches::Get("KYTY_READBACK_QUEUE_ASYNC", 0);
	return mode.load(std::memory_order_relaxed);
}

// KYTY_READBACK_QUEUE_GUEST_COPY (live, default 1): the faulting guest thread writes its
// KYTY_READBACK_QUEUE_ASYNC copy back to guest memory itself (CopyQueueReadbackOnGuest), as a
// main-queue download's deferred operation runs on the waiting guest thread. With 0 the GPU thread
// does it in LandQueueReadback: Wolverine combat 2026-10-23 (Tracy), 207 x 119 us = 24.6 ms of the
// GPU thread per frame, which then held every guest command back.
bool QueueGuestCopyEnabled() {
	static auto& enabled = Common::LiveSwitches::Get("KYTY_READBACK_QUEUE_GUEST_COPY", 1);
	return enabled.load(std::memory_order_relaxed) != 0;
}

// KYTY_READBACK_QUEUE_FLUSH_US=N (live, default 0 = off): a KYTY_READBACK_QUEUE_ASYNC readback also
// submits the main queue's open command buffer when N microseconds have passed since its last
// submit (1 = every time), as the main-queue readback does (it submits to wait for its copy). The
// readback itself still waits only for its own copy. Wolverine combat 2026-10-23: with the
// readback queue the main queue went from 12-13k to 3-4k command buffers per 5 s and the GPU from
// 57% to 37% busy; the readback copy took 142 us instead of 808 us (Tracy).
int64_t QueueReadbackFlushUs() {
	static auto& interval = Common::LiveSwitches::Get("KYTY_READBACK_QUEUE_FLUSH_US", 0);
	return interval.load(std::memory_order_relaxed);
}

bool DirectReadbackEnabled() {
	return DirectReadbackMode() != 0;
}

bool AsyncWriteReadbackEnabled() {
	static auto& enabled = Common::LiveSwitches::Get("KYTY_ASYNC_WRITE_READBACK", 0);
	return enabled.load(std::memory_order_relaxed) != 0;
}

// KYTY_ASYNC_READ_SNAPSHOT=1 (live, default 0): see Memory::RequestServeFromBacking. Wolverine's
// busiest readback buffer is rewritten by the GPU before the asynchronous download lands (run 41:
// the writer was still running for 194 of 402 readbacks per 5 s); the guest never waits for it,
// so it reads whatever the GPU had written by then, as the download captured.
bool AsyncReadSnapshotEnabled() {
	static auto& enabled = Common::LiveSwitches::Get("KYTY_ASYNC_READ_SNAPSHOT", 0);
	return enabled.load(std::memory_order_relaxed) != 0;
}

// KYTY_STALE_INFLIGHT_READS=1 (live, research, default 0): a guest read of GPU-written memory
// whose writer has not finished on the host GPU is served from guest memory as it is (the bytes
// of the last download), with no download, flush or wait; the page stays GPU-owned. Labels are
// published after the GPU work (KYTY_LABELS_AFTER_GPU), so the guest has not synchronized with
// that writer yet: on the PS5 it would read whatever memory held at that moment. Wolverine
// 2026-10-22 (KYTY_READBACK_STATS, walking into new areas): about half the reads of its two
// busiest readback buffers met a running writer, 0.5-2.8 s of the GPU thread per 5 s.
bool StaleInflightReadsEnabled() {
	static auto& enabled = Common::LiveSwitches::Get("KYTY_STALE_INFLIGHT_READS", 0);
	return enabled.load(std::memory_order_relaxed) != 0;
}

bool AsyncReadReadbackEnabled() {
	// Run 37 A/B at the Wolverine spot: readback time on the GPU thread 147 -> 113 ms/s, mean
	// frame 73.5 -> 72.5 ms. The busiest buffer gains nothing: the GPU rewrites its pages before
	// the download lands, so FinishWriteReadback falls back to the synchronous path.
	static auto& enabled = Common::LiveSwitches::Get("KYTY_ASYNC_READBACK", 1);
	return enabled.load(std::memory_order_relaxed) != 0;
}

// KYTY_READBACK_STALE_ALL (live, research, default 0): guest-thread reads of GPU-written memory do
// not wait for a readback. 1: served from guest memory as it is (the bytes of the last download),
// with no download at all; the page stays GPU-owned, so every later read faults and is served the
// same way, and the bytes never refresh while the GPU keeps writing the page. 2: as 1, but the
// fault also posts the window's readback to the GPU thread without waiting for it; when its
// download lands the window is published and unprotected as after a waited readback, so the
// guest sees the GPU's bytes one download later instead of never. Wolverine combat 2026-10-22:
// the main guest thread waited 50-70 ms of a 66-83 ms frame; 1 (with KYTY_INDIRECT_DRAW_GUARD=0)
// took 11-12 fps to 18-23, with orange blots on the ground (stale data).
int64_t StaleAllMode() {
	static auto& mode = Common::LiveSwitches::Get("KYTY_READBACK_STALE_ALL", 0);
	return mode.load(std::memory_order_relaxed);
}

void NoteStaleAllRead(bool posted) {
	static std::atomic<uint64_t> served {0};
	static std::atomic<uint64_t> readbacks {0};
	static std::atomic<int64_t>  report_ms {0};
	const auto count  = served.fetch_add(1, std::memory_order_relaxed) + 1;
	if (posted) {
		readbacks.fetch_add(1, std::memory_order_relaxed);
	}
	const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	                        std::chrono::steady_clock::now().time_since_epoch())
	                        .count();
	if (auto last = report_ms.load(std::memory_order_relaxed);
	    now_ms - last >= 5000 && report_ms.compare_exchange_strong(last, now_ms)) {
		std::printf("Stale-all reads (5 s): %" PRIu64 " guest reads served without waiting, %" PRIu64
		            " background readbacks posted\n",
		            count, readbacks.exchange(0, std::memory_order_relaxed));
		std::fflush(stdout);
		served.store(0, std::memory_order_relaxed);
	}
}

// A guest fault on GPU-written memory downloads an aligned window around the faulting bytes, so
// nearby accesses share one GPU drain. KYTY_READBACK_WINDOW_KB (a live switch; a power of two,
// at least the tracker page) changes its width for A/B measurements. The default, 2 MiB, measured
// best in Marvel's Wolverine (512 KiB +3.5% frame time, 8 MiB +6%, 64 KiB +17%).
uint64_t ReadbackWindowSize() {
	constexpr int64_t DefaultKib = 2048;
	static auto&      kib        = Common::LiveSwitches::Get("KYTY_READBACK_WINDOW_KB", DefaultKib);
	const auto        value      = kib.load(std::memory_order_relaxed);
	const auto        bytes      = static_cast<uint64_t>(value) * 1024;
	if (value <= 0 || value > 1024 * 1024 || bytes < TRACKER_PAGE_SIZE ||
	    (bytes & (bytes - 1)) != 0) {
		return static_cast<uint64_t>(DefaultKib) * 1024;
	}
	return bytes;
}

} // namespace

void BufferCache::SetReadbackStatsShader(uint64_t hash) {
	ReadbackStats::SetShader(hash);
}

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging_buffer.Size());
		const auto offset = m_staging_buffer.Copy(bytes, chunk, 4);
		buffer.CopyFrom(m_scheduler.Current(), m_staging_buffer, offset, buffer.Offset(address),
		                chunk, vk::AccessFlagBits::eHostWrite);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

void BufferCache::Register(BufferId id) {
	KYTY_PROFILER_FUNCTION();
	ChangeRegister<true>(id);
}

void BufferCache::Unregister(BufferId id) {
	KYTY_PROFILER_FUNCTION();
	ChangeRegister<false>(id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId id) {
	auto& buffer = m_slot_buffers[id];
	PageTable::PageRange pages {};
	EXIT_IF(!(GuestRange {buffer.CpuAddress(), buffer.Size()}.Valid()) ||
	        !PageTable::TryGetPageRange(buffer.CpuAddress(), buffer.Size(), pages));
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		if constexpr (insert) {
			m_page_table[page] = id;
		} else {
			m_page_table[page] = {};
		}
	}
	const auto size_pages = pages.last_exclusive - pages.first;
	if constexpr (insert) {
		const auto [it, inserted] = m_buffers.emplace(buffer.CpuAddress(), id);
		(void)it;
		EXIT_IF(!inserted);
		m_bda_ever_registered.Add(buffer.CpuAddress(), size_pages << CACHING_PAGEBITS);
		m_total_used_memory += buffer.Size();
		g_cpu_dirty_epoch.fetch_add(1, std::memory_order_release);
		buffer.lru_id = m_lru_cache.Insert(id, LruClock());
		WriteBdaEntries(PageIndex(buffer.CpuAddress()), size_pages, buffer.BufferDeviceAddress());
	} else {
		const auto found = m_buffers.find(buffer.CpuAddress());
		EXIT_IF(found == m_buffers.end() || found->second != id);
		m_buffers.erase(found);
		EXIT_IF(buffer.Size() > m_total_used_memory);
		m_total_used_memory -= buffer.Size();
		m_lru_cache.Free(buffer.lru_id);
		WriteBdaEntries(PageIndex(buffer.CpuAddress()), size_pages, 0);
		buffer.is_deleted = true;
	}
}

void BufferCache::WriteBdaEntries(uint64_t first_page, uint64_t pages,
                                  vk::DeviceAddress first_address) {
	constexpr auto Entry = sizeof(vk::DeviceAddress);
	EXIT_IF(first_page + pages > CACHING_NUMPAGES);
	if (m_bda_directory.empty()) {
		m_bda_directory.assign(BDA_DIRECTORY_ENTRIES, 0);
		m_bda_chunk_live.assign(BDA_CHUNK_COUNT, 0);
		m_bda_free_chunks.resize(BDA_CHUNK_COUNT);
		for (uint32_t slot = 0; slot < BDA_CHUNK_COUNT; slot++) {
			m_bda_free_chunks[slot] = static_cast<uint32_t>(BDA_CHUNK_COUNT - 1u - slot);
		}
	}
	std::vector<vk::DeviceAddress> addresses;
	for (uint64_t page = first_page; page < first_page + pages;) {
		const auto directory = page >> BDA_CHUNK_BITS;
		const auto run_end   = std::min(first_page + pages, (directory + 1u) << BDA_CHUNK_BITS);
		const auto count     = run_end - page;
		auto&      slot_plus = m_bda_directory[directory];
		if (first_address != 0 && slot_plus == 0) {
			if (m_bda_free_chunks.empty()) {
				EXIT("BufferCache: BDA chunk pool exhausted (%" PRIu64 " chunks of %" PRIu64
				     " MiB guest space); raise BDA_CHUNK_COUNT\n",
				     BDA_CHUNK_COUNT, (BDA_CHUNK_PAGES << CACHING_PAGEBITS) >> 20u);
			}
			slot_plus = m_bda_free_chunks.back() + 1u;
			m_bda_free_chunks.pop_back();
			m_bda_chunks_peak = std::max(
			    m_bda_chunks_peak, static_cast<uint32_t>(BDA_CHUNK_COUNT - m_bda_free_chunks.size()));
			const vk::DeviceAddress chunk_element =
			    BDA_DIRECTORY_ENTRIES + uint64_t {slot_plus - 1u} * BDA_CHUNK_PAGES;
			WriteDataBuffer(m_bda_pagetable_buffer, directory * Entry, &chunk_element, Entry);
		}
		if (slot_plus != 0) {
			const auto slot    = slot_plus - 1u;
			const auto element = BDA_DIRECTORY_ENTRIES + uint64_t {slot} * BDA_CHUNK_PAGES +
			                     (page & (BDA_CHUNK_PAGES - 1u));
			if (first_address != 0) {
				addresses.resize(count);
				for (uint64_t i = 0; i < count; i++) {
					addresses[i] = first_address + ((page - first_page + i) << CACHING_PAGEBITS);
				}
				WriteDataBuffer(m_bda_pagetable_buffer, element * Entry, addresses.data(),
				                count * Entry);
				m_bda_chunk_live[slot] += static_cast<uint32_t>(count);
			} else {
				m_bda_pagetable_buffer.Fill(element * Entry, count * Entry, 0);
				EXIT_IF(m_bda_chunk_live[slot] < count);
				m_bda_chunk_live[slot] -= static_cast<uint32_t>(count);
				if (m_bda_chunk_live[slot] == 0) {
					// Its pages are all zero again: the chunk can serve another region.
					m_bda_pagetable_buffer.Fill(directory * Entry, Entry, 0);
					m_bda_free_chunks.push_back(slot);
					slot_plus = 0;
				}
			}
		}
		page = run_end;
	}
}

void BufferCache::DescribeGuestPages(uint64_t address, uint64_t size) {
	PageTable::PageRange pages {};
	if (!PageTable::TryGetPageRange(address, size, pages)) {
		return;
	}
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		const auto  page_address = static_cast<uint64_t>(page) << CACHING_PAGEBITS;
		const auto* id           = m_page_table.Find(page);
		const auto* buffer = id != nullptr && *id ? m_slot_buffers.try_get(*id) : nullptr;
		if (buffer == nullptr) {
			std::printf("      guest page 0x%" PRIx64 ": no buffer (table entry should be 0)\n",
			            page_address);
			continue;
		}
		const auto first_page = PageIndex(buffer->CpuAddress()) << CACHING_PAGEBITS;
		std::printf("      guest page 0x%" PRIx64 ": buffer guest=0x%" PRIx64 " size=0x%" PRIx64
		            " deleted=%d table va=0x%" PRIx64 "\n",
		            page_address, buffer->CpuAddress(), buffer->Size(), buffer->is_deleted ? 1 : 0,
		            buffer->HasDeviceAddress()
		                ? buffer->BufferDeviceAddress() + (page_address - first_page)
		                : 0);
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
	if (!buffer.is_deleted) {
		m_lru_cache.Touch(buffer.lru_id, LruClock());
	}
}

void BufferCache::DeleteBuffer(BufferId id) {
	if (IsBufferInvalid(id)) {
		return;
	}
	Unregister(id);
	EraseBuffer(id);
}

// KYTY_BUFFER_RETIRE_FRAMES=N (live, research, default 0): a deleted buffer keeps its device
// memory until the GPU is past the tick it was deleted at and N more frames were presented, at
// most KYTY_BUFFER_RETIRE_MB (live, default 256) held. The buffer half of the stale-read test
// (notes/cloud/T3_IMAGE_LIFETIME.md): runs 88/97/99 lost the device reading "a destroyed buffer's
// VA + offset"; if that stops with this on, the reader holds a buffer address past the delete.
void BufferCache::EraseBuffer(BufferId id, bool gpu_done) {
	KYTY_PROFILER_FUNCTION();
	if (!m_pending_write_readbacks.empty()) {
		CompleteQueueReadbacksOf(m_slot_buffers[id].Handle());
	}
	static auto& retire_frames = Common::LiveSwitches::Get("KYTY_BUFFER_RETIRE_FRAMES", 0);
	const auto   frames        = retire_frames.load(std::memory_order_relaxed);
	if (frames > 0 && m_scheduler.Active()) {
		const auto bytes = m_slot_buffers[id].Size();
		m_retiring_buffers.push_back(
		    {id, m_scheduler.CurrentTick(),
		     m_graphics.presented_frames.load(std::memory_order_relaxed) +
		         static_cast<uint64_t>(frames),
		     bytes});
		m_retiring_bytes += bytes;
		static auto& retire_mb = Common::LiveSwitches::Get("KYTY_BUFFER_RETIRE_MB", 256);
		const auto   cap =
		    static_cast<uint64_t>(std::max<int64_t>(0, retire_mb.load(std::memory_order_relaxed)))
		    << 20u;
		while (m_retiring_bytes > cap && m_retiring_buffers.size() > 1) {
			const auto oldest = m_retiring_buffers.front();
			m_retiring_buffers.pop_front();
			m_retiring_bytes -= std::min(m_retiring_bytes, oldest.bytes);
			m_scheduler.DeferOperation([this, oldest_id = oldest.id] { m_slot_buffers.erase(oldest_id); });
		}
		return;
	}
	if (m_scheduler.Active() && !gpu_done) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

void BufferCache::RetireBuffers() {
	if (m_retiring_buffers.empty()) {
		return;
	}
	static auto& retire_frames = Common::LiveSwitches::Get("KYTY_BUFFER_RETIRE_FRAMES", 0);
	const bool   all   = retire_frames.load(std::memory_order_relaxed) <= 0;
	const auto   frame = m_graphics.presented_frames.load(std::memory_order_relaxed);
	while (!m_retiring_buffers.empty()) {
		const auto& front = m_retiring_buffers.front();
		if (!all && (frame < front.frame || !m_scheduler.IsFree(front.tick))) {
			break;
		}
		if (m_scheduler.Active() && !m_scheduler.IsFree(front.tick)) {
			m_scheduler.DeferOperation([this, id = front.id] { m_slot_buffers.erase(id); });
		} else {
			// Free, or no scheduler to defer to (it drained when it went inactive).
			m_slot_buffers.erase(front.id);
		}
		m_retiring_bytes -= std::min(m_retiring_bytes, front.bytes);
		m_retiring_buffers.pop_front();
	}
}

bool BufferCache::DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	// One reservation cannot exceed the download ring, so a larger range goes in ring-sized
	// windows; a window that does not fit drains the ring before it is mapped.
	const auto capacity = m_download_buffer.Size();
	bool       any      = false;
	for (uint64_t offset = 0; offset < size; offset += capacity) {
		any |= DownloadBufferWindow(buffer, vaddr + offset, std::min(capacity, size - offset));
	}
	return any;
}

bool BufferCache::DownloadBufferWindow(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "buffer download");
		    std::unique_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		    Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    copies.emplace_back(start - buffer_address, total_size, end - start);
			    // Keep packed ranges on separate cache lines, as in shadPS4.
			    total_size += Common::AlignUp(end - start, 64);
			    m_downloading_ranges.Add(start, end - start);
		    });
		    m_gpu_modified_ranges.Subtract(address, bytes);
	    });
	if (copies.empty()) {
		return false;
	}
	const auto capacity = m_download_buffer.Size();
	for (size_t first = 0; first < copies.size();) {
		const auto base       = copies[first].dstOffset;
		auto       last       = first;
		uint64_t   batch_size = 0;
		while (last < copies.size()) {
			const auto end = copies[last].dstOffset - base + Common::AlignUp(copies[last].size, 64);
			if (end > capacity) {
				break;
			}
			batch_size = end;
			last++;
		}
		EXIT_IF(last == first);
		std::vector<vk::BufferCopy> batch(copies.begin() + static_cast<std::ptrdiff_t>(first),
		                                  copies.begin() + static_cast<std::ptrdiff_t>(last));
		for (auto& copy: batch) {
			copy.dstOffset -= base;
		}
		DownloadBufferCopies(buffer, std::move(batch), batch_size);
		first = last;
	}
	return true;
}

void BufferCache::DownloadBufferCopies(Buffer& buffer, std::vector<vk::BufferCopy> copies,
                                       uint64_t total_size) {
	const auto buffer_address = buffer.CpuAddress();
	auto [mapped, offset]     = m_download_buffer.Map(total_size, 64);
	std::unique_ptr<Buffer> temporary;
	if (mapped == nullptr) {
		temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		                                     vk::BufferUsageFlagBits::eTransferDst, total_size);
		mapped = temporary->Mapped().data();
	} else {
		m_download_buffer.Commit();
	}
	const auto& download = temporary ? *temporary : m_download_buffer;
	for (auto& copy: copies) {
		copy.dstOffset += offset;
	}

	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto              native = command.Recorder();
	vk::BufferMemoryBarrier before {};
	before.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = buffer.Handle();
	before.offset              = 0;
	before.size                = buffer.Size();
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &before, 0,
	                       nullptr);
	native.copyBuffer(buffer.Handle(), download.Handle(),
	                  static_cast<uint32_t>(copies.size()), copies.data());

	auto after          = before;
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	after.buffer        = download.Handle();
	after.offset        = offset;
	after.size          = total_size;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands |
	                           vk::PipelineStageFlagBits::eHost,
	                       {}, 0, nullptr, 1, &after, 0, nullptr);
	m_scheduler.DeferPriorityOperation([this, mapped, offset, total_size, buffer_address,
	                                    copies = std::move(copies), owner = std::move(temporary)] {
		(owner ? *owner : m_download_buffer).Invalidate(offset, total_size);
		if (ReadbackStats::On()) {
			uint64_t             changed = 0;
			uint64_t             total   = 0;
			std::vector<uint8_t> current;
			for (const auto& copy: copies) {
				current.resize(copy.size);
				const auto* downloaded = mapped + (copy.dstOffset - offset);
				if (Libs::LibKernel::Memory::TryReadBacking(buffer_address + copy.srcOffset,
				                                            current.data(), copy.size)) {
					for (uint64_t b = 0; b < copy.size; b++) {
						changed += current[b] != downloaded[b] ? 1 : 0;
					}
					total += copy.size;
				}
			}
			ReadbackStats::NoteCompare(changed, total);
		}
		for (const auto& copy: copies) {
			Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
			                                      mapped + (copy.dstOffset - offset), copy.size);
		}
		std::unique_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		for (const auto& copy: copies) {
			m_downloading_ranges.Subtract(buffer_address + copy.srcOffset, copy.size);
		}
	});
}

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             BDA_PAGETABLE_SIZE),
      m_memory_tracker(page_manager),
      m_staging_buffer(graphics, scheduler, MemoryUsage::Upload, 512 * MiB),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, 64 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 64 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_texture_cache(texture_cache) {
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
	SetVulkanObjectNameF(m_graphics.device, m_bda_pagetable_buffer.Handle(),
	                     "BDA Page Table Buffer");
	SetGuestPageDescriber(
	    [this](uint64_t address, uint64_t size) { DescribeGuestPages(address, size); });
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	SetVulkanObjectNameF(m_graphics.device, GetBuffer(null_id).Handle(), "Kyty.NullBuffer");
	if (!m_graphics.CanReportMemoryUsage()) {
		return;
	}
	constexpr int64_t GiB              = 1024ll * 1024 * 1024;
	constexpr int64_t target_threshold = 8 * GiB;
	const auto        budget =
	    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
	const auto threshold = std::min(budget, target_threshold);
	const auto expected  = std::min(budget - 6 * threshold / 10, budget - GiB);
	const auto critical  = std::min(budget - 2 * threshold / 10, budget - GiB / 2);
	m_trigger_gc_memory  = static_cast<uint64_t>(std::max<int64_t>(expected, GiB));
	m_critical_gc_memory = static_cast<uint64_t>(std::max<int64_t>(critical, 2 * GiB));
}

BufferCache::~BufferCache() {
	if (auto& queue = m_queue_readbacks; queue.semaphore != nullptr) {
		WaitQueueReadback(QueueReadbackTickBit | (queue.next_value - 1));
		for (auto& slot: queue.slots) {
			slot.staging.reset();
		}
		m_graphics.device.destroyCommandPool(queue.pool, nullptr);
		m_graphics.device.destroySemaphore(queue.semaphore, nullptr);
		queue.pool      = nullptr;
		queue.semaphore = nullptr;
	}
	if (auto& state = m_copy_queue_readback; state.pool != nullptr) {
		state.staging.reset();
		m_graphics.device.destroyFence(state.fence, nullptr);
		m_graphics.device.destroyCommandPool(state.pool, nullptr);
		state = {};
	}
	if (!m_gpu_modified_ranges.Empty()) {
		EXIT("BufferCache: destroyed with pending GPU-modified ranges\n");
	}
	for (const auto& [vaddr, id]: m_buffers) {
		(void)vaddr;
		const auto& buffer = m_slot_buffers[id];
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: destroyed with GPU-modified buffer\n");
		}
	}
	m_buffers.clear();
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size, uint64_t widen) {
	KYTY_PROFILER_FUNCTION();
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid memory-invalidation range\n");
	}
	m_memory_tracker.InvalidateRegion(
	    vaddr, size, [this, vaddr, size] { ReadMemory(vaddr, size, true); }, widen);
}

bool BufferCache::WriteClean(uint64_t vaddr, const void* data, uint64_t size) {
	if (!GuestGpu::IsGpuThread() || size == 0 || !GuestRange {vaddr, size}.Valid() ||
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size)) {
		return false;
	}
	{
		// A download publishes the GPU's older value of these bytes when it lands.
		std::shared_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		if (m_downloading_ranges.Intersects(vaddr, size)) {
			return false;
		}
	}

	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner == nullptr || !*owner || !m_slot_buffers[*owner].IsInBounds(vaddr, size) ||
	    !Libs::LibKernel::Memory::TryWriteBacking(vaddr, data, size)) {
		return false;
	}
	WriteDataBuffer(m_slot_buffers[*owner], vaddr, data, size);
	if (HasGpuDirtyBytes(vaddr, size)) {
		// Overwritten in full: guest memory holds the value the GPU copy will have.
		std::unique_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		m_gpu_modified_ranges.Subtract(vaddr, size);
	}
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write) {
	KYTY_PROFILER_FUNCTION();
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	auto& gpu = m_scheduler.Context().GetGpu();
	// KYTY_ASYNC_WRITE_READBACK=1 (a live switch, off by default): a guest write to GPU-written
	// memory no longer drains the GPU on the GPU thread. The GPU thread records the window's
	// download and submits it; the faulting guest thread waits for that submission, and a second
	// command lifts the window's GPU ownership, or resolves the fault synchronously if the GPU
	// wrote there again in the meantime.
	// KYTY_ASYNC_READBACK=1 (live, default 0) does the same for guest reads. Wolverine run 36
	// (KYTY_READBACK_STATS): every readback at the spot came from a guest thread, and serving
	// them synchronously cost the GPU thread ~1.3 s per 5 s, mostly waiting for the GPU.
	const bool stale_reads = !is_write && !GuestGpu::IsGpuThread() &&
	                         !Libs::LibKernel::Memory::ForceSyncReadback() &&
	                         StaleInflightReadsEnabled();
	if (stale_reads && ServeStaleRead(vaddr)) {
		Libs::LibKernel::Memory::RequestServeFromBacking();
		return;
	}
	if (const auto stale_all = !is_write && !GuestGpu::IsGpuThread() &&
	                                   !Libs::LibKernel::Memory::ForceSyncReadback()
	                               ? StaleAllMode()
	                               : 0;
	    stale_all == 1 || stale_all == 2) {
		bool posted = false;
		if (stale_all == 2 && !ServeRefreshingRead(vaddr)) {
			posted = gpu.TrySendCommand([this, vaddr, size] { BeginBackgroundReadback(vaddr, size); });
		}
		NoteStaleAllRead(posted);
		Libs::LibKernel::Memory::RequestServeFromBacking();
		return;
	}
	if (!GuestGpu::IsGpuThread() && !Libs::LibKernel::Memory::ForceSyncReadback() &&
	    (is_write ? AsyncWriteReadbackEnabled() : AsyncReadReadbackEnabled())) {
		uint64_t tick         = 0;
		uint64_t window_begin = 0;
		uint64_t window_end   = 0;
		uint64_t stale_until  = 0;
		gpu.SendCommandSync([&] {
			ReadbackStats::s_forwarded = true;
			tick = BeginWriteReadback(vaddr, size, is_write, window_begin, window_end);
			stale_until = std::exchange(m_stale_read_until, 0);
			ReadbackStats::s_forwarded = false;
		});
		if (stale_until != 0) {
			NoteStaleRead(vaddr, stale_until);
			Libs::LibKernel::Memory::RequestServeFromBacking();
			return;
		}
		if (tick != 0) {
			{
				KYTY_PROFILER_BLOCK("BufferCache::WaitWriteReadback");
				Common::Telemetry::ScopedTime telemetry(Common::Telemetry::Counter::ReadbackWait);
				Common::Telemetry::Add(Common::Telemetry::Counter::ReadbackWaits, 1);
				if ((tick & QueueReadbackTickBit) != 0) {
					// KYTY_READBACK_QUEUE_ASYNC: only the readback queue's copy is waited for.
					WaitQueueReadback(tick);
					if (QueueGuestCopyEnabled()) {
						CopyQueueReadbackOnGuest(tick);
					}
				} else {
					// Submitted by BeginWriteReadback, so this waits without submitting.
					EXIT_IF(tick >= m_scheduler.CurrentTick());
					m_scheduler.Wait(tick);
					m_scheduler.WaitPriorityOperations(tick);
				}
			}
			bool redirtied = false;
			gpu.SendCommandSync([&] {
				redirtied = !FinishWriteReadback(vaddr, size, is_write, window_begin, window_end,
				                                 tick, !is_write && AsyncReadSnapshotEnabled());
			});
			if (redirtied) {
				// The download holds the bytes as of this read; the fault handler completes the
				// load from them (KYTY_ASYNC_READ_SNAPSHOT).
				Libs::LibKernel::Memory::RequestServeFromBacking();
			}
		}
		return;
	}
	gpu.SendCommandSync([this, vaddr, size, is_write] {
		ReadbackStats::s_forwarded = true;
		ReadMemoryOnGpu(vaddr, size, is_write);
		ReadbackStats::s_forwarded = false;
	});
}

void BufferCache::ReadMemoryOnGpu(uint64_t vaddr, uint64_t size, bool is_write) {
	KYTY_PROFILER_BLOCK("BufferCache::ReadMemory(GPU thread)");
	Common::Telemetry::ScopedTime telemetry(Common::Telemetry::Counter::GpuReadMemory);
	ReadbackStats::Scope stats(vaddr, is_write);
	if (is_write && !IsRegionRegistered(vaddr, size)) {
		return;
	}
	auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];
	stats.SetBuffer(buffer.CpuAddress(), buffer.Size());

	// The page is protected as GPU-written, but none of its bytes are waiting for a download:
	// the bytes the GPU wrote are elsewhere in the window, or were downloaded with another
	// page. The page is current, so lift its protection. Downloading the window instead drained
	// the GPU (~30 ms a fault) for guest reads of structs next to the command processor's
	// labels.
	const auto page_begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto page_end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	// Pages of an asynchronous write readback stay GPU-owned until its download is
	// published.
	CompletePendingWriteReadbacks(page_begin, page_end);
	// KYTY_WRITE_BITMAP: the window's tracked pages the GPU did not write stop being GPU-written,
	// so this page may need no download and the window downloads only what was written.
	const bool unprotected = RefineWithWriteBitmap(buffer, vaddr, size);
	if ((unprotected || m_memory_tracker.IsRegionGpuModified(page_begin, page_end - page_begin)) &&
	    !HasGpuDirtyBytes(page_begin, page_end - page_begin)) {
		if (!unprotected) {
			m_memory_tracker.UnmarkRegionAsGpuModified(page_begin, page_end - page_begin);
		}
		if (is_write) {
			m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		}
		stats.SetOutcome(ReadbackStats::Unmarked);
		return;
	}

	// Widen nearby CPU reads so they share one GPU drain.
	const uint64_t WindowSize   = ReadbackWindowSize();
	const auto     buffer_begin = buffer.CpuAddress();
	const auto     buffer_end   = buffer_begin + buffer.Size();
	auto           window_begin = std::max(Common::AlignDown(vaddr, WindowSize), buffer_begin);
	auto window_end = std::min(std::max(window_begin + WindowSize, vaddr + size), buffer_end);
	if (const auto narrow_begin = window_begin, narrow_end = window_end;
	    WidenSparseReadback(buffer, window_begin, window_end) && RangeTickMode() != 0 &&
	    m_scheduler.IsFree(WriterTick(buffer, narrow_begin, narrow_end)) &&
	    !m_scheduler.IsFree(WriterTick(buffer, window_begin, window_end))) {
		// KYTY_READBACK_RANGE_TICK: the whole buffer would wait for a running writer, the
		// window alone for none.
		window_begin = narrow_begin;
		window_end   = narrow_end;
	}

	if (TryDirectReadback(buffer, vaddr, size, is_write, window_begin, window_end)) {
		stats.SetOutcome(ReadbackStats::DirectRead);
		return;
	}

	if (stats.Enabled()) {
		stats.FindWriter([this](uint64_t tick) { return m_scheduler.IsFree(tick); });
		uint64_t         dirty_bytes = 0;
		std::shared_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		m_gpu_modified_ranges.ForEachInRange(
		    window_begin, window_end - window_begin,
		    [&](uint64_t start, uint64_t end) { dirty_bytes += end - start; });
		stats.SetDownloadBytes(dirty_bytes);
		stats.SetOwnBytesDirty(m_gpu_modified_ranges.Intersects(vaddr, std::max<uint64_t>(size, 1)));
	}
	if (TryCopyQueueReadback(buffer, window_begin, window_end)) {
		stats.SetOutcome(ReadbackStats::Downloaded);
		m_memory_tracker.UnmarkRegionAsGpuModified(window_begin, window_end - window_begin);
	} else if (DownloadBufferMemory(buffer, window_begin, window_end - window_begin)) {
		stats.SetOutcome(ReadbackStats::Downloaded);
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Wait(tick);
		m_scheduler.WaitPriorityOperations(tick);
		m_memory_tracker.UnmarkRegionAsGpuModified(window_begin, window_end - window_begin);
	} else {
		stats.SetOutcome(ReadbackStats::NothingToDownload);
	}
	if (is_write) {
		m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
	}
}

// KYTY_DIRECT_READBACK (a live switch, off by default): when the buffer lives in host-visible
// device memory and every GPU write recorded into it has completed, the window's GPU-written bytes
// are copied straight from the mapping into guest memory: no GPU copy, submission or drain.
// Submissions end with a device-to-host barrier while such buffers exist (CommandScheduler).
// The window is the one a download would cover, so as many later faults are spared; a window with
// more than KYTY_DIRECT_READBACK_KB (live, default 32) GPU-written bytes downloads as before, since
// the host reads the mapping uncached. Wolverine 2026-10-22, faulting pages only: ~50k direct
// readbacks per 5 s instead of ~5k downloads, 10-12 -> 3-4 fps.
bool BufferCache::TryDirectReadback(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_write,
                                    uint64_t window_begin, uint64_t window_end) {
	if (!DirectReadbackEnabled() || buffer.Mapped().empty() ||
	    !m_scheduler.IsFree(WriterTick(buffer, window_begin, window_end))) {
		return false;
	}
	const auto begin = Common::AlignDown(window_begin, TRACKER_PAGE_SIZE);
	const auto end   = std::min(Common::AlignUp(window_end, TRACKER_PAGE_SIZE),
	                            buffer.CpuAddress() + buffer.Size());
	if (end <= begin || !buffer.IsInBounds(begin, end - begin) ||
	    OverlapsPendingWriteReadback(begin, end)) {
		return false;
	}
	static auto& budget_kb = Common::LiveSwitches::Get("KYTY_DIRECT_READBACK_KB", 32);
	const auto   budget =
	    static_cast<uint64_t>(std::max<int64_t>(budget_kb.load(std::memory_order_relaxed), 0))
	    << 10u;
	KYTY_PROFILER_BLOCK("BufferCache::DirectReadback");
	const auto start_time = std::chrono::steady_clock::now();
	// As the downloads: the GPU-written bytes of the window's GPU-owned pages (tracker lock, then
	// the dirty ranges, as DownloadBufferWindow).
	auto& pages = m_direct_pages;
	pages.clear();
	uint64_t dirty = 0;
	m_memory_tracker.ForEachDownloadRange<false>(
	    begin, end - begin, [&](uint64_t address, uint64_t bytes) noexcept {
		    pages.emplace_back(address, bytes);
		    std::shared_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		    Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		    m_gpu_modified_ranges.ForEachInRange(
		        address, bytes, [&](uint64_t start, uint64_t stop) { dirty += stop - start; });
	    });
	if (dirty > budget) {
		m_direct_stats.over_budget++;
		return false;
	}
	uint64_t copied = 0;
	{
		std::unique_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		// A queued download of these bytes would publish them again when it lands.
		if (m_downloading_ranges.Intersects(begin, end - begin)) {
			return false;
		}
		// Taken again under the lock: a guest write can have overwritten some of them since.
		auto& ranges = m_direct_ranges;
		ranges.clear();
		for (const auto [address, bytes]: pages) {
			m_gpu_modified_ranges.ForEachInRange(address, bytes,
			                                     [&](uint64_t start, uint64_t stop) {
				                                     ranges.emplace_back(start, stop);
			                                     });
		}
		const auto* mapped = buffer.Mapped().data();
		for (const auto [start, stop]: ranges) {
			Libs::LibKernel::Memory::WriteBacking(start, mapped + buffer.Offset(start),
			                                      stop - start);
			m_gpu_modified_ranges.Subtract(start, stop - start);
			copied += stop - start;
		}
	}
	m_memory_tracker.UnmarkRegionAsGpuModified(begin, end - begin);
	if (is_write) {
		m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
	}
	m_direct_readbacks++;
	m_direct_stats.bytes += copied;
	m_direct_stats.ns += static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
	                                                         start_time)
	        .count());
	return true;
}

// Research: KYTY_COPY_QUEUE_READBACK=1 (live). A readback recorded the window's download into the
// current command buffer and waited for its tick: a drain of everything recorded and queued,
// ~4 ms a fault. In Wolverine the writer of the faulting bytes has almost always finished long
// before (readback stats: 1016 of 1017), so the bytes are final in device memory. When every GPU
// write to the buffer has executed, the GPU-written bytes of the window are copied on the second
// queue of the graphics family instead, behind nothing: its submission waits for the writer's
// tick on the timeline semaphore (the memory dependency), and only this copy is waited for.
static bool CopyQueueReadbackEnabled() {
	// Wolverine run 18, same process: 0 -> 110 frames / 15 s, 1 -> 126-136; readback time on
	// Thread_Gpu 1.22 -> 0.62 s per 5 s; no visual change.
	static auto& enabled = Common::LiveSwitches::Get("KYTY_COPY_QUEUE_READBACK", 1);
	return enabled.load(std::memory_order_relaxed) != 0;
}

// KYTY_COPY_QUEUE_INFLIGHT=1 (live, default 0): the copy-queue readback also serves buffers whose
// last GPU write has not finished yet. Its submission already waits for the writer's tick on the
// timeline semaphore, so only that work is waited for; before, such a readback recorded the copy
// on the main queue and waited for everything recorded so far (run 31 readback stats: 180 of 406
// readbacks of Wolverine's busiest buffer per 5 s). A writer still in the open command buffer is
// submitted first.
static bool CopyQueueInflightEnabled() {
	static auto& enabled = Common::LiveSwitches::Get("KYTY_COPY_QUEUE_INFLIGHT", 0);
	return enabled.load(std::memory_order_relaxed) != 0;
}

bool BufferCache::TryCopyQueueReadback(Buffer& buffer, uint64_t window_begin, uint64_t window_end) {
	constexpr uint64_t StagingSize = 4ull * 1024 * 1024;
	if (!CopyQueueReadbackEnabled() || m_graphics.readback_queue == nullptr ||
	    OverlapsPendingWriteReadback(window_begin, window_end)) {
		return false;
	}
	const uint64_t writer_tick      = WriterTick(buffer, window_begin, window_end);
	const bool     writer_in_flight = !m_scheduler.IsFree(writer_tick);
	if (writer_in_flight && !CopyQueueInflightEnabled()) {
		return false;
	}
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	{
		std::shared_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		// A queued main-queue download of these bytes would publish them again when it lands.
		if (m_downloading_ranges.Intersects(window_begin, window_end - window_begin)) {
			return false;
		}
		m_memory_tracker.ForEachDownloadRange<false>(
		    window_begin, window_end - window_begin,
		    [&](uint64_t address, uint64_t bytes) noexcept {
			    m_gpu_modified_ranges.ForEachInRange(
			        address, bytes, [&](uint64_t start, uint64_t end) {
				        copies.emplace_back(start - buffer_address, total_size, end - start);
				        total_size += Common::AlignUp(end - start, 64);
			        });
		    });
	}
	if (copies.empty() || total_size > StagingSize) {
		return false;
	}
	if (writer_in_flight) {
		KYTY_PROFILER_BLOCK("BufferCache::CopyQueueReadbackFlush");
		if (writer_tick >= m_scheduler.CurrentTick()) {
			// The writer is in the open command buffer: submit it so the copy can wait for its
			// tick.
			m_scheduler.Flush();
		}
		// With KYTY_RECORD_THREAD a submitted tick can still be queued on the recording thread.
		// The copy must not wait for a value whose signal the driver has not seen: run 34 hung
		// with the readback waiting on it, the present (holding queue_mutex) stalled behind that
		// wait, and the recording thread blocked on queue_mutex with the writer's submission.
		(void)m_scheduler.DrainRecording();
	}

	KYTY_PROFILER_BLOCK("BufferCache::CopyQueueReadback");
	Common::Telemetry::ScopedTime telemetry(Common::Telemetry::Counter::GpuCopyQueue);
	auto& state  = m_copy_queue_readback;
	auto  device = m_graphics.device;
	if (state.pool == nullptr) {
		vk::CommandPoolCreateInfo pool_info {};
		pool_info.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
		pool_info.queueFamilyIndex = m_graphics.queue_family;
		RequireVulkanSuccess(device.createCommandPool(&pool_info, nullptr, &state.pool),
		                     "create readback command pool");
		vk::CommandBufferAllocateInfo allocate {};
		allocate.commandPool        = state.pool;
		allocate.level              = vk::CommandBufferLevel::ePrimary;
		allocate.commandBufferCount = 1;
		RequireVulkanSuccess(device.allocateCommandBuffers(&allocate, &state.command),
		                     "allocate readback command buffer");
		vk::FenceCreateInfo fence_info {};
		RequireVulkanSuccess(device.createFence(&fence_info, nullptr, &state.fence),
		                     "create readback fence");
		state.staging =
		    std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		                             vk::BufferUsageFlagBits::eTransferDst, StagingSize);
	}

	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	RequireVulkanSuccess(state.command.begin(&begin), "begin readback command buffer");
	state.command.copyBuffer(buffer.Handle(), state.staging->Handle(),
	                         static_cast<uint32_t>(copies.size()), copies.data());
	vk::BufferMemoryBarrier to_host {};
	to_host.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	to_host.dstAccessMask       = vk::AccessFlagBits::eHostRead;
	to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_host.buffer              = state.staging->Handle();
	to_host.offset              = 0;
	to_host.size                = total_size;
	state.command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                              vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1, &to_host, 0,
	                              nullptr);
	RequireVulkanSuccess(state.command.end(), "end readback command buffer");

	// writer_tick: the last GPU work that wrote the copied bytes (WriterTick).
	const vk::Semaphore             master      = m_scheduler.GetMasterSemaphore().Handle();
	const vk::PipelineStageFlags    wait_stage  = vk::PipelineStageFlagBits::eTransfer;
	vk::TimelineSemaphoreSubmitInfo timeline {};
	timeline.waitSemaphoreValueCount = 1;
	timeline.pWaitSemaphoreValues    = &writer_tick;
	vk::SubmitInfo submit {};
	submit.pNext              = &timeline;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores    = &master;
	submit.pWaitDstStageMask  = &wait_stage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &state.command;
	Common::Telemetry::ScopedGpuTime submit_time(Common::Telemetry::Counter::GpuSubmit);
	RequireVulkanSuccess(m_graphics.readback_queue.submit(1, &submit, state.fence),
	                     "submit readback");
	Common::Telemetry::ScopedGpuTime fence_wait(Common::Telemetry::Counter::GpuFenceWait);
	RequireVulkanSuccess(device.waitForFences(1, &state.fence, VK_TRUE, UINT64_MAX),
	                     "wait for readback");
	RequireVulkanSuccess(device.resetFences(1, &state.fence), "reset readback fence");

	state.staging->Invalidate(0, total_size);
	const auto* mapped = state.staging->Mapped().data();
	for (const auto& copy: copies) {
		Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
		                                      mapped + copy.dstOffset, copy.size);
	}
	// Guest memory holds the bytes before they stop counting as GPU-written, so a concurrent
	// clean read never sees them stale.
	std::unique_lock lock(m_dirty_ranges_mutex, std::defer_lock);
	Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
	for (const auto& copy: copies) {
		m_gpu_modified_ranges.Subtract(buffer_address + copy.srcOffset, copy.size);
	}
	return true;
}

// GPU thread. Returns the tick the guest must wait for, or 0 when the fault is already resolved.
uint64_t BufferCache::BeginWriteReadback(uint64_t vaddr, uint64_t size, bool is_write,
                                         uint64_t& window_begin, uint64_t& window_end) {
	KYTY_PROFILER_FUNCTION();
	if (!is_write && !IsRegionRegistered(vaddr, size)) {
		// A read of an unregistered region: the synchronous path finds its buffer.
		ReadMemoryOnGpu(vaddr, size, false);
		return 0;
	}
	ReadbackStats::Scope stats(vaddr, is_write);
	if (!IsRegionRegistered(vaddr, size)) {
		return 0;
	}
	const auto page_begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto page_end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	// Another guest's readback of this page is still landing: wait for the same download.
	for (const auto& pending: m_pending_write_readbacks) {
		if (pending.begin < page_end && page_begin < pending.end) {
			window_begin = pending.begin;
			window_end   = pending.end;
			return pending.tick;
		}
	}
	auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];
	stats.SetBuffer(buffer.CpuAddress(), buffer.Size());
	// KYTY_WRITE_BITMAP, as in ReadMemoryOnGpu.
	const bool unprotected = RefineWithWriteBitmap(buffer, vaddr, size);
	const auto read_page = vaddr / TRACKER_PAGE_SIZE;
	if (!is_write && StaleInflightReadsEnabled() && !Libs::LibKernel::Memory::ForceSyncReadback() &&
	    !m_scheduler.IsFree(buffer.last_gpu_write_tick)) {
		// KYTY_STALE_INFLIGHT_READS: the guest reads guest memory as it is, if a download
		// refreshed this page within KYTY_STALE_INFLIGHT_READS_MS (live, default 100). A large
		// buffer always has some writer running, and its pages would otherwise never refresh.
		static auto& max_age_ms = Common::LiveSwitches::Get("KYTY_STALE_INFLIGHT_READS_MS", 100);
		const auto   found      = m_page_downloaded.find(read_page);
		if (found != m_page_downloaded.end() &&
		    std::chrono::steady_clock::now() - found->second <=
		        std::chrono::milliseconds(max_age_ms.load(std::memory_order_relaxed))) {
			m_stale_read_until = buffer.last_gpu_write_tick;
			return 0;
		}
	}

	// As in ReadMemoryOnGpu: a GPU-owned page without bytes to download only needs unprotecting.
	if ((unprotected || m_memory_tracker.IsRegionGpuModified(page_begin, page_end - page_begin)) &&
	    !HasGpuDirtyBytes(page_begin, page_end - page_begin)) {
		if (!unprotected) {
			m_memory_tracker.UnmarkRegionAsGpuModified(page_begin, page_end - page_begin);
		}
		if (is_write) {
			m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		}
		stats.SetOutcome(ReadbackStats::Unmarked);
		return 0;
	}

	const uint64_t WindowSize   = ReadbackWindowSize();
	const auto     buffer_begin = buffer.CpuAddress();
	const auto     buffer_end   = buffer_begin + buffer.Size();
	window_begin                = std::max(Common::AlignDown(vaddr, WindowSize), buffer_begin);
	window_end = std::min(std::max(window_begin + WindowSize, vaddr + size), buffer_end);
	if (OverlapsPendingWriteReadback(window_begin, window_end)) {
		// The page itself is not pending (checked above), but the window shares bytes with a
		// landing readback (neighbouring pages read by other guest threads): download only the
		// faulting pages. Before, reads went synchronous here (run 42: ~110 ms/s left on the GPU
		// thread for Wolverine's busiest readback buffer).
		window_begin = std::max(page_begin, buffer_begin);
		window_end   = std::min(page_end, buffer_end);
	}
	if (const auto narrow_begin = window_begin, narrow_end = window_end;
	    WidenSparseReadback(buffer, window_begin, window_end) && RangeTickMode() != 0 &&
	    m_scheduler.IsFree(WriterTick(buffer, narrow_begin, narrow_end)) &&
	    !m_scheduler.IsFree(WriterTick(buffer, window_begin, window_end))) {
		// KYTY_READBACK_RANGE_TICK: the whole buffer would wait for a running writer, the
		// window alone for none.
		window_begin = narrow_begin;
		window_end   = narrow_end;
	}
	if (stats.Enabled()) {
		// Every path below downloads the window's GPU-written bytes (the readback queue's too).
		stats.FindWriter([this](uint64_t tick) { return m_scheduler.IsFree(tick); });
		uint64_t         dirty_bytes = 0;
		std::shared_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		m_gpu_modified_ranges.ForEachInRange(
		    window_begin, window_end - window_begin,
		    [&](uint64_t start, uint64_t end) { dirty_bytes += end - start; });
		stats.SetDownloadBytes(dirty_bytes);
		stats.SetOwnBytesDirty(m_gpu_modified_ranges.Intersects(vaddr, std::max<uint64_t>(size, 1)));
	}

	// KYTY_DIRECT_READBACK: the bytes are in host-visible device memory, no download needed.
	if (DirectReadbackEnabled()) {
		if (buffer.Mapped().empty()) {
			m_direct_stats.unmapped++;
		} else if (TryDirectReadback(buffer, vaddr, size, is_write, window_begin, window_end)) {
			m_direct_stats.immediate++;
			ReportDirectReadbacks();
			stats.SetOutcome(ReadbackStats::DirectRead);
			return 0;
		} else if (DirectReadbackMode() >= 2 && !m_scheduler.IsFree(WriterTick(buffer, window_begin, window_end))) {
			// The guest waits for the writer's tick only, not for everything recorded after it;
			// FinishWriteReadback then copies from the mapping (an empty window marks this).
			const auto writer_tick = WriterTick(buffer, window_begin, window_end);
			if (writer_tick >= m_scheduler.CurrentTick()) {
				m_scheduler.Flush();
			}
			window_begin = 0;
			window_end   = 0;
			stats.SetOutcome(ReadbackStats::DirectRead);
			return writer_tick;
		}
		ReportDirectReadbacks();
	}

	// KYTY_READBACK_QUEUE_ASYNC: the copy runs on the readback queue, only the guest waits for it.
	if (const auto queue_mode = QueueReadbackMode(); queue_mode != 0) {
		const auto writer_tick = WriterTick(buffer, window_begin, window_end);
		if (queue_mode >= 2 || m_scheduler.IsFree(writer_tick)) {
			if (const auto tick = BeginQueueReadback(buffer, window_begin, window_end, writer_tick);
			    tick != 0) {
				// KYTY_READBACK_QUEUE_FLUSH_US: keep the main queue fed as a main-queue readback does.
				if (const auto interval = QueueReadbackFlushUs(); interval > 0) {
					const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
					                        std::chrono::steady_clock::now().time_since_epoch())
					                        .count();
					if (now_us - m_scheduler.LastSubmitUs() >= interval) {
						m_scheduler.Flush();
						m_queue_readback_stats.flushes++;
					}
				}
				ReportQueueReadbacks();
				stats.SetOutcome(ReadbackStats::Downloaded);
				return tick;
			}
		}
		ReportQueueReadbacks();
	}

	// KYTY_READBACK_RANGE_TICK: the window's bytes were written by work that has finished (1), or
	// by work the readback queue can wait for alone (2): download them there instead of behind
	// everything recorded on the main queue.
	if (const auto range_mode = RangeTickMode(); range_mode != 0 && QueueReadbackMode() == 0) {
		const auto writer_tick = WriterTick(buffer, window_begin, window_end);
		const bool range_done  = m_scheduler.IsFree(writer_tick);
		if ((range_done || range_mode >= 2) &&
		    TryCopyQueueReadback(buffer, window_begin, window_end)) {
			m_memory_tracker.UnmarkRegionAsGpuModified(window_begin, window_end - window_begin);
			if (is_write) {
				m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
			}
			if (range_done) {
				m_range_tick_stats.copy_done++;
				if (!m_scheduler.IsFree(buffer.last_gpu_write_tick)) {
					m_range_tick_stats.spared++;
				}
			} else {
				m_range_tick_stats.copy_inflight++;
			}
			ReportRangeTickReadbacks();
			stats.SetOutcome(ReadbackStats::Downloaded);
			return 0;
		}
		m_range_tick_stats.main_queue++;
		if (!range_done) {
			m_range_tick_stats.range_running++;
		}
		ReportRangeTickReadbacks();
	}

	if (!DownloadBufferMemory(buffer, window_begin, window_end - window_begin)) {
		stats.SetOutcome(ReadbackStats::NothingToDownload);
		if (is_write) {
			m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		}
		return 0;
	}
	stats.SetOutcome(ReadbackStats::Downloaded);
	if (StaleInflightReadsEnabled()) {
		if (m_page_downloaded.size() >= 65536) {
			m_page_downloaded.clear();
		}
		m_page_downloaded[read_page] = std::chrono::steady_clock::now();
	}
	const auto tick = m_scheduler.CurrentTick();
	m_scheduler.Flush();
	m_pending_write_readbacks.push_back({window_begin, window_end, tick});
	return tick;
}

// KYTY_STALE_INFLIGHT_READS, any guest thread: true while reads of this page are served from guest
// memory as it is (its writer has not finished), with a report every 5 s.
bool BufferCache::ServeStaleRead(uint64_t vaddr) {
	uint64_t until = 0;
	{
		std::scoped_lock lock(m_stale_mutex);
		const auto       found = m_stale_pages.find(vaddr / TRACKER_PAGE_SIZE);
		if (found == m_stale_pages.end()) {
			return false;
		}
		until = found->second;
	}
	if (m_scheduler.IsFree(until)) {
		std::scoped_lock lock(m_stale_mutex);
		if (const auto found = m_stale_pages.find(vaddr / TRACKER_PAGE_SIZE);
		    found != m_stale_pages.end() && found->second == until) {
			m_stale_pages.erase(found);
		}
		return false;
	}
	const auto served = m_stale_reads.fetch_add(1, std::memory_order_relaxed) + 1;
	static std::atomic<int64_t> report_ms {0};
	const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	                        std::chrono::steady_clock::now().time_since_epoch())
	                        .count();
	if (auto last = report_ms.load(std::memory_order_relaxed);
	    now_ms - last >= 5000 && report_ms.compare_exchange_strong(last, now_ms)) {
		size_t pages = 0;
		{
			std::scoped_lock lock(m_stale_mutex);
			pages = m_stale_pages.size();
		}
		std::printf("Stale reads (5 s): %" PRIu64 " guest reads served from guest memory, %zu pages\n",
		            served, pages);
		std::fflush(stdout);
		m_stale_reads.store(0, std::memory_order_relaxed);
	}
	return true;
}

void BufferCache::NoteStaleRead(uint64_t vaddr, uint64_t until) {
	std::scoped_lock lock(m_stale_mutex);
	if (m_stale_pages.size() >= 4096) {
		m_stale_pages.clear();
	}
	m_stale_pages[vaddr / TRACKER_PAGE_SIZE] = until;
	m_stale_reads.fetch_add(1, std::memory_order_relaxed);
}

// KYTY_READBACK_STALE_ALL=2, any guest thread: true while a background readback covers the
// page. Otherwise the page is entered as posted and the caller posts its readback.
bool BufferCache::ServeRefreshingRead(uint64_t vaddr) {
	const auto       page_begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	std::scoped_lock lock(m_stale_mutex);
	for (const auto& refresh: m_refreshes) {
		if (refresh.begin <= vaddr && vaddr < refresh.end &&
		    (refresh.tick == UINT64_MAX || !m_scheduler.IsFree(refresh.tick))) {
			return true;
		}
	}
	if (m_refreshes.size() >= 4096) {
		// Entries whose download has landed but whose completion has not run yet.
		std::erase_if(m_refreshes, [this](const BackgroundRefresh& refresh) {
			return refresh.tick != UINT64_MAX && m_scheduler.IsFree(refresh.tick);
		});
	}
	m_refreshes.push_back({page_begin, page_begin + TRACKER_PAGE_SIZE, UINT64_MAX});
	return false;
}

// GPU thread, KYTY_READBACK_STALE_ALL=2: records the window's download as a guest thread's
// asynchronous readback does, without anyone waiting; EndBackgroundReadback publishes it.
void BufferCache::BeginBackgroundReadback(uint64_t vaddr, uint64_t size) {
	const auto page_begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto page_end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	// Downloads of the page that have already landed are published first, without waiting for
	// one that has not.
	for (size_t i = 0; i < m_pending_write_readbacks.size();) {
		const auto& pending = m_pending_write_readbacks[i];
		if (pending.begin < page_end && page_begin < pending.end &&
		    m_scheduler.IsFree(pending.tick)) {
			CompletePendingWriteReadback(i);
		} else {
			i++;
		}
	}
	uint64_t   window_begin = 0;
	uint64_t   window_end   = 0;
	const auto tick         = BeginWriteReadback(vaddr, size, false, window_begin, window_end);
	const bool landing      = tick != 0 && window_begin != window_end;
	{
		std::scoped_lock lock(m_stale_mutex);
		std::erase_if(m_refreshes, [&](const BackgroundRefresh& refresh) {
			return refresh.tick == UINT64_MAX && refresh.begin == page_begin;
		});
		if (landing) {
			m_refreshes.push_back({window_begin, window_end, tick});
		}
	}
	if (landing) {
		m_scheduler.DeferOperation([this, window_begin, window_end, tick] {
			EndBackgroundReadback(window_begin, window_end, tick);
		});
	}
}

// GPU thread, a deferred operation after the background readback's download has landed: as
// CompletePendingWriteReadback, unless the GPU wrote the window again it stops being GPU-owned.
// No waits: a deferred operation runs after its tick and that tick's priority operations, so
// the download is published already (and a deferred operation must not wait for them).
void BufferCache::EndBackgroundReadback(uint64_t begin, uint64_t end, uint64_t tick) {
	for (size_t i = 0; i < m_pending_write_readbacks.size(); i++) {
		const auto& pending = m_pending_write_readbacks[i];
		if (pending.begin == begin && pending.end == end && pending.tick == tick) {
			m_pending_write_readbacks.erase(m_pending_write_readbacks.begin() +
			                                static_cast<std::ptrdiff_t>(i));
			if (!HasGpuDirtyBytes(begin, end - begin)) {
				m_memory_tracker.UnmarkRegionAsGpuModified(begin, end - begin);
			}
			break;
		}
	}
	std::scoped_lock lock(m_stale_mutex);
	std::erase_if(m_refreshes, [&](const BackgroundRefresh& refresh) {
		return refresh.begin == begin && refresh.end == end && refresh.tick == tick;
	});
}

// GPU thread, after the guest waited for the readback's tick and its publication.
bool BufferCache::FinishWriteReadback(uint64_t vaddr, uint64_t size, bool is_write,
                                      uint64_t window_begin, uint64_t window_end, uint64_t tick,
                                      bool snapshot) {
	KYTY_PROFILER_FUNCTION();
	if (window_begin == window_end) {
		return FinishDirectReadback(vaddr, size, is_write, tick, snapshot);
	}
	bool queue_copy_clean = (tick & QueueReadbackTickBit) == 0;
	for (size_t i = 0; i < m_pending_write_readbacks.size(); i++) {
		const auto& pending = m_pending_write_readbacks[i];
		if (pending.begin == window_begin && pending.end == window_end && pending.tick == tick) {
			queue_copy_clean = CompletePendingWriteReadback(i);
			break;
		}
	}
	if (!queue_copy_clean) {
		// The readback queue's copy may have raced the GPU's newer write: no snapshot of it.
		snapshot = false;
	}
	if (!IsRegionRegistered(vaddr, size)) {
		return true;
	}
	const auto page_begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto page_end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	if (m_memory_tracker.IsRegionGpuModified(page_begin, page_end - page_begin)) {
		if (snapshot) {
			// The GPU wrote the page again after the download was recorded: the backing store
			// holds the bytes as of the guest's read, which the caller serves; the page stays
			// GPU-owned and protected.
			m_snapshot_reads++;
			return false;
		}
		// The GPU wrote the window again while the download was landing, or another readback
		// still covers the page: resolve this fault the synchronous way.
		ReadMemoryOnGpu(vaddr, size, is_write);
		return true;
	}
	if (is_write) {
		m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
	}
	return true;
}

// GPU thread, KYTY_DIRECT_READBACK=2: the guest waited for the writer's tick that
// BeginWriteReadback found running. False when `snapshot` and the GPU wrote the buffer again
// since: the page's GPU-written bytes are copied from the mapping as they are now, the page stays
// GPU-owned, and the caller reads the copy (as KYTY_ASYNC_READ_SNAPSHOT does with a download).
bool BufferCache::FinishDirectReadback(uint64_t vaddr, uint64_t size, bool is_write,
                                       uint64_t tick, bool snapshot) {
	if (!IsRegionRegistered(vaddr, size)) {
		return true;
	}
	auto&          buffer       = m_slot_buffers[FindBuffer(vaddr, size)];
	const uint64_t WindowSize   = ReadbackWindowSize();
	const auto     buffer_begin = buffer.CpuAddress();
	const auto     window_begin = std::max(Common::AlignDown(vaddr, WindowSize), buffer_begin);
	const auto     window_end   = std::min(std::max(window_begin + WindowSize, vaddr + size),
	                                       buffer_begin + buffer.Size());
	if (TryDirectReadback(buffer, vaddr, size, is_write, window_begin, window_end)) {
		m_direct_stats.after_writer++;
		ReportDirectReadbacks();
		return true;
	}
	const auto page_begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto page_end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	if (snapshot && !buffer.Mapped().empty() && m_scheduler.IsFree(tick) &&
	    buffer.IsInBounds(page_begin, page_end - page_begin) &&
	    !OverlapsPendingWriteReadback(page_begin, page_end)) {
		std::unique_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		if (!m_downloading_ranges.Intersects(page_begin, page_end - page_begin)) {
			const auto* mapped = buffer.Mapped().data();
			m_gpu_modified_ranges.ForEachInRange(
			    page_begin, page_end - page_begin, [&](uint64_t start, uint64_t end) {
				    Libs::LibKernel::Memory::WriteBacking(start, mapped + buffer.Offset(start),
				                                          end - start);
			    });
			lock.unlock();
			m_snapshot_reads++;
			m_direct_stats.snapshots++;
			ReportDirectReadbacks();
			return false;
		}
	}
	m_direct_stats.fallbacks++;
	ReportDirectReadbacks();
	ReadMemoryOnGpu(vaddr, size, is_write);
	return true;
}

// KYTY_DIRECT_READBACK, every 5 s: how the guest faults on GPU-written buffers were served.
void BufferCache::ReportDirectReadbacks() {
	const auto now = std::chrono::steady_clock::now();
	if (now - m_direct_stats.report < std::chrono::seconds(5)) {
		return;
	}
	::printf("Direct readbacks (5 s): %" PRIu64 " from the mapping (guest threads: %" PRIu64
	         " at once, %" PRIu64 " after the writer), %" PRIu64 " KiB in %.1f ms, %" PRIu64
	         " over the budget, %" PRIu64 " snapshots, %" PRIu64 " synchronous, %" PRIu64
	         " faults on unmapped buffers\n",
	         m_direct_readbacks, m_direct_stats.immediate, m_direct_stats.after_writer,
	         m_direct_stats.bytes / 1024u, static_cast<double>(m_direct_stats.ns) / 1e6,
	         m_direct_stats.over_budget, m_direct_stats.snapshots, m_direct_stats.fallbacks,
	         m_direct_stats.unmapped);
	std::fflush(stdout);
	m_direct_readbacks = 0;
	m_direct_stats     = {.report = now};
}

// GPU thread: waits for a pending readback's publication and, unless the GPU wrote its window
// again, lifts the window's GPU ownership as the synchronous path does after its download.
bool BufferCache::CompletePendingWriteReadback(size_t index) {
	const auto pending = m_pending_write_readbacks[index];
	m_pending_write_readbacks.erase(m_pending_write_readbacks.begin() +
	                                static_cast<std::ptrdiff_t>(index));
	bool clean = true;
	if ((pending.tick & QueueReadbackTickBit) != 0) {
		clean = LandQueueReadback(pending.tick);
	} else {
		m_scheduler.Wait(pending.tick);
		m_scheduler.WaitPriorityOperations(pending.tick);
	}
	UnmarkPublishedRange(pending.begin, pending.end);
	return clean;
}

// After a readback of [begin, end) was published: the range stops being GPU-owned unless the GPU
// wrote it again. A range wider than a readback window (KYTY_READBACK_SPARSE_KB) is released in
// window steps, so bytes the GPU wrote again in one part keep only that part protected.
void BufferCache::UnmarkPublishedRange(uint64_t begin, uint64_t end) {
	if (!HasGpuDirtyBytes(begin, end - begin)) {
		m_memory_tracker.UnmarkRegionAsGpuModified(begin, end - begin);
		return;
	}
	const uint64_t step = ReadbackWindowSize();
	if (end - begin <= step) {
		return;
	}
	for (uint64_t at = begin; at < end;) {
		const auto next = std::min(Common::AlignDown(at, step) + step, end);
		if (!HasGpuDirtyBytes(at, next - at)) {
			m_memory_tracker.UnmarkRegionAsGpuModified(at, next - at);
		}
		at = next;
	}
}

// KYTY_READBACK_SPARSE_KB=N (live, default 0 = off): a readback of a buffer holding at most N KiB
// of GPU-written bytes in all downloads all of them, instead of the window around the fault.
// Wolverine combat 2026-10-22 (KYTY_READBACK_STATS): 3743 guest reads per 5 s of the 2.8 GB heap
// 0x1210000000 on 332 pages, 2.7 KiB downloaded each; every one waited for the GPU (~1 ms), from
// up to seven guest threads at once. One readback of the heap's few GPU-written bytes serves
// them all until the GPU writes it again.
bool BufferCache::WidenSparseReadback(const Buffer& buffer, uint64_t& window_begin,
                                      uint64_t& window_end) {
	static auto& sparse_kb = Common::LiveSwitches::Get("KYTY_READBACK_SPARSE_KB", 0);
	const auto   limit =
	    static_cast<uint64_t>(std::max<int64_t>(sparse_kb.load(std::memory_order_relaxed), 0)) << 10u;
	const auto begin = buffer.CpuAddress();
	const auto end   = begin + buffer.Size();
	if (limit == 0 || (window_begin <= begin && end <= window_end)) {
		return false;
	}
	uint64_t dirty = 0;
	{
		std::shared_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		m_gpu_modified_ranges.ForEachInRange(
		    begin, end - begin, [&](uint64_t start, uint64_t stop) { dirty += stop - start; });
	}
	if (dirty == 0 || dirty > limit || OverlapsPendingWriteReadback(begin, end)) {
		m_sparse_stats.declined++;
		ReportSparseReadbacks();
		return false;
	}
	window_begin = begin;
	window_end   = end;
	m_sparse_stats.widened++;
	m_sparse_stats.bytes += dirty;
	ReportSparseReadbacks();
	return true;
}

void BufferCache::NoteGpuWriteTick(uint64_t vaddr, uint64_t size) {
	if (size == 0) {
		return;
	}
	const uint64_t begin = vaddr;
	const uint64_t end   = vaddr + size;
	const uint64_t tick  = m_scheduler.CurrentTick();
	auto           it    = m_range_writes.upper_bound(begin);
	if (it != m_range_writes.begin()) {
		if (const auto prev = std::prev(it); prev->second.end > begin) {
			it = prev;
		}
	}
	// The newer write replaces the overlapped parts; the parts outside [begin, end) keep their
	// older ticks.
	bool       has_left = false, has_right = false;
	uint64_t   left_begin = 0, right_end = 0;
	RangeWrite left {}, right {};
	while (it != m_range_writes.end() && it->first < end) {
		if (it->first < begin) {
			has_left   = true;
			left_begin = it->first;
			left       = {begin, it->second.tick};
		}
		if (it->second.end > end) {
			has_right = true;
			right_end = it->second.end;
			right     = {right_end, it->second.tick};
		}
		it = m_range_writes.erase(it);
	}
	if (has_left) {
		m_range_writes.emplace(left_begin, left);
	}
	if (has_right) {
		m_range_writes.emplace(end, right);
	}
	m_range_writes.emplace(begin, RangeWrite {end, tick});
	if (m_range_writes.size() > m_range_writes_prune_at) {
		// A finished write needs no wait: forget it.
		std::erase_if(m_range_writes,
		              [this](const auto& entry) { return m_scheduler.IsFree(entry.second.tick); });
		m_range_writes_prune_at = std::max<size_t>(16384, m_range_writes.size() * 2);
	}
}

uint64_t BufferCache::WriterTick(const Buffer& buffer, uint64_t begin, uint64_t end) const {
	if (RangeTickMode() == 0 || end <= begin) {
		return buffer.last_gpu_write_tick;
	}
	uint64_t tick = 0;
	auto     it   = m_range_writes.upper_bound(begin);
	if (it != m_range_writes.begin()) {
		if (const auto prev = std::prev(it); prev->second.end > begin) {
			it = prev;
		}
	}
	for (; it != m_range_writes.end() && it->first < end; ++it) {
		tick = std::max(tick, it->second.tick);
	}
	return std::min(tick, buffer.last_gpu_write_tick);
}

// KYTY_READBACK_RANGE_TICK, every 5 s (GPU thread).
void BufferCache::ReportRangeTickReadbacks() {
	const auto now = std::chrono::steady_clock::now();
	if (now - m_range_tick_stats.report < std::chrono::seconds(5)) {
		return;
	}
	auto& s = m_range_tick_stats;
	::printf("Range-tick readbacks (5 s): %" PRIu64 " on the readback queue with the writer done (%" PRIu64
	         " while the buffer's last writer still ran), %" PRIu64 " with the writer running, %" PRIu64
	         " left to the main queue (%" PRIu64 " with the range's writer running), %zu ranges\n",
	         s.copy_done, s.spared, s.copy_inflight, s.main_queue, s.range_running,
	         m_range_writes.size());
	std::fflush(stdout);
	s = {.report = now};
}

// GPU thread, KYTY_READBACK_QUEUE_ASYNC: records the window's GPU-written bytes into a free slot's
// staging buffer on the readback queue, behind the writer's tick only, and submits it without a
// fence. The bytes count as downloading until LandQueueReadback publishes them, as with a
// main-queue download.
uint64_t BufferCache::BeginQueueReadback(Buffer& buffer, uint64_t window_begin, uint64_t window_end,
                                         uint64_t writer_tick) {
	constexpr uint64_t StagingSize = 4ull * 1024 * 1024;
	auto&              stats       = m_queue_readback_stats;
	if (m_graphics.readback_queue == nullptr || !CopyQueueReadbackEnabled()) {
		return 0;
	}
	if (OverlapsPendingWriteReadback(window_begin, window_end)) {
		stats.busy++;
		return 0;
	}
	auto&              queue = m_queue_readbacks;
	QueueReadbackSlot* slot  = nullptr;
	for (auto& candidate: queue.slots) {
		if (candidate.value == 0) {
			slot = &candidate;
			break;
		}
	}
	if (slot == nullptr) {
		stats.no_slot++;
		return 0;
	}

	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	{
		std::shared_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		// A queued main-queue download of these bytes would publish them again when it lands.
		if (m_downloading_ranges.Intersects(window_begin, window_end - window_begin)) {
			stats.busy++;
			return 0;
		}
		m_memory_tracker.ForEachDownloadRange<false>(
		    window_begin, window_end - window_begin, [&](uint64_t address, uint64_t bytes) noexcept {
			    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
				    copies.emplace_back(start - buffer_address, total_size, end - start);
				    total_size += Common::AlignUp(end - start, 64);
			    });
		    });
	}
	if (copies.empty()) {
		return 0;
	}
	if (total_size > StagingSize) {
		stats.too_large++;
		return 0;
	}
	{
		// Only the GPU thread adds GPU-written or downloading ranges, so the copies still hold.
		std::unique_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		for (const auto& copy: copies) {
			m_downloading_ranges.Add(buffer_address + copy.srcOffset, copy.size);
			m_gpu_modified_ranges.Subtract(buffer_address + copy.srcOffset, copy.size);
		}
	}
	const bool writer_in_flight = !m_scheduler.IsFree(writer_tick);
	if (writer_in_flight) {
		if (writer_tick >= m_scheduler.CurrentTick()) {
			// The writer is in the open command buffer: submit it so the copy can wait for its tick.
			m_scheduler.Flush();
		}
		// As in TryCopyQueueReadback: the copy must not wait for a tick still queued for recording.
		(void)m_scheduler.DrainRecording();
	}

	KYTY_PROFILER_BLOCK("BufferCache::BeginQueueReadback");
	// A guest thread whose readback this slot held before may still try to claim it: wait until
	// it lets go, then keep it from claiming the half-written slot.
	for (int expected = QueueCopyPending;
	     !slot->state.compare_exchange_weak(expected, QueuePosting, std::memory_order_acq_rel);
	     expected = QueueCopyPending) {
		std::this_thread::yield();
	}
	auto device = m_graphics.device;
	if (queue.semaphore == nullptr) {
		vk::CommandPoolCreateInfo pool_info {};
		pool_info.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
		pool_info.queueFamilyIndex = m_graphics.queue_family;
		RequireVulkanSuccess(device.createCommandPool(&pool_info, nullptr, &queue.pool),
		                     "create queue readback command pool");
		vk::SemaphoreTypeCreateInfo type_info {};
		type_info.semaphoreType = vk::SemaphoreType::eTimeline;
		type_info.initialValue  = 0;
		vk::SemaphoreCreateInfo semaphore_info {};
		semaphore_info.pNext = &type_info;
		RequireVulkanSuccess(device.createSemaphore(&semaphore_info, nullptr, &queue.semaphore),
		                     "create queue readback semaphore");
	}
	if (slot->command == nullptr) {
		vk::CommandBufferAllocateInfo allocate {};
		allocate.commandPool        = queue.pool;
		allocate.level              = vk::CommandBufferLevel::ePrimary;
		allocate.commandBufferCount = 1;
		RequireVulkanSuccess(device.allocateCommandBuffers(&allocate, &slot->command),
		                     "allocate queue readback command buffer");
		slot->staging = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		                                         vk::BufferUsageFlagBits::eTransferDst, StagingSize);
	}

	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	RequireVulkanSuccess(slot->command.begin(&begin), "begin queue readback command buffer");
	slot->command.copyBuffer(buffer.Handle(), slot->staging->Handle(),
	                         static_cast<uint32_t>(copies.size()), copies.data());
	vk::BufferMemoryBarrier to_host {};
	to_host.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	to_host.dstAccessMask       = vk::AccessFlagBits::eHostRead;
	to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_host.buffer              = slot->staging->Handle();
	to_host.offset              = 0;
	to_host.size                = total_size;
	slot->command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                              vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1, &to_host, 0,
	                              nullptr);
	RequireVulkanSuccess(slot->command.end(), "end queue readback command buffer");

	const uint64_t                  value      = queue.next_value++;
	const vk::Semaphore             master     = m_scheduler.GetMasterSemaphore().Handle();
	const vk::PipelineStageFlags    wait_stage = vk::PipelineStageFlagBits::eTransfer;
	vk::TimelineSemaphoreSubmitInfo timeline {};
	timeline.waitSemaphoreValueCount   = 1;
	timeline.pWaitSemaphoreValues      = &writer_tick;
	timeline.signalSemaphoreValueCount = 1;
	timeline.pSignalSemaphoreValues    = &value;
	vk::SubmitInfo submit {};
	submit.pNext                = &timeline;
	submit.waitSemaphoreCount   = 1;
	submit.pWaitSemaphores      = &master;
	submit.pWaitDstStageMask    = &wait_stage;
	submit.commandBufferCount   = 1;
	submit.pCommandBuffers      = &slot->command;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores    = &queue.semaphore;
	Common::Telemetry::ScopedGpuTime submit_time(Common::Telemetry::Counter::GpuSubmit);
	RequireVulkanSuccess(m_graphics.readback_queue.submit(1, &submit, nullptr),
	                     "submit queue readback");

	slot->copies         = std::move(copies);
	slot->source         = buffer.Handle();
	slot->source_address = buffer_address;
	slot->total_size     = total_size;
	slot->value          = value;
	const auto     index = static_cast<uint64_t>(slot - queue.slots.data());
	const uint64_t tick  = QueueReadbackTickBit | (index << QueueReadbackSlotShift) | value;
	slot->posted.store(value, std::memory_order_release);
	slot->state.store(QueueCopyPending, std::memory_order_release);
	m_pending_write_readbacks.push_back({window_begin, window_end, tick});
	stats.posted++;
	if (writer_in_flight) {
		stats.inflight++;
	}
	return tick;
}

// Any thread: until the readback queue's copy of `tick` has executed.
void BufferCache::WaitQueueReadback(uint64_t tick) const {
	const uint64_t        value = tick & QueueReadbackValueMask;
	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_queue_readbacks.semaphore;
	wait_info.pValues        = &value;
	Common::Telemetry::ScopedGpuTime fence_wait(Common::Telemetry::Counter::GpuFenceWait);
	RequireVulkanSuccess(m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX),
	                     "wait for queue readback");
}

BufferCache::QueueReadbackSlot* BufferCache::FindQueueReadbackSlot(uint64_t tick) {
	const auto index = (tick & ~QueueReadbackTickBit) >> QueueReadbackSlotShift;
	if (index >= m_queue_readbacks.slots.size()) {
		return nullptr;
	}
	auto& slot = m_queue_readbacks.slots[index];
	return slot.value == (tick & QueueReadbackValueMask) ? &slot : nullptr;
}

// Guest thread, KYTY_READBACK_QUEUE_GUEST_COPY: the copy of `tick` has executed (WaitQueueReadback).
// Claims the slot and writes its bytes back, unless the GPU thread or another guest thread waiting
// for the same readback already does; the GPU thread then only does the bookkeeping. A slot the
// GPU thread has landed and posted again since is left alone.
void BufferCache::CopyQueueReadbackOnGuest(uint64_t tick) {
	KYTY_PROFILER_FUNCTION();
	const auto index = (tick & ~QueueReadbackTickBit) >> QueueReadbackSlotShift;
	const auto value = tick & QueueReadbackValueMask;
	if (index >= m_queue_readbacks.slots.size()) {
		return;
	}
	auto& slot = m_queue_readbacks.slots[index];
	if (slot.posted.load(std::memory_order_acquire) != value) {
		return;
	}
	int expected = QueueCopyPending;
	if (!slot.state.compare_exchange_strong(expected, QueueCopying, std::memory_order_acq_rel)) {
		return;
	}
	if (slot.posted.load(std::memory_order_acquire) != value) {
		// Landed and posted again between the two checks.
		slot.state.store(QueueCopyPending, std::memory_order_release);
		return;
	}
	slot.staging->Invalidate(0, slot.total_size);
	const auto* mapped = slot.staging->Mapped().data();
	for (const auto& copy: slot.copies) {
		Libs::LibKernel::Memory::WriteBacking(slot.source_address + copy.srcOffset,
		                                      mapped + copy.dstOffset, copy.size);
	}
	slot.state.store(QueueCopied, std::memory_order_release);
}

// GPU thread, from CompletePendingWriteReadback (the pending entry is gone).
bool BufferCache::LandQueueReadback(uint64_t tick) {
	KYTY_PROFILER_FUNCTION();
	auto* slot = FindQueueReadbackSlot(tick);
	EXIT_IF(slot == nullptr);
	// Take the write-back over unless a guest thread has done it or is doing it.
	bool guest_copied = false;
	for (;;) {
		int state = slot->state.load(std::memory_order_acquire);
		if (state == QueueCopied) {
			guest_copied = true;
			break;
		}
		if (state == QueueCopyPending &&
		    slot->state.compare_exchange_strong(state, QueueCopying, std::memory_order_acq_rel)) {
			break;
		}
		std::this_thread::yield();
	}
	if (guest_copied) {
		m_queue_readback_stats.guest_copies++;
	} else {
		WaitQueueReadback(tick);
		slot->staging->Invalidate(0, slot->total_size);
		const auto* mapped = slot->staging->Mapped().data();
		for (const auto& copy: slot->copies) {
			Libs::LibKernel::Memory::WriteBacking(slot->source_address + copy.srcOffset,
			                                      mapped + copy.dstOffset, copy.size);
		}
	}
	{
		std::unique_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		for (const auto& copy: slot->copies) {
			m_downloading_ranges.Subtract(slot->source_address + copy.srcOffset, copy.size);
		}
	}
	// The main queue may have written the bytes while the readback queue copied them.
	bool clean = true;
	for (const auto& copy: slot->copies) {
		if (HasGpuDirtyBytes(slot->source_address + copy.srcOffset, copy.size)) {
			clean = false;
			break;
		}
	}
	slot->copies.clear();
	slot->source = nullptr;
	slot->value  = 0;
	slot->posted.store(0, std::memory_order_release);
	slot->state.store(QueueCopyPending, std::memory_order_release);
	m_queue_readback_stats.landed++;
	if (!clean) {
		m_queue_readback_stats.redirtied++;
	}
	return clean;
}

void BufferCache::CompleteQueueReadbacksOf(vk::Buffer source) {
	for (size_t i = 0; i < m_pending_write_readbacks.size();) {
		const auto  tick = m_pending_write_readbacks[i].tick;
		const auto* slot = (tick & QueueReadbackTickBit) != 0 ? FindQueueReadbackSlot(tick) : nullptr;
		if (slot != nullptr && slot->source == source) {
			(void)CompletePendingWriteReadback(i);
		} else {
			i++;
		}
	}
}

// KYTY_READBACK_QUEUE_ASYNC, every 5 s (GPU thread).
void BufferCache::ReportQueueReadbacks() {
	const auto now = std::chrono::steady_clock::now();
	auto&      s   = m_queue_readback_stats;
	if (now - s.report < std::chrono::seconds(5)) {
		return;
	}
	::printf("Queue readbacks (5 s): %" PRIu64 " posted (%" PRIu64 " with the writer running, %" PRIu64
	         " main-queue flushes), %" PRIu64 " landed (%" PRIu64 " written back by the guest, %" PRIu64
	         " written again meanwhile), main queue instead: %" PRIu64 " no free slot, %" PRIu64
	         " over 4 MiB, %" PRIu64 " overlapping a download\n",
	         s.posted, s.inflight, s.flushes, s.landed, s.guest_copies, s.redirtied, s.no_slot,
	         s.too_large, s.busy);
	std::fflush(stdout);
	s = {.report = now};
}

// KYTY_READBACK_SPARSE_KB, every 5 s (GPU thread).
void BufferCache::ReportSparseReadbacks() {
	const auto now = std::chrono::steady_clock::now();
	if (now - m_sparse_stats.report < std::chrono::seconds(5)) {
		return;
	}
	::printf("Sparse readbacks (5 s): %" PRIu64 " widened to the whole buffer (%" PRIu64
	         " KiB), %" PRIu64 " declined\n",
	         m_sparse_stats.widened, m_sparse_stats.bytes / 1024u, m_sparse_stats.declined);
	std::fflush(stdout);
	m_sparse_stats = {.report = now};
}

void BufferCache::CompletePendingWriteReadbacks(uint64_t begin, uint64_t end) {
	for (size_t i = 0; i < m_pending_write_readbacks.size();) {
		const auto& pending = m_pending_write_readbacks[i];
		if (pending.begin < end && begin < pending.end) {
			CompletePendingWriteReadback(i);
		} else {
			i++;
		}
	}
}

bool BufferCache::OverlapsPendingWriteReadback(uint64_t begin, uint64_t end) const {
	return std::ranges::any_of(m_pending_write_readbacks, [&](const PendingWriteReadback& pending) {
		return pending.begin < end && begin < pending.end;
	});
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0) {
		return NULL_BUFFER_ID;
	}
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid buffer discovery request\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			return *owner;
		}
	}
	return CreateBuffer(vaddr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(uint64_t vaddr, uint64_t size) {
	KYTY_PROFILER_FUNCTION();
	static constexpr int      StreamLeapThreshold = 16;
	static constexpr uint64_t StreamLeapSize      = CACHING_PAGESIZE * 128;

	auto       begin      = vaddr;
	auto       end        = vaddr + size;
	const auto find_first = [&](uint64_t address) {
		auto first = m_buffers.lower_bound(address);
		if (first != m_buffers.begin()) {
			const auto  previous = std::prev(first);
			const auto& buffer   = m_slot_buffers[previous->second];
			if (buffer.CpuAddress() + buffer.Size() > address) {
				first = previous;
			}
		}
		return first;
	};
	auto first           = find_first(begin);
	auto last            = first;
	int  stream_score    = 0;
	bool has_stream_leap = false;
	for (; last != m_buffers.end() && last->first < end; ++last) {
		const auto& buffer        = m_slot_buffers[last->second];
		const auto  buffer_begin  = buffer.CpuAddress();
		const auto  buffer_end    = buffer_begin + buffer.Size();
		const bool  expands_left  = buffer_begin < begin;
		const bool  expands_right = buffer_end > end;
		begin                     = std::min(begin, buffer_begin);
		end                       = std::max(end, buffer_end);
		if (!has_stream_leap && (stream_score += buffer.StreamScore()) > StreamLeapThreshold) {
			has_stream_leap = true;
			// Reserve space in the incoming stream's direction of growth.
			// The old buffer extending left of the request predicts growth to the right, and vice versa.
			if (expands_left) {
				end += std::min(StreamLeapSize, (vaddr < LOWER_ADDRESS_SIZE ? LOWER_ADDRESS_SIZE
				                                       : LibKernel::Memory::kExtendedMemoryBase +
				                                             LibKernel::Memory::kExtendedMemorySize) - end);
			}
			if (expands_right) {
				const auto minimum = vaddr < LOWER_ADDRESS_SIZE
				                         ? CACHING_PAGESIZE * 2
				                         : LibKernel::Memory::kExtendedMemoryBase;
				if (begin > minimum) {
					begin -= std::min(StreamLeapSize, begin - minimum);
				}
				first = find_first(begin);
				begin = std::min(begin, first->first);
			}
		}
	}
	return {first, last, begin, end, has_stream_leap};
}

void BufferCache::JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score) {
	KYTY_PROFILER_FUNCTION();
	auto& new_buffer = m_slot_buffers[new_id];
	auto& overlap    = m_slot_buffers[overlap_id];
	if (accumulate_stream_score) {
		new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
	}
	new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
	                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	NoteGpuWriteTick(overlap.CpuAddress(), overlap.Size());
	new_buffer.last_gpu_write_tick = m_scheduler.CurrentTick();
	DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
	KYTY_PROFILER_FUNCTION();
	Common::Telemetry::ScopedTime telemetry(Common::Telemetry::Counter::CreateBuffer);
	EXIT_IF(m_scheduler.Current().IsInvalid());
	const auto end = Common::AlignUp(vaddr + size, CACHING_PAGESIZE);
	if (vaddr < CACHING_PAGESIZE) {
		// Guest page 0 is never mapped; a request here is a garbage or null descriptor that
		// slipped past the null checks. The memory tracker cannot hold address 0, so name the
		// caller now rather than in the garbage collector later.
		static std::atomic<uint32_t> reported = 0;
		if (reported.fetch_add(1) < 8) {
			LOGF("BufferCache: buffer requested in guest page 0: vaddr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n%s",
			     vaddr, size, Common::HostBacktrace().c_str());
		}
	}
	vaddr = Common::AlignDown(vaddr, CACHING_PAGESIZE);
	size               = end - vaddr;
	const auto overlap = ResolveOverlaps(vaddr, size);

	m_buffers_created++;
	const auto id = m_slot_buffers.insert(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, overlap.begin,
	    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, overlap.end - overlap.begin);
	auto& buffer          = m_slot_buffers[id];
	buffer.origin_shader  = s_diag_shader_hash;
	buffer.origin_request = size;
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", overlap.begin,
	                     overlap.end - overlap.begin);
	for (auto it = overlap.first; it != overlap.last;) {
		const auto old_id = (it++)->second;
		JoinOverlap(id, old_id, !overlap.has_stream_leap);
	}
	Register(id);
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer) {
	// KYTY_SYNC_STATS=1 (live): every 5 s, synchronizations and the bytes they walked, split by
	// read and written ranges (a written range is locked and marked GPU-modified page by page).
	static auto& stats = Common::LiveSwitches::Get("KYTY_SYNC_STATS", 0);
	if (stats.load(std::memory_order_relaxed) != 0) {
		static uint64_t calls[2]   = {};
		static uint64_t bytes[2]   = {};
		static uint64_t largest[2] = {};
		static auto     report     = std::chrono::steady_clock::now();

		const size_t kind = is_written ? 1 : 0;
		calls[kind]++;
		bytes[kind] += size;
		largest[kind] = std::max(largest[kind], size);

		const auto now = std::chrono::steady_clock::now();
		if (now - report >= std::chrono::seconds(5)) {
			report               = now;
			constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
			::printf("Buffer sync (5 s): read %" PRIu64 " calls %.1f GiB (largest %.1f MiB), "
			         "written %" PRIu64 " calls %.1f GiB (largest %.1f MiB)\n",
			         calls[0], static_cast<double>(bytes[0]) / GiB,
			         static_cast<double>(largest[0]) / 1048576.0, calls[1],
			         static_cast<double>(bytes[1]) / GiB,
			         static_cast<double>(largest[1]) / 1048576.0);
			std::fflush(stdout);
			calls[0] = calls[1] = bytes[0] = bytes[1] = largest[0] = largest[1] = 0;
		}
	}
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size = 0;
	vk::Buffer                  source;
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    copies.emplace_back(total_size, buffer.Offset(address), bytes);
		    total_size += bytes;
	    },
	    [&]() noexcept { source = UploadCopies(buffer, copies, total_size); });
	if (source) {
		auto& command = m_scheduler.Current();
		command.EndRendering();
		const auto              native = command.Recorder();
		vk::BufferMemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                       vk::AccessFlagBits::eTransferRead |
		                       vk::AccessFlagBits::eTransferWrite;
		before.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer              = buffer.Handle();
		before.offset              = 0;
		before.size                = buffer.Size();
		native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &before, 0, nullptr);
		native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
		                  copies.data());
		auto after          = before;
		after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                       vk::PipelineStageFlagBits::eAllCommands,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
	}
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

// Reads guest memory for an upload. The guest can unmap memory a cached buffer still covers: AMM
// maps streaming textures in blocks, and a texture upload once read one byte past the end of the
// block it had mapped. Such bytes read as zero, like ObtainBufferForImage's direct path.
static void ReadGuestForUpload(uint8_t* destination, uint64_t address, uint64_t size) {
	if (!Libs::LibKernel::Memory::TryReadBacking(address, destination, size) &&
	    !Libs::LibKernel::Memory::TryReadSparseBacking(address, destination, size)) {
		static std::atomic<uint32_t> reported {0};
		if (reported.fetch_add(1) < 16) {
			LOGF("BufferCache: upload of unmapped guest memory 0x%016" PRIx64 " size=0x%" PRIx64
			     " reads as zero\n",
			     address, size);
		}
		std::memset(destination, 0, static_cast<size_t>(size));
	}
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     uint64_t total_size) {
	if (copies.empty()) {
		return nullptr;
	}

	auto [mapped, base_offset] = m_staging_buffer.Map(total_size, 4);
	if (mapped != nullptr) {
		for (auto& copy: copies) {
			const auto address = buffer.CpuAddress() + copy.dstOffset;
			ReadGuestForUpload(mapped + copy.srcOffset, address, copy.size);
			copy.srcOffset += base_offset;
		}
		m_staging_buffer.Commit();
		return m_staging_buffer.Handle();
	}

	auto temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Upload, 0,
	                                         vk::BufferUsageFlagBits::eTransferSrc, total_size);
	for (const auto& copy: copies) {
		const auto address = buffer.CpuAddress() + copy.dstOffset;
		ReadGuestForUpload(temporary->Mapped().data() + copy.srcOffset, address, copy.size);
	}
	temporary->Flush(0, total_size);
	const auto handle = temporary->Handle();
	m_scheduler.DeferOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return handle;
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id, bool needs_device_address) {
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || !GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: buffer request requires a recording command buffer\n");
	}

	if (!is_written && size <= CACHING_PAGESIZE &&
	    (!needs_device_address || m_stream_buffer.HasDeviceAddress()) &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		if (mapped != nullptr) {
			std::memcpy(mapped, reinterpret_cast<const void*>(vaddr), size);
			m_stream_buffer.Commit();
			return {&m_stream_buffer, offset};
		}
	}

	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	(void)SynchronizeBuffer(buffer, vaddr, size, is_written, is_texel_buffer);
	if (is_written) {
		MarkGpuWritten(vaddr, size);
		buffer.last_gpu_write_tick = m_scheduler.CurrentTick();
	}
	return {&buffer, buffer.Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferWritten(uint64_t vaddr, uint64_t size,
                                                              uint64_t written_vaddr,
                                                              uint64_t written_size, BufferId id) {
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || !GuestRange {vaddr, size}.Valid() ||
	    written_vaddr < vaddr || written_size > size || written_vaddr - vaddr > size - written_size) {
		EXIT("BufferCache: invalid written-range buffer request\n");
	}
	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	// The whole binding is current on the GPU, as for a written buffer; only the bytes the
	// stores can reach become GPU-written.
	(void)SynchronizeBuffer(buffer, vaddr, size, false, false);
	if (written_size != 0) {
		(void)SynchronizeBuffer(buffer, written_vaddr, written_size, true, false);
		MarkGpuWritten(written_vaddr, written_size);
		buffer.last_gpu_write_tick = m_scheduler.CurrentTick();
	}
	return {&buffer, buffer.Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferTracked(uint64_t vaddr, uint64_t size,
                                                              bool is_texel_buffer, BufferId id) {
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || !GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: buffer request requires a recording command buffer\n");
	}
	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	(void)SynchronizeBuffer(buffer, vaddr, size, true, is_texel_buffer);
	MarkGpuWritten(vaddr, size, true);
	buffer.last_gpu_write_tick = m_scheduler.CurrentTick();
	return {&buffer, buffer.Offset(vaddr)};
}

uint64_t BufferCache::WriteBitmapMinBytes() {
	static auto& min_kb = Common::LiveSwitches::Get("KYTY_WRITE_BITMAP_MIN_KB", 1024);
	return static_cast<uint64_t>(std::max<int64_t>(min_kb.load(std::memory_order_relaxed), 4)) << 10u;
}

// GPU thread. The bitmap lives in host-coherent device memory the host maps (BAR): the host reads
// it uncached, each word at most once per refinement (mionstyle7 270bba48: page by page it took
// 41% of the GPU thread). Created at the first tracked binding and kept: shaders in flight and
// tracked pages refer to it after the switch goes off.
vk::DeviceAddress BufferCache::WriteBitmapAddress() {
	static auto& enabled = Common::LiveSwitches::Get("KYTY_WRITE_BITMAP", 0);
	if (enabled.load(std::memory_order_relaxed) == 0 || m_write_bitmap_failed) {
		return 0;
	}
	constexpr uint64_t Bytes = WriteBitmapLimit / TRACKER_PAGE_SIZE / 8u;
	if (m_write_bitmap == nullptr) {
		m_write_bitmap = std::make_unique<Buffer>(
		    m_graphics, m_scheduler, MemoryUsage::Stream, 0,
		    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
		    Bytes);
		if (!m_write_bitmap->IsCoherent() || m_write_bitmap->Mapped().size() < Bytes ||
		    !m_write_bitmap->HasDeviceAddress()) {
			::printf("BufferCache: GPU write bitmap unavailable (no host-coherent mapped device "
			         "memory)\n");
			std::fflush(stdout);
			m_write_bitmap.reset();
			m_write_bitmap_failed = true;
			return 0;
		}
		std::memset(m_write_bitmap->Mapped().data(), 0, static_cast<size_t>(Bytes));
		// Submissions now end with a device-to-host barrier: the host reads the bits once the
		// tick of the writes completes.
		RequireHostReadBarrier();
		::printf("BufferCache: GPU write bitmap on (%" PRIu64 " MiB, written bindings of at least %" PRIu64
		         " KiB)\n",
		         Bytes >> 20u, WriteBitmapMinBytes() >> 10u);
		std::fflush(stdout);
	}
	return m_write_bitmap->BufferDeviceAddress();
}

// KYTY_WRITE_BITMAP_UNPROTECT (live, default 1): pages a refinement drops lose their protection at
// once. Otherwise each was unprotected by a fault of its own, where a download used to unprotect
// its whole window (ABBA 2026-10-25, combat: 2.3x the guest faults, -10.9% fps).
static bool WriteBitmapUnprotectEnabled() {
	static auto& enabled = Common::LiveSwitches::Get("KYTY_WRITE_BITMAP_UNPROTECT", 1);
	return enabled.load(std::memory_order_relaxed) != 0;
}

bool BufferCache::RefineWithWriteBitmap(const Buffer& buffer, uint64_t vaddr, uint64_t size) {
	if (m_write_bitmap == nullptr) {
		return false;
	}
	KYTY_PROFILER_BLOCK("BufferCache::RefineWithWriteBitmap");
	constexpr uint64_t Page     = TRACKER_PAGE_SIZE;
	constexpr uint64_t WordSpan = Page * 32u;
	// The readback window ReadMemoryOnGpu and BeginWriteReadback download (see there).
	const auto window       = ReadbackWindowSize();
	const auto buffer_begin = buffer.CpuAddress();
	const auto window_begin = std::max(Common::AlignDown(vaddr, window), buffer_begin);
	const auto window_end =
	    std::min(std::max(window_begin + window, vaddr + size), buffer_begin + buffer.Size());
	const auto begin = Common::AlignDown(window_begin, Page);
	const auto end   = std::min(Common::AlignUp(window_end, Page), WriteBitmapLimit);
	if (end <= begin) {
		return false;
	}
	// GPU thread, which alone changes the tracked set, the writes in flight and the GPU-written
	// set: the bitmap is read without the lock (its uncached reads held it ~7 ms a frame, ABBA
	// 2026-10-25), which only covers the changes guest threads may be reading.
	if (m_bitmap_tracked.Empty() || !m_bitmap_tracked.Intersects(begin, end - begin)) {
		return false;
	}
	m_bitmap_stats.refinements++;
	// Tracked writes the GPU may still run, on any page of the words looked at: their pages stay
	// GPU-written, and those words are not written back (the GPU may still OR bits into them).
	const auto words_begin = Common::AlignDown(begin, WordSpan);
	const auto words_end   = Common::AlignUp(end, WordSpan);
	RangeSet   in_flight;
	for (auto it = m_bitmap_inflight.begin(); it != m_bitmap_inflight.end();) {
		const auto first           = it->first;
		const auto [last, tick]    = it->second;
		if (m_scheduler.IsFree(tick)) {
			it = m_bitmap_inflight.erase(it);
			continue;
		}
		if (first < words_end && words_begin < last) {
			in_flight.Add(first, last - first);
		}
		++it;
	}
	std::vector<std::pair<uint64_t, uint64_t>> pieces;
	m_gpu_modified_ranges.ForEachInRange(begin, end - begin, [&](uint64_t from, uint64_t to) {
		pieces.emplace_back(from, to);
	});
	auto*      words  = reinterpret_cast<uint32_t*>(m_write_bitmap->Mapped().data());
	const auto append = [](std::vector<std::pair<uint64_t, uint64_t>>& runs, uint64_t page) {
		if (!runs.empty() && runs.back().second == page) {
			runs.back().second = page + Page;
		} else {
			runs.emplace_back(page, page + Page);
		}
	};
	std::vector<std::pair<uint64_t, uint64_t>> unwritten;
	std::vector<std::pair<uint64_t, uint64_t>> written;
	uint64_t cached_index = UINT64_MAX;
	uint32_t cached_word  = 0;
	bool     cached_dirty = false;
	bool     cached_keep  = false;
	const auto flush_word = [&] {
		if (cached_dirty && !cached_keep) {
			words[cached_index] = cached_word;
		}
		cached_dirty = false;
	};
	for (const auto& [from, to]: pieces) {
		// Only whole pages whose GPU-written bytes all came through tracked bindings.
		m_bitmap_tracked.ForEachInRange(from, to - from, [&](uint64_t tracked_begin,
		                                                     uint64_t tracked_end) {
			for (auto page = Common::AlignUp(tracked_begin, Page); page + Page <= tracked_end;
			     page += Page) {
				const auto index      = page / Page;
				const auto word_index = index >> 5u;
				if (word_index != cached_index) {
					flush_word();
					cached_index = word_index;
					cached_word  = words[word_index];
					cached_keep  = !in_flight.Empty() && in_flight.Intersects(word_index * WordSpan,
					                                                          WordSpan);
				}
				if (cached_keep && in_flight.Intersects(page, Page)) {
					m_bitmap_stats.in_flight++;
					continue;
				}
				const auto bit = uint32_t {1} << (index & 31u);
				if ((cached_word & bit) == 0) {
					append(unwritten, page);
					m_bitmap_stats.dropped++;
				} else {
					// Downloaded with its window, now or later: until then it is not tracked, so a
					// cleared bit cannot drop it.
					append(written, page);
					m_bitmap_stats.written++;
					if (!cached_keep) {
						cached_word &= ~bit;
						cached_dirty = true;
					}
				}
			}
		});
	}
	flush_word();
	const bool unprotect = WriteBitmapUnprotectEnabled();
	{
		std::unique_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		for (const auto& [from, to]: unwritten) {
			m_gpu_modified_ranges.Subtract(from, to - from);
		}
		for (const auto& [from, to]: written) {
			m_bitmap_tracked.Subtract(from, to - from);
		}
		if (unprotect) {
			// A landing download publishes its older bytes over these pages: they stay protected.
			std::erase_if(unwritten, [&](const auto& run) {
				return m_downloading_ranges.Intersects(run.first, run.second - run.first) ||
				       OverlapsPendingWriteReadback(run.first, run.second);
			});
		}
	}
	if (!unprotect) {
		return false;
	}
	// As a fault on one of them would (ReadMemoryOnGpu): no byte of these pages waits for a
	// download. The tracker is changed outside the lock, as the downloads do.
	const auto page_begin = Common::AlignDown(vaddr, Page);
	const auto page_end   = Common::AlignUp(vaddr + size, Page);
	bool       faulting   = false;
	for (const auto& [from, to]: unwritten) {
		m_memory_tracker.UnmarkRegionAsGpuModified(from, to - from);
		m_bitmap_stats.unprotected += (to - from) / Page;
		faulting = faulting || (from <= page_begin && page_end <= to);
	}
	return faulting;
}

// KYTY_WRITE_BITMAP, every 5 s (GPU thread).
void BufferCache::ReportWriteBitmap() {
	auto&      s   = m_bitmap_stats;
	const auto now = std::chrono::steady_clock::now();
	if (m_write_bitmap == nullptr || now - s.report < std::chrono::seconds(5)) {
		return;
	}
	uint64_t tracked = 0;
	{
		std::shared_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		m_bitmap_tracked.ForEach([&](uint64_t begin, uint64_t end) { tracked += end - begin; });
	}
	::printf("Write bitmap (5 s): %" PRIu64 " tracked bindings (%" PRIu64 " MiB), %" PRIu64
	         " refinements: %" PRIu64 " pages dropped unwritten, %" PRIu64 " written, %" PRIu64
	         " left to a running write, %" PRIu64 " unprotected; %" PRIu64
	         " MiB tracked, %zu writes in flight\n",
	         s.bindings, s.bytes >> 20u, s.refinements, s.dropped, s.written, s.in_flight,
	         s.unprotected, tracked >> 20u, m_bitmap_inflight.size());
	std::fflush(stdout);
	s = {.report = now};
}

void BufferCache::MarkGpuWritten(uint64_t vaddr, uint64_t size, bool tracked) {
	KYTY_PROFILER_BLOCK("Obtain::MarkWritten");
	NoteGpuWriteTick(vaddr, size);
	{
		std::unique_lock lock(m_dirty_ranges_mutex, std::defer_lock);
		Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
		if (tracked || !m_bitmap_tracked.Empty()) {
			const auto first = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
			const auto last  = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
			if (!tracked) {
				// Other GPU writes (untracked bindings, copies, fills) never set bits.
				m_bitmap_tracked.Subtract(first, last - first);
			} else {
				if (!m_bitmap_tracked.Contains(first, last - first)) {
					// A page stays untracked while it holds bytes an untracked write (or a written
					// page a refinement found) left GPU-written: the GPU-written pieces outside the
					// tracked set, widened to pages, are left out.
					std::vector<std::pair<uint64_t, uint64_t>> untracked;
					m_gpu_modified_ranges.ForEachInRange(first, last - first, [&](uint64_t from, uint64_t to) {
						uint64_t at = from;
						m_bitmap_tracked.ForEachInRange(from, to - from, [&](uint64_t begin, uint64_t end) {
							if (begin > at) {
								untracked.emplace_back(at, begin);
							}
							at = std::max(at, end);
						});
						if (at < to) {
							untracked.emplace_back(at, to);
						}
					});
					RangeSet add;
					add.Add(first, last - first);
					for (const auto& [from, to]: untracked) {
						const auto page = Common::AlignDown(from, TRACKER_PAGE_SIZE);
						add.Subtract(page, Common::AlignUp(to, TRACKER_PAGE_SIZE) - page);
					}
					add.ForEach([&](uint64_t from, uint64_t to) { m_bitmap_tracked.Add(from, to - from); });
				}
				auto& entry  = m_bitmap_inflight[first];
				entry.first  = std::max(entry.first, last);
				entry.second = m_scheduler.CurrentTick();
				if (m_bitmap_inflight.size() > 8192) {
					std::erase_if(m_bitmap_inflight, [this](const auto& item) {
						return m_scheduler.IsFree(item.second.second);
					});
				}
				m_bitmap_stats.bindings++;
				m_bitmap_stats.bytes += size;
			}
		}
		m_gpu_modified_ranges.Add(vaddr, size);
	}
	ReadbackStats::NoteWrite(vaddr, size, m_scheduler.CurrentTick());
	// Diagnostics: KYTY_WATCH_GPU_WRITE=<address>[+<size>][,<address>[+<size>]...] (hex) names
	// the bindings that mark bytes of those ranges GPU-written: the first write of each
	// (range, shader), with a host stack when no shader is being bound (copies, fills).
	static const std::vector<std::pair<uint64_t, uint64_t>> watches = [] {
		std::vector<std::pair<uint64_t, uint64_t>> result;
		const char*                                value = std::getenv("KYTY_WATCH_GPU_WRITE");
		while (value != nullptr && *value != '\0') {
			char*      end   = nullptr;
			const auto begin = std::strtoull(value, &end, 16);
			uint64_t   bytes = 1;
			if (end != nullptr && *end == '+') {
				bytes = std::strtoull(end + 1, &end, 16);
			}
			result.emplace_back(begin, bytes);
			value = end != nullptr && *end == ',' ? end + 1 : nullptr;
		}
		return result;
	}();
	static std::atomic<uint32_t> watched {0};
	for (size_t index = 0; index < watches.size(); index++) {
		const auto& [watch_begin, watch_size] = watches[index];
		if (watch_size == 0 || vaddr >= watch_begin + watch_size ||
		    watch_begin >= vaddr + size) {
			continue;
		}
		static std::mutex                             seen_mutex;
		static std::set<std::pair<size_t, uint64_t>> seen;
		bool                                          first = false;
		{
			std::lock_guard lock(seen_mutex);
			first = seen.emplace(index, s_diag_shader_hash).second;
		}
		if (first && watched.fetch_add(1) < 128) {
			::printf("GPU write covers watched #%zu 0x%016" PRIx64 "+0x%" PRIx64
			         ": range=0x%016" PRIx64 " size=0x%" PRIx64 " shader=0x%016" PRIx64 "\n%s",
			         index, watch_begin, watch_size, vaddr, size, s_diag_shader_hash,
			         s_diag_shader_hash == 0 ? Common::HostBacktrace().c_str() : "");
		}
	}
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid image source\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			TouchBuffer(buffer);
			(void)SynchronizeBuffer(buffer, vaddr, size, false, false);
			return {&buffer, buffer.Offset(vaddr)};
		}
	}
	if (IsRegionGpuModified(vaddr, size)) {
		return ObtainBuffer(vaddr, size, false, false);
	}

	auto [staging, stage_offset] = m_staging_buffer.Map(size, 16);
	if (staging == nullptr) {
		EXIT("BufferCache: staging reservation failed for guest image\n");
	}
	if (!Libs::LibKernel::Memory::TryReadSparseBacking(vaddr, staging, size)) {
		std::memset(staging, 0, static_cast<size_t>(size));
	}
	m_staging_buffer.Commit();
	return {&m_staging_buffer, stage_offset};
}

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
	if ((vaddr & 3u) != 0 || size == 0 || (size & 3u) != 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: fill range must be dword aligned\n");
	}
	if (is_gds) {
		if (vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - vaddr) {
			EXIT("BufferCache: GDS fill range is out of bounds\n");
		}
		m_gds_buffer.Fill(vaddr, size, value);
		return;
	}
	if (vaddr == 0) {
		EXIT("BufferCache: invalid fill memory address\n");
	}
	(void)m_texture_cache.ClearMeta(vaddr);
	if (!IsRegionGpuModified(vaddr, size)) {
		// Access the guest mapping so write faults invalidate cached buffers and images.
		auto* destination = reinterpret_cast<uint32_t*>(vaddr);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true);
	dst->Fill(dst_offset, size, value);
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	const bool dst_memory = !dst_gds;
	const bool src_memory = !src_gds;
	if ((dst_memory && dst_vaddr == 0) || (src_memory && src_vaddr == 0) || size == 0 ||
	    ((dst_gds || src_gds) && ((dst_vaddr | src_vaddr | size) & 3u) != 0) ||
	    size > UINT64_MAX - dst_vaddr || size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) ||
	    (dst_gds && (dst_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - dst_vaddr)) ||
	    (src_gds && (src_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - src_vaddr))) {
		EXIT("BufferCache: invalid copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " size=0x%016" PRIx64 " src_gds=%d dst_gds=%d\n",
		     src_vaddr, dst_vaddr, size, static_cast<int>(src_gds), static_cast<int>(dst_gds));
	}
	if (src_memory && dst_memory && !IsRegionGpuModified(dst_vaddr, size) &&
	    !IsRegionGpuModified(src_vaddr, size) && !m_texture_cache.FindImageFromRange(src_vaddr, size)) {
		std::memcpy(reinterpret_cast<void*>(dst_vaddr), reinterpret_cast<const void*>(src_vaddr),
		            size);
		return;
	}

	auto& command = m_scheduler.Current();
	if (dst_memory) {
		m_texture_cache.InvalidateMemoryFromGPU(dst_vaddr, size);
	}
	const auto src_id      = src_memory ? FindBuffer(src_vaddr, size) : BufferId {};
	const auto dst_id      = dst_memory ? FindBuffer(dst_vaddr, size) : BufferId {};
	auto [src, src_offset] = src_memory ? ObtainBuffer(src_vaddr, size, false, true, src_id)
	                                    : std::pair {&m_gds_buffer, src_vaddr};
	auto [dst, dst_offset] = dst_memory ? ObtainBuffer(dst_vaddr, size, true, true, dst_id)
	                                    : std::pair {&m_gds_buffer, dst_vaddr};
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid registered-region query\n");
	}
	// Cached buffers are ordered and non-overlapping. The last buffer beginning before the query
	// end is therefore the only possible intersection.
	const auto candidate = m_buffers.lower_bound(vaddr + size);
	if (candidate == m_buffers.begin()) {
		return false;
	}
	const auto& [address, id] = *std::prev(candidate);
	return address + m_slot_buffers[id].Size() > vaddr;
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	// Bytes of a pending asynchronous write readback are GPU-owned until published.
	return m_gpu_modified_ranges.Intersects(vaddr, size) ||
	       OverlapsPendingWriteReadback(vaddr, vaddr + size);
}

bool BufferCache::IsCleanForConcurrentRead(uint64_t vaddr, uint64_t size) const {
	std::shared_lock lock(m_dirty_ranges_mutex, std::defer_lock);
	Common::Telemetry::LockCounted(lock, Common::Telemetry::Counter::BufferLockWait);
	return !m_gpu_modified_ranges.Intersects(vaddr, size) &&
	       !m_downloading_ranges.Intersects(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

uint64_t BufferCache::LruClock() const noexcept {
	return m_graphics.presented_frames.load(std::memory_order_relaxed) + m_gc_tick / 512;
}

// KYTY_BDA_VERIFY=N (live, seconds, 0 = off, research): every N seconds the GPU's BDA page table
// entries of all live buffers are copied to host memory and compared with the addresses
// ChangeRegister wrote. Wolverine run 88 lost the device reading a buffer page through the
// address of a buffer destroyed 316 s earlier, while the host's view of the table named the
// live buffer that replaced it: this tells a GPU table out of step from a stale reference
// elsewhere. Mismatches print the guest page, both addresses and what owned the stale one.
void BufferCache::VerifyBdaPageTable() {
	static auto& interval = Common::LiveSwitches::Get("KYTY_BDA_VERIFY", 0);
	if (m_bda_verify_tick != 0) {
		if (!m_scheduler.IsFree(m_bda_verify_tick)) {
			return;
		}
		auto& download = *m_bda_verify_download;
		if (!download.IsCoherent()) {
			download.Invalidate(0, download.Size());
		}
		const auto* data       = download.Mapped().data();
		uint64_t    pages      = 0;
		uint64_t    mismatches = 0;
		uint64_t    stale      = 0;
		const auto table_entry = [&](uint64_t element) {
			vk::DeviceAddress value = 0;
			std::memcpy(&value, data + element * sizeof(value), sizeof(value));
			return value;
		};
		for (const auto& run: m_bda_verify_runs) {
			for (uint64_t i = 0; i < run.pages; i++) {
				// Through the GPU's own directory, as a shader reads it.
				const auto page  = PageIndex(run.guest_address + (i << CACHING_PAGEBITS));
				const auto chunk = table_entry(page >> BDA_CHUNK_BITS);
				const auto actual =
				    chunk != 0 ? table_entry(chunk + (page & (BDA_CHUNK_PAGES - 1u))) : 0;
				const auto expected = m_bda_verify_expected[run.first_address + i];
				pages++;
				if (actual == expected) {
					continue;
				}
				stale += expected == 0 ? 1u : 0u;
				if (mismatches++ < 16) {
					std::printf("BDA verify: guest page 0x%" PRIx64 " GPU table 0x%" PRIx64
					            " expected 0x%" PRIx64 "%s\n",
					            run.guest_address + (i << CACHING_PAGEBITS), actual, expected,
					            expected == 0 ? " (no buffer: stale entry)" : "");
					if (actual != 0) {
						AddressBindingDescribe(actual);
					}
				}
			}
		}
		std::printf("BDA verify: %" PRIu64 " pages in %zu registered ranges, %" PRIu64
		            " mismatches (%" PRIu64 " stale on pages without a buffer); chunks %zu in use,"
		            " peak %u of %" PRIu64 "\n",
		            pages, m_bda_verify_runs.size(), mismatches, stale,
		            static_cast<size_t>(BDA_CHUNK_COUNT - m_bda_free_chunks.size()),
		            m_bda_chunks_peak, BDA_CHUNK_COUNT);
		std::fflush(stdout);
		m_bda_verify_tick = 0;
		m_bda_verify_last = std::chrono::steady_clock::now();
		return;
	}
	const auto seconds = interval.load(std::memory_order_relaxed);
	if (seconds <= 0 ||
	    std::chrono::steady_clock::now() - m_bda_verify_last < std::chrono::seconds(seconds)) {
		return;
	}
	// Every guest range ever registered, not only live buffers: a page whose buffer is gone must
	// read 0. Expected values are taken now, in recording order with the copy below.
	m_bda_verify_runs.clear();
	m_bda_verify_expected.clear();
	uint64_t bytes = 0;
	m_bda_ever_registered.ForEach([&](uint64_t start, uint64_t end) {
		PageTable::PageRange range {};
		if (!PageTable::TryGetPageRange(start, end - start, range)) {
			return;
		}
		const auto pages = static_cast<uint64_t>(range.last_exclusive - range.first);
		m_bda_verify_runs.push_back({
		    .guest_address   = start,
		    .table_offset    = PageIndex(start) * sizeof(vk::DeviceAddress),
		    .download_offset = bytes,
		    .first_address   = m_bda_verify_expected.size(),
		    .pages           = pages,
		});
		for (size_t page = range.first; page < range.last_exclusive; ++page) {
			const auto* id     = m_page_table.Find(page);
			const auto* buffer = id != nullptr && *id ? m_slot_buffers.try_get(*id) : nullptr;
			vk::DeviceAddress expected = 0;
			if (buffer != nullptr && buffer->HasDeviceAddress()) {
				const auto page_address = static_cast<uint64_t>(page) << CACHING_PAGEBITS;
				const auto first_page   = PageIndex(buffer->CpuAddress()) << CACHING_PAGEBITS;
				const auto packed_page  = PageIndex(page_address) << CACHING_PAGEBITS;
				expected                = buffer->BufferDeviceAddress() + (packed_page - first_page);
			}
			m_bda_verify_expected.push_back(expected);
		}
		bytes += pages * sizeof(vk::DeviceAddress);
	});
	if (bytes == 0) {
		return;
	}
	// The whole two-level table (directory + chunk pool) in one copy.
	if (m_bda_verify_download == nullptr) {
		m_bda_verify_download = std::make_unique<Buffer>(
		    m_graphics, m_scheduler, MemoryUsage::Download, 0,
		    vk::BufferUsageFlagBits::eTransferDst, BDA_PAGETABLE_SIZE);
	}
	m_bda_verify_download->CopyFrom(m_scheduler.Current(), m_bda_pagetable_buffer, 0, 0,
	                                BDA_PAGETABLE_SIZE);
	m_bda_verify_tick = m_scheduler.CurrentTick();
}

// KYTY_BUFFER_AUDIT=1 (live, research, PLAN.md P1.3): every 10 s, what the buffer cache holds —
// count and size histogram, and the largest buffers with their guest range, frames since use
// and whether textures live in the same memory — to find buffer memory worth collecting.
void BufferCache::AuditBuffers() {
	static auto& audit = Common::LiveSwitches::Get("KYTY_BUFFER_AUDIT", 0);
	static auto  last  = std::chrono::steady_clock::now();
	const auto   now   = std::chrono::steady_clock::now();
	if (audit.load(std::memory_order_relaxed) == 0 || now - last < std::chrono::seconds(10)) {
		return;
	}
	last = now;
	constexpr std::array<uint64_t, 6> Limits {1u << 20u, 4u << 20u, 16u << 20u, 64u << 20u,
	                                          256u << 20u, UINT64_MAX};
	std::array<uint64_t, Limits.size()> counts {};
	std::array<uint64_t, Limits.size()> bytes {};
	std::vector<BufferId>               ids;
	uint64_t                            total = 0;
	for (const auto& [address, id]: m_buffers) {
		const auto& buffer = m_slot_buffers[id];
		const auto  size   = buffer.Size();
		const auto  bucket = static_cast<size_t>(
		    std::ranges::find_if(Limits, [&](uint64_t limit) { return size < limit; }) -
		    Limits.begin());
		counts[bucket]++;
		bytes[bucket] += size;
		total += size;
		ids.push_back(id);
	}
	static constexpr const char* Names[] = {"<1M", "<4M", "<16M", "<64M", "<256M", ">=256M"};
	std::printf("Buffer audit: %zu buffers, %.2f GiB:", ids.size(),
	            static_cast<double>(total) / (1u << 30u));
	for (size_t i = 0; i < Limits.size(); i++) {
		std::printf(" %s %" PRIu64 " (%.0f MiB)", Names[i], counts[i],
		            static_cast<double>(bytes[i]) / (1u << 20u));
	}
	std::printf("\n");
	std::ranges::sort(ids, [&](BufferId a, BufferId b) {
		return m_slot_buffers[a].Size() > m_slot_buffers[b].Size();
	});
	const auto clock = LruClock();
	for (size_t i = 0; i < std::min<size_t>(ids.size(), 12); i++) {
		const auto& buffer = m_slot_buffers[ids[i]];
		const auto  idle   = clock - std::min(clock, m_lru_cache.TickOf(buffer.lru_id));
		std::printf("  0x%011" PRIx64 " %7.1f MiB idle %4" PRIu64 " frames, created for %.1f MiB "
		            "by shader 0x%016" PRIx64 "%s%s\n",
		            buffer.CpuAddress(), static_cast<double>(buffer.Size()) / (1u << 20u), idle,
		            static_cast<double>(buffer.origin_request) / (1u << 20u), buffer.origin_shader,
		            m_texture_cache.HasImagesInRegion(buffer.CpuAddress(), buffer.Size())
		                ? " shares memory with textures"
		                : "",
		            m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())
		                ? " GPU-written"
		                : "");
	}
	std::fflush(stdout);
}

void BufferCache::RunGarbageCollector() {
	VerifyBdaPageTable();
	AuditBuffers();
	RetireBuffers();
	ReportWriteBitmap();
	m_gc_tick++;
	const auto clock = LruClock();
	// Pressure is judged by this cache's own bytes. Device-wide usage also counts the images
	// spilled to host memory, which on a 6 GB card keeps it above the critical mark forever
	// and has the collector destroy and recreate every buffer twice a second (a third of
	// the GPU thread's time in the world). KYTY_GC_DEVICE_BYTES=1 restores that policy.
	static const bool device_bytes = std::getenv("KYTY_GC_DEVICE_BYTES") != nullptr;
	if (device_bytes && m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	// KYTY_GC_COMBINED=1: the images count too. Each cache derives its thresholds from the whole
	// device budget, so judged apart the two can fill well past it together; on a 12 GB card
	// Windows then pages cache memory to system RAM and every frame waits on PCIe.
	// Wolverine run 16 (paging, device usage 11.0 > budget 9.9 GiB): on trimmed to 9.8 GiB, GPU
	// busy 86% -> 27%, 167 -> 117 ms/frame; a one-time stutter while it trims.
	static auto&   combined = Common::LiveSwitches::Get("KYTY_GC_COMBINED", 1);
	// KYTY_GC_HEADROOM_MB: see TextureCache::RunGarbageCollector.
	static auto&   headroom = Common::LiveSwitches::Get("KYTY_GC_HEADROOM_MB", 0);
	const uint64_t used =
	    m_total_used_memory +
	    (combined.load(std::memory_order_relaxed) != 0 ? m_texture_cache.UsedMemory() : 0) +
	    (static_cast<uint64_t>(std::max<int64_t>(0, headroom.load(std::memory_order_relaxed)))
	     << 20u);
	if (used < m_trigger_gc_memory) {
		return;
	}
	const bool aggressive = used >= m_critical_gc_memory;
	// KYTY_BUFFER_STATS=1 (live): every 5 s, the collections and what they freed, and the
	// buffers created meanwhile, to see whether the collector churns buffers still in use.
	static auto& churn_stats = Common::LiveSwitches::Get("KYTY_BUFFER_STATS", 0);
	struct GcReport {
		uint64_t runs = 0, deleted = 0, downloaded = 0, deleted_bytes = 0;
		uint64_t created_seen                    = 0;
		std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now();
	};
	static GcReport gc_report;
	const auto      report_gc = [&](size_t deleted, size_t downloaded, uint64_t bytes) {
        if (churn_stats.load(std::memory_order_relaxed) == 0) {
            return;
        }
        gc_report.runs++;
        gc_report.deleted += deleted;
        gc_report.downloaded += downloaded;
        gc_report.deleted_bytes += bytes;
        const auto now = std::chrono::steady_clock::now();
        if (now - gc_report.at >= std::chrono::seconds(5)) {
            ::printf("Buffer GC (5 s): %" PRIu64
			              " runs (used %.2f GiB, trigger %.2f, critical %.2f%s), "
			                   "%" PRIu64 " deleted (%.1f MiB), %" PRIu64 " downloaded first; %" PRIu64
			              " buffers created\n",
			              gc_report.runs, static_cast<double>(used) / (1u << 30u),
			              static_cast<double>(m_trigger_gc_memory) / (1u << 30u),
			              static_cast<double>(m_critical_gc_memory) / (1u << 30u),
                     aggressive ? ", aggressive" : "", gc_report.deleted,
			              static_cast<double>(gc_report.deleted_bytes) / (1u << 20u),
			              gc_report.downloaded, m_buffers_created - gc_report.created_seen);
            gc_report              = {};
            gc_report.created_seen = m_buffers_created;
        }
	};
	// Ages in frames, as in the texture cache: a buffer used this frame or the last is
	// never a candidate, whatever the submission count.
	// KYTY_BUFFER_GC_AGE (live, frames): the age below the critical mark for buffers that share no
	// memory with a cached image (default set below).
	// Wolverine sits between the marks (7.7 of 5.2/8.4 GiB with the images): at 4 the collector ran
	// on every submission and retired ~170 small buffers/s that came back at once (~340 created/s,
	// run 28). Run 29 A/B 4/120: buffers created 1500 -> 15 per 5 s, same memory use, median 83.1
	// -> 66.8 ms, mean 76.7 -> 72.5 ms. Above the critical mark the age stays 2 frames.
	// But 120 for every buffer loses UI glyphs (run 30: the title menu draws "C N INU G M"): a
	// buffer kept alive over an image's memory feeds the image stale bytes (see the GC comment
	// below and SynchronizeBufferFromImage). So the longer age applies only to buffers that share
	// no bytes with a cached image; those keep the base age of 4 frames. Run 31 A/B 4/120 with this
	// rule (age 120 from startup): menu and HUD text intact, buffers created ~1500 -> ~15 per 5 s,
	// median 83.2 -> 66.9 ms, mean 76.8 -> 73.2 ms.
	static auto&   gc_age = Common::LiveSwitches::Get("KYTY_BUFFER_GC_AGE", 120);
	const uint64_t relaxed_age =
	    static_cast<uint64_t>(std::max<int64_t>(2, gc_age.load(std::memory_order_relaxed)));
	// KYTY_BUFFER_GC_CRITICAL_AGE (live, frames, default 60): the age above the critical mark.
	// Wolverine combat (run 49): the working set sat at 8.4-8.5 GiB, just over the critical mark,
	// so at age 2 the collector stayed aggressive and every 5 s deleted ~1000 buffers (~200
	// downloaded first, each waiting for the GPU) that were all created again (~2500). Age 60
	// cut that churn 10x with no frame-time loss (run 50); each deletion also risked the stale
	// buffer reads behind the device losts of runs 55/57.
	static auto&   critical_age = Common::LiveSwitches::Get("KYTY_BUFFER_GC_CRITICAL_AGE", 60);
	const uint64_t aggressive_age =
	    static_cast<uint64_t>(std::max<int64_t>(1, critical_age.load(std::memory_order_relaxed)));
	const uint64_t age        = std::min<uint64_t>(aggressive ? aggressive_age : 4, clock);
	const size_t   limit      = aggressive ? 64 : 32;
	// Kept buffers stay at the front of the LRU list; bound the walk past them.
	constexpr size_t MaxKeptVisits = 256;
	size_t           kept_visits   = 0;

	std::vector<BufferId> dirty_buffers;
	size_t                retire_count  = 0;
	uint64_t              retired_bytes = 0;
	m_lru_cache.ForEachItemBelow(clock - age, [&](BufferId id) {
		auto& buffer = m_slot_buffers[id];
		EXIT_IF(buffer.is_deleted);
		if (buffer.CpuAddress() == 0) {
			// See CreateBuffer: the tracker rejects address 0, so this one is never collected.
			return false;
		}
		if (!aggressive && relaxed_age > age &&
		    clock - m_lru_cache.TickOf(buffer.lru_id) < relaxed_age &&
		    !m_texture_cache.HasImagesInRegion(buffer.CpuAddress(), buffer.Size())) {
			return ++kept_visits == MaxKeptVisits;
		}
		if (OverlapsPendingWriteReadback(buffer.CpuAddress(),
		                                 buffer.CpuAddress() + buffer.Size())) {
			// A guest write's readback is still landing: its pages are GPU-owned without dirty
			// bytes, which the download below would reject. Collect the buffer later.
			return false;
		}
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		// An aged GPU-dirty buffer is downloaded and dropped under either policy. Retaining it
		// (the non-aggressive rule before) leaves its GPU-written bytes owning the pages
		// forever, and the images that share those pages are then re-sourced from the
		// buffer's stale contents: the world renders black. Measured 2026-09-21.
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		if (dirty) {
			EXIT_IF(!DownloadBufferMemory(buffer, buffer.CpuAddress(), buffer.Size()));
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
			retired_bytes += buffer.Size();
			DeleteBuffer(id);
		}
		return ++retire_count == limit;
	});
	report_gc(retire_count, dirty_buffers.size(), retired_bytes);
	if (dirty_buffers.empty()) {
		return;
	}

	// Publish all queued downloads before releasing their tracked pages and owners.
	const auto completion_tick = m_scheduler.CurrentTick();
	m_scheduler.Wait(completion_tick);
	m_scheduler.WaitPriorityOperations(completion_tick);
	for (const auto id: dirty_buffers) {
		auto& buffer = m_slot_buffers[id];
		m_memory_tracker.UnmarkRegionAsGpuModified(buffer.CpuAddress(), buffer.Size());
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size()) ||
		    m_gpu_modified_ranges.Intersects(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: garbage collection retained GPU ownership\n");
		}
		m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
		Unregister(id);
		// The GPU is past these buffers (waited above): freed at once unless retired.
		EraseBuffer(id, true);
	}
}

void BufferCache::ProcessFaultBuffer() {
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::SynchronizeCpuDirtyBuffersInRange(uint64_t vaddr, uint64_t size) {
	m_memory_tracker.ForEachMaybeCpuDirtyRegion(
	    vaddr, size, [this](uint64_t address, uint64_t bytes) {
		    SynchronizeBuffersInRange(address, bytes);
	    });
}

void BufferCache::SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size) {
	const auto end = vaddr + size;
	auto       it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		auto&      buffer = m_slot_buffers[it->second];
		const auto start  = std::max(buffer.CpuAddress(), vaddr);
		const auto finish = std::min(buffer.CpuAddress() + buffer.Size(), end);
		if (start < finish) {
			(void)SynchronizeBuffer(buffer, start, finish - start, false, false);
		}
	}
}

} // namespace Libs::Graphics
