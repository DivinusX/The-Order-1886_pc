// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "ps4_overlay.h"
#include "ps4_settings.h"
#include "ps4_graphics.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_order_resolve.h"
#include "video_core/renderer_vulkan/vk_upscale_providers.h"
#include "video_core/renderer_vulkan/vk_warmup_inbox.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <vk_mem_alloc.h>
#ifdef MemoryBarrier
#undef MemoryBarrier // winnt.h macro (through fmt), clashes with vk::MemoryBarrier
#endif

namespace Vulkan {

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

// Cinematic band fill (Ps4Settings::ultrawide, issue #45): the game draws a 2.4:1 picture
// band, centred in a 16:9 frame (Original framing; the UI patch keeps menus, subtitles and
// prompts inside the band). Showing only that band adapts to ANY window shape - 20:9
// Sunshine/Moonlight streams, 21:9 monitors, 32:9 - instead of boxing a 16:9 frame inside
// them; FitImage adds whatever thin bars the exact ratio still needs.
constexpr float UltrawideBandRatio = 12.0f / 5.0f;

static bool UltrawideActive(float frame_ratio) {
    const auto& s = Ps4Settings::Get();
    // Only frames taller than the band have bars to crop (16:9; not an odd window shape).
    return s.ultrawide.load(std::memory_order_relaxed) &&
           s.ultrawide_available.load(std::memory_order_relaxed) &&
           frame_ratio < UltrawideBandRatio * 0.95f;
}

[[nodiscard]] static vk::ImageBlit MakeImageBlitBand(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                              s32 swapchain_height) {
    const s32 band_height = std::min<s32>(frame_height, static_cast<s32>(frame_width / UltrawideBandRatio));
    const s32 band_top = (frame_height - band_height) / 2;
    const auto dst = FitImage(frame_width, band_height, swapchain_width, swapchain_height);
    auto blit = MakeImageBlit(frame_width, frame_height, dst.extent.width, dst.extent.height,
                              dst.offset.x, dst.offset.y);
    blit.srcOffsets[0].y = band_top;
    blit.srcOffsets[1].y = band_top + band_height;
    return blit;
}

// F12 screenshots of the game's frame (without the overlay), saved as PNG to the user
// folder's screenshots/ once the GPU is done; the conversion follows upstream shadPS4.
namespace {

float PqToNits(float encoded) {
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;
    const float vp = std::pow(std::clamp(encoded, 0.0f, 1.0f), 1.0f / m2);
    return 10000.0f * std::pow(std::max(vp - c1, 0.0f) / std::max(c2 - c3 * vp, 1e-6f), 1.0f / m1);
}

u8 HdrToSrgb8(float nits) {
    const float x = std::max(nits, 0.0f) / 100.0f; // 100-nit SDR white
    const float linear = std::clamp(2.0f * x / (1.0f + x), 0.0f, 1.0f);
    const float srgb = linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return u8(std::clamp(srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
}

bool ScreenshotToRgba8(const u8* src, u32 width, u32 height, vk::Format format, bool hdr,
                       std::vector<u8>& rgba) {
    const size_t pixels = size_t(width) * height;
    rgba.resize(pixels * 4);
    switch (format) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        for (size_t i = 0; i < pixels; ++i) {
            std::memcpy(&rgba[i * 4], &src[i * 4], 3);
            rgba[i * 4 + 3] = 255;
        }
        return true;
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        for (size_t i = 0; i < pixels; ++i) {
            rgba[i * 4 + 0] = src[i * 4 + 2];
            rgba[i * 4 + 1] = src[i * 4 + 1];
            rgba[i * 4 + 2] = src[i * 4 + 0];
            rgba[i * 4 + 3] = 255;
        }
        return true;
    case vk::Format::eA2B10G10R10UnormPack32: {
        static const auto nits = [] {
            std::array<float, 1024> lut{};
            for (size_t i = 0; i < lut.size(); ++i) {
                lut[i] = PqToNits(float(i) / 1023.0f);
            }
            return lut;
        }();
        for (size_t i = 0; i < pixels; ++i) {
            u32 packed;
            std::memcpy(&packed, &src[i * 4], 4);
            const u32 r = packed & 0x3FF, g = (packed >> 10) & 0x3FF, b = (packed >> 20) & 0x3FF;
            if (hdr) {
                // Rec.2020 PQ to SDR Rec.709.
                const float r2020 = nits[r], g2020 = nits[g], b2020 = nits[b];
                rgba[i * 4 + 0] = HdrToSrgb8(1.6605f * r2020 - 0.5876f * g2020 - 0.0728f * b2020);
                rgba[i * 4 + 1] = HdrToSrgb8(-0.1246f * r2020 + 1.1329f * g2020 - 0.0083f * b2020);
                rgba[i * 4 + 2] = HdrToSrgb8(-0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020);
            } else {
                rgba[i * 4 + 0] = u8((r * 255 + 511) / 1023);
                rgba[i * 4 + 1] = u8((g * 255 + 511) / 1023);
                rgba[i * 4 + 2] = u8((b * 255 + 511) / 1023);
            }
            rgba[i * 4 + 3] = 255;
        }
        return true;
    }
    default:
        return false;
    }
}

std::filesystem::path ScreenshotPath() {
    // Issue #42: a player looks for screenshots in the game's own folder. On a standard
    // install the user data lives under <game>/user/order1886 (this path may be nested
    // further, into the GPU cache dir — see where reports land), so captures move to
    // <game>/Screenshots. Harness and diagnostic layouts have no user/order1886 ancestor
    // and keep the old location, so test sessions stay isolated.
    auto dir = Common::FS::GetUserPath(Common::FS::PathType::ScreenshotsDir);
    static const std::filesystem::path game_root = [&dir] {
        for (auto p = dir; p.has_relative_path() && p != p.parent_path(); p = p.parent_path()) {
            if (p.filename() == "order1886" && p.parent_path().filename() == "user") {
                return p.parent_path().parent_path();
            }
        }
        return std::filesystem::path{};
    }();
    if (!game_root.empty()) {
        dir = game_root / "Screenshots";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string serial{Common::ElfInfo::Instance().GameSerial()};
    if (serial.empty()) {
        serial = "ps4";
    }
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now_time);
#else
    localtime_r(&now_time, &local);
#endif
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    char name[96];
    std::snprintf(name, sizeof(name), "%s_%04d%02d%02d_%02d%02d%02d_%03d.png", serial.c_str(),
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                  local.tm_sec, int(ms));
    return dir / name;
}

/// Records the copy of `image` (layout TransferSrcOptimal) and saves it once the GPU is done.
void CaptureScreenshot(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                       const Frame& frame) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(frame.width) * frame.height * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation, &info) !=
        VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "Screenshot: no memory for the readback");
        return;
    }
    cmdbuf.copyImageToBuffer(frame.image, vk::ImageLayout::eTransferSrcOptimal, buffer,
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageExtent = {frame.width, frame.height, 1}});
    const vk::MemoryBarrier to_host{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eHostRead,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                           vk::DependencyFlags{}, to_host, {}, {});
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      width = frame.width, height = frame.height, format = frame.format,
                                      hdr = frame.is_hdr, path = ScreenshotPath()] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        std::vector<u8> rgba;
        const bool converted =
            ScreenshotToRgba8(static_cast<const u8*>(info.pMappedData), width, height, format, hdr, rgba);
        vmaDestroyBuffer(allocator, buffer, allocation);
        if (!converted) {
            LOG_ERROR(Render_Vulkan, "Screenshot: unsupported format {}", vk::to_string(format));
            Ps4Overlay::ShowNotice("Screenshot failed: unsupported image format");
            return;
        }
        size_t png_size = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), int(width), int(height), 4,
                                                               &png_size, 6, MZ_FALSE);
        bool saved = false;
        if (png) {
#ifdef _WIN32
            FILE* f = _wfopen(path.wstring().c_str(), L"wb"); // the user folder may not be ASCII
#else
            FILE* f = std::fopen(path.c_str(), "wb");
#endif
            if (f) {
                saved = std::fwrite(png, 1, png_size, f) == png_size;
                saved = std::fclose(f) == 0 && saved;
            }
            mz_free(png);
        }
        if (saved) {
            std::printf("Screenshot: %s (%ux%u)\n", Common::FS::PathToUTF8String(path).c_str(), width, height);
            Ps4Overlay::ShowNotice("Screenshot saved: " + Common::FS::PathToUTF8String(path.filename()));
        } else {
            LOG_ERROR(Render_Vulkan, "Screenshot: cannot write {}", Common::FS::PathToUTF8String(path));
            Ps4Overlay::ShowNotice("Screenshot failed: cannot write the file");
        }
    });
}

} // namespace

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // #47: frame generation decided at startup (the pool and context size around it).
    framegen_wanted = Ps4Settings::Get().frame_generation.load();
    if (framegen_wanted) {
        framegen.Load();
        framegen_ready = framegen.available();
        if (!framegen_ready) {
            std::printf("FrameGen: %s; presenting game frames only\n", framegen.reason().c_str());
            std::fflush(stdout);
        } else {
            std::printf("FrameGen: provider %s %s armed\n", framegen.name().c_str(),
                        framegen.version().c_str());
            std::fflush(stdout);
        }
    }

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    // The FSR 4 v07 shaders need float16, int8, packed dot products and linear compute
    // derivatives; ask the device once, before any provider loads (#12).
    Ps4Settings::ConfigureUpscalerSupport(instance.IsFsr4Int8Supported(),
                                          instance.IsFsr411Supported(), nullptr);
    {
        const auto providers = DetectUpscaleProviders(instance);
        ReportUpscalerFallback(Ps4Settings::Get().requested_upscaler, providers);
        // What the in-game menu may offer for the next launch: a provider that loads now, or one
        // whose library loaded but whose instance or device extensions are only enabled once it
        // is the selected upscaler (DLSS needs instance extensions, FSR 4 device extensions).
        // Without the instance case DLSS vanished from the menu once deselected (#33).
        unsigned mask = 0;
        for (const auto& provider : providers) {
            const bool next_launch = provider.loaded &&
                                     (provider.reason.find("(offered") != std::string::npos ||
                                      provider.reason.find("only when it is the selected upscaler") !=
                                          std::string::npos);
            if (!provider.available && !next_launch) continue;
            for (int u = 1; u < Ps4Settings::UpscalerCount; ++u) {
                if (std::string_view(provider.key) == Ps4Settings::UpscalerName(u)) mask |= 1u << u;
            }
        }
        Ps4Settings::Get().upscaler_available_mask = mask;
    }
    pp_pass.Create(device, swapchain.GetSurfaceFormat().format);
    Ps4Overlay::Init(instance, swapchain.GetSurfaceFormat().format, num_images);

    const bool binary_cache = EmulatorSettingsImpl::Flag("ORDER1886_PIPELINE_BINARIES", true) &&
                              EmulatorSettings.IsPipelineCacheEnabled();
    const bool capture_only = EmulatorSettingsImpl::Flag("ORDER1886_PIPELINE_BINARY_ISOLATED", false) &&
                              EmulatorSettingsImpl::Flag("ORDER1886_PIPELINE_CAPTURE_ONLY", false);
    if (UseOnDemandPipelines(binary_cache, instance.GetPipelineBinaryGlobalKey().keySize != 0,
                            capture_only)) {
        rasterizer->GetPipelineCache().StartOnDemand();
        return;
    }

    // by default the cached pipelines load while the game runs (a corner note shows the
    // progress). Otherwise rebuilding them takes up to minutes before the first game frame:
    // show what is happening instead of a black window.
    const char* background_warmup = std::getenv("ORDER1886_BACKGROUND_WARMUP");
    if ((!background_warmup || *background_warmup != '0') &&
        rasterizer->GetPipelineCache().StartBackgroundWarmUp()) {
        return;
    }
    Ps4Overlay::SetLoading(true, 0, 0);
    PresentLoadingFrame();
    auto last = std::chrono::steady_clock::now();
    rasterizer->GetPipelineCache().WarmUp([&](u32 done, u32 total) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::milliseconds(250)) {
            return;
        }
        last = now;
        Ps4Overlay::SetLoading(true, done, total);
        PresentLoadingFrame();
    });
    Ps4Overlay::SetLoading(false, 0, 0);

}

Presenter::~Presenter() {

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        if (frame.fg_hdr) {
            device.destroyImageView(frame.fg_hdr_view);
            vmaDestroyImage(instance.GetAllocator(), frame.fg_hdr, frame.fg_hdr_alloc);
            device.destroyImageView(frame.fg_ldr_view);
            vmaDestroyImage(instance.GetAllocator(), frame.fg_ldr, frame.fg_ldr_alloc);
            device.destroyImageView(frame.fg_pre_view);
            vmaDestroyImage(instance.GetAllocator(), frame.fg_pre, frame.fg_pre_alloc);
            device.destroyImageView(frame.fg_display_view);
            vmaDestroyImage(instance.GetAllocator(), frame.fg_display, frame.fg_display_alloc);
        }
        device.destroyFence(frame.present_done);
    }
    framegen.Destroy();
}

static void DumpRaw(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                    vk::Image image, vk::ImageLayout layout, u32 w, u32 h, const char* name,
                    int index);

// #47: HUD regions re-blitted from the real frame onto the generated one (normalized
// display coords; reticle, subtitle/QTE band, weapon HUD, objective, top prompts).

void Presenter::FramegenRecord(Frame* frame, vk::CommandBuffer cmdbuf, const SceneFrame& scene) {
    static const bool step_trace = std::getenv("ORDER1886_FG_DUMP") != nullptr;
    (void)step_trace;
#define FGSTEP(fmt, ...) if (step_trace) std::fprintf(stderr, "FGstep " fmt "\n", ##__VA_ARGS__)
    const vk::Device device = instance.GetDevice();
    const u32 w = scene.width, h = scene.height;
    if ((fg_hdr_w != w || fg_hdr_h != h) && frame->fg_hdr) {
        device.destroyImageView(frame->fg_hdr_view);
        vmaDestroyImage(instance.GetAllocator(), frame->fg_hdr, frame->fg_hdr_alloc);
        device.destroyImageView(frame->fg_ldr_view);
        vmaDestroyImage(instance.GetAllocator(), frame->fg_ldr, frame->fg_ldr_alloc);
        device.destroyImageView(frame->fg_pre_view);
        vmaDestroyImage(instance.GetAllocator(), frame->fg_pre, frame->fg_pre_alloc);
        device.destroyImageView(frame->fg_display_view);
        vmaDestroyImage(instance.GetAllocator(), frame->fg_display, frame->fg_display_alloc);
        frame->fg_hdr = VK_NULL_HANDLE;
        frame->fg_display = VK_NULL_HANDLE;
        framegen_ctx_ready = false;
    }
    FGSTEP("staging");
    if (!frame->fg_hdr || fg_hdr_w != w || fg_hdr_h != h) {
        const auto make = [&](vk::Format format, vk::ImageUsageFlags usage, vk::Image& image,
                              VmaAllocation& alloc, vk::ImageView& view, u32 iw, u32 ih) {
            const VkImageCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                       .imageType = VK_IMAGE_TYPE_2D,
                                       .format = static_cast<VkFormat>(format),
                                       .extent = {iw, ih, 1},
                                       .mipLevels = 1,
                                       .arrayLayers = 1,
                                       .samples = VK_SAMPLE_COUNT_1_BIT,
                                       .tiling = VK_IMAGE_TILING_OPTIMAL,
                                       .usage = static_cast<VkImageUsageFlags>(usage),
                                       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
            const VmaAllocationCreateInfo ai{.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
                                             .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
            VkImage raw{};
            VmaAllocation raw_mem{};
            if (vmaCreateImage(instance.GetAllocator(), &ci, &ai, &raw, &raw_mem, nullptr) !=
                VK_SUCCESS) {
                return false;
            }
            image = vk::Image{raw};
            alloc = raw_mem;
            view = Check<"framegen view">(device.createImageView(
                {.image = image,
                 .viewType = vk::ImageViewType::e2D,
                 .format = format,
                 .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}));
            return true;
        };
        // FSR 3 frame generation insists on float scene colour, so the LDR band travels in
        // an RGBA16F container (the crop blit converts; values stay in 0..1).
        const auto hdr_f = vk::Format::eR16G16B16A16Sfloat;
        if (!make(hdr_f,
                  vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled |
                      vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
                  frame->fg_ldr, frame->fg_ldr_alloc, frame->fg_ldr_view, w, h) ||
            !make(hdr_f,
                  vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                      vk::ImageUsageFlagBits::eTransferSrc,
                  frame->fg_hdr, frame->fg_hdr_alloc, frame->fg_hdr_view, w, h) ||
            !make(hdr_f,
                  vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                  frame->fg_pre, frame->fg_pre_alloc, frame->fg_pre_view, frame->width,
                  frame->height) ||
            !make(frame->format,
                  vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc |
                      vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                  frame->fg_display, frame->fg_display_alloc, frame->fg_display_view,
                  frame->width, frame->height)) {
            framegen_ready = false;
            std::printf("FrameGen: staging allocation failed; feature off\n");
            std::fflush(stdout);
            return;
        }
        fg_hdr_w = w;
        fg_hdr_h = h;
    }
    if (!framegen_ctx_ready) {
        FGSTEP("ctxcreate");
        if (!framegen.Create(instance, w, h, w, h, false)) {
            if (++framegen_fail_streak > 8) {
                framegen_ready = false;
                std::printf("FrameGen: provider create failed; feature off\n");
                std::fflush(stdout);
            }
            return;
        }
        framegen_ctx_ready = true;
        fg_seq = 0;
        std::printf("FrameGen: generating from %ux%u feed into %ux%u output\n", w, h,
                    frame->width, frame->height);
        std::fflush(stdout);
    }

    // Feed the SDK the game's own composited LDR band (post tonemap, post letterbox):
    // generated frames then match the real ones exactly except where motion was
    // interpolated - no tonemap mismatch flicker. Depth/motion stay in scene space.
    {
        const auto fit = FitImage(static_cast<s32>(w), static_cast<s32>(h),
                                  static_cast<s32>(frame->width),
                                  static_cast<s32>(frame->height));
        const vk::ImageMemoryBarrier2 to_src{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eColorAttachmentWrite},
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eTransferRead},
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .image = frame->image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        const vk::ImageMemoryBarrier2 ldr_fresh{
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eTransferWrite},
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = frame->fg_ldr,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        const std::array in{to_src, ldr_fresh};
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 2,
                                                   .pImageMemoryBarriers = in.data()});
        const vk::ImageBlit crop{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffsets = {{vk::Offset3D{fit.offset.x, fit.offset.y, 0},
                            vk::Offset3D{fit.offset.x + static_cast<s32>(fit.extent.width),
                                         fit.offset.y + static_cast<s32>(fit.extent.height), 1}}},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets = {{vk::Offset3D{0, 0, 0}, vk::Offset3D{static_cast<s32>(w), static_cast<s32>(h), 1}}},
        };
        cmdbuf.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, frame->fg_ldr,
                         vk::ImageLayout::eTransferDstOptimal, crop, vk::Filter::eNearest);
        const vk::ImageMemoryBarrier2 ldr_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eTransferWrite},
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eShaderRead},
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = frame->fg_ldr,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        const vk::ImageMemoryBarrier2 back{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eTransferRead},
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = frame->image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        const std::array out{ldr_read, back};
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 2,
                                                   .pImageMemoryBarriers = out.data()});
    }
    FramegenScene fscene{
        .color = frame->fg_ldr,
        .depth = scene.depth,
        .motion = scene.motion,
        .color_format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .width = scene.width,
        .height = scene.height,
        .frame_ms = scene.frame_ms,
        .depth_reversed = scene.depth_reversed,
    };
    const bool first = fg_seq == 0;
    ++fg_seq;
    FGSTEP("feed seq=%llu", (unsigned long long)fg_seq);
    FGSTEP("predispatch");
    if (!framegen.PresentFrame(cmdbuf, fscene, frame->fg_hdr, w, h, fg_seq, first,
                               Presenter::CompletedPresentations())) {
        if (++framegen_fail_streak > 8) {
            framegen_ready = false;
            std::printf("FrameGen: provider failures; feature off\n");
            std::fflush(stdout);
        } else {
            fg_seq = 0; // force a history reset on the next attempt
        }
        return;
    }
    framegen_fail_streak = 0;

    // Band the scene (e.g. the 2.4:1 cinematic window) into frame space with black bars,
    // exactly like the game composes its own letterbox, then post-process the frame-sized HDR.
    const auto fit = FitImage(static_cast<s32>(w), static_cast<s32>(h),
                              static_cast<s32>(frame->width), static_cast<s32>(frame->height));
    {
        const vk::ImageMemoryBarrier2 pre{
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eTransferWrite},
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = frame->fg_pre,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &pre});
        const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
        cmdbuf.clearColorImage(frame->fg_pre, vk::ImageLayout::eTransferDstOptimal, black,
                               vk::ImageSubresourceRange{.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                          .levelCount = 1,
                                                          .layerCount = 1});
        const vk::ImageMemoryBarrier2 hdr_to_src{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eShaderWrite},
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eTransferRead},
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .image = frame->fg_hdr,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &hdr_to_src});
        const vk::ImageBlit band{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffsets = {{vk::Offset3D{0, 0, 0}, vk::Offset3D{static_cast<s32>(w), static_cast<s32>(h), 1}}},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets = {{vk::Offset3D{fit.offset.x, fit.offset.y, 0},
                           vk::Offset3D{fit.offset.x + static_cast<s32>(fit.extent.width),
                                        fit.offset.y + static_cast<s32>(fit.extent.height), 1}}},
        };
        cmdbuf.blitImage(frame->fg_hdr, vk::ImageLayout::eTransferSrcOptimal, frame->fg_pre,
                         vk::ImageLayout::eTransferDstOptimal, band, vk::Filter::eLinear);
        const vk::ImageMemoryBarrier2 pre_to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eTransferWrite},
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eShaderRead},
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = frame->fg_pre,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &pre_to_read});
    }

    const vk::ImageSubresourceRange range{.aspectMask = vk::ImageAspectFlagBits::eColor,
                                          .levelCount = 1,
                                          .layerCount = 1};
    const auto bar = [](vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                        vk::AccessFlags2 src, vk::PipelineStageFlags2 src_stage,
                        vk::AccessFlags2 dst, vk::PipelineStageFlags2 dst_stage) {
        return vk::ImageMemoryBarrier2{.srcStageMask = src_stage,
                                       .srcAccessMask = src,
                                       .dstStageMask = dst_stage,
                                       .dstAccessMask = dst,
                                       .oldLayout = from,
                                       .newLayout = to,
                                       .image = image,
                                       .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto rw = vk::AccessFlags2{vk::AccessFlagBits2::eShaderRead |
                                     vk::AccessFlagBits2::eShaderWrite};
    {
        const std::array barriers{
            bar(frame->fg_hdr, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral,
                vk::AccessFlags2{vk::AccessFlagBits2::eTransferRead},
                vk::PipelineStageFlagBits2::eTransfer, {}, all),
            bar(frame->fg_display, vk::ImageLayout::eUndefined,
                vk::ImageLayout::eColorAttachmentOptimal, {}, {}, rw,
                vk::PipelineStageFlagBits2::eColorAttachmentOutput),
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 2,
                                                   .pImageMemoryBarriers = barriers.data()});
    }
    FGSTEP("banddone");
    Frame shadow = *frame;
    shadow.image = frame->fg_display;
    shadow.image_view = frame->fg_display_view;
    // One composite: generated content where the SDK produced it, the game's own frame
    // wherever the interpolator left nothing (UI overlays, prompt hotspots) - no region
    // guessing, no separate re-blit; the shader decodes and re-encodes, so whatever
    // transfer curve the inputs sit on, the output matches the real presents.
    pp_pass.RenderGenerated(cmdbuf, frame->fg_pre_view, frame->image_view, shadow, pp_settings);
    if (const char* fg_dump = std::getenv("ORDER1886_FG_DUMP"); fg_dump && *fg_dump) {
        static int fg_dump_index = 0;
        // Burst window: dumps stall the queue, so sample a short slice - by default late
        // enough to land in real gameplay once the title/cinematic part has passed.
        static const int dump_from = [] {
            const char* v = std::getenv("ORDER1886_FG_DUMP_FROM");
            return v && *v ? std::atoi(v) : 120;
        }();
        if (fg_dump_index >= dump_from && fg_dump_index < dump_from + 24) {
            DumpRaw(instance, draw_scheduler, cmdbuf, frame->fg_display,
                    vk::ImageLayout::eGeneral, frame->width, frame->height, "fgframe",
                    fg_dump_index);
            DumpRaw(instance, draw_scheduler, cmdbuf, frame->image, vk::ImageLayout::eGeneral,
                    frame->width, frame->height, "realframe", fg_dump_index);
        }
        ++fg_dump_index;
    }
    FGSTEP("hudcopied");
    frame->fg_valid = true;
}

void Presenter::PresentGenerated(Frame& frame, const vk::Extent2D& extent) {
    if (extent.width == 0 || extent.height == 0) {
        return;
    }
    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            return; // the real frame showed; skipping the generated one is safe
        }
    }
    const vk::Image swapchain_image = swapchain.Image();
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::ImageSubresourceRange range{.aspectMask = vk::ImageAspectFlagBits::eColor,
                                          .levelCount = 1,
                                          .layerCount = 1};
    // fg_display is already the fully composited frame (the band and its letterbox were
    // applied during recording), so this present is a plain frame-to-surface fit - unlike
    // the real present, which bands the raw render frame itself.
    const auto blit = MakeImageBlit(frame.width, frame.height, extent.width, extent.height, 0, 0);
    {
        const std::array barriers{
            vk::ImageMemoryBarrier2{.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                    .dstAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eTransferWrite},
                                    .oldLayout = vk::ImageLayout::eUndefined,
                                    .newLayout = vk::ImageLayout::eTransferDstOptimal,
                                    .image = swapchain_image,
                                    .subresourceRange = range},
            vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                    .srcAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eMemoryWrite},
                                    .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                    .dstAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eTransferRead},
                                    .oldLayout = vk::ImageLayout::eGeneral,
                                    .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                                    .image = frame.fg_display,
                                    .subresourceRange = range},
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 2,
                                                   .pImageMemoryBarriers = barriers.data()});
        cmdbuf.blitImage(frame.fg_display, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
        if (std::getenv("ORDER1886_FG_DUMP")) { // swapgen: what the surface actually receives
            static int swapgen_index = 0;
            if (swapgen_index % 400 < 12) {
                DumpRaw(instance, present_scheduler, cmdbuf, swapchain_image,
                        vk::ImageLayout::eTransferDstOptimal, extent.width, extent.height,
                        "swapgen", swapgen_index % 400);
            }
            ++swapgen_index;
        }
        const vk::ImageMemoryBarrier2 done{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eTransferRead},
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = frame.fg_display,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                   .pImageMemoryBarriers = &done});
    }
    const bool overlay = Ps4Overlay::Visible();
    {
        const vk::ImageMemoryBarrier2 to_next{
            .srcAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eTransferWrite},
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = overlay
                                ? vk::AccessFlags2{vk::AccessFlagBits2::eColorAttachmentRead |
                                                  vk::AccessFlagBits2::eColorAttachmentWrite}
                                : vk::AccessFlags2{},
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                 : vk::ImageLayout::ePresentSrcKHR,
            .image = swapchain_image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                   .pImageMemoryBarriers = &to_next});
        if (overlay) {
            Ps4Overlay::Render(cmdbuf, swapchain.ImageView(), extent);
            const vk::ImageMemoryBarrier2 to_present{
                .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .srcAccessMask = vk::AccessFlags2{vk::AccessFlagBits2::eColorAttachmentWrite},
                .dstStageMask = vk::PipelineStageFlagBits2::eBottomOfPipe,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .image = swapchain_image,
                .subresourceRange = range,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                       .pImageMemoryBarriers = &to_present});
        }
    }
    SubmitInfo info{};
    // The binary acquired semaphore was consumed by the real present this cycle; the
    // generated frame must instead wait on the draw timeline - fg_display's HUD regions are
    // written there, and without this wait the present queue raced it (black rectangles).
    info.AddWait(frame.ready_semaphore, frame.ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    scheduler.Flush(info);
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        swapchain.Present();
    }
    if (++fg_presented % 600 == 0) {
        std::printf("FrameGen: %llu generated presents issued\n",
                    static_cast<unsigned long long>(fg_presented));
        std::fflush(stdout);
    }
    framegen.Retire(fg_seq);
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
    frame->format = format;
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            completed_presentations.fetch_add(1, std::memory_order_release);
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

// raw copy of an image for ORDER1886_FRAME_DUMP_TRIGGER, written once the GPU is done.
static void DumpRaw(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                    vk::Image image, vk::ImageLayout layout, u32 w, u32 h, const char* name,
                    int index) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(w) * h * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    cmdbuf.copyImageToBuffer(image, layout, buffer,
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageExtent = {w, h, 1}});
    const char* dir = std::getenv("ORDER1886_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s_%03d_%ux%u.raw", dir && *dir ? dir : "out/dump", name,
                  index, w, h);
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      size = buffer_ci.size, file = std::string{path}] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(info.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    // CPU-submitted (non-EOP) flips run between GPU commands (Liverpool::SendCommand), possibly
    // while the draw recording thread still records into the same command buffer; recording
    // the frame directly then races it inside the driver (TOport 5534ce9). Let that thread
    // finish first (no-op elsewhere).
    rasterizer->DrainDrawPipe();
    // ORDER1886_FRAME_DUMP_TRIGGER=<file>: when the file exists it is removed and this frame's
    // guest display buffer and presented image are written to ORDER1886_DUMP_DIR (default out/dump)
    // as raw 32-bit pixels (menus and movies included; the upscaler dumps scene frames only).
    static const char* frame_dump_trigger = std::getenv("ORDER1886_FRAME_DUMP_TRIGGER");
    static int frame_dump_index = 0;
    const bool frame_dump = frame_dump_trigger && std::remove(frame_dump_trigger) == 0;
    // scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    Frame* frame = GetRenderFrame();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    if (upscaled) {
        const auto device = instance.GetDevice();
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = view_info.format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = display.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    } else {
        auto& image = texture_cache.GetImage(image_id);
        if (frame_dump) {
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(),
                    vk::ImageLayout::eTransferSrcOptimal, image.info.size.width,
                    image.info.size.height, "display", frame_dump_index);
        }
        image_view = *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    // FSR 1, RCAS and its strength come from the in-game menu, every frame.
    const auto& graphics = Ps4Settings::Get();
    fsr_settings.enable = graphics.fsr1;
    fsr_settings.use_rcas = graphics.rcas;
    fsr_settings.rcas_attenuation = Ps4Graphics::RcasAttenuation(graphics.rcas_strength);
    Ps4Settings::Get().image_width = int(image_size.width);
    Ps4Settings::Get().image_height = int(image_size.height);
    Ps4Settings::Get().window_width = int(frame->width);
    Ps4Settings::Get().window_height = int(frame->height);
    Ps4Settings::Get().rcas_applied =
        fsr_settings.use_rcas &&
        Ps4Graphics::FsrPassRuns(fsr_settings.enable, true, int(image_size.width), int(image_size.height),
                                  int(frame->width), int(frame->height));
    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);
    if (frame_dump) {
        // The presented image as the swapchain blit reads it (General after the pass).
        const vk::MemoryBarrier2 done{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                      .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                      .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        DumpRaw(instance, draw_scheduler, cmdbuf, frame->image, vk::ImageLayout::eGeneral,
                frame->width, frame->height, "output", frame_dump_index);
        ++frame_dump_index;
    }

    // #47: record the generated frame (if any) BEFORE the ready signal, so the present
    // queue's second cycle sees it through the ordering that already covers frame->image.
    frame->fg_valid = false;
    const auto fg_now = std::chrono::steady_clock::now();
    if (framegen_ready && !frame->is_hdr && Ps4Settings::Get().vsync.load()) {
        const u32 vblank_hz = std::max<u32>(1, EmulatorSettings.GetVblankFrequency());
        const auto slot = std::chrono::microseconds(1000000 / vblank_hz);
        const bool paced = fg_last_flip.time_since_epoch().count() == 0 ||
                           (fg_now - fg_last_flip) * 5 > slot * 7; // >1.4 vblank slots
        if (paced) {
            SceneFrame scene{};
            if (ConsumeSceneFrame(fg_scene_revision, scene)) {
                FramegenRecord(frame, cmdbuf, scene);
            }
        }
    }
    fg_last_flip = fg_now;

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);

    // the GPU command thread runs at most ORDER1886_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    // The in-game menu changes it live (Ps4Settings::frames_queued; ORDER1886_FRAMES_AHEAD at start).
    const u32 frames_ahead = u32(std::max(0, Ps4Settings::Get().frames_queued.load()));
    if (!frames_ahead) {
        recent_frame_ticks.clear();
    } else {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                draw_scheduler.Wait(tick);
            }
        }
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::PresentLoadingFrame() {
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        return;
    }
    swapchain.UpdateVsync();
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }
    if (!swapchain.AcquireNextImage()) {
        return;
    }
    const vk::Image image = swapchain.Image();
    const vk::Extent2D extent = swapchain.GetExtent();
    const auto cmdbuf = present_scheduler.CommandBuffer();
    const vk::ImageSubresourceRange range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };
    const auto barrier = [&](vk::ImageLayout from, vk::ImageLayout to, vk::AccessFlags src,
                             vk::AccessFlags dst) {
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
                               vk::ImageMemoryBarrier{
                                   .srcAccessMask = src,
                                   .dstAccessMask = dst,
                                   .oldLayout = from,
                                   .newLayout = to,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = image,
                                   .subresourceRange = range,
                               });
    };
    barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, {},
            vk::AccessFlagBits::eTransferWrite);
    cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal,
                           vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}},
                           range);
    barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal,
            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eColorAttachmentWrite);
    Ps4Overlay::Render(cmdbuf, swapchain.ImageView(), extent);
    barrier(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR,
            vk::AccessFlagBits::eColorAttachmentWrite, {});
    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    present_scheduler.Flush(info);
    bool presented = false;
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        presented = swapchain.Present();
    }
    if (presented) Ps4Overlay::OnPresented(true);
    // A few frames a second during start-up: waiting keeps semaphores and the swapchain simple.
    present_scheduler.Finish();
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // a minimized window has no pixels on Windows (0x0); keep the swapchain and skip
    // presenting until the window is restored.
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        free_frame();
        return;
    }

    // Recreate the swapchain if the window was resized.
    swapchain.UpdateVsync();
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            free_frame();
            return;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    // the game frame is blitted (letterboxed) straight into the swapchain image.
    const vk::Image swapchain_image = swapchain.Image();
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
        cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, black, color_range);
        const vk::MemoryBarrier clear_done{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        const bool band = UltrawideActive(float(frame->width) / float(std::max(frame->height, 1u)));
        cmdbuf.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         band ? MakeImageBlitBand(frame->width, frame->height, extent.width, extent.height)
                              : MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height),
                         vk::Filter::eLinear);
        if (VideoCore::ConsumeGameOnlyScreenshotRequests() != 0) {
            CaptureScreenshot(instance, scheduler, cmdbuf, *frame);
        }
        // the settings menu / FPS counter over the frame, at display resolution.
        const bool overlay = Ps4Overlay::Visible();
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        if (overlay) {
            Ps4Overlay::Render(cmdbuf, swapchain.ImageView(), extent);
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);

    // Present to swapchain.
    bool presented = false;
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        presented = swapchain.Present();
        if (!presented && window.GetWidth() != 0 && window.GetHeight() != 0) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }
    if (presented) Ps4Overlay::OnPresented(false);

    // #47: the generated frame displays in the vblank slot after the real one (FIFO paces
    // it); cycle B is submitted on the same queue after cycle A, which already waited the
    // draw timeline, so no extra wait is needed here.
    if (presented && frame->fg_valid) {
        frame->fg_valid = false;
        PresentGenerated(*frame, swapchain.GetExtent());
    }

    free_frame();
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
    }
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // no surface (minimized window): keep the last frame size
    }
    // A selected output resolution replaces the window size: FSR 1 / RCAS render at that
    // size and the swapchain blit fits the result to the window. Read live: this runs every
    // present, and a changed size makes GetRenderFrame recreate the frame (in-game menu).
    if (const int out = Ps4Settings::Get().output_res; out != Ps4Settings::OutputAutomatic) {
        width = Ps4Settings::OutputWidths[out];
        height = Ps4Settings::OutputHeights[out];
    }
    const float ratio = (float)width / (float)height;

    if (UltrawideActive(expected_ratio)) {
        // Only the 2.4:1 band is shown: size the frame so the band (not the whole 16:9 frame)
        // fills the window, 1:1 on a 21:9 display (3440 wide -> 3440x1433 band).
        const s32 band_width = std::min<s32>(width, static_cast<s32>(height * UltrawideBandRatio));
        expected_frame_width = std::max<s32>(band_width & ~1, 2);
        expected_frame_height = std::max<s32>(static_cast<s32>(expected_frame_width / expected_ratio) & ~1, 2);
        return;
    }
    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
