#include "graphics/host_gpu/renderer/indirectDispatch.h"

#include "common/assert.h"
#include "common/liveSwitches.h"
#include "gpu_tiler_shaders/dispatch_indirect_groups_spv.h"
#include "gpu_tiler_shaders/draw_indirect_guard_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include "common/logging/log.h"

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

} // namespace Libs::Graphics
