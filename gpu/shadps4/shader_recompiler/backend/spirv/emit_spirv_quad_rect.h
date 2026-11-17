// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include "common/types.h"

namespace Shader {
struct HwFragmentRuntimeInfo;
struct Info;
}

namespace Shader::Backend::SPIRV {

enum class AuxShaderType : u32 {
    RectListTCS,
    QuadListTCS,
    PassthroughTES,
};

[[nodiscard]] std::vector<u32> EmitAuxilaryTessShader(AuxShaderType type,
                                                      const HwFragmentRuntimeInfo& fs_info);

/// Geometry shader for fragment barycentric emulation (bary_emulation.h): passes the vertex
/// shader's outputs through, writes each per-vertex fragment input's three corners to their
/// flat locations and the barycentrics as unit vectors at the corners. Empty when the vertex
/// shader writes Layer or ViewportIndex (a geometry shader cannot read them back).
/// `after_aux_tes`: the stage before is the rect/quad-list PassthroughTES (its gl_PerVertex
/// block and its attribute locations, which carry no clip-distance input).
[[nodiscard]] std::vector<u32> EmitBarycentricGeometryShader(const HwFragmentRuntimeInfo& fs_rt,
                                                             const Info& fs_info,
                                                             const Info& vs_info,
                                                             bool after_aux_tes = false);

} // namespace Shader::Backend::SPIRV
