// Provider ABI for Order-specific frame generation (FSR 3.1.6 style optical-flow
// interpolation). Kept separate from order1886_upscale.h because the vendored FSR
// frame-generation backend shares private SDK symbols with the 3.1.5 temporal
// upscaler and cannot be linked into the same translation unit; each provider is a
// distinct dlopen'd module (RTLD_LOCAL). Inputs are the pre-HUD scene at render
// resolution; the caller composites the guest HUD and letterbox around the output.
//
// Lifecycle mirrors ffxVkFsr3_3_1_6FrameGeneration*: create once, then per presented
// guest frame record_prepare (latch colour/depth/motion + camera), record_dispatch
// (emit the interpolated mid-time frame), retire (release history older than the
// host-proven present watermark). The retirement watermark is the same
// gpu_frames_completed the temporal upscalers use.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORDER1886_FRAMEGEN_ABI 1u

typedef enum OrderFramegenStatus {
    ORDER_FRAMEGEN_OK = 0,
    ORDER_FRAMEGEN_INVALID_ARGUMENT = 1,
    ORDER_FRAMEGEN_UNSUPPORTED = 2,   // GPU/driver lacks a required feature
    ORDER_FRAMEGEN_BACKEND_ERROR = 3,
    ORDER_FRAMEGEN_IN_FLIGHT = 4,     // caller advanced faster than history allows
} OrderFramegenStatus;

enum {
    ORDER_FRAMEGEN_HDR_SCRGB = 1u << 0,  // colour is scRGB linear/HDR; else sRGB
    ORDER_FRAMEGEN_DEPTH_INVERTED = 1u << 1,
    ORDER_FRAMEGEN_DEPTH_INFINITE = 1u << 2,
};

typedef struct OrderFramegenDevice {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
    VkQueue graphics_queue;
    uint32_t graphics_queue_family;
} OrderFramegenDevice;

typedef struct OrderFramegenConfig {
    uint32_t max_render_width, max_render_height;
    uint32_t display_width, display_height;
    VkFormat color_format;               // provider reports supported via ORDER_FRAMEGEN_UNSUPPORTED
    uint32_t flags;                      // ORDER_FRAMEGEN_*
} OrderFramegenConfig;

typedef struct OrderFramegenImage {
    VkImage image;
    VkFormat format;
    uint32_t width, height;
    VkImageLayout layout;
    VkImageUsageFlags usage;
} OrderFramegenImage;

// One presented guest frame's scene inputs for record_prepare.
typedef struct OrderFramegenPrepare {
    VkCommandBuffer command_buffer;
    OrderFramegenImage color, depth, motion;  // RGBA16F / R32F / RG16F at render size
    uint32_t render_width, render_height;
    float jitter_x, jitter_y;                  // 0: the game renders un-jittered
    float motion_scale_x, motion_scale_y;      // 1.0: motion is already in pixels (verify sign live)
    float frame_time_ms, min_luminance, max_luminance;
    float camera_near, camera_far, view_space_to_meters, vertical_fov_radians;
    float camera_position[3], camera_up[3], camera_right[3], camera_forward[3];
    uint64_t frame_id;
    uint32_t reset_history;
    uint32_t flags;                            // ORDER_FRAMEGEN_*
} OrderFramegenPrepare;

// Emit the interpolated frame between frame N-1 and N into a caller-owned output.
typedef struct OrderFramegenDispatch {
    VkCommandBuffer command_buffer;
    OrderFramegenImage color;      // current real colour (distinct from prepare's, per SDK)
    OrderFramegenImage output;     // display-size interpolated result
    OrderFramegenImage distortion; // optional RG16F UV_after-UV_before; image==NULL when unused
    uint32_t display_width, display_height;
    uint32_t interp_x, interp_y, interp_width, interp_height; // 0 => whole extent
    float frame_time_ms, min_luminance, max_luminance;
    float camera_near, camera_far, view_space_to_meters, vertical_fov_radians;
    uint64_t frame_id;
    uint32_t reset_history;
    uint64_t gpu_frames_completed;  // present fence watermark for history retirement
    uint32_t flags;
} OrderFramegenDispatch;

typedef struct OrderFramegenMemoryUsage {
    uint64_t resident_bytes;
} OrderFramegenMemoryUsage;

typedef struct OrderFramegenProviderApi {
    uint32_t abi_version;
    const char *name;
    const char *version;
    // Query before instance/device creation; may add instance/device extensions.
    OrderFramegenStatus (*required_instance_extensions)(uint32_t *count, const char *const **names);
    OrderFramegenStatus (*required_device_extensions)(VkInstance, VkPhysicalDevice, uint32_t *count, const char *const **names);
    OrderFramegenStatus (*create)(const OrderFramegenDevice *, const OrderFramegenConfig *, void **out_context);
    OrderFramegenStatus (*memory_usage)(void *context, OrderFramegenMemoryUsage *out);
    OrderFramegenStatus (*record_prepare)(void *context, const OrderFramegenPrepare *);
    OrderFramegenStatus (*record_dispatch)(void *context, const OrderFramegenDispatch *);
    OrderFramegenStatus (*retire)(void *context, uint64_t completed_frame_id);
    void (*destroy)(void *context);
} OrderFramegenProviderApi;

typedef OrderFramegenStatus (*OrderFramegenGetApi)(uint32_t requested_abi, const OrderFramegenProviderApi **out);

#ifdef __cplusplus
}
#endif
