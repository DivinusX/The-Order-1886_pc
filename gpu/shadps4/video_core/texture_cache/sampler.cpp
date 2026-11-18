// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include "ps4_settings.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/sampler.h"

namespace VideoCore {

Sampler::Sampler(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                 const std::optional<std::array<float, 4>>& custom_border_color,
                 const bool is_depth, float extra_lod_bias, bool scene_material) {
    using namespace Vulkan;
    const bool game_anisotropy = AmdGpu::IsAnisoFilter(sampler.xy_mag_filter) ||
                                 AmdGpu::IsAnisoFilter(sampler.xy_min_filter);
    // the menu's anisotropic filtering, on filtered and mipmapped textures (not on
    // point-sampled lookups, unmipmapped render targets or depth compares).
    // The Quality menu's anisotropy (when set) replaces the launcher's and also caps the game's
    // own anisotropic samplers, so Low really costs less.
    const auto& settings = Ps4Settings::Get();
    const int quality_aniso = settings.quality_anisotropy;
    const int forced = quality_aniso > 0 ? quality_aniso : settings.startup_anisotropy;
    const bool force_anisotropy = forced > 1 && !is_depth &&
                                  sampler.xy_min_filter != AmdGpu::Filter::Point &&
                                  sampler.mip_filter != AmdGpu::MipFilter::None;
    const bool anisotropy_enable =
        instance.IsAnisotropicFilteringSupported() && (game_anisotropy || force_anisotropy);
    float game_aniso = game_anisotropy ? sampler.MaxAniso() : 1.0f;
    if (quality_aniso > 0) game_aniso = std::min(game_aniso, float(quality_aniso));
    const float requested = std::max(game_aniso, force_anisotropy ? float(forced) : 1.0f);
    const float max_anisotropy =
        anisotropy_enable ? std::clamp(requested, 1.0f, instance.MaxSamplerAnisotropy()) : 1.0f;
    // Texture quality: mipmapped colour textures of scene materials (pixel shaders of
    // depth-tested draws) start this many levels down (less memory bandwidth, softer
    // surfaces). The UI (font atlas, button icons), full-screen passes, compute, render
    // targets (one level), point lookups and depth compares keep the game's own range: a
    // lower level of the font atlas turns letters into neighbouring glyphs.
    float min_lod = sampler.MinLod(), max_lod = sampler.MaxLod();
    const int skip = scene_material ? 2 - std::clamp(settings.texture_quality.load(), 0, 2) : 0;
    if (skip > 0 && !is_depth && sampler.mip_filter != AmdGpu::MipFilter::None && max_lod > 0.0f) {
        min_lod = std::max(min_lod, float(skip));
        max_lod = std::max(max_lod, min_lod);
    }
    auto border_color = LiverpoolToVK::BorderColor(sampler.border_color_type);
    if (border_color == vk::BorderColor::eFloatCustomEXT &&
        !instance.IsCustomBorderColorSupported()) {
        LOG_WARNING(Render_Vulkan, "Custom border color is not supported, falling back to black");
        border_color = vk::BorderColor::eFloatOpaqueBlack;
    }

    const auto custom_color = [&]() -> std::optional<vk::SamplerCustomBorderColorCreateInfoEXT> {
        if (border_color == vk::BorderColor::eFloatCustomEXT && custom_border_color) {
            const vk::SamplerCustomBorderColorCreateInfoEXT ret{
                .customBorderColor =
                    vk::ClearColorValue{
                        .float32 = *custom_border_color,
                    },
                .format = vk::Format::eR32G32B32A32Sfloat,
            };
            return ret;
        } else {
            return std::nullopt;
        }
    }();

    const vk::SamplerCreateInfo sampler_ci = {
        .pNext = custom_color ? &*custom_color : nullptr,
        .magFilter = LiverpoolToVK::Filter(sampler.xy_mag_filter),
        .minFilter = LiverpoolToVK::Filter(sampler.xy_min_filter),
        .mipmapMode = LiverpoolToVK::MipFilter(sampler.mip_filter),
        .addressModeU = LiverpoolToVK::ClampMode(sampler.clamp_x),
        .addressModeV = LiverpoolToVK::ClampMode(sampler.clamp_y),
        .addressModeW = LiverpoolToVK::ClampMode(sampler.clamp_z),
        .mipLodBias = std::clamp(sampler.LodBias() + extra_lod_bias, -instance.MaxSamplerLodBias(),
                                 instance.MaxSamplerLodBias()),
        .anisotropyEnable = anisotropy_enable,
        .maxAnisotropy = max_anisotropy,
        // GCN compares per instruction; a plain read of a compare sampler is undefined in Vulkan.
        .compareEnable = is_depth,
        .compareOp = LiverpoolToVK::DepthCompare(sampler.depth_compare_func),
        .minLod = min_lod,
        .maxLod = max_lod,
        .borderColor = border_color,
        .unnormalizedCoordinates = false, // Handled in shader due to Vulkan limitations.
    };
    auto [sampler_result, smplr] = instance.GetDevice().createSamplerUnique(sampler_ci);
    ASSERT_MSG(sampler_result == vk::Result::eSuccess, "Failed to create sampler: {}",
               vk::to_string(sampler_result));
    handle = std::move(smplr);
}

Sampler::~Sampler() = default;

} // namespace VideoCore
