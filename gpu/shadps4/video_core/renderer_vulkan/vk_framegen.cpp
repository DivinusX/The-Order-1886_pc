// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_framegen.h"
#include "video_core/renderer_vulkan/vk_instance.h"

#include <cstdlib>
#include <filesystem>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace Vulkan {
namespace {

OrderFramegenGetApi ResolveGetApi() {
    // The provider sits beside the runtime (bin/) or in bin/gpu/, like the upscaler providers.
#ifdef _WIN32
    HMODULE module = LoadLibraryExW(L"order1886_fg.dll", nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
        return nullptr;
    }
    return reinterpret_cast<OrderFramegenGetApi>(
        reinterpret_cast<void*>(GetProcAddress(module, "order1886_fg_get_api")));
#else
    std::vector<std::filesystem::path> candidates;
    std::vector<char> buf(4096);
    const ssize_t length = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (length > 0) {
        buf[static_cast<size_t>(length)] = '\0';
        const auto dir = std::filesystem::path(buf.data()).parent_path();
        candidates.push_back(dir / "order1886_fg.so");
        candidates.push_back(dir / "gpu" / "order1886_fg.so");
    }
    candidates.push_back(std::filesystem::path("order1886_fg.so"));
    for (const auto& candidate : candidates) {
        if (!std::filesystem::exists(candidate)) {
            continue;
        }
        void* module = ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!module) {
            continue;
        }
        return reinterpret_cast<OrderFramegenGetApi>(::dlsym(module, "order1886_fg_get_api"));
    }
    return nullptr;
#endif
}

OrderFramegenImage Img(vk::Image image, VkFormat format, u32 w, u32 h, VkImageLayout layout) {
    return OrderFramegenImage{static_cast<VkImage>(image), format, w, h, layout,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT};
}

} // namespace

void FrameGeneration::Load() {
    if (loaded_) {
        return;
    }
    loaded_ = true;
    const auto get_api = ResolveGetApi();
    if (!get_api) {
        reason_ = "order1886_fg module not found beside the runtime";
        return;
    }
    const OrderFramegenProviderApi* api = nullptr;
    if (get_api(ORDER1886_FRAMEGEN_ABI, &api) != ORDER_FRAMEGEN_OK || !api ||
        api->abi_version != ORDER1886_FRAMEGEN_ABI) {
        reason_ = "frame generation provider does not implement ABI " +
                  std::to_string(ORDER1886_FRAMEGEN_ABI);
        return;
    }
    api_ = api;
    reason_ = "loaded";
}

bool FrameGeneration::Create(const Instance& instance, u32 max_render_w, u32 max_render_h,
                             u32 display_w, u32 display_h, bool hdr_scrgb) {
    if (!available() || context_) {
        return context_ != nullptr;
    }
    const auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
    OrderFramegenDevice device{
        .instance = static_cast<VkInstance>(instance.GetInstance()),
        .physical_device = static_cast<VkPhysicalDevice>(instance.GetPhysicalDevice()),
        .device = static_cast<VkDevice>(instance.GetDevice()),
        .get_instance_proc_addr = dispatch.vkGetInstanceProcAddr,
        .get_device_proc_addr = dispatch.vkGetDeviceProcAddr,
        .graphics_queue = static_cast<VkQueue>(instance.GetGraphicsQueue()),
        .graphics_queue_family = instance.GetGraphicsQueueFamilyIndex(),
    };
    OrderFramegenConfig config{
        .max_render_width = max_render_w,
        .max_render_height = max_render_h,
        .display_width = display_w,
        .display_height = display_h,
        .color_format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .flags = hdr_scrgb ? static_cast<u32>(ORDER_FRAMEGEN_HDR_SCRGB) : 0u,
    };
    void* context = nullptr;
    if (api_->create(&device, &config, &context) != ORDER_FRAMEGEN_OK || !context) {
        reason_ = "frame generation context create failed";
        return false;
    }
    context_ = context;
    OrderFramegenMemoryUsage usage{};
    api_->memory_usage(context_, &usage);
    resident_bytes_ = usage.resident_bytes;
    reason_ = "active";
    return true;
}

void FrameGeneration::Recreate(const Instance& instance, u32 max_render_w, u32 max_render_h,
                               u32 display_w, u32 display_h, bool hdr_scrgb) {
    Destroy();
    Create(instance, max_render_w, max_render_h, display_w, display_h, hdr_scrgb);
}

void FrameGeneration::Destroy() {
    if (context_ && api_) {
        api_->destroy(context_);
    }
    context_ = nullptr;
    resident_bytes_ = 0;
}

bool FrameGeneration::PresentFrame(vk::CommandBuffer cmdbuf, const FramegenScene& scene,
                                   vk::Image out, u32 out_w, u32 out_h, u64 frame_id, bool reset,
                                   u64 gpu_frames_completed) {
    if (!context_) {
        return false;
    }
    const auto read_only = static_cast<VkImageLayout>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    OrderFramegenPrepare prepare{
        .command_buffer = static_cast<VkCommandBuffer>(cmdbuf),
        .color = Img(scene.color, scene.color_format, scene.width, scene.height, read_only),
        .depth = Img(scene.depth, VK_FORMAT_R32_SFLOAT, scene.width, scene.height, read_only),
        .motion = Img(scene.motion, VK_FORMAT_R16G16_SFLOAT, scene.width, scene.height, read_only),
        .render_width = scene.width,
        .render_height = scene.height,
        .frame_time_ms = scene.frame_ms,
        .min_luminance = 1.0f,
        .max_luminance = 1000.0f,
        // Placeholders until the game camera is located (#47): DLSS ships with the same values
        // and the SDK accepts them; they only shape disocclusion fill, not whether it runs.
        .camera_near = 0.1f,
        .camera_far = 1000.0f,
        .view_space_to_meters = 1.0f,
        .vertical_fov_radians = 0.7f,
        .frame_id = frame_id,
        .reset_history = reset ? 1u : 0u,
        .flags = scene.depth_reversed ? static_cast<u32>(ORDER_FRAMEGEN_DEPTH_INVERTED) : 0u,
    };
    if (api_->record_prepare(context_, &prepare) != ORDER_FRAMEGEN_OK) {
        return false; // the game's own presentation stands this frame
    }
    OrderFramegenDispatch dispatch{
        .command_buffer = static_cast<VkCommandBuffer>(cmdbuf),
        .color = Img(scene.color, scene.color_format, scene.width, scene.height, read_only),
        .output = Img(out, VK_FORMAT_R16G16B16A16_SFLOAT, out_w, out_h,
                      static_cast<VkImageLayout>(VK_IMAGE_LAYOUT_GENERAL)),
        .distortion = {}, // image==NULL: the game applies no post displacement between flips
        .display_width = out_w,
        .display_height = out_h,
        .interp_width = out_w,
        .interp_height = out_h,
        .frame_time_ms = scene.frame_ms,
        .min_luminance = 1.0f,
        .max_luminance = 1000.0f,
        .camera_near = 0.1f,
        .camera_far = 1000.0f,
        .view_space_to_meters = 1.0f,
        .vertical_fov_radians = 0.7f,
        .frame_id = frame_id,
        .reset_history = reset ? 1u : 0u,
        .gpu_frames_completed = gpu_frames_completed,
        .flags = scene.depth_reversed ? static_cast<u32>(ORDER_FRAMEGEN_DEPTH_INVERTED) : 0u,
    };
    return api_->record_dispatch(context_, &dispatch) == ORDER_FRAMEGEN_OK;
}

void FrameGeneration::Retire(u64 completed_frame_id) {
    if (context_) {
        api_->retire(context_, completed_frame_id);
    }
}

} // namespace Vulkan
