// SPDX-License-Identifier: GPL-2.0-or-later
// Fragment barycentric emulation for GPUs without VK_AMD_shader_explicit_vertex_parameter and
// VK_KHR_fragment_shader_barycentric (GTX 10 / Pascal, issue #26).
//
// The Order's pixel shaders read the raw attribute of each triangle corner (V_INTERP_MOV with
// P0/P10/P20) and blend them with the barycentrics the PS4 hands them in VGPRs. Without either
// extension the fragment shader cannot see the corners, so the scene renders as silhouettes.
// A generated geometry shader (EmitBarycentricGS) gives the fragment shader what the KHR
// extension would: each per-vertex attribute as a flat array of the three corners, and the
// perspective and linear barycentrics as ordinary interpolated vec3 inputs written as the unit
// vectors at the corners. Both stages take their locations from ComputeBaryEmulation, so they
// always agree.

#pragma once

#include <array>
#include <bitset>
#include "shader_recompiler/info.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

/// User locations a fragment shader may consume on the GPUs that need this (Pascal: 128
/// components). The layout does not go above it.
constexpr u32 BaryEmulationMaxLocations = 32;

struct BaryEmulationLayout {
    bool needed = false; ///< the fragment shader reads per-vertex attributes or barycentrics
    bool ok = false;     ///< everything fits below BaryEmulationMaxLocations
    /// Location of each fragment input as the vertex stage writes it (and the fragment shader
    /// declares it when it is not per-vertex); -1 for defaulted inputs.
    std::array<s8, 32> source{};
    /// First of three consecutive locations holding the corners of a per-vertex input; -1 else.
    std::array<s8, 32> corners{};
    s8 clip = -1;    ///< emulated clip-distance input (location 0), passed through
    s8 smooth = -1;  ///< perspective barycentrics (vec3), -1 when unused
    s8 linear = -1;  ///< linear (no-perspective) barycentrics (vec3), -1 when unused
};

inline bool NeedsSmoothBarycentrics(const Info& info) {
    return info.loads.GetAny(IR::Attribute::BaryCoordSmooth) ||
           info.loads.GetAny(IR::Attribute::BaryCoordSmoothCentroid) ||
           info.loads.GetAny(IR::Attribute::BaryCoordSmoothSample) ||
           info.loads.GetAny(IR::Attribute::BaryCoordPullModel);
}

inline bool NeedsLinearBarycentrics(const Info& info) {
    return info.loads.GetAny(IR::Attribute::BaryCoordNoPersp) ||
           info.loads.GetAny(IR::Attribute::BaryCoordNoPerspSample);
}

inline BaryEmulationLayout ComputeBaryEmulation(const HwFragmentRuntimeInfo& fs, const Info& info) {
    BaryEmulationLayout layout;
    layout.source.fill(-1);
    layout.corners.fill(-1);
    const bool clip = fs.clip_distance_emulation;
    const u32 num_inputs = fs.num_inputs - (clip ? 1 : 0);
    std::bitset<BaryEmulationMaxLocations> used;
    if (clip) {
        layout.clip = 0;
        used.set(0);
    }
    bool per_vertex = false;
    for (u32 i = 0; i < num_inputs && i < fs.inputs.size(); ++i) {
        if (fs.inputs[i].IsDefault()) {
            continue;
        }
        const u32 location = fs.inputs[i].param_index + (clip ? 1 : 0);
        layout.source[i] = s8(location);
        if (info.fs_interpolation[i].primary == Qualifier::PerVertex) {
            per_vertex = true; // its own location is free: the corners move elsewhere
        } else if (location < BaryEmulationMaxLocations) {
            used.set(location);
        } else {
            return layout; // not representable; leave ok == false
        }
    }
    const bool smooth = NeedsSmoothBarycentrics(info);
    const bool linear = NeedsLinearBarycentrics(info);
    layout.needed = per_vertex || smooth || linear;
    if (!layout.needed) {
        return layout;
    }
    // Lowest free run of `count` locations.
    const auto take = [&](u32 count) -> s8 {
        for (u32 start = 0; start + count <= BaryEmulationMaxLocations; ++start) {
            bool free = true;
            for (u32 k = 0; k < count; ++k) {
                free &= !used.test(start + k);
            }
            if (free) {
                for (u32 k = 0; k < count; ++k) {
                    used.set(start + k);
                }
                return s8(start);
            }
        }
        return -1;
    };
    for (u32 i = 0; i < num_inputs && i < fs.inputs.size(); ++i) {
        if (layout.source[i] >= 0 && info.fs_interpolation[i].primary == Qualifier::PerVertex) {
            layout.corners[i] = take(3);
            if (layout.corners[i] < 0) {
                return layout;
            }
        }
    }
    if (smooth && (layout.smooth = take(1)) < 0) {
        return layout;
    }
    if (linear && (layout.linear = take(1)) < 0) {
        return layout;
    }
    layout.ok = true;
    return layout;
}

} // namespace Shader::Backend::SPIRV
