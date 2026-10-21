#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

std::vector<uint32_t> EmitProgram(const IR::Program& program,
                                  ShaderStageInputInfo input_info);

// Optional image features of the device, set once by the device layer.
struct HostImageFeatures {
	// shaderResourceMinLod: the MinLod image operand, used for IMAGE_SAMPLE*_CL.
	bool min_lod = false;
};

void              SetHostImageFeatures(const HostImageFeatures& features);
HostImageFeatures GetHostImageFeatures();

// AMD hosts: V_MIN/V_MAX_F32 are plain GLSL.std.450 NMin/NMax, whose zeros the hardware orders
// like the guest (upstream 2864818). Set once by the device layer.
void SetHostNativeFloatMinMax(bool native);
bool GetHostNativeFloatMinMax();

// Right shift from the host's device-scope shader clock to the guest's 100 MHz S_MEMREALTIME:
// 3 for a 1 GHz clock (NVIDIA), 0 for a 100 MHz one (AMD). Set once by the device layer.
void     SetRealtimeClockShift(uint32_t shift);
uint32_t GetRealtimeClockShift();

// Why a mesh program cannot run in passes (ShaderMeshInputInfo::passes), or nullptr.
[[nodiscard]] const char* MeshPassesUnsupported(const IR::Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_ */
