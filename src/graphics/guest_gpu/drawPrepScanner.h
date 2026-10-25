#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_DRAWPREPSCANNER_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_DRAWPREPSCANNER_H_

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <thread>

namespace Libs::Graphics {

class RenderContext;
class CommandProcessor;

namespace HW {
class Context;
} // namespace HW

// The draw-prep scanner (host_gpu/renderer/drawPrep.h, KYTY_DRAW_PREP): reads each graphics and
// compute submission when it is queued, ahead of the GPU thread, follows its register writes on a
// command processor of its own per queue (which never executes anything), and prepares the
// descriptor walks of its draws and dispatches for the GPU thread to take.
class DrawPrepScanner final {
public:
	explicit DrawPrepScanner(RenderContext& renderer);
	~DrawPrepScanner();
	DrawPrepScanner(const DrawPrepScanner&)            = delete;
	DrawPrepScanner& operator=(const DrawPrepScanner&) = delete;

	// Under the GPU queue lock, in queue order: a submission's command stream (graphics queue 0,
	// compute queues 1..), and whether the GPU thread resets the queue's processor before it.
	void EnqueueSubmission(uint32_t queue, uint64_t seq, std::span<const uint32_t> commands,
	                       bool reset);

private:
	struct Item {
		uint32_t                  queue = 0;
		uint64_t                  seq   = 0;
		std::span<const uint32_t> commands;
		bool                      reset = false;
	};
	// The register copy of one GPU queue (each queue has its own command processor).
	struct QueueState {
		std::unique_ptr<CommandProcessor> cp;
		std::unique_ptr<HW::Context>      saved_ctx; // the context-state push
		bool                              pushed  = false;
		// The register copy is unreliable (a register table the scanner could not read, a
		// context-state sequence it could not follow) until the processor is reset.
		bool                              tainted = false;
	};
	QueueState& State(uint32_t queue);

	void Run();
	void Scan(const Item& item);
	void ScanDraw(uint64_t seq, uint32_t ordinal, const uint32_t* packet);
	void ScanDispatch(uint64_t seq, uint32_t ordinal, const uint32_t* packet, uint32_t opcode);
	bool ApplyRegisterIndirect(uint32_t header, const uint32_t* body);
	void ApplyContextState(uint32_t operation);
	void ResetState();

	RenderContext&                              m_renderer;
	std::array<std::unique_ptr<QueueState>, 64> m_queues;
	QueueState*                                 m_state = nullptr; // of the item being scanned
	uint32_t                                    m_queue = 0;

	std::mutex              m_mutex;
	std::condition_variable m_work;
	std::deque<Item>        m_items;
	bool                    m_stop = false;
	std::thread             m_thread;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_GUEST_GPU_DRAWPREPSCANNER_H_
