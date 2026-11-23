// SPDX-License-Identifier: GPL-2.0-or-later
// Frame generation (#47): loads the order1886_fg provider once and drives the FSR 3.1.6
// interpolated-frame lifecycle over the reconstructed scene the temporal resolve already
// produces. Kept separate from the upscaler provider table because the FG backend cannot link
// beside the FSR 3.1.5 upscaler (shared private SDK symbols), so it is its own dlopen'd module
// with a distinct ABI (shim/order1886_framegen.h).
//
// It is a pure GPU-work recorder: it does not present, pace, or own swapchain images. The
// caller (the resolve pass on the GPU thread) supplies the render-resolution scene colour,
// depth and motion and receives the interpolated frame into a caller-owned image.
#pragma once
#include <string>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "order1886_framegen.h"

namespace Vulkan {

class Instance;

// One resolved scene at render resolution for a generated frame.
struct FramegenScene {
    vk::Image color{}, depth{}, motion{};
    VkFormat color_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    u32 width = 0, height = 0;
    float frame_ms = 33.3f;
    bool depth_reversed = false;
};

class FrameGeneration {
public:
    // Loads the provider module (idempotent). On failure, available() is false and reason() says
    // why; the caller leaves the game's own presentation untouched.
    void Load();
    bool available() const { return api_ != nullptr; }
    bool has_context() const { return context_ != nullptr; }
    const std::string& reason() const { return reason_; }

    // One-time context for a max render size and a fixed display size. Recreate() after a swapchain
    // resize. Colour format matches the scene input (RGBA16F for the resolved scene).
    bool Create(const Instance& instance, u32 max_render_w, u32 max_render_h, u32 display_w,
                u32 display_h, bool hdr_scrgb,
                VkFormat color_format = VK_FORMAT_R16G16B16A16_SFLOAT);
    void Recreate(const Instance& instance, u32 max_render_w, u32 max_render_h, u32 display_w,
                  u32 display_h, bool hdr_scrgb);
    void Destroy();
    bool active() const { return context_ != nullptr; }
    std::string name() const { return api_ && api_->name ? api_->name : "order1886-fg"; }
    std::string version() const { return api_ && api_->version ? api_->version : "?"; }

    // Record one guest frame: latch the scene, then emit the interpolated frame into `out`
    // (display size, GENERAL, distinct from the scene images). `reset` clears SDK history (scene
    // change / first frame). `gpu_frames_completed` is the present-fence watermark for retirement.
    // false when the provider refused the frame; the caller must stop feeding and reset the
    // history before trying again (a rejected prepare leaves the SDK state wedged).
    bool PresentFrame(vk::CommandBuffer cmdbuf, const FramegenScene& scene, vk::Image out,
                      u32 out_w, u32 out_h, u64 frame_id, bool reset,
                      u64 gpu_frames_completed);
    // Release SDK history older than the host-proven present watermark.
    void Retire(u64 completed_frame_id);

    u64 resident_bytes() const { return resident_bytes_; }

private:
    const OrderFramegenProviderApi* api_ = nullptr;
    bool loaded_ = false;
    void* context_ = nullptr;
    std::string reason_{"not loaded"};
    u64 resident_bytes_ = 0;
};

} // namespace Vulkan
