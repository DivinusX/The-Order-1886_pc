// SPDX-License-Identifier: GPL-2.0-or-later
// See vk_order_resolve.h. Ported from IgorAugustoHH/The-Order-1886-PC-Port (vk_order_dlss.cpp),
// onto the provider ABI (shim/order1886_upscale.h) instead of a DLSS-only bridge.

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>

#include "ps4_settings.h"
#include "video_core/host_shaders/order_clean_resolve_comp.h"
#include "video_core/host_shaders/order_resolve_prep_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_order_resolve.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_upscale_providers.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

struct Push {
    u32 width, height;
    float velocity_scale;
    float log2_exposure;
    u32 samples;
    u32 exposure_from_texture;
    u32 invert_depth; ///< flip reversed depth for the fixed-permutation FSR 4 v07 model
};

/// One image the prep pass writes and the provider reads (or, for output, writes).
struct Target {
    std::optional<VideoCore::UniqueImage> image;
    vk::UniqueImageView view;
    vk::Format format;
    vk::ImageUsageFlags usage;
};

struct Resolve {
    const UpscaleProvider* provider = nullptr;
    void* context = nullptr;
    std::atomic<bool> failed{false};
    bool depth_inverted = false;  ///< seen on the scene's depth-writing draws
    bool context_inverted = false; ///< what the provider context was created with
    VideoCore::ImageId scene_color{}, scene_depth{};

    vk::Device device;
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    std::array<Target, 4> targets; // color, motion, depth, output
    u32 width = 0, height = 0;

    bool reset = true;
    std::chrono::steady_clock::time_point last_time{};
    std::atomic<u64> frames{0};
    u32 skips_logged = 0;

    void CreatePipeline(const Instance& instance) {
        device = instance.GetDevice();
        std::array<vk::DescriptorSetLayoutBinding, 7> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = i >= 4 ? vk::DescriptorType::eStorageImage
                                                    : vk::DescriptorType::eSampledImage,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        set_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange range{vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push)};
        layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto module = CompileSPV(ORDER_RESOLVE_PREP_COMP, device);
        pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *layout,
                }));
        device.destroyShaderModule(module);
        const auto sampled = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
        targets[0].format = vk::Format::eR16G16B16A16Sfloat;
        targets[1].format = vk::Format::eR16G16Sfloat;
        targets[2].format = vk::Format::eR32Sfloat;
        targets[3].format = vk::Format::eR16G16B16A16Sfloat;
        for (auto& target : targets) {
            target.usage = sampled;
        }
        // DLSS clears its output with a transfer, so the output is also a transfer destination.
        targets[3].usage |= vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
    }

    /// Images and provider context for a w x h scene. The caller has waited for the GPU.
    bool Resize(const Instance& instance, u32 w, u32 h) {
        if (context) {
            provider->api.destroy(context);
            context = nullptr;
        }
        for (auto& target : targets) {
            target.view.reset();
            target.image.emplace(device, instance.GetAllocator());
            target.image->Create(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = target.format,
                .extent = {w, h, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .usage = target.usage,
                .initialLayout = vk::ImageLayout::eUndefined,
            });
            target.view = Check(device.createImageViewUnique({
                .image = *target.image,
                .viewType = vk::ImageViewType::e2D,
                .format = target.format,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            }));
        }
        width = w;
        height = h;
        context_inverted = depth_inverted;
        reset = true;
        const auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
        const OrderUpscaleDevice upscale_device{
            .instance = static_cast<VkInstance>(instance.GetInstance()),
            .physical_device = static_cast<VkPhysicalDevice>(instance.GetPhysicalDevice()),
            .device = static_cast<VkDevice>(device),
            .get_instance_proc_addr = dispatch.vkGetInstanceProcAddr,
            .get_device_proc_addr = dispatch.vkGetDeviceProcAddr,
            .graphics_queue = static_cast<VkQueue>(instance.GetGraphicsQueue()),
            .graphics_queue_family = instance.GetGraphicsQueueFamilyIndex(),
            .shader_storage_image_write_without_format_enabled =
                instance.IsStorageImageWriteWithoutFormatEnabled(),
        };
        // ponytail: no camera planes captured yet, so depth is treated as infinite-far.
        const OrderUpscaleConfig config{
            .max_render_width = w,
            .max_render_height = h,
            .output_width = w,
            .output_height = h,
            .flags = ORDER_UPSCALE_HDR | ORDER_UPSCALE_DEPTH_INFINITE |
                     (depth_inverted ? ORDER_UPSCALE_DEPTH_INVERTED : 0u),
        };
        if (!provider) {
            std::printf("Upscaler: frame-generation feed at %ux%u (no temporal provider, depth "
                        "inverted %d)\n",
                        w, h, depth_inverted ? 1 : 0);
            std::fflush(stdout);
            return true;
        }
        char error[256]{};
        if (provider->api.create(&upscale_device, &config, &context, error, sizeof(error)) !=
            ORDER_UPSCALE_OK) {
            std::printf("Upscaler: %s cannot start (%s); the game's temporal resolve stays\n",
                        provider->key, error);
            std::fflush(stdout);
            failed = true;
            return false;
        }
        std::printf("Upscaler: %s %s replaces The Order's temporal resolve at %ux%u "
                    "(native, depth inverted %d)\n",
                    provider->name.c_str(), provider->version.c_str(), w, h,
                    depth_inverted ? 1 : 0);
        std::fflush(stdout);
        return true;
    }

    void Skip(const char* why) {
        if (skips_logged < 8) {
            ++skips_logged;
            std::printf("Upscaler: game resolve kept (%s)\n", why);
            std::fflush(stdout);
        }
    }
};

Resolve& State() {
    static Resolve resolve;
    return resolve;
}

/// The provider for the launcher's upscaler, when it loads on this device.
const UpscaleProvider* Selected(const Instance& instance) {
    static const UpscaleProvider* selected = [&]() -> const UpscaleProvider* {
        const int requested = Ps4Settings::Get().requested_upscaler;
        if (requested == Ps4Settings::UpscalerOff) {
            return nullptr;
        }
        const std::string_view key = Ps4Settings::UpscalerName(requested);
        for (const auto& provider : DetectUpscaleProviders(instance)) {
            if (provider.available && key == provider.key) {
                return &provider;
            }
        }
        return nullptr;
    }();
    return selected;
}

bool Wanted() {
    // #47: frame generation runs on the scene this pass decodes even when no temporal upscaler
    // was requested; the provider path below then records the prep pass only.
    static const bool wanted = Ps4Settings::Get().requested_upscaler != Ps4Settings::UpscalerOff ||
                               Ps4Settings::Get().frame_generation.load();
    return wanted;
}

float Float(std::span<const u32> flatbuf, size_t index) {
    float value;
    std::memcpy(&value, &flatbuf[index], sizeof(value));
    return value;
}

/// Push constants for order_clean_resolve.comp, whose fields are the prep pass's, without
/// the velocity and depth terms.
struct CleanPush {
    u32 width, height;
    u32 samples;
    u32 exposure_from_texture;
    float log2_exposure;
};

/// The clean-replace pipeline and its own resolve destination. Compiled once, from the first
/// resolve it replaces. The result lands in the game's output through a copy, exactly like the
/// provider path's. There is no history to track here, so dispatches whose images do not match
/// the frame map fall back per call.
struct CleanResolve {
    vk::Device device{};
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    std::optional<VideoCore::UniqueImage> image; ///< own resolve destination
    vk::UniqueImageView view;
    u32 width = 0, height = 0;
    u64 frames = 0;
    u32 skips_logged = 0;
    bool ready = false;

    bool Create(const Instance& instance) {
        device = instance.GetDevice();
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = i == 2 ? vk::DescriptorType::eStorageImage
                                                    : vk::DescriptorType::eSampledImage,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        set_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange range{vk::ShaderStageFlagBits::eCompute, 0, sizeof(CleanPush)};
        layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto module = CompileSPV(ORDER_CLEAN_RESOLVE_COMP, device);
        pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *layout,
                }));
        device.destroyShaderModule(module);
        std::printf("Temporal AA: the engine's sample resolve replaces the game's temporal one\n");
        std::fflush(stdout);
        return true;
    }

    /// The engine destination for a w x h scene; the caller has waited for the GPU.
    void Resize(const Instance& instance, u32 w, u32 h) {
        view.reset();
        image.emplace(device, instance.GetAllocator());
        image->Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        view = Check(device.createImageViewUnique({
            .image = *image,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        width = w;
        height = h;
    }

    void Skip(const char* why) {
        if (skips_logged < 8) {
            ++skips_logged;
            std::printf("Temporal AA: game resolve kept (%s)\n", why);
            std::fflush(stdout);
        }
    }
};

std::mutex scene_mutex;
SceneFrame scene;
std::atomic<u64> scene_revision{0};

bool ConsumeSceneFrame(u64& revision, SceneFrame& out) {
    const u64 now = scene_revision.load(std::memory_order_acquire);
    if (now == revision) {
        return false;
    }
    std::lock_guard lock{scene_mutex};
    out = scene;
    revision = now;
    return true;
}

CleanResolve& CleanState() {
    static CleanResolve state;
    return state;
}

} // namespace

void OnSceneTargets(VideoCore::TextureCache& texture_cache, VideoCore::ImageId color,
                    VideoCore::ImageId depth, const AmdGpu::DepthControl& depth_control) {
    if (!Wanted() || !color || !depth) {
        return;
    }
    const auto& image = texture_cache.GetImage(color);
    if (image.info.num_samples > 1 && image.info.pixel_format == vk::Format::eR16G16B16A16Sfloat) {
        auto& resolve = State();
        resolve.scene_color = color;
        resolve.scene_depth = depth;
        // The scene's depth-writing draws tell near from far: Greater tests mean reversed depth.
        using Func = AmdGpu::CompareFunc;
        const auto func = depth_control.depth_func;
        if (depth_control.depth_enable && depth_control.depth_write_enable &&
            func != Func::Always && func != Func::Equal && func != Func::NotEqual &&
            func != Func::Never) {
            resolve.depth_inverted = func == Func::Greater || func == Func::GreaterEqual;
        }
    }
}

void ReplaceTemporalResolve(Rasterizer& rasterizer, std::span<const VideoCore::ImageId> images,
                            std::span<const u32> flatbuf) {
    if (!Wanted()) {
        return;
    }
    auto& runtime = rasterizer.GetRuntime();
    auto& texture_cache = rasterizer.GetTextureCache();
    auto& scheduler = runtime.GetScheduler();
    const auto& instance = runtime.GetInstance();
    auto& resolve = State();
    if (resolve.failed) {
        return;
    }
    if (!resolve.provider) {
        resolve.provider = Selected(instance);
        if (!resolve.provider && !Ps4Settings::Get().frame_generation.load()) {
            resolve.failed = true; // ReportUpscalerFallback already logged why
            return;
        }
        resolve.CreatePipeline(instance);
    }

    VideoCore::ImageId exposure_id{}, velocity_id{}, color_id{}, output_id{};
    for (const auto id : images) {
        const auto& image = texture_cache.GetImage(id);
        const auto format = image.info.pixel_format;
        if (format == vk::Format::eR32Sfloat && image.info.size.width == 1) {
            exposure_id = id;
        } else if (format == vk::Format::eA2B10G10R10UnormPack32) {
            velocity_id = id;
        } else if (format == vk::Format::eR16G16B16A16Sfloat && image.info.num_samples > 1) {
            color_id = id;
        } else if (format == vk::Format::eR16G16B16A16Sfloat &&
                   image.backing->state.layout == vk::ImageLayout::eGeneral) {
            output_id = id; // bound for storage; the history is bound for sampling
        }
    }
    if (!exposure_id || !velocity_id || !color_id || !output_id || flatbuf.size() < 66) {
        return resolve.Skip("resolve resources not recognised");
    }
    if (color_id != resolve.scene_color || !resolve.scene_depth) {
        return resolve.Skip("no depth target drawn with the scene color");
    }
    auto& color = texture_cache.GetImage(color_id);
    auto& velocity = texture_cache.GetImage(velocity_id);
    auto& exposure = texture_cache.GetImage(exposure_id);
    auto& depth = texture_cache.GetImage(resolve.scene_depth);
    auto& output = texture_cache.GetImage(output_id);
    const u32 w = output.info.size.width, h = output.info.size.height;
    const auto same = [&](const VideoCore::Image& image) {
        return image.info.size.width == w && image.info.size.height == h;
    };
    if (!same(color) || !same(velocity) || !same(depth) || !depth.info.props.is_depth ||
        depth.info.num_samples != color.info.num_samples ||
        !(output.usage_flags & vk::ImageUsageFlagBits::eTransferDst)) {
        return resolve.Skip("resource sizes, samples or usage differ");
    }
    if (w != resolve.width || h != resolve.height ||
        resolve.depth_inverted != resolve.context_inverted) {
        scheduler.Finish(); // the previous images and context may still be in use
        if (!resolve.Resize(instance, w, h)) {
            return;
        }
    }

    const auto now = std::chrono::steady_clock::now();
    // The FSR 4 v07 bundle is compiled with DEPTH_INVERTED=0: the model wants standard
    // (near=0) depth, so the prep pass flips The Order's reversed depth when FS4 is the
    // provider. FSR 3.1 and DLAA keep their own depth-convention flags unchanged.
    const bool fsr4 = resolve.provider && std::string_view(resolve.provider->key) == "fsr4";
    float frame_ms = std::chrono::duration<float, std::milli>(now - resolve.last_time).count();
    if (frame_ms <= 0.0f || frame_ms > 250.0f) {
        resolve.reset = true; // paused, loading or a skipped resolve: the history is stale
        frame_ms = 33.3f;
    }
    resolve.last_time = now;

    const auto view = [](VideoCore::Image& image) {
        VideoCore::ImageViewInfo info{};
        info.format = image.info.pixel_format;
        return *image.FindView(info).image_view;
    };
    const u64 completed_presentations = Presenter::CompletedPresentations();
    const vk::ImageView color_in = view(color), velocity_in = view(velocity),
                        exposure_in = view(exposure), depth_in = view(depth);
    const auto read = [&](VideoCore::Image& image, vk::ImageLayout layout) {
        runtime.Transit(&image, layout, vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
    };
    read(color, vk::ImageLayout::eShaderReadOnlyOptimal);
    read(velocity, vk::ImageLayout::eShaderReadOnlyOptimal);
    read(exposure, vk::ImageLayout::eShaderReadOnlyOptimal);
    read(depth, vk::ImageLayout::eDepthStencilReadOnlyOptimal);
    // The copy target; the game's own result stays in it until the provider succeeds.
    runtime.Transit(&output, vk::ImageLayout::eTransferDstOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
    runtime.FlushBarriers();

    const Push push{
        .width = w,
        .height = h,
        .velocity_scale = Float(flatbuf, 63),
        .log2_exposure = Float(flatbuf, 60),
        .samples = color.info.num_samples,
        .exposure_from_texture = Float(flatbuf, 61) != 0.0f ? 1u : 0u,
        .invert_depth = (fsr4 && resolve.depth_inverted) ? 1u : 0u,
    };
    std::array<vk::Image, 4> own;
    std::array<vk::ImageView, 4> own_views;
    for (u32 i = 0; i < own.size(); ++i) {
        own[i] = *resolve.targets[i].image;
        own_views[i] = *resolve.targets[i].view;
    }
    const vk::Image game_output = output.GetImage();
    const bool reset = resolve.reset;
    resolve.reset = false;
    scheduler.EndRendering();
    scheduler.Record([=, &resolve](vk::CommandBuffer cmdbuf) {
        const auto barrier = [&](vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                                 vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                 vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
            return vk::ImageMemoryBarrier2{
                .srcStageMask = src_stage,
                .srcAccessMask = src_access,
                .dstStageMask = dst_stage,
                .dstAccessMask = dst_access,
                .oldLayout = from,
                .newLayout = to,
                .image = image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
        };
        const auto compute = vk::PipelineStageFlagBits2::eComputeShader;
        const auto all = vk::PipelineStageFlagBits2::eAllCommands;
        const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
        // Every image is rewritten each frame: the provider keeps its own history.
        std::array<vk::ImageMemoryBarrier2, 4> start;
        for (u32 i = 0; i < own.size(); ++i) {
            start[i] = barrier(own[i], vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, all,
                               rw, compute, rw);
        }
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 4, .pImageMemoryBarriers = start.data()});

        const std::array<vk::DescriptorImageInfo, 7> infos{{
            {{}, color_in, vk::ImageLayout::eShaderReadOnlyOptimal},
            {{}, velocity_in, vk::ImageLayout::eShaderReadOnlyOptimal},
            {{}, depth_in, vk::ImageLayout::eDepthStencilReadOnlyOptimal},
            {{}, exposure_in, vk::ImageLayout::eShaderReadOnlyOptimal},
            {{}, own_views[0], vk::ImageLayout::eGeneral},
            {{}, own_views[1], vk::ImageLayout::eGeneral},
            {{}, own_views[2], vk::ImageLayout::eGeneral},
        }};
        std::array<vk::WriteDescriptorSet, 7> writes{};
        for (u32 i = 0; i < writes.size(); ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = i >= 4 ? vk::DescriptorType::eStorageImage
                                                  : vk::DescriptorType::eSampledImage,
                         .pImageInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *resolve.pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *resolve.layout, 0, writes);
        cmdbuf.pushConstants(*resolve.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                             &push);
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);

        std::array<vk::ImageMemoryBarrier2, 3> inputs;
        for (u32 i = 0; i < inputs.size(); ++i) {
            inputs[i] = barrier(own[i], vk::ImageLayout::eGeneral,
                                vk::ImageLayout::eShaderReadOnlyOptimal, compute,
                                vk::AccessFlagBits2::eShaderWrite, compute,
                                vk::AccessFlagBits2::eShaderRead);
        }
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 3, .pImageMemoryBarriers = inputs.data()});

        const auto image = [&](u32 i, vk::ImageLayout layout) {
            const auto& target = resolve.targets[i];
            return OrderUpscaleImage{static_cast<VkImage>(own[i]), static_cast<VkImageView>(own_views[i]),
                                     static_cast<VkFormat>(target.format),
                                     w, h, static_cast<VkImageUsageFlags>(target.usage),
                                     static_cast<VkImageLayout>(layout)};
        };
        if (!resolve.provider) {
            return; // prep-only (frame generation feed): the game's own result stands.
        }
        const auto read_only = vk::ImageLayout::eShaderReadOnlyOptimal;
        const OrderUpscaleFrame frame{
            .command_buffer = static_cast<VkCommandBuffer>(cmdbuf),
            .color = image(0, read_only),
            .depth = image(2, read_only),
            .motion = image(1, read_only),
            .output = image(3, vk::ImageLayout::eGeneral),
            .render_width = w,
            .render_height = h,
            .jitter_x = 0.0f, // the game renders without projection jitter
            .jitter_y = 0.0f,
            .motion_scale_x = 1.0f, // motion is already in pixels
            .motion_scale_y = 1.0f,
            .frame_time_ms = frame_ms,
            .pre_exposure = 1.0f,
            // ponytail: placeholders until the game's camera constants are located.
            .camera_near = 0.1f,
            .camera_far = 1000.0f, // finite: DLSS checks far > near even with infinite depth
            .vertical_fov = 0.7f,
            .reset_history = reset ? 1u : 0u,
            .gpu_frames_completed = static_cast<uint64_t>(completed_presentations),
        };
        char error[256]{};
        if (resolve.provider->api.dispatch(resolve.context, &frame, error, sizeof(error)) !=
            ORDER_UPSCALE_OK) {
            // The game's result is still in its output; it stays from now on.
            std::printf("Upscaler: %s failed (%s); the game's temporal resolve takes over\n",
                        resolve.provider->key, error);
            std::fflush(stdout);
            resolve.failed = true;
            return;
        }
        const auto copy = barrier(own[3], vk::ImageLayout::eGeneral,
                                  vk::ImageLayout::eTransferSrcOptimal, all, rw,
                                  vk::PipelineStageFlagBits2::eTransfer,
                                  vk::AccessFlagBits2::eTransferRead);
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &copy});
        const vk::ImageCopy region{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .extent = {w, h, 1},
        };
        cmdbuf.copyImage(own[3], vk::ImageLayout::eTransferSrcOptimal, game_output,
                         vk::ImageLayout::eTransferDstOptimal, region);
        const u64 count = ++resolve.frames;
        if (count == 1 || count % 1800 == 0) {
            std::printf("Upscaler: %s reconstructed %llu frames (%ux%u, %u samples, velocity "
                        "scale %g)\n",
                        resolve.provider->key, static_cast<unsigned long long>(count), w, h,
                        push.samples, push.velocity_scale);
            std::fflush(stdout);
        }
    });
    // #47: publish the decoded scene (targets are in SHADER_READ_ONLY_OPTIMAL here and are
    // only recreated after scheduler.Finish, which orders every consumer on this scheduler).
    if (Ps4Settings::Get().frame_generation.load()) {
        std::lock_guard lock{scene_mutex};
        scene.color = own[0];
        scene.motion = own[1];
        scene.depth = own[2];
        scene.color_view = own_views[0];
        scene.motion_view = own_views[1];
        scene.depth_view = own_views[2];
        scene.width = w;
        scene.height = h;
        scene.frame_ms = frame_ms;
        scene.depth_reversed = resolve.depth_inverted;
        scene_revision.fetch_add(1, std::memory_order_release);
    }
}

bool CleanResolveWanted() {
    // A running provider overwrites the game's result anyway; the toggle only matters when
    // nothing else replaces the resolve.
    return !Wanted() && !Ps4Settings::Get().temporal_aa.load(std::memory_order_relaxed);
}

bool RecordCleanResolve(Rasterizer& rasterizer, std::span<const VideoCore::ImageId> images,
                        std::span<const u32> flatbuf) {
    auto& runtime = rasterizer.GetRuntime();
    auto& texture_cache = rasterizer.GetTextureCache();
    auto& scheduler = runtime.GetScheduler();
    const auto& instance = runtime.GetInstance();
    auto& state = CleanState();
    if (!state.ready) {
        if (!state.Create(instance)) {
            return false;
        }
        state.ready = true;
    }

    VideoCore::ImageId exposure_id{}, color_id{}, output_id{};
    for (const auto id : images) {
        const auto& image = texture_cache.GetImage(id);
        const auto format = image.info.pixel_format;
        if (format == vk::Format::eR32Sfloat && image.info.size.width == 1) {
            exposure_id = id;
        } else if (format == vk::Format::eR16G16B16A16Sfloat && image.info.num_samples > 1) {
            color_id = id;
        } else if (format == vk::Format::eR16G16B16A16Sfloat &&
                   image.backing->state.layout == vk::ImageLayout::eGeneral) {
            output_id = id; // bound for storage; the history is bound for sampling
        }
    }
    if (!exposure_id || !color_id || !output_id || flatbuf.size() < 66) {
        state.Skip("resolve images not recognised");
        return false;
    }
    auto& color = texture_cache.GetImage(color_id);
    auto& exposure = texture_cache.GetImage(exposure_id);
    auto& output = texture_cache.GetImage(output_id);
    if (!(output.usage_flags & vk::ImageUsageFlagBits::eTransferDst)) {
        state.Skip("resolve output cannot receive a copy");
        return false;
    }
    const u32 w = output.info.size.width, h = output.info.size.height;
    if (color.info.size.width != w || color.info.size.height != h) {
        state.Skip("scene and resolve output sizes differ");
        return false;
    }

    // The game's dispatch is about to run with exactly these bindings - the scene and the
    // exposure sampled, the output as storage - so the caller's resource binding has already
    // moved the images to the layouts this needs; these transits only flush the barriers that
    // make the scene draws' writes visible. The output is prepared as the copy's destination,
    // like the provider path.
    runtime.Transit(&color, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&exposure, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&output, vk::ImageLayout::eTransferDstOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
    runtime.FlushBarriers();

    if (w != state.width || h != state.height) {
        scheduler.Finish(); // the previous destination may still be in use
        state.Resize(instance, w, h);
    }

    const auto view = [](VideoCore::Image& image) {
        VideoCore::ImageViewInfo info{};
        info.format = image.info.pixel_format;
        return *image.FindView(info).image_view;
    };
    const vk::ImageView color_in = view(color), exposure_in = view(exposure);
    const CleanPush push{
        .width = w,
        .height = h,
        .samples = color.info.num_samples,
        .exposure_from_texture = Float(flatbuf, 61) != 0.0f ? 1u : 0u,
        .log2_exposure = Float(flatbuf, 60),
    };
    scheduler.Record([&state, color_in, exposure_in, push, w, h,
                      game_output = output.GetImage()](vk::CommandBuffer cmdbuf) {
        const auto compute = vk::PipelineStageFlagBits2::eComputeShader;
        const auto transfer = vk::PipelineStageFlagBits2::eTransfer;
        const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
        const vk::Image destination = *state.image;
        const auto own = [&](vk::ImageLayout from, vk::ImageLayout to,
                             vk::PipelineStageFlagBits2 src_stage, vk::AccessFlags2 src_access,
                             vk::PipelineStageFlagBits2 dst_stage, vk::AccessFlags2 dst_access) {
            const vk::ImageMemoryBarrier2 barrier{
                .srcStageMask = src_stage,
                .srcAccessMask = src_access,
                .dstStageMask = dst_stage,
                .dstAccessMask = dst_access,
                .oldLayout = from,
                .newLayout = to,
                .image = destination,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        };
        own(vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
            rw, compute, rw);

        const std::array<vk::DescriptorImageInfo, 3> infos{{
            {{}, color_in, vk::ImageLayout::eShaderReadOnlyOptimal},
            {{}, exposure_in, vk::ImageLayout::eShaderReadOnlyOptimal},
            {{}, *state.view, vk::ImageLayout::eGeneral},
        }};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (u32 i = 0; i < writes.size(); ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = i == 2 ? vk::DescriptorType::eStorageImage
                                                  : vk::DescriptorType::eSampledImage,
                         .pImageInfo = &infos[i]};
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *state.pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *state.layout, 0, writes);
        cmdbuf.pushConstants(*state.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                             &push);
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);

        own(vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal, compute,
            vk::AccessFlagBits2::eShaderWrite, transfer, vk::AccessFlagBits2::eTransferRead);
        const vk::ImageCopy region{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .extent = {w, h, 1},
        };
        cmdbuf.copyImage(destination, vk::ImageLayout::eTransferSrcOptimal, game_output,
                         vk::ImageLayout::eTransferDstOptimal, region);
        const u64 count = ++state.frames;
        if (count == 1 || count % 1800 == 0) {
            std::printf("Temporal AA: resolved without history for %llu frames (%ux%u, %u samples)\n",
                        static_cast<unsigned long long>(count), w, h, push.samples);
            std::fflush(stdout);
        }
    });
    return true;
}

} // namespace Vulkan
