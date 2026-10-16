#ifndef KYTY_PIPELINE_TUNING_H_
#define KYTY_PIPELINE_TUNING_H_

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace Libs::Graphics::PipelineTuning {

inline uint32_t ReadUnsigned(std::string_view text, uint32_t fallback) {
	if (text.empty()) {
		return fallback;
	}
	uint32_t value = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
	return result.ec == std::errc {} && result.ptr == text.data() + text.size()
	           ? value
	           : fallback;
}

// Preserve the original automatic policy unless an explicit worker count is requested.
inline uint32_t WorkerCount(std::string_view configured, uint32_t hardware_threads) {
	const auto automatic = std::clamp(hardware_threads / 4u, 1u, 4u);
	const auto requested = ReadUnsigned(configured, 0);
	return requested == 0 ? automatic : std::clamp(requested, 1u, 8u);
}

// Zero allows new, safe-to-defer draws to be skipped immediately. Memory-writing draws
// retain PipelineCache's existing synchronous path. Bound malformed or excessive budgets.
inline uint32_t WaitMilliseconds(std::string_view configured) {
	return std::min(ReadUnsigned(configured, 20), 100u);
}

} // namespace Libs::Graphics::PipelineTuning

#endif
