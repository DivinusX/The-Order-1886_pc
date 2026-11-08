// SPDX-License-Identifier: GPL-2.0-or-later
// AMD FSR 4 (source-v07 INT8/DOT4) provider for The Order's temporal resolve (#12).
//
// Runs the FireBurn/FSR-Vulkan v07 backend through the same OrderUpscale ABI as the
// FSR 3.1 provider, so vk_order_resolve.cpp feeds it the game's own color, depth and
// velocity targets unchanged. The v07 shader/model bundle ships outside the runtime
// (MIT-licensed AMD v07 sources, compiled to SPIR-V by Q2RTX's recipe); this module
// loads it from ORDER1886_FSR4_ASSETS or a "fsr4" folder next to the runtime binary.
//
// The bundle is compiled with fixed permutations DEPTH_INVERTED=0 and
// JITTERED_MOTION_VECTORS=0: the shim's prep pass flips reversed depth into the
// standard orientation for this provider (vk_order_resolve.cpp) and the game renders
// without projection jitter, so both arrive as the model expects.
//
// Frame lifetime: the v07 backend keeps at most three un-retired dispatches and
// requires the host to retire only after the GPU finished them. The ABI has no host
// fence callback, so the provider records its own timeline-semaphore signal into the
// command buffer it dispatches into; a later dispatch retires exactly the frames the
// GPU has passed.
#include "order1886_upscale.h"

#include "ffx_vk_fsr4_v07.h"
#include "ffx_vk_fsr4_v07_assets.h"
#include "ffx_vk_fsr4_v07_schedule.h"
#include "ffx_vk_fsr4_v07_types.h"

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr uint32_t known_flags = ORDER_UPSCALE_HDR | ORDER_UPSCALE_DEPTH_INVERTED |
    ORDER_UPSCALE_DEPTH_INFINITE | ORDER_UPSCALE_MOTION_JITTERED;
constexpr size_t initializer_bytes = 89216;   // FFX_FSR4_V07_INITIALIZER_BYTES
constexpr size_t pre_weights_bytes = 1024;    // FFX_FSR4_V07_PRE_PASS_WEIGHTS_BYTES

OrderUpscaleStatus fail(OrderUpscaleStatus status, const char* message, char* error, size_t size) noexcept {
    if (error && size) std::snprintf(error, size, "%s", message);
    return status;
}

bool read_file(const std::filesystem::path& path, std::vector<uint8_t>& out) {
#ifdef _WIN32
    // path::value_type is wchar_t on Windows: the narrow fopen would not even compile.
    std::FILE* file = _wfopen(path.c_str(), L"rb");
#else
    std::FILE* file = std::fopen(path.c_str(), "rb");
#endif
    if (!file) return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0 || size > 16 << 20) { std::fclose(file); return false; }
    out.resize(static_cast<size_t>(size));
    const size_t read = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    return read == out.size();
}

// Roots that may hold the v07 bundle, in order: an explicit environment override,
// a "fsr4" folder (or the folder itself) beside the runtime binary, and the cwd.
std::vector<std::filesystem::path> asset_roots() {
    std::vector<std::filesystem::path> roots;
    if (const char* env = std::getenv("ORDER1886_FSR4_ASSETS"); env && *env)
        roots.emplace_back(env);
#ifdef _WIN32
    // The same beside-the-exe search as vk_upscale_providers.cpp: the worker starts the
    // runtime with cwd = user/order1886, so the current-path roots below never hit.
    std::vector<wchar_t> buf(4096);
    const DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size() - 1));
    if (len > 0 && len < buf.size()) {
        auto dir = std::filesystem::path(std::wstring(buf.data(), len)).parent_path();
        roots.push_back(dir / "fsr4");
        roots.push_back(dir / "gpu" / "fsr4");
        roots.push_back(std::move(dir));
    }
#else
    std::vector<char> buf(4096);
    const ssize_t len = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len > 0) {
        buf[static_cast<size_t>(len)] = '\0';
        auto dir = std::filesystem::path(buf.data()).parent_path();
        roots.push_back(dir / "fsr4");
        roots.push_back(dir / "gpu" / "fsr4");
        roots.push_back(std::move(dir));
    }
#endif
    roots.emplace_back(std::filesystem::current_path() / "fsr4");
    roots.emplace_back(std::filesystem::current_path());
    return roots;
}

// Loads the full v07 asset set for one preset/tier. Every module is validated for
// SPIR-V framing; CreateContext applies the provider's own descriptor-ABI checks.
bool load_assets(FfxFsr4ModelPreset preset, uint32_t out_w, uint32_t out_h,
                 std::vector<std::vector<uint32_t>>& words, std::vector<std::string>& entries,
                 std::vector<uint8_t>& initializer, std::vector<uint8_t>& pre_weights,
                 std::string& problem) {
    FfxFsr4V07AssetSet assets{};
    if (!ffxFsr4V07BuildAssetSet(preset, out_w, out_h, &assets)) {
        problem = "unsupported preset or output extent";
        return false;
    }
    // Pass index mapping from ffx_vk_fsr4_v07.h: [0] pre, [1-12] model, [13] post,
    // [14] RCAS, [15] SPD auto exposure. Model entries keep their HLSL names.
    entries = {"main", "fsr4_model_v07_i8_pass1",   "fsr4_model_v07_i8_pass2",
               "fsr4_model_v07_i8_pass3",            "fsr4_model_v07_i8_pass4",
               "fsr4_model_v07_i8_pass5",            "fsr4_model_v07_i8_pass6",
               "fsr4_model_v07_i8_pass7",            "fsr4_model_v07_i8_pass8",
               "fsr4_model_v07_i8_pass9",            "fsr4_model_v07_i8_pass10",
               "fsr4_model_v07_i8_pass11",           "fsr4_model_v07_i8_pass12",
               "main", "main", "main"};
    const std::vector<std::string> names = {
        assets.pre,
        assets.model[0],  assets.model[1],  assets.model[2],  assets.model[3],
        assets.model[4],  assets.model[5],  assets.model[6],  assets.model[7],
        assets.model[8],  assets.model[9],  assets.model[10], assets.model[11],
        assets.post, assets.rcas, assets.spdAutoExposure,
    };
    for (const auto& root : asset_roots()) {
        std::vector<std::vector<uint32_t>> loaded;
        loaded.reserve(names.size());
        bool ok = true;
        for (const auto& name : names) {
            std::vector<uint8_t> bytes;
            if (!read_file(root / name, bytes) || bytes.size() % 4 != 0 || bytes.size() < 8 ||
                (bytes[0] | uint32_t(bytes[1] << 8) | uint32_t(bytes[2] << 16) |
                 uint32_t(bytes[3] << 24)) != 0x07230203u) {
                ok = false;
                break;
            }
            loaded.emplace_back();
            auto& out = loaded.back();
            out.assign(reinterpret_cast<const uint32_t*>(bytes.data()),
                       reinterpret_cast<const uint32_t*>(bytes.data()) + bytes.size() / 4);
        }
        if (!ok) continue;
        std::vector<uint8_t> init, weights;
        if (!read_file(root / assets.initializer, init) || init.size() != initializer_bytes ||
            !read_file(root / assets.prePassWeights, weights) || weights.size() != pre_weights_bytes)
            continue;
        words = std::move(loaded);
        initializer = std::move(init);
        pre_weights = std::move(weights);
        return true;
    }
    problem = "FSR 4 v07 asset bundle (";
    problem += assets.model[0];
    problem += " ...) not found: set ORDER1886_FSR4_ASSETS or place the fsr4 folder beside the runtime";
    return false;
}

bool auto_exposure_enabled() {
    const char* env = std::getenv("ORDER1886_FSR4_AUTO_EXPOSURE");
    return !env || env[0] != '0';
}

float rcas_sharpness() {
    const char* env = std::getenv("ORDER1886_FSR4_SHARPNESS");
    if (!env || !*env) return 0.2f;
    const float value = std::strtof(env, nullptr);
    return value > 0.0f && value <= 1.0f ? value : 0.2f;
}

struct Context {
    OrderUpscaleConfig config{};
    std::vector<std::vector<uint32_t>> shader_words;
    std::vector<uint8_t> initializer, pre_weights, scratch;
    FfxInterface iface{};
    FfxFsr4VkContext* backend = nullptr;
    ffxContext effect = nullptr;
    bool reset = true;
    uint64_t next_frame = 0;      // frames begun
    uint64_t retired = 0;         // highest frame id proven complete by the host watermark
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    PFN_vkQueueWaitIdle wait_idle = nullptr;

    ~Context() noexcept {
        try {
            if (effect) ffxFsr4V07DestroyContext(&effect, nullptr);
            if (backend) ffxFsr4VkDestroyContext(backend);
        } catch (...) {}
    }
};

OrderUpscaleStatus instance_extensions(uint32_t* count, const char* const** names,
                                       char* error, size_t size) noexcept {
    if (count) *count = 0;
    if (names) *names = nullptr;
    if (!count || !names) return fail(ORDER_UPSCALE_INVALID_ARGUMENT, "Extension output is null", error, size);
    return fail(ORDER_UPSCALE_OK, "", error, size);
}

OrderUpscaleStatus device_extensions(VkInstance instance, VkPhysicalDevice physical,
    uint32_t* count, const char* const** names, char* error, size_t size) noexcept {
    static const char* const needed[] = {VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME};
    if (!instance || !physical || !count || !names)
        return fail(ORDER_UPSCALE_INVALID_ARGUMENT, "Vulkan device query handles are null", error, size);
    *count = 1;
    *names = needed;
    return ORDER_UPSCALE_OK;
}

OrderUpscaleStatus create(const OrderUpscaleDevice* device, const OrderUpscaleConfig* config,
                          void** context, char* error, size_t size) noexcept {
    if (context) *context = nullptr;
    if (!context || !device || !config)
        return fail(ORDER_UPSCALE_INVALID_ARGUMENT, "Create argument is null", error, size);
    try {
        if (!config->max_render_width || !config->max_render_height || !config->output_width ||
            !config->output_height || config->max_render_width > config->output_width ||
            config->max_render_height > config->output_height || (config->flags & ~known_flags))
            return fail(ORDER_UPSCALE_INVALID_ARGUMENT, "Invalid upscale dimensions or flags", error, size);
        if (!device->instance || !device->physical_device || !device->device || !device->graphics_queue ||
            !device->get_instance_proc_addr || !device->get_device_proc_addr)
            return fail(ORDER_UPSCALE_UNSUPPORTED, "Incomplete Vulkan device handles or loaders", error, size);
        if (device->shader_storage_image_write_without_format_enabled != VK_TRUE)
            return fail(ORDER_UPSCALE_UNSUPPORTED, "FSR 4 requires enabled shaderStorageImageWriteWithoutFormat", error, size);
        const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            device->get_instance_proc_addr(device->instance, "vkGetPhysicalDeviceProperties"));
        if (!properties)
            return fail(ORDER_UPSCALE_UNSUPPORTED, "Vulkan physical-device queries unavailable", error, size);
        VkPhysicalDeviceProperties limits{};
        properties(device->physical_device, &limits);
        if (limits.apiVersion < VK_API_VERSION_1_2)
            return fail(ORDER_UPSCALE_UNSUPPORTED, "FSR 4 provider requires Vulkan 1.2", error, size);
        if (config->output_width > FFX_FSR4_DOT4_MAX_OUTPUT_WIDTH ||
            config->output_height > FFX_FSR4_DOT4_MAX_OUTPUT_HEIGHT)
            return fail(ORDER_UPSCALE_UNSUPPORTED, "Output exceeds the FSR 4 v07 model extent limit", error, size);

        const FfxFsr4ModelPreset preset = ffxFsr4SelectModelPreset(config->max_render_width,
                                                                    config->output_width, false);
        auto result = std::make_unique<Context>();
        result->config = *config;
        result->device = device->device;
        std::vector<std::string> entries;
        std::string problem;
        if (!load_assets(preset, config->output_width, config->output_height, result->shader_words,
                         entries, result->initializer, result->pre_weights, problem))
            return fail(ORDER_UPSCALE_UNSUPPORTED, problem.c_str(), error, size);

        FfxFsr4VkCreateInfo backend_info{};
        backend_info.device = device->device;
        backend_info.physicalDevice = device->physical_device;
        for (uint32_t i = 0; i < FFX_FSR4_VK_PASS_COUNT; ++i) {
            backend_info.shaders[i].spirv = result->shader_words[i].data();
            backend_info.shaders[i].sizeBytes = result->shader_words[i].size() * sizeof(uint32_t);
            backend_info.shaders[i].entryPoint = entries[i].c_str();
        }
        backend_info.modelInitializer = result->initializer.data();
        backend_info.modelInitializerSize = result->initializer.size();
        backend_info.prePassWeights = result->pre_weights.data();
        backend_info.prePassWeightsSize = result->pre_weights.size();
        result->scratch.resize(ffxFsr4VkGetScratchMemorySize());
        backend_info.scratchBuffer = result->scratch.data();
        backend_info.scratchBufferSize = result->scratch.size();
        if (ffxFsr4VkCreateContext(&backend_info, &result->iface) != VK_SUCCESS)
            return fail(ORDER_UPSCALE_BACKEND_ERROR, "FSR 4 v07 Vulkan backend failed to create (missing "
                        "float16/int8/dot-product/compute-derivative support?)", error, size);
        result->backend = static_cast<FfxFsr4VkContext*>(result->iface.device);

        uint32_t flags = 0;
        if (config->flags & ORDER_UPSCALE_HDR) flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
        if (config->flags & ORDER_UPSCALE_DEPTH_INVERTED) flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
        if (config->flags & ORDER_UPSCALE_DEPTH_INFINITE) flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
        if (config->flags & ORDER_UPSCALE_MOTION_JITTERED) flags |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
        if (auto_exposure_enabled()) flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;

        ffxCreateContextDescUpscale upscale{};
        upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        upscale.header.pNext = nullptr;
        upscale.flags = flags;
        upscale.maxRenderSize = {config->max_render_width, config->max_render_height};
        upscale.maxUpscaleSize = {config->output_width, config->output_height};
        ffxFsr4V07SetBackendInterface(&result->iface);
        const auto created = ffxFsr4V07CreateContext(&result->effect, &upscale.header, nullptr);
        ffxFsr4V07SetBackendInterface(nullptr);
        if (created != FFX_API_RETURN_OK || !result->effect) {
            ffxFsr4VkDestroyContext(result->backend);
            result->backend = nullptr;
            return fail(ORDER_UPSCALE_BACKEND_ERROR, "FSR 4 v07 provider context creation failed", error, size);
        }

        result->queue = device->graphics_queue;
        result->wait_idle = reinterpret_cast<PFN_vkQueueWaitIdle>(
            device->get_device_proc_addr(device->device, "vkQueueWaitIdle"));
        if (!result->wait_idle) {
            ffxFsr4V07DestroyContext(&result->effect, nullptr);
            ffxFsr4VkDestroyContext(result->backend);
            result->backend = nullptr;
            return fail(ORDER_UPSCALE_UNSUPPORTED, "Vulkan device entry points are incomplete", error, size);
        }
        *context = result.release();
        std::printf("Upscaler: FSR 4 v07 (%s model, tier bundle, auto exposure %s) created for %ux%u -> %ux%u\n",
                    ffxFsr4ModelPresetName(preset), auto_exposure_enabled() ? "on" : "off",
                    config->max_render_width, config->max_render_height,
                    config->output_width, config->output_height);
        return ORDER_UPSCALE_OK;
    } catch (...) {
        return fail(ORDER_UPSCALE_BACKEND_ERROR, "FSR 4 context creation threw an exception", error, size);
    }
}

bool valid_image(const OrderUpscaleImage& image, VkFormat format, uint32_t w, uint32_t h,
                 uint32_t max_w, uint32_t max_h, bool output = false) noexcept {
    return image.image && image.view && image.format == format && image.width >= w && image.height >= h &&
        image.width <= max_w && image.height <= max_h &&
        (image.usage & (output ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT)) &&
        image.layout == (output ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

FfxApiResource api_image(const OrderUpscaleImage& image, uint32_t format, bool output) noexcept {
    FfxApiResource resource{};
    resource.resource = reinterpret_cast<void*>(image.view);
    resource.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    resource.description.format = format;
    resource.description.width = image.width;
    resource.description.height = image.height;
    resource.description.depth = 1;
    resource.description.mipCount = 1;
    resource.description.usage = image.usage;
    resource.state = output ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ;
    return resource;
}

// The v07 backend transitions and restores imported images from the state records
// registered here; the layouts are the ones the shim guaranteed at dispatch time.
VkResult set_import(FfxInterface& iface, const OrderUpscaleImage& image, bool output) {
    const auto access = static_cast<VkAccessFlags>(
        output ? (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT) : VK_ACCESS_SHADER_READ_BIT);
    const FfxFsr4VkExternalImageState state{
        sizeof(FfxFsr4VkExternalImageState),
        image.image,
        image.view,
        output ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        access,
        output ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        access,
    };
    return ffxFsr4VkSetExternalImageState(&iface, &state);
}

OrderUpscaleStatus dispatch(void* opaque, const OrderUpscaleFrame* frame, char* error, size_t size) noexcept {
    if (!opaque || !frame) return fail(ORDER_UPSCALE_INVALID_ARGUMENT, "Dispatch argument is null", error, size);
    auto& context = *static_cast<Context*>(opaque);
    try {
        const auto& c = context.config;
        const auto& f = *frame;
        if (!f.command_buffer || !f.render_width || !f.render_height || f.render_width > c.max_render_width ||
            f.render_height > c.max_render_height || f.reset_history > 1 ||
            !valid_image(f.color, VK_FORMAT_R16G16B16A16_SFLOAT, f.render_width, f.render_height, c.max_render_width, c.max_render_height) ||
            !valid_image(f.depth, VK_FORMAT_R32_SFLOAT, f.render_width, f.render_height, c.max_render_width, c.max_render_height) ||
            !valid_image(f.motion, VK_FORMAT_R16G16_SFLOAT, f.render_width, f.render_height, c.max_render_width, c.max_render_height) ||
            !valid_image(f.output, VK_FORMAT_R16G16B16A16_SFLOAT, c.output_width, c.output_height, c.output_width, c.output_height, true) ||
            f.output.image == f.color.image || f.output.image == f.depth.image || f.output.image == f.motion.image)
            return fail(ORDER_UPSCALE_INVALID_ARGUMENT, "Invalid FSR 4 image, dimensions, layout or reset flag", error, size);
        const float values[] = {f.jitter_x, f.jitter_y, f.motion_scale_x, f.motion_scale_y, f.frame_time_ms,
            f.pre_exposure, f.camera_near, f.camera_far, f.vertical_fov};
        for (float value : values) if (!std::isfinite(value))
            return fail(ORDER_UPSCALE_INVALID_ARGUMENT, "FSR 4 temporal input is not finite", error, size);
        if (f.motion_scale_x == 0 || f.motion_scale_y == 0 || f.frame_time_ms <= 0 || f.pre_exposure <= 0 ||
            f.camera_near <= 0 || f.camera_far < 0 || f.vertical_fov <= 0 || f.vertical_fov >= 3.14159265358979323846f ||
            (!(c.flags & ORDER_UPSCALE_DEPTH_INFINITE) && f.camera_far <= f.camera_near))
            return fail(ORDER_UPSCALE_INVALID_ARGUMENT, "FSR 4 camera, motion scale, exposure or frame time is invalid", error, size);

        // The host proves completion through the present fence (OrderUpscaleFrame ABI 2).
        // Retire only ids already issued, so history/descriptor slots free exactly when the
        // GPU provably left them; no per-frame waits, no guesses.
        const uint64_t completed = std::min<uint64_t>(f.gpu_frames_completed, context.next_frame);
        if (completed > context.retired) {
            ffxFsr4VkRetireFrame(&context.iface, completed);
            context.retired = completed;
        }
        const uint64_t frame_id = ++context.next_frame;
        auto begun = ffxFsr4VkBeginFrame(&context.iface, frame_id);
        if (begun == VK_NOT_READY) {
            // Slots exhausted: nothing was presented since the last retirement, so the queue
            // is three or more frames behind the game's recording thread. Drain it once and
            // retire everything issued; this is self-repair, not steady-state pacing.
            if (context.wait_idle(context.queue) != VK_SUCCESS) {
                --context.next_frame;
                return fail(ORDER_UPSCALE_BACKEND_ERROR, "FSR 4 slot recovery failed (queue wait)", error, size);
            }
            const uint64_t issued = context.next_frame - 1;
            if (issued > context.retired) {
                ffxFsr4VkRetireFrame(&context.iface, issued);
                context.retired = issued;
            }
            begun = ffxFsr4VkBeginFrame(&context.iface, frame_id);
        }
        if (begun != VK_SUCCESS) {
            --context.next_frame;
            return fail(begun == VK_NOT_READY ? ORDER_UPSCALE_UNSUPPORTED : ORDER_UPSCALE_BACKEND_ERROR,
                        begun == VK_NOT_READY
                            ? "FSR 4 frame slots exhausted even after a queue drain"
                            : "FSR 4 BeginFrame failed", error, size);
        }

        for (const auto* import : {&f.color, &f.depth, &f.motion}) {
            if (set_import(context.iface, *import, false) != VK_SUCCESS) {
                return fail(ORDER_UPSCALE_BACKEND_ERROR, "FSR 4 rejected an imported input image", error, size);
            }
        }
        if (set_import(context.iface, f.output, true) != VK_SUCCESS) {
            return fail(ORDER_UPSCALE_BACKEND_ERROR, "FSR 4 rejected the output image", error, size);
        }

        ffxDispatchDescUpscale desc{};
        desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        desc.header.pNext = nullptr;
        desc.commandList = f.command_buffer;
        desc.color = api_image(f.color, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, false);
        desc.depth = api_image(f.depth, FFX_SURFACE_FORMAT_R32_FLOAT, false);
        desc.motionVectors = api_image(f.motion, FFX_SURFACE_FORMAT_R16G16_FLOAT, false);
        desc.output = api_image(f.output, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, true);
        desc.jitterOffset = {f.jitter_x, f.jitter_y};
        desc.motionVectorScale = {f.motion_scale_x, f.motion_scale_y};
        desc.renderSize = {f.render_width, f.render_height};
        desc.upscaleSize = {c.output_width, c.output_height};
        desc.enableSharpening = true;
        desc.sharpness = rcas_sharpness();
        desc.enableAutoExposure = auto_exposure_enabled() ? true : false;
        desc.frameTimeDelta = f.frame_time_ms;
        desc.preExposure = f.pre_exposure;
        desc.reset = (context.reset || f.reset_history) ? true : false;
        desc.cameraNear = f.camera_near;
        desc.cameraFar = f.camera_far;
        desc.cameraFovAngleVertical = f.vertical_fov;
        desc.viewSpaceToMetersFactor = 1.0f;

        const auto dispatched = ffxFsr4V07Dispatch(&context.effect, &desc.header);
        if (dispatched != FFX_API_RETURN_OK) {
            // A failed recording may already contain commands: the caller discards the buffer.
            // The opened id stays counted; the watermark will retire it once the queue passes it.
            context.reset = true;
            return fail(ORDER_UPSCALE_BACKEND_ERROR, "FSR 4 dispatch failed", error, size);
        }
        context.reset = false;
        return ORDER_UPSCALE_OK;
    } catch (...) {
        context.reset = true;
        return fail(ORDER_UPSCALE_BACKEND_ERROR, "FSR 4 dispatch threw an exception", error, size);
    }
}

void destroy(void* context) noexcept { delete static_cast<Context*>(context); }

} // namespace

extern "C"
#ifdef _WIN32
__declspec(dllexport)
#endif
OrderUpscaleStatus order1886_fsr_get_api(uint32_t abi, OrderUpscaleProviderApi* api) noexcept {
    if (!api) return ORDER_UPSCALE_INVALID_ARGUMENT;
    *api = {};
    if (abi != ORDER1886_UPSCALE_ABI) return ORDER_UPSCALE_UNSUPPORTED;
    *api = {ORDER1886_UPSCALE_ABI, "AMD FSR 4", "v07 INT8/DOT4 (source)",
        instance_extensions, device_extensions, create, dispatch, destroy};
    return ORDER_UPSCALE_OK;
}
