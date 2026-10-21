#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_BINDLESSBINDINGS_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_BINDLESSBINDINGS_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Descriptor set 1 of a pipeline that samples bindless images (the host side is BindlessTable).
inline constexpr uint32_t BindlessDescriptorSet  = 1;
inline constexpr uint32_t BindlessImages2D       = 0;
inline constexpr uint32_t BindlessImages2DArray  = 1;
inline constexpr uint32_t BindlessImagesCube     = 2;
inline constexpr uint32_t BindlessImages3D       = 3;
// Storage views of the bindless images pixel shaders write (Wolverine's POPS blood shaders), in
// GENERAL layout. Their keys translate through heaps of their own.
inline constexpr uint32_t BindlessStorageImages2D      = 4;
inline constexpr uint32_t BindlessStorageImages2DArray = 5;
inline constexpr uint32_t BindlessTranslation    = 6;
inline constexpr uint32_t BindlessFeedback       = 7;
// Samplers mirrored from guest sampler heaps (16-byte S# records); slot 0 is the default sampler.
inline constexpr uint32_t BindlessSamplers       = 8;
// A translation entry for a key whose texture is not resident yet; the shader samples slot 0.
inline constexpr uint32_t BindlessPending        = 0xffffffffu;
// Feedback flags a shader stores per key it samples: the texture is pending, or resident and used.
inline constexpr uint32_t BindlessFlagPending    = 1;
inline constexpr uint32_t BindlessFlagUsed       = 2;
// The S# a sampler without a usable heap entry gets: wrap on every axis, trilinear, full LOD range.
inline constexpr std::array<uint32_t, 4> BindlessDefaultSampler {
    0x00000000u,                             // wrap on every axis
    0xfffu << 12u,                           // max_lod 255.9
    (1u << 20u) | (1u << 22u) | (2u << 26u), // bilinear mag/min, linear mip
    0x00000000u};

// The sampler used where a bindless sampler cannot be resolved (BindlessDefaultSampler, and the
// shader-side fallbacks in ResourceTracking). KYTY_DEFAULT_SAMPLER_CLAMP=1 (environment, read
// once) clamps to the edge texel on every axis instead of wrapping: a texture meant to be drawn
// once (Wolverine's moon) wrapped repeats across the whole surface.
inline const std::array<uint32_t, 4>& FallbackSampler() {
	static const std::array<uint32_t, 4> words = [] {
		auto        result = BindlessDefaultSampler;
		const char* clamp  = std::getenv("KYTY_DEFAULT_SAMPLER_CLAMP");
		if (clamp != nullptr && std::strcmp(clamp, "0") != 0) {
			result[0] = 2u | (2u << 3u) | (2u << 6u); // CLAMP_X/Y/Z = clamp to last texel
		}
		return result;
	}();
	return words;
}

// The hex shader hashes of a comma-separated list (an environment variable), e.g. "0x1a,2b".
inline std::unordered_set<uint64_t> ParseHashList(const char* list) {
	std::unordered_set<uint64_t> result;
	if (list == nullptr) {
		return result;
	}
	std::string text(list);
	size_t      start = 0;
	while (start < text.size()) {
		const auto end = text.find(',', start);
		const auto item =
		    text.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (!item.empty()) {
			result.insert(std::strtoull(item.c_str(), nullptr, 16));
		}
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	return result;
}

// Title workaround, separate from the generic fallback: KYTY_CLAMP_SAMPLER_SHADERS=hash,hash,...
// (hex, environment) gives the shaders listed a clamping fallback sampler while every other
// shader keeps the wrapping one. Wolverine's night sky draws its moon through an unresolved
// bindless sampler; wrap tiles it across the sky, but terrain needs wrap.
inline const std::array<uint32_t, 4>& FallbackSamplerFor(uint64_t shader_hash) {
	static const std::unordered_set<uint64_t> clamped =
	    ParseHashList(std::getenv("KYTY_CLAMP_SAMPLER_SHADERS"));
	static const std::array<uint32_t, 4> clamp = [] {
		auto result = BindlessDefaultSampler;
		result[0]   = 2u | (2u << 3u) | (2u << 6u);
		return result;
	}();
	return clamped.contains(shader_hash) ? clamp : FallbackSampler();
}

// Bindless sampler kinds (BindlessTable::Sampler* flags): a use keeps the depth-compare function,
// forces point filtering (integer and format-converted images) or uses integer border colours.
inline constexpr uint32_t BindlessSamplerKeepDepthCompare = 1u;
inline constexpr uint32_t BindlessSamplerPointFiltering   = 2u;
inline constexpr uint32_t BindlessSamplerIntegerBorder    = 4u;
inline constexpr uint32_t BindlessSamplerKinds            = 8u;

// KYTY_BINDLESS_SAMPLER_CLASS_DEFAULTS=1 (environment, research, read once): a key outside its
// heap (or a heap the host could not mirror) selects a default sampler of its own kind, in slots
// 0..7 (slot = kind flags), instead of slot 0's linear float sampler for every kind. Linear
// filtering of an integer view, or a float border on one, is undefined in Vulkan.
// Set by the host's bindless table when its sampler array holds the 8 per-kind default slots;
// a device with a smaller array keeps slot 0 for every kind (the shader indexes the array).
inline std::atomic<bool>& BindlessSamplerKindSlotsAvailable() {
	static std::atomic<bool> available {false};
	return available;
}

// Set by the host's bindless table when the device takes update-after-bind storage image arrays
// indexed non-uniformly. Without them a pixel shader's bindless image writes are dropped.
inline std::atomic<bool>& BindlessStorageAvailable() {
	static std::atomic<bool> available {false};
	return available;
}

// KYTY_SHADER_BINDLESS_STORAGE_WRITES (environment, read once): unset = 2D and 2D-array bindless
// image writes go through the storage arrays (their images stay GENERAL); 0 = a pixel shader's
// bindless writes are dropped (3ddfd07); 1 = the old storage-typed alias of the sampled arrays
// (AMD hosts corrupt textures and lose the device: Wolverine's POPS blood shaders).
inline int BindlessStorageWritesMode() {
	static const int mode = [] {
		const char* value = std::getenv("KYTY_SHADER_BINDLESS_STORAGE_WRITES");
		if (value == nullptr || value[0] == '\0') {
			return 2;
		}
		return value[0] == '0' ? 0 : value[0] == '1' ? 1 : 2;
	}();
	return mode;
}

inline bool BindlessStorageArrays() {
	return BindlessStorageWritesMode() == 2 &&
	       BindlessStorageAvailable().load(std::memory_order_acquire);
}

inline bool BindlessSamplerClassDefaults() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_BINDLESS_SAMPLER_CLASS_DEFAULTS");
		return value != nullptr && value[0] == '1';
	}();
	return enabled && BindlessSamplerKindSlotsAvailable().load(std::memory_order_acquire);
}

inline bool BindlessSamplerClassDefaultsRequested() {
	static const bool requested = [] {
		const char* value = std::getenv("KYTY_BINDLESS_SAMPLER_CLASS_DEFAULTS");
		return value != nullptr && value[0] == '1';
	}();
	return requested;
}

inline uint32_t BindlessSamplerKind(bool depth_compare, bool point_filtering, bool integer_border) {
	return (depth_compare ? BindlessSamplerKeepDepthCompare : 0u) |
	       (point_filtering ? BindlessSamplerPointFiltering : 0u) |
	       (integer_border ? BindlessSamplerIntegerBorder : 0u);
}

// Research: the tail of the feedback buffer holds loop-watchdog trip reports. Word
// WatchdogReportBase counts reports; report n is WatchdogReportWords words starting at
// WatchdogReportBase + WatchdogReportWords * (n + 1). No heap region reaches this far.
inline constexpr uint32_t WatchdogReportBase     = 0xf0000u;
// KYTY_TRACE_BINDLESS=<shader hash>: that shader's bindless reads store key, raw translation, heap
// base and entry count in these words of the feedback buffer (research; below the reports).
inline constexpr uint32_t BindlessTraceBase      = WatchdogReportBase - 8u;
inline constexpr uint32_t WatchdogReportWords    = 64;
inline constexpr uint32_t WatchdogReportSlots    = 1023;
inline constexpr uint32_t WatchdogReportPhis     = 24; // header phi values per lane half

// KYTY_SHADER_LOOP_WATCHDOG=1 (environment, read once; the name puts it in the program cache
// signature): every program gets the loop watchdog, not only those that read memory through
// device addresses. A guest loop that never ends on the host (a wave-size or memory-ordering
// difference) then leaves after its iteration budget and reports itself instead of hanging the
// GPU until the driver resets it.
inline bool LoopWatchdogAllPrograms() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_SHADER_LOOP_WATCHDOG");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_BINDLESSBINDINGS_H_
