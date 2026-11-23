// SPDX-License-Identifier: GPL-2.0-or-later
// A temporal upscaler provider (vk_upscale_providers.h) in place of The Order's own temporal
// resolve (#7, docs/order1886-frame-map.md). The resolve already receives what a provider needs:
// the MSAA HDR scene, the velocity target and the exposure; depth is the target drawn with that
// scene color. The provider's result is copied into the resolve's output, which is also the
// game's history, so everything downstream (motion blur, DoF, bloom, tonemap) is unchanged.
// Design ported from IgorAugustoHH/The-Order-1886-PC-Port (vk_order_dlss.cpp).
#pragma once

#include <span>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Rasterizer;

/// The game's temporal resolve: customresolve_cs_msaa4x_temporal and its 2x variant.
[[nodiscard]] constexpr bool IsOrderTemporalResolve(u64 cs_hash) noexcept {
    return cs_hash == 0x92311aca || cs_hash == 0xe57f0150;
}

/// Every draw: remembers the depth target an MSAA RGBA16F scene color is drawn with, and whether
/// that depth is reversed (from the draw's depth test).
void OnSceneTargets(VideoCore::TextureCache& texture_cache, VideoCore::ImageId color,
                    VideoCore::ImageId depth, const AmdGpu::DepthControl& depth_control);

/// After the game's resolve dispatch is recorded: when the launcher asked for an upscaler whose
/// provider loads, records the provider and copies its result over the resolve's output. When it
/// cannot (inputs not recognised, provider error), the game's own result stays.
void ReplaceTemporalResolve(Rasterizer& rasterizer, std::span<const VideoCore::ImageId> images,
                            std::span<const u32> flatbuf);

/// F1 > Quality "Temporal AA: Off" (issue #29) with no upscaler provider requested: the game's
/// own samples can be resolved by the engine without the history blend, in place of the game's
/// dispatch. Read per dispatch, so the menu switches it live.
[[nodiscard]] bool CleanResolveWanted();

/// The resolved scene published for #47 frame generation. The prep pass leaves
/// colour/motion/depth (targets[0..2]) in SHADER_READ_ONLY_OPTIMAL after its dispatch; the
/// frame generator reads exactly those views. revision advances once per resolved frame, so
/// the present thread consumes each scene exactly once and ignores frames the game resolved
/// itself (menus, loads). Images are resolve-owned and only recreated after scheduler.Finish,
/// which covers every consumer recorded on the draw scheduler.
struct SceneFrame {
    vk::Image color{}, motion{}, depth{};
    vk::ImageView color_view{}, motion_view{}, depth_view{};
    u32 width = 0, height = 0;
    float frame_ms = 33.3f;
    bool depth_reversed = false;
};

/// Copy the newest resolved scene into out; false when nothing resolved since revision.
bool ConsumeSceneFrame(u64& revision, SceneFrame& out);

/// Records that engine-side resolve over the game's resolve output, where the game's dispatch
/// would run. Returns true when recorded - the caller then skips the game's dispatch; false
/// when the game's resolve must run instead (images not recognised, device cannot compile).
bool RecordCleanResolve(Rasterizer& rasterizer, std::span<const VideoCore::ImageId> images,
                        std::span<const u32> flatbuf);

} // namespace Vulkan
