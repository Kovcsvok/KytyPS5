#include "graphics/host_gpu/renderer/indirectDispatch.h"

#include "common/assert.h"
#include "common/liveSwitches.h"
#include "gpu_tiler_shaders/dispatch_indirect_groups_spv.h"
#include "gpu_tiler_shaders/draw_indirect_guard_spv.h"
#include "gpu_tiler_shaders/mesh_indirect_args_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include "common/logging/log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics {

namespace {

struct PushConstants {
	uint32_t threads_low;
	uint32_t threads_high;
	uint32_t local_size[3];
	uint32_t output_dword;
};

struct GuardPushConstants {
	uint32_t record_low;
	uint32_t record_high;
	uint32_t dwords;
	uint32_t output_dword;
	uint32_t history_dword;
	uint32_t tag;
	uint32_t counter;
};

struct MeshPushConstants {
	uint32_t record_low;
	uint32_t record_high;
	uint32_t sequence_low;
	uint32_t sequence_high;
	uint32_t index_low;
	uint32_t index_high;
	uint32_t indexed;
	uint32_t max_count;
	uint32_t element_size;
	uint32_t primitive_size;
	uint32_t primitive_step;
	uint32_t primitives_per_group;
	uint32_t max_groups_x;
	uint32_t max_groups_y;
	uint32_t max_groups_total;
};
static_assert(sizeof(MeshPushConstants) == 60);

// The stages of a guest mesh pipeline's push-constant range (shaders.cpp: fragment always).
constexpr vk::ShaderStageFlags MeshDrawStages =
    vk::ShaderStageFlagBits::eMeshEXT | vk::ShaderStageFlagBits::eFragment;

IndirectDrawGuard* g_guard = nullptr;

// Report layout (dwords): the dropped count, then the last dropped record.
constexpr uint32_t GuardReportBytes = 256;

} // namespace

IndirectDispatchGroups::IndirectDispatchGroups(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics),
      m_groups(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
               vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer |
                   vk::BufferUsageFlagBits::eShaderDeviceAddress,
               Entries * EntryDwords * sizeof(uint32_t)) {
	SetVulkanObjectNameF(graphics.device, m_groups.Handle(), "Indirect Dispatch Groups");

	const vk::DescriptorSetLayoutBinding binding {0, vk::DescriptorType::eStorageBuffer, 1,
	                                              vk::ShaderStageFlagBits::eCompute, nullptr};
	vk::DescriptorSetLayoutCreateInfo    layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = 1;
	layout_info.pBindings    = &binding;
	RequireVulkanSuccess(graphics.device.createDescriptorSetLayout(&layout_info, nullptr,
	                                                               &m_set_layout),
	                     "create indirect-dispatch descriptor layout");

	const vk::PushConstantRange  push_range {vk::ShaderStageFlagBits::eCompute, 0,
                                            sizeof(PushConstants)};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &m_set_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                          &m_pipeline_layout),
	                     "create indirect-dispatch pipeline layout");

	const auto                        module = CompileSPV(DISPATCH_INDIRECT_GROUPS_SPV, graphics.device);
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = m_pipeline_layout;
	const auto result =
	    graphics.device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &m_pipeline);
	graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create indirect-dispatch pipeline");
}

IndirectDispatchGroups::~IndirectDispatchGroups() {
	m_graphics.device.destroyPipeline(m_pipeline, nullptr);
	m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_set_layout, nullptr);
}

IndirectDispatchGroups::Result
IndirectDispatchGroups::Convert(const CommandRecorder& command, vk::DeviceAddress threads,
                                const std::array<uint32_t, 3>& local_size) {
	const auto entry  = m_next;
	m_next            = (m_next + 1u) % Entries;
	const auto offset = vk::DeviceSize {entry} * EntryDwords * sizeof(uint32_t);

	// The counts come from earlier shader or transfer writes; the entry's previous indirect read
	// must finish before it is rewritten.
	vk::MemoryBarrier counts_ready {};
	counts_ready.srcAccessMask =
	    vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
	counts_ready.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eComputeShader, {}, 1, &counts_ready, 0,
	                        nullptr, 0, nullptr);

	const vk::DescriptorBufferInfo info {m_groups.Handle(), 0, VK_WHOLE_SIZE};
	vk::WriteDescriptorSet         write {};
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eStorageBuffer;
	write.pBufferInfo     = &info;
	const PushConstants push {
	    static_cast<uint32_t>(threads), static_cast<uint32_t>(threads >> 32u),
	    {local_size[0], local_size[1], local_size[2]}, entry * EntryDwords};
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0, 1, &write);
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
	                      &push);
	command.dispatch(1, 1, 1);

	vk::MemoryBarrier groups_ready {};
	groups_ready.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	groups_ready.dstAccessMask =
	    vk::AccessFlagBits::eIndirectCommandRead | vk::AccessFlagBits::eShaderRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eDrawIndirect |
	                            vk::PipelineStageFlagBits::eComputeShader,
	                        {}, 1, &groups_ready, 0, nullptr, 0, nullptr);
	return {m_groups.Handle(), offset,
	        m_groups.BufferDeviceAddress() + offset + 4u * sizeof(uint32_t)};
}

// KYTY_INDIRECT_DRAW_GUARD (live, default 1): 0 sends indirect draws to the GPU without the
// validating dispatch (and its barriers and render pass break) in front of each.
bool IndirectDrawGuard::Enabled() {
	static auto& enabled = Common::LiveSwitches::Get("KYTY_INDIRECT_DRAW_GUARD", 1);
	return enabled.load(std::memory_order_relaxed) != 0;
}

IndirectDrawGuard::IndirectDrawGuard(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics),
      m_records(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
                vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer,
                Entries * EntryDwords * sizeof(uint32_t)),
      m_report(graphics, scheduler, MemoryUsage::Stream, 0, vk::BufferUsageFlagBits::eStorageBuffer,
               GuardReportBytes),
      m_history(graphics, scheduler, MemoryUsage::Download, 0,
                vk::BufferUsageFlagBits::eStorageBuffer, HistoryEntries * 8u * sizeof(uint32_t)) {
	EXIT_IF(m_history.Mapped().size() < HistoryEntries * 8u * sizeof(uint32_t));
	std::memset(m_history.Mapped().data(), 0, HistoryEntries * 8u * sizeof(uint32_t));
	if (!m_history.IsCoherent()) {
		m_history.Flush(0, HistoryEntries * 8u * sizeof(uint32_t));
	}
	g_guard = this;
	SetDeviceLossHook(&IndirectDrawGuard::DumpHistory);
	SetVulkanObjectNameF(graphics.device, m_records.Handle(), "Indirect Draw Guard");
	EXIT_IF(m_report.Mapped().size() < GuardReportBytes);
	std::memset(m_report.Mapped().data(), 0, GuardReportBytes);
	if (!m_report.IsCoherent()) {
		m_report.Flush(0, GuardReportBytes);
	}

	const std::array<vk::DescriptorSetLayoutBinding, 3> bindings {{
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	}};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
	layout_info.pBindings    = bindings.data();
	RequireVulkanSuccess(graphics.device.createDescriptorSetLayout(&layout_info, nullptr,
	                                                               &m_set_layout),
	                     "create indirect-draw guard descriptor layout");

	const vk::PushConstantRange  push_range {vk::ShaderStageFlagBits::eCompute, 0,
                                            sizeof(GuardPushConstants)};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &m_set_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                          &m_pipeline_layout),
	                     "create indirect-draw guard pipeline layout");

	const auto                        module = CompileSPV(DRAW_INDIRECT_GUARD_SPV, graphics.device);
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = m_pipeline_layout;
	const auto result =
	    graphics.device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &m_pipeline);
	graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create indirect-draw guard pipeline");
}

IndirectDrawGuard::~IndirectDrawGuard() {
	SetDeviceLossHook(nullptr);
	g_guard = nullptr;
	m_graphics.device.destroyPipeline(m_pipeline, nullptr);
	m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_set_layout, nullptr);
}

void IndirectDrawGuard::Report() {
	const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
	                     std::chrono::steady_clock::now().time_since_epoch())
	                     .count();
	if (now - m_last_report_us < 5000000) {
		return;
	}
	m_last_report_us = now;
	if (!m_report.IsCoherent()) {
		m_report.Invalidate(0, GuardReportBytes);
	}
	std::array<uint32_t, 6> words {};
	std::memcpy(words.data(), m_report.Mapped().data(), sizeof(words));
	if (words[0] == m_reported) {
		return;
	}
	m_reported = words[0];
	std::printf("Indirect draw guard: %u draws with impossible counts dropped; last record "
	            "%u,%u,%u,%u,%u\n",
	            words[0], words[1], words[2], words[3], words[4], words[5]);
	std::fflush(stdout);
	LOGF("Indirect draw guard: %u draws with impossible counts dropped; last record "
	     "%u,%u,%u,%u,%u\n",
	     words[0], words[1], words[2], words[3], words[4], words[5]);
}

void IndirectDrawGuard::DumpHistory() {
	auto* guard = g_guard;
	if (guard == nullptr) {
		return;
	}
	const auto bytes = HistoryEntries * 8u * sizeof(uint32_t);
	if (!guard->m_history.IsCoherent()) {
		guard->m_history.Invalidate(0, bytes);
	}
	std::array<uint32_t, HistoryEntries * 8u> words {};
	std::memcpy(words.data(), guard->m_history.Mapped().data(), bytes);
	std::printf("--- Indirect draw records (last %u written by the GPU; tag = GPU marker seq) ---\n",
	            HistoryEntries);
	const auto newest = guard->m_counter;
	for (uint32_t n = HistoryEntries; n > 0; n--) {
		if (newest < n) {
			continue;
		}
		const auto  counter = newest - n;
		const auto* entry   = words.data() + (counter % HistoryEntries) * 8u;
		const bool  written = entry[1] == counter && (entry[2] & 0xffu) != 0u;
		std::printf("  #%u %s tag=%u dwords=%u%s: %u,%u,%u,%u,%u\n", counter,
		            written ? "ran" : "not run", entry[0], entry[2] & 0xffu,
		            (entry[2] & 0x100u) != 0 ? " DROPPED" : "", entry[3], entry[4], entry[5],
		            entry[6], entry[7]);
	}
	std::fflush(stdout);
}

IndirectDrawGuard::Result IndirectDrawGuard::Guard(const CommandRecorder& command,
                                                   vk::DeviceAddress record, bool indexed) {
	Report();
	const auto entry  = m_next;
	m_next            = (m_next + 1u) % Entries;
	const auto offset = vk::DeviceSize {entry} * EntryDwords * sizeof(uint32_t);

	// The record comes from earlier shader or transfer writes; the entry's previous indirect
	// read must finish before it is rewritten.
	vk::MemoryBarrier record_ready {};
	record_ready.srcAccessMask = vk::AccessFlagBits::eShaderWrite |
	                             vk::AccessFlagBits::eTransferWrite |
	                             vk::AccessFlagBits::eIndirectCommandRead;
	record_ready.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eComputeShader, {}, 1, &record_ready, 0,
	                        nullptr, 0, nullptr);

	const std::array<vk::DescriptorBufferInfo, 3> infos {{
	    {m_records.Handle(), 0, VK_WHOLE_SIZE},
	    {m_report.Handle(), 0, VK_WHOLE_SIZE},
	    {m_history.Handle(), 0, VK_WHOLE_SIZE},
	}};
	std::array<vk::WriteDescriptorSet, 3> writes {};
	for (uint32_t i = 0; i < writes.size(); i++) {
		writes[i].dstBinding      = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[i].pBufferInfo     = &infos[i];
	}
	const auto counter = m_counter++;
	const GuardPushConstants push {static_cast<uint32_t>(record),
	                               static_cast<uint32_t>(record >> 32u),
	                               indexed ? 5u : 4u,
	                               entry * EntryDwords,
	                               (counter % HistoryEntries) * 8u,
	                               static_cast<uint32_t>(PeekDiagnosticSequence()),
	                               counter};
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0,
	                             static_cast<uint32_t>(writes.size()), writes.data());
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
	                      &push);
	command.dispatch(1, 1, 1);

	vk::MemoryBarrier record_written {};
	record_written.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	record_written.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eDrawIndirect, {}, 1, &record_written, 0,
	                        nullptr, 0, nullptr);
	return {m_records.Handle(), offset};
}

bool MeshIndirectDraws::Enabled(const GraphicContext& graphics) {
	static auto& enabled = Common::LiveSwitches::Get("KYTY_DGC_DRAWS", 1);
	return graphics.device_generated_commands_enabled && graphics.mesh_shader_enabled &&
	       enabled.load(std::memory_order_relaxed) != 0;
}

MeshIndirectDraws::MeshIndirectDraws(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler),
      m_sequences(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
                  vk::BufferUsageFlagBits::eStorageBuffer |
                      vk::BufferUsageFlagBits::eIndirectBuffer |
                      vk::BufferUsageFlagBits::eShaderDeviceAddress,
                  Entries * EntryBytes) {
	SetVulkanObjectNameF(graphics.device, m_sequences.Handle(), "Mesh Indirect Sequences");

	const vk::PushConstantRange  push_range {vk::ShaderStageFlagBits::eCompute, 0,
                                            sizeof(MeshPushConstants)};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                          &m_pipeline_layout),
	                     "create mesh-indirect pipeline layout");

	const auto                        module = CompileSPV(MESH_INDIRECT_ARGS_SPV, graphics.device);
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = m_pipeline_layout;
	const auto result =
	    graphics.device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &m_pipeline);
	graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create mesh-indirect pipeline");
	std::printf("Mesh indirect draws: device-generated commands in use\n");
	std::fflush(stdout);
}

MeshIndirectDraws::~MeshIndirectDraws() {
	auto device = m_graphics.device;
	for (const auto& [pipeline_layout, layout]: m_layouts) {
		device.destroyIndirectCommandsLayoutEXT(vk::IndirectCommandsLayoutEXT {layout}, nullptr);
	}
	if (m_preprocess) {
		device.destroyBuffer(m_preprocess, nullptr);
		device.freeMemory(m_preprocess_memory, nullptr);
	}
	device.destroyPipeline(m_pipeline, nullptr);
	device.destroyPipelineLayout(m_pipeline_layout, nullptr);
}

vk::IndirectCommandsLayoutEXT MeshIndirectDraws::LayoutFor(vk::PipelineLayout pipeline_layout) {
	const auto found = m_layouts.find(static_cast<VkPipelineLayout>(pipeline_layout));
	if (found != m_layouts.end()) {
		return vk::IndirectCommandsLayoutEXT {found->second};
	}
	constexpr uint32_t DrawWordsBytes =
	    ShaderRecompiler::IR::PushData::MeshDrawDwordCount * sizeof(uint32_t);
	vk::IndirectCommandsPushConstantTokenEXT push_token {};
	push_token.updateRange = vk::PushConstantRange {MeshDrawStages, 0, DrawWordsBytes};
	std::array<vk::IndirectCommandsLayoutTokenEXT, 2> tokens {};
	tokens[0].type               = vk::IndirectCommandsTokenTypeEXT::ePushConstant;
	tokens[0].data.pPushConstant = &push_token;
	tokens[0].offset             = 0;
	tokens[1].type               = vk::IndirectCommandsTokenTypeEXT::eDrawMeshTasks;
	tokens[1].offset             = DrawWordsBytes;
	vk::IndirectCommandsLayoutCreateInfoEXT info {};
	info.shaderStages   = MeshDrawStages;
	info.indirectStride = SequenceStride;
	info.pipelineLayout = pipeline_layout;
	info.tokenCount     = static_cast<uint32_t>(tokens.size());
	info.pTokens        = tokens.data();
	vk::IndirectCommandsLayoutEXT layout = nullptr;
	RequireVulkanSuccess(m_graphics.device.createIndirectCommandsLayoutEXT(&info, nullptr, &layout),
	                     "create mesh indirect commands layout");
	m_layouts.emplace(static_cast<VkPipelineLayout>(pipeline_layout),
	                  static_cast<VkIndirectCommandsLayoutEXT>(layout));
	return layout;
}

// The preprocess buffer is sized for the largest requirement seen; a larger one replaces it
// once the GPU is done with the old one.
vk::DeviceSize MeshIndirectDraws::PreprocessSizeFor(vk::Pipeline                  pipeline,
                                                    vk::IndirectCommandsLayoutEXT layout) {
	const auto found = m_preprocess_sizes.find(static_cast<VkPipeline>(pipeline));
	if (found != m_preprocess_sizes.end()) {
		return found->second;
	}
	auto                                           device = m_graphics.device;
	vk::GeneratedCommandsPipelineInfoEXT           pipeline_info {};
	pipeline_info.pipeline = pipeline;
	vk::GeneratedCommandsMemoryRequirementsInfoEXT info {};
	info.pNext                  = &pipeline_info;
	info.indirectCommandsLayout = layout;
	info.maxSequenceCount       = 1;
	info.maxDrawCount           = 1;
	vk::MemoryRequirements2 requirements {};
	device.getGeneratedCommandsMemoryRequirementsEXT(&info, &requirements);
	const auto& memory = requirements.memoryRequirements;
	const auto  size   = std::max<vk::DeviceSize>(memory.size, 4);
	EXIT_IF(memory.alignment > PreprocessAlignment);
	m_preprocess_sizes.emplace(static_cast<VkPipeline>(pipeline), size);
	if (size + PreprocessAlignment <= m_preprocess_capacity) {
		return size;
	}
	if (m_preprocess) {
		m_scheduler.DeferOperation([device, buffer = m_preprocess, old = m_preprocess_memory] {
			device.destroyBuffer(buffer, nullptr);
			device.freeMemory(old, nullptr);
		});
		m_preprocess        = nullptr;
		m_preprocess_memory = nullptr;
	}
	const auto capacity = std::max<vk::DeviceSize>(2 * (size + PreprocessAlignment), 64 * 1024);
	vk::BufferUsageFlags2CreateInfo usage {};
	usage.usage = vk::BufferUsageFlagBits2::ePreprocessBufferEXT |
	              vk::BufferUsageFlagBits2::eShaderDeviceAddress;
	vk::BufferCreateInfo buffer_info {};
	buffer_info.pNext = &usage;
	buffer_info.size  = capacity;
	RequireVulkanSuccess(device.createBuffer(&buffer_info, nullptr, &m_preprocess),
	                     "create mesh indirect preprocess buffer");
	vk::MemoryRequirements buffer_memory {};
	device.getBufferMemoryRequirements(m_preprocess, &buffer_memory);
	const auto types  = buffer_memory.memoryTypeBits & memory.memoryTypeBits;
	const auto props  = m_graphics.physical_device.getMemoryProperties();
	uint32_t   chosen = UINT32_MAX;
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((types & (1u << i)) == 0) {
			continue;
		}
		const bool local =
		    static_cast<bool>(props.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal);
		if (chosen == UINT32_MAX || local) {
			chosen = i;
		}
		if (local) {
			break;
		}
	}
	EXIT_IF(chosen == UINT32_MAX);
	vk::MemoryAllocateFlagsInfo flags {};
	flags.flags = vk::MemoryAllocateFlagBits::eDeviceAddress;
	vk::MemoryAllocateInfo allocate {};
	allocate.pNext           = &flags;
	allocate.allocationSize  = buffer_memory.size;
	allocate.memoryTypeIndex = chosen;
	RequireVulkanSuccess(device.allocateMemory(&allocate, nullptr, &m_preprocess_memory),
	                     "allocate mesh indirect preprocess memory");
	RequireVulkanSuccess(device.bindBufferMemory(m_preprocess, m_preprocess_memory, 0),
	                     "bind mesh indirect preprocess memory");
	vk::BufferDeviceAddressInfo address_info {};
	address_info.buffer   = m_preprocess;
	m_preprocess_address  = device.getBufferAddress(address_info);
	m_preprocess_capacity = capacity;
	return size;
}

uint32_t MeshIndirectDraws::Convert(const CommandRecorder& command, const Params& params) {
	Report();
	const auto entry = m_next;
	m_next           = (m_next + 1u) % Entries;

	// The record comes from earlier shader or transfer writes; every earlier generated draw
	// (which reads its entry and the preprocess buffer) must finish before this one is written.
	vk::MemoryBarrier record_ready {};
	record_ready.srcAccessMask = vk::AccessFlagBits::eShaderWrite |
	                             vk::AccessFlagBits::eTransferWrite |
	                             vk::AccessFlagBits::eIndirectCommandRead;
	record_ready.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eComputeShader, {}, 1, &record_ready, 0,
	                        nullptr, 0, nullptr);

	const auto& limits   = m_graphics.mesh_shader_properties;
	const auto  sequence = m_sequences.BufferDeviceAddress() + vk::DeviceSize {entry} * EntryBytes;
	const MeshPushConstants push {
	    static_cast<uint32_t>(params.record),
	    static_cast<uint32_t>(params.record >> 32u),
	    static_cast<uint32_t>(sequence),
	    static_cast<uint32_t>(sequence >> 32u),
	    static_cast<uint32_t>(params.index_address),
	    static_cast<uint32_t>(params.index_address >> 32u),
	    params.indexed ? 1u : 0u,
	    params.max_count,
	    params.element_size,
	    params.primitive_size,
	    std::max(params.primitive_step, 1u),
	    std::max(params.primitives_per_group, 1u),
	    limits.maxMeshWorkGroupCount[0],
	    limits.maxMeshWorkGroupCount[1],
	    limits.maxMeshWorkGroupTotalCount};
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
	                      &push);
	command.dispatch(1, 1, 1);

	// The draw reads the sequence as indirect commands, preprocessed as part of its execution.
	vk::MemoryBarrier2 sequence_ready {};
	sequence_ready.srcStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
	sequence_ready.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	sequence_ready.dstStageMask  = vk::PipelineStageFlagBits2::eDrawIndirect |
	                              vk::PipelineStageFlagBits2::eCommandPreprocessEXT;
	sequence_ready.dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead |
	                               vk::AccessFlagBits2::eCommandPreprocessReadEXT;
	vk::DependencyInfo dependency {};
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers    = &sequence_ready;
	command.pipelineBarrier2(dependency);
	m_draws++;
	return entry;
}

void MeshIndirectDraws::Execute(const CommandRecorder& command, vk::Pipeline pipeline,
                                vk::PipelineLayout pipeline_layout, vk::ShaderStageFlags stages,
                                uint32_t entry) {
	const auto layout          = LayoutFor(pipeline_layout);
	const auto preprocess_size = PreprocessSizeFor(pipeline, layout);
	const auto preprocess =
	    (m_preprocess_address + PreprocessAlignment - 1) & ~(PreprocessAlignment - 1);
	command.executeGeneratedCommandsEXT(
	    pipeline, stages, layout,
	    m_sequences.BufferDeviceAddress() + vk::DeviceSize {entry} * EntryBytes, SequenceStride,
	    preprocess, preprocess_size);
}

void MeshIndirectDraws::Report() {
	const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
	                     std::chrono::steady_clock::now().time_since_epoch())
	                     .count();
	if (now - m_last_report_us < 5000000) {
		return;
	}
	m_last_report_us = now;
	std::printf("Mesh indirect draws (5 s): %llu generated on the GPU\n",
	            static_cast<unsigned long long>(m_draws));
	std::fflush(stdout);
	m_draws = 0;
}

} // namespace Libs::Graphics
