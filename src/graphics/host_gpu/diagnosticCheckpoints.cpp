#include "graphics/host_gpu/vulkanCommon.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/addressBindingReport.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr size_t CHECKPOINT_RING_SIZE = 1u << 16u;

std::array<DiagnosticCheckpoint, CHECKPOINT_RING_SIZE> g_checkpoints {};
std::atomic<uint64_t>                                  g_checkpoint_sequence {0};
// CPU history is separate from NV checkpoint storage: it is not proof of GPU progress.
std::array<DiagnosticCheckpoint, 256> g_host_history {};
std::mutex g_host_history_mutex;
uint64_t g_host_sequence = 0;

const char* OpName(uint32_t op) {
	switch (op) {
		case 0: return "DispatchDirect";
		case 1: return "DrawIndex";
		case 2: return "DrawIndexAuto";
		case 3: return "EopWrite";
		case 4: return "EopInterrupt";
		case 5: return "EopWriteBack";
		case 6: return "EopFlip";
		case 7: return "EopWriteBackFlip";
		case 8: return "EopOnlyFlip";
		case 9: return "DispatchIndirect";
		default: return "Unknown";
	}
}

void Print(const char* stage, const DiagnosticCheckpoint& checkpoint) {
	std::printf("  [%s] seq=%" PRIu64 " op=%s submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64
	            " shader=0x%016" PRIx64 "\n",
	            stage, checkpoint.sequence, OpName(checkpoint.op), checkpoint.submit_id,
	            checkpoint.arg0, checkpoint.arg1, checkpoint.arg2, checkpoint.arg3,
	            checkpoint.arg4, checkpoint.arg5);
	LOGF("  [%s] seq=%" PRIu64 " op=%s submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64
	     " shader=0x%016" PRIx64 "\n",
	     stage, checkpoint.sequence, OpName(checkpoint.op), checkpoint.submit_id, checkpoint.arg0,
	     checkpoint.arg1, checkpoint.arg2, checkpoint.arg3, checkpoint.arg4, checkpoint.arg5);
}

// The ring slot of a 32-bit marker value, or nullptr once a later checkpoint reused the slot.
const DiagnosticCheckpoint* FindCheckpoint(uint32_t marker) {
	const auto& slot = g_checkpoints[marker % CHECKPOINT_RING_SIZE];
	return static_cast<uint32_t>(slot.sequence) == marker ? &slot : nullptr;
}

void DumpGpuMarkers(GraphicContext& graphics) {
	if (!graphics.gpu_markers_enabled || graphics.gpu_marker_values == nullptr) {
		return;
	}
	const uint32_t top    = graphics.gpu_marker_values[0];
	const uint32_t bottom = graphics.gpu_marker_values[1];
	const auto     last   = g_checkpoint_sequence.load(std::memory_order_relaxed);
	std::printf("--- AMD GPU markers: reached seq=%u, everything before seq=%u finished "
	            "(last recorded seq=%" PRIu64 ") ---\n",
	            top, bottom, last);
	LOGF("--- AMD GPU markers: reached seq=%u, everything before seq=%u finished (last recorded "
	     "seq=%" PRIu64 ") ---\n",
	     top, bottom, last);
	if (top == 0 && bottom == 0) {
		std::fflush(stdout);
		return;
	}
	// The oldest unfinished operation (bottom) is the one the GPU hung in or waited on; the ones
	// up to top had started too. Show a little context before it.
	const uint32_t first = bottom > 16u ? bottom - 16u : 1u;
	const uint32_t end   = std::max(top, bottom) + 1u;
	const uint32_t limit = std::min(end, first + 160u);
	for (uint32_t value = first; value < limit; value++) {
		const auto* checkpoint = FindCheckpoint(value);
		if (checkpoint == nullptr) {
			continue;
		}
		const char* label = value < bottom ? "done" : value == bottom ? "HUNG?" : "started";
		Print(label, *checkpoint);
	}
	// The shaders of the oldest unfinished draw or dispatch, ready for skip_live.txt or
	// --skip-shaders: the draw's pixel shader is in arg4 (DrawIndexAuto phases), the vertex-side or
	// compute shader in arg5.
	for (uint32_t value = bottom; value < limit; value++) {
		const auto* checkpoint = FindCheckpoint(value);
		if (checkpoint == nullptr || checkpoint->arg5 == 0) {
			continue;
		}
		const bool dispatch = checkpoint->op == 0 || checkpoint->op == 9;
		const uint64_t pixel = dispatch ? 0 : checkpoint->arg4;
		std::printf("--- GPU hang suspect: seq=%u op=%s shader=0x%016" PRIx64
		            " pixel=0x%016" PRIx64 " ---\n",
		            value, OpName(checkpoint->op), checkpoint->arg5, pixel);
		LOGF("--- GPU hang suspect: seq=%u op=%s shader=0x%016" PRIx64 " pixel=0x%016" PRIx64
		     " ---\n",
		     value, OpName(checkpoint->op), checkpoint->arg5, pixel);
		break;
	}
	std::fflush(stdout);
}

}

namespace {
std::atomic<void (*)()> g_device_loss_hook {nullptr};
} // namespace

uint64_t PeekDiagnosticSequence() {
	return g_checkpoint_sequence.load(std::memory_order_relaxed) + 1;
}

void SetDeviceLossHook(void (*hook)()) {
	g_device_loss_hook.store(hook, std::memory_order_relaxed);
}

void CreateGpuMarkers(GraphicContext& graphics) {
	if (!graphics.gpu_markers_enabled) {
		return;
	}
	vk::BufferCreateInfo buffer_info {};
	buffer_info.size        = 256;
	buffer_info.usage       = vk::BufferUsageFlagBits::eTransferDst;
	buffer_info.sharingMode = vk::SharingMode::eExclusive;
	auto result = graphics.device.createBuffer(&buffer_info, nullptr, &graphics.gpu_marker_buffer);
	if (result != vk::Result::eSuccess) {
		LOGF("GPU markers: buffer creation failed: %s\n", vk::to_string(result).c_str());
		graphics.gpu_markers_enabled = false;
		return;
	}
	vk::MemoryRequirements requirements {};
	graphics.device.getBufferMemoryRequirements(graphics.gpu_marker_buffer, &requirements);
	// Device-coherent uncached memory (VK_AMD_device_coherent_memory) keeps the last writes
	// visible after the device is lost; host-coherent memory is the fallback.
	const auto host = vk::MemoryPropertyFlagBits::eHostVisible |
	                  vk::MemoryPropertyFlagBits::eHostCoherent;
	const auto uncached = host | vk::MemoryPropertyFlagBits::eDeviceCoherentAMD |
	                      vk::MemoryPropertyFlagBits::eDeviceUncachedAMD;
	const auto find_type = [&](vk::MemoryPropertyFlags wanted) {
		const auto& properties = graphics.physical_device_memory_properties;
		for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
			if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
			    (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
				return i;
			}
		}
		return UINT32_MAX;
	};
	uint32_t type = graphics.device_coherent_memory_enabled ? find_type(uncached) : UINT32_MAX;
	const bool uncached_type = type != UINT32_MAX;
	if (type == UINT32_MAX) {
		type = find_type(host);
	}
	if (type == UINT32_MAX) {
		LOGF("GPU markers: no host-visible memory type\n");
		graphics.device.destroyBuffer(graphics.gpu_marker_buffer, nullptr);
		graphics.gpu_marker_buffer   = nullptr;
		graphics.gpu_markers_enabled = false;
		return;
	}
	vk::MemoryAllocateInfo allocate_info {};
	allocate_info.allocationSize  = requirements.size;
	allocate_info.memoryTypeIndex = type;
	result = graphics.device.allocateMemory(&allocate_info, nullptr, &graphics.gpu_marker_memory);
	void* mapped = nullptr;
	if (result == vk::Result::eSuccess) {
		result = graphics.device.bindBufferMemory(graphics.gpu_marker_buffer,
		                                          graphics.gpu_marker_memory, 0);
	}
	if (result == vk::Result::eSuccess) {
		result = graphics.device.mapMemory(graphics.gpu_marker_memory, 0, VK_WHOLE_SIZE, {},
		                                   &mapped);
	}
	if (result != vk::Result::eSuccess || mapped == nullptr) {
		LOGF("GPU markers: memory setup failed: %s\n", vk::to_string(result).c_str());
		graphics.gpu_markers_enabled = false;
		return;
	}
	std::memset(mapped, 0, 256);
	graphics.gpu_marker_values = static_cast<volatile uint32_t*>(mapped);
	std::printf("GPU markers: VK_AMD_buffer_marker on (%s memory)\n",
	            uncached_type ? "device-uncached" : "host-coherent");
	LOGF("GPU markers: VK_AMD_buffer_marker on (%s memory)\n",
	     uncached_type ? "device-uncached" : "host-coherent");
}

const DiagnosticCheckpoint* RecordDiagnosticCheckpoint(const DiagnosticCheckpoint& checkpoint) {
	const auto sequence = g_checkpoint_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
	auto&      slot     = g_checkpoints[sequence % CHECKPOINT_RING_SIZE];
	slot                = checkpoint;
	slot.sequence       = sequence;
	return &slot;
}

void RecordHostDiagnostic(const DiagnosticCheckpoint& checkpoint) {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_HOST_BREADCRUMBS");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	if (!enabled) return;
	std::lock_guard lock(g_host_history_mutex);
	auto& slot = g_host_history[g_host_sequence % g_host_history.size()];
	slot = checkpoint;
	slot.sequence = ++g_host_sequence;
}

static void DumpDeviceFault(GraphicContext& graphics) {
	if (!graphics.device_fault_enabled) {
		return;
	}
	vk::DeviceFaultCountsEXT counts {};
	auto result = graphics.device.getFaultInfoEXT(&counts, nullptr);
	if (result != vk::Result::eSuccess && result != vk::Result::eIncomplete) {
		std::printf("--- Device fault: query failed: %s ---\n", vk::to_string(result).c_str());
		return;
	}
	std::vector<vk::DeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
	std::vector<vk::DeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);
	std::vector<uint8_t>                       binary(static_cast<size_t>(counts.vendorBinarySize));
	vk::DeviceFaultInfoEXT                     info {};
	info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
	info.pVendorInfos  = vendors.empty() ? nullptr : vendors.data();
	info.pVendorBinaryData = binary.empty() ? nullptr : binary.data();
	result = graphics.device.getFaultInfoEXT(&counts, &info);
	std::printf("--- Device fault (%s): \"%s\" addresses=%u vendor=%u binary=%" PRIu64 " ---\n",
	            vk::to_string(result).c_str(), info.description.data(), counts.addressInfoCount,
	            counts.vendorInfoCount, static_cast<uint64_t>(counts.vendorBinarySize));
	LOGF("--- Device fault (%s): \"%s\" addresses=%u vendor=%u binary=%" PRIu64 " ---\n",
	     vk::to_string(result).c_str(), info.description.data(), counts.addressInfoCount,
	     counts.vendorInfoCount, static_cast<uint64_t>(counts.vendorBinarySize));
	if (!binary.empty()) {
		if (auto* file = std::fopen("_device_fault.nv-gpudmp", "wb"); file != nullptr) {
			std::fwrite(binary.data(), 1, binary.size(), file);
			std::fclose(file);
			std::printf("  vendor binary written to _device_fault.nv-gpudmp\n");
		}
	}
	for (uint32_t i = 0; i < counts.addressInfoCount; i++) {
		const auto& address = addresses[i];
		std::printf("  address[%u]: %s at 0x%016" PRIx64 " (precision 0x%" PRIx64 ")\n", i,
		            vk::to_string(address.addressType).c_str(),
		            static_cast<uint64_t>(address.reportedAddress),
		            static_cast<uint64_t>(address.addressPrecision));
		LOGF("  address[%u]: %s at 0x%016" PRIx64 " (precision 0x%" PRIx64 ")\n", i,
		     vk::to_string(address.addressType).c_str(),
		     static_cast<uint64_t>(address.reportedAddress),
		     static_cast<uint64_t>(address.addressPrecision));
		if (address.addressType != vk::DeviceFaultAddressTypeEXT::eInstructionPointerUnknown &&
		    address.addressType != vk::DeviceFaultAddressTypeEXT::eInstructionPointerInvalid &&
		    address.addressType != vk::DeviceFaultAddressTypeEXT::eInstructionPointerFault) {
			DescribeBufferDeviceAddress(static_cast<uint64_t>(address.reportedAddress));
			AddressBindingDescribe(static_cast<uint64_t>(address.reportedAddress));
		}
	}
	for (uint32_t i = 0; i < counts.vendorInfoCount; i++) {
		const auto& vendor = vendors[i];
		std::printf("  vendor[%u]: \"%s\" code=0x%016" PRIx64 " data=0x%016" PRIx64 "\n", i,
		            vendor.description.data(), static_cast<uint64_t>(vendor.vendorFaultCode),
		            static_cast<uint64_t>(vendor.vendorFaultData));
		LOGF("  vendor[%u]: \"%s\" code=0x%016" PRIx64 " data=0x%016" PRIx64 "\n", i,
		     vendor.description.data(), static_cast<uint64_t>(vendor.vendorFaultCode),
		     static_cast<uint64_t>(vendor.vendorFaultData));
	}
	std::fflush(stdout);
}

void DumpDeviceLossDiagnostics(GraphicContext& graphics) {
	static std::atomic_flag dumped = ATOMIC_FLAG_INIT;
	if (!dumped.test_and_set(std::memory_order_relaxed)) {
		std::array<DiagnosticCheckpoint, 256> snapshot;
		uint64_t sequence;
		{
			std::lock_guard lock(g_host_history_mutex);
			snapshot = g_host_history;
			sequence = g_host_sequence;
		}
		if (sequence != 0) {
			std::printf("--- Host operation history (CPU recorded, NOT GPU completion) ---\n");
			const auto first = sequence > snapshot.size() ? sequence - snapshot.size() : 0;
			for (auto i = first; i < sequence; ++i) Print("host", snapshot[i % snapshot.size()]);
			std::fflush(stdout);
		}
		DumpGpuMarkers(graphics);
		if (auto* hook = g_device_loss_hook.load(std::memory_order_relaxed); hook != nullptr) {
			hook();
		}
	}
	DumpDeviceFault(graphics);
	if (!graphics.diagnostic_checkpoints_enabled || graphics.queue == nullptr) {
		return;
	}
	std::vector<vk::CheckpointDataNV> data;
	{
		Common::LockGuard lock(graphics.queue_mutex);
		data = graphics.queue.getCheckpointDataNV();
	}
	std::printf("--- Diagnostic checkpoints (%zu, last recorded seq=%" PRIu64 ") ---\n",
	            data.size(), g_checkpoint_sequence.load(std::memory_order_relaxed));
	LOGF("--- Diagnostic checkpoints (%zu, last recorded seq=%" PRIu64 ") ---\n", data.size(),
	     g_checkpoint_sequence.load(std::memory_order_relaxed));
	for (const auto& entry: data) {
		const auto* checkpoint = static_cast<const DiagnosticCheckpoint*>(entry.pCheckpointMarker);
		const auto  stage      = vk::to_string(entry.stage);
		const bool  in_ring    = checkpoint >= g_checkpoints.data() &&
		                      checkpoint < g_checkpoints.data() + g_checkpoints.size();
		std::printf("  [%s] marker=%p ring=[%p, %p) %s\n", stage.c_str(),
		            static_cast<const void*>(checkpoint),
		            static_cast<const void*>(g_checkpoints.data()),
		            static_cast<const void*>(g_checkpoints.data() + g_checkpoints.size()),
		            in_ring ? "inside" : "OUTSIDE");
		if (checkpoint == nullptr || !in_ring) {
			continue;
		}
		Print(stage.c_str(), *checkpoint);
	}
	std::fflush(stdout);
}

}
