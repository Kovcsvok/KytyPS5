#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_

#include "common/common.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <memory>

namespace Libs::Graphics {

enum class PageFaultAccess { Read, Write, Execute, Unknown };

class PageManager final {
public:
	PageManager();
	// The owner must stop all PageManager callers before destruction.
	~PageManager();

	KYTY_CLASS_NO_COPY(PageManager);

	[[nodiscard]] uint64_t GetPageSize() const;

	template <bool track>
	void UpdatePageWatchers(uint64_t vaddr, uint64_t size);
	template <bool track, bool is_read = false>
	void UpdatePageWatchersForRegion(uint64_t base_addr, RegionBits& mask);

	// Whether the page holding vaddr is read-protected for tracking. Unlocked: a hint that can
	// be stale by the time the caller acts on it.
	[[nodiscard]] bool IsReadWatched(uint64_t vaddr) const noexcept;
	// The write and access watcher counts of the 4 KiB page holding vaddr (0 when untracked).
	// Unlocked, like IsReadWatched: for fault triage.
	void Watchers(uint64_t vaddr, uint32_t& write, uint32_t& access) const noexcept;

	// KYTY_REPROTECT_BATCH (carbonimax 9ef01206 / 3a154025; BufferCache::FlushDeferredUploads).
	// While active, the protections the GPU thread would add (read-only / no-access) inside a
	// DeferCapture scope (the upload re-protections of a read-only synchronization) are recorded
	// instead of applied; the watcher counts change at once. Flush applies them in merged runs
	// (gaps of up to 16 pages): under each region's lock, every page whose current state still
	// asks for that protection, never an older one. Until then the recorded pages stay writable,
	// so their bytes must be read after Flush. Any thread may flush; only the GPU thread records.
	static void BeginDeferredProtect() noexcept;
	static void FlushDeferredProtect() noexcept;
	static void EndDeferredProtect() noexcept;
	[[nodiscard]] static bool DeferringProtect() noexcept;
	class DeferCapture {
	public:
		DeferCapture() noexcept;
		~DeferCapture();
		DeferCapture(const DeferCapture&)            = delete;
		DeferCapture& operator=(const DeferCapture&) = delete;

	private:
		bool m_previous = false;
	};

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
