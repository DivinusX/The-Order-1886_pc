// SPDX-License-Identifier: GPL-2.0-or-later
// Order frame-generation provider: a thin, side-effect-free wrapper over the vendored
// FidelityFX FSR 3.1.6 frame-generation backend (gpu/third_party/fsr-vulkan). It exists
// as a separate dlopen'd module because the FSR frame-generation backend shares private
// SDK symbols with the 3.1.5 temporal upscaler and cannot be linked beside it.
//
// The engine owns all Vulkan objects, WSI and the present loop; this module only records
// the SDK's compute work into a caller-provided command buffer and manages the SDK context.
#include "order1886_framegen.h"
#include "ffx_vk_fsr3_3_1_5_bridge.h"

namespace {

FfxVkFsr3_3_1_6FrameGenerationImage map(const OrderFramegenImage &i) {
    FfxVkFsr3_3_1_6FrameGenerationImage o{};
    o.image = i.image;
    o.format = i.format;
    o.width = i.width;
    o.height = i.height;
    o.layout = i.layout;
    o.usage = i.usage;
    return o;
}

FfxVkFsr3_3_1_6FrameGenerationTransferFunction transfer(uint32_t flags) {
    return (flags & ORDER_FRAMEGEN_HDR_SCRGB) ? FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SCRGB
                                              : FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB;
}

OrderFramegenStatus result(FfxVkFsr3_3_1_6FrameGenerationResult r) {
    switch (r) {
        case FFX_VK_FSR3_3_1_6_FRAMEGEN_OK: return ORDER_FRAMEGEN_OK;
        case FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT: return ORDER_FRAMEGEN_INVALID_ARGUMENT;
        case FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_OUT_OF_MEMORY: return ORDER_FRAMEGEN_BACKEND_ERROR;
        case FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_BACKEND: return ORDER_FRAMEGEN_BACKEND_ERROR;
        case FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_IN_FLIGHT: return ORDER_FRAMEGEN_IN_FLIGHT;
    }
    return ORDER_FRAMEGEN_BACKEND_ERROR;
}

OrderFramegenStatus create(const OrderFramegenDevice *dev, const OrderFramegenConfig *cfg, void **out) {
    if (!dev || !cfg || !out || cfg->max_render_width == 0 || cfg->max_render_height == 0 ||
        cfg->display_width == 0 || cfg->display_height == 0) {
        return ORDER_FRAMEGEN_INVALID_ARGUMENT;
    }
    FfxVkFsr3_3_1_6FrameGenerationCreateInfo info{};
    info.physicalDevice = dev->physical_device;
    info.device = dev->device;
    info.maxRenderWidth = cfg->max_render_width;
    info.maxRenderHeight = cfg->max_render_height;
    info.displayWidth = cfg->display_width;
    info.displayHeight = cfg->display_height;
    info.colorFormat = cfg->color_format;
    FfxVkFsr3_3_1_6FrameGenerationContext *ctx = nullptr;
    const auto r = ffxVkFsr3_3_1_6FrameGenerationContextCreate(&info, &ctx);
    if (r != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        return result(r);
    }
    *out = ctx;
    return ORDER_FRAMEGEN_OK;
}

OrderFramegenStatus memory_usage(void *handle, OrderFramegenMemoryUsage *out) {
    if (!handle || !out) {
        return ORDER_FRAMEGEN_INVALID_ARGUMENT;
    }
    FfxVkFsr3_3_1_6FrameGenerationMemoryUsage usage{};
    const auto r = ffxVkFsr3_3_1_6FrameGenerationContextGetMemoryUsage(
        static_cast<FfxVkFsr3_3_1_6FrameGenerationContext *>(handle), &usage);
    out->resident_bytes = usage.totalUsageInBytes;
    return result(r);
}

OrderFramegenStatus record_prepare(void *handle, const OrderFramegenPrepare *p) {
    if (!handle || !p) {
        return ORDER_FRAMEGEN_INVALID_ARGUMENT;
    }
    FfxVkFsr3_3_1_6FrameGenerationPrepareInfo info{};
    info.commandBuffer = p->command_buffer;
    info.color = map(p->color);
    info.depth = map(p->depth);
    info.motionVectors = map(p->motion);
    info.renderWidth = p->render_width;
    info.renderHeight = p->render_height;
    info.jitterOffsetX = p->jitter_x;
    info.jitterOffsetY = p->jitter_y;
    info.motionVectorScaleX = p->motion_scale_x;
    info.motionVectorScaleY = p->motion_scale_y;
    info.frameTimeMilliseconds = p->frame_time_ms;
    info.minLuminance = p->min_luminance;
    info.maxLuminance = p->max_luminance;
    info.transferFunction = transfer(p->flags);
    info.cameraNear = p->camera_near;
    info.cameraFar = p->camera_far;
    info.viewSpaceToMeters = p->view_space_to_meters;
    info.cameraVerticalFovRadians = p->vertical_fov_radians;
    for (int i = 0; i < 3; ++i) {
        info.cameraPosition[i] = p->camera_position[i];
        info.cameraUp[i] = p->camera_up[i];
        info.cameraRight[i] = p->camera_right[i];
        info.cameraForward[i] = p->camera_forward[i];
    }
    info.frameId = p->frame_id;
    info.reset = p->reset_history ? VK_TRUE : VK_FALSE;
    return result(ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(
        static_cast<FfxVkFsr3_3_1_6FrameGenerationContext *>(handle), &info));
}

OrderFramegenStatus record_dispatch(void *handle, const OrderFramegenDispatch *d) {
    if (!handle || !d) {
        return ORDER_FRAMEGEN_INVALID_ARGUMENT;
    }
    FfxVkFsr3_3_1_6FrameGenerationDispatchInfo info{};
    info.commandBuffer = d->command_buffer;
    info.color = map(d->color);
    info.output = map(d->output);
    if (d->distortion.image != VK_NULL_HANDLE) {
        info.distortionField = map(d->distortion);
    }
    info.displayWidth = d->display_width;
    info.displayHeight = d->display_height;
    info.interpolationX = d->interp_x;
    info.interpolationY = d->interp_y;
    info.interpolationWidth = d->interp_width;
    info.interpolationHeight = d->interp_height;
    info.frameTimeMilliseconds = d->frame_time_ms;
    info.minLuminance = d->min_luminance;
    info.maxLuminance = d->max_luminance;
    info.transferFunction = transfer(d->flags);
    info.cameraNear = d->camera_near;
    info.cameraFar = d->camera_far;
    info.viewSpaceToMeters = d->view_space_to_meters;
    info.cameraVerticalFovRadians = d->vertical_fov_radians;
    info.frameId = d->frame_id;
    info.reset = d->reset_history ? VK_TRUE : VK_FALSE;
    return result(ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(
        static_cast<FfxVkFsr3_3_1_6FrameGenerationContext *>(handle), &info));
}

OrderFramegenStatus retire(void *handle, uint64_t completed) {
    if (!handle) {
        return ORDER_FRAMEGEN_INVALID_ARGUMENT;
    }
    return result(ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(
        static_cast<FfxVkFsr3_3_1_6FrameGenerationContext *>(handle), completed));
}

void destroy(void *handle) {
    if (handle) {
        ffxVkFsr3_3_1_6FrameGenerationContextDestroy(
            static_cast<FfxVkFsr3_3_1_6FrameGenerationContext *>(handle));
    }
}

OrderFramegenStatus instance_extensions(uint32_t *count, const char *const **names) {
    if (!count || !names) {
        return ORDER_FRAMEGEN_INVALID_ARGUMENT;
    }
    *count = 0;  // the SDK backend adds none beyond the core Vulkan instance
    *names = nullptr;
    return ORDER_FRAMEGEN_OK;
}

OrderFramegenStatus device_extensions(VkInstance, VkPhysicalDevice physical, uint32_t *count, const char *const **names) {
    if (!count || !names) {
        return ORDER_FRAMEGEN_INVALID_ARGUMENT;
    }
    // The FG backend records compute into the caller's command buffer against a
    // caller-created device; it requests no device extensions of its own here.
    // If a future FG path needs compute-derivative-group-quad, report it from this
    // probe (the SDK create would then fail UNSUPPORTED rather than corrupt a device).
    (void)physical;
    *count = 0;
    *names = nullptr;
    return ORDER_FRAMEGEN_OK;
}

const OrderFramegenProviderApi kApi = {
    ORDER1886_FRAMEGEN_ABI, "order1886-fg", "1.0",
    instance_extensions, device_extensions, create, memory_usage,
    record_prepare, record_dispatch, retire, destroy,
};

}  // namespace

extern "C" OrderFramegenStatus order1886_fg_get_api(uint32_t requested_abi, const OrderFramegenProviderApi **out) {
    if (!out || requested_abi != ORDER1886_FRAMEGEN_ABI) {
        return ORDER_FRAMEGEN_INVALID_ARGUMENT;
    }
    *out = &kApi;
    return ORDER_FRAMEGEN_OK;
}
