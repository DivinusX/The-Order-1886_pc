// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <cstdio>
#include "video_core/renderer_vulkan/vk_instance.h"

// Keep the dispatcher's ABI consistent with the renderer library.
#undef NDEBUG
#include <cassert>

namespace Vulkan {
bool InitializePipelineBinaryCache(vk::Device device, bool supported,
                                   vk::PipelineBinaryKeyKHR& global_key);
}

namespace {
unsigned key_queries{};
VkResult key_result = VK_SUCCESS;
uint32_t key_size = 2;

void SetCache(const char* value) {
#ifdef _WIN32
    _putenv_s("ORDER1886_PIPELINE_BINARIES", value);
#else
    if (*value) setenv("ORDER1886_PIPELINE_BINARIES", value, 1);
    else unsetenv("ORDER1886_PIPELINE_BINARIES");
#endif
}

VKAPI_ATTR VkResult VKAPI_CALL GetKey(VkDevice, const VkPipelineCreateInfoKHR* info,
                                     VkPipelineBinaryKeyKHR* key) {
    assert(!info);
    ++key_queries;
    key->keySize = key_size;
    key->key[0] = 7;
    return key_result;
}

void TestFallbacks() {
    auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
    const auto original = dispatch;
    dispatch.vkGetPipelineKeyKHR = GetKey;
    dispatch.vkCreatePipelineBinariesKHR = [](VkDevice, const VkPipelineBinaryCreateInfoKHR*,
        const VkAllocationCallbacks*, VkPipelineBinaryHandlesInfoKHR*) -> VkResult {
        std::abort();
    };
    dispatch.vkGetPipelineBinaryDataKHR = [](VkDevice, const VkPipelineBinaryDataInfoKHR*,
        VkPipelineBinaryKeyKHR*, size_t*, void*) -> VkResult { std::abort(); };
    dispatch.vkDestroyPipelineBinaryKHR = [](VkDevice, VkPipelineBinaryKHR,
        const VkAllocationCallbacks*) { std::abort(); };
    dispatch.vkReleaseCapturedPipelineDataKHR = [](VkDevice,
        const VkReleaseCapturedPipelineDataInfoKHR*, const VkAllocationCallbacks*) -> VkResult {
        std::abort();
    };
    const vk::Device device{reinterpret_cast<VkDevice>(uintptr_t{1})};
    vk::PipelineBinaryKeyKHR key{.keySize = 2};
    SetCache("0");
    assert(!Vulkan::InitializePipelineBinaryCache(device, true, key));
    assert(key_queries == 0 && key.keySize == 0);

    SetCache("1");
    assert(!Vulkan::InitializePipelineBinaryCache(device, false, key));
    assert(key_queries == 0 && key.keySize == 0);

    const auto get_data = dispatch.vkGetPipelineBinaryDataKHR;
    dispatch.vkGetPipelineBinaryDataKHR = nullptr;
    assert(!Vulkan::InitializePipelineBinaryCache(device, true, key));
    assert(key_queries == 0 && key.keySize == 0);
    dispatch.vkGetPipelineBinaryDataKHR = get_data;

    key_result = VK_ERROR_UNKNOWN;
    assert(!Vulkan::InitializePipelineBinaryCache(device, true, key));
    assert(key_queries == 1 && key.keySize == 0);

    key_result = VK_SUCCESS;
    key_size = 0;
    assert(!Vulkan::InitializePipelineBinaryCache(device, true, key));
    assert(key_queries == 2 && key.keySize == 0);

    key_size = VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR + 1;
    assert(!Vulkan::InitializePipelineBinaryCache(device, true, key));
    assert(key_queries == 3 && key.keySize == 0);

    SetCache("");
    key_size = VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR;
    assert(Vulkan::InitializePipelineBinaryCache(device, true, key));
    assert(key_queries == 4 && key.keySize == key_size && key.key[0] == 7);
    dispatch = original;
}
}

// A GPU-free CI runner (the Windows CPU-check job has no Vulkan installable client
// driver) cannot build a real Vulkan::Instance - the renderer library asserts and the
// test dies. Probe the loader for at least one physical device and skip cleanly when
// there is none; a machine with a driver (Linux CI, any player PC) still runs the test.
// The project links Vulkan through a dynamic loader with VK_NO_PROTOTYPES, so the raw
// vk* entry points are not declared; resolve them as function pointers via the loader,
// the same way vk_platform.cpp does, touching no global dispatcher and throwing nothing.
static bool has_vulkan_device() {
    vk::detail::DynamicLoader dl;
    auto get_instance_proc = dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    if (!get_instance_proc)
        return false;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(get_instance_proc(nullptr, "vkCreateInstance"));
    if (!create)
        return false;
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                .pApplicationName = "pipeline-binary-test-probe",
                                .apiVersion = VK_API_VERSION_1_1};
    const VkInstanceCreateInfo ci{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                  .pApplicationInfo = &app};
    VkInstance instance{};
    if (create(&ci, nullptr, &instance) != VK_SUCCESS)
        return false;
    // instance-level entry points resolve through the created instance handle
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        get_instance_proc(instance, "vkEnumeratePhysicalDevices"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get_instance_proc(instance, "vkDestroyInstance"));
    if (!enumerate || !destroy) {
        if (destroy)
            destroy(instance, nullptr);
        return false;
    }
    uint32_t count = 0;
    const VkResult enumerated = enumerate(instance, &count, nullptr);
    destroy(instance, nullptr);
    return enumerated == VK_SUCCESS && count > 0;
}

int main(int argc, char**) {
    if (!has_vulkan_device()) {
        std::puts("pipeline-binary-initialization-test: SKIP (no Vulkan device)");
        return 0;
    }
    const bool enabled = argc > 1;
    SetCache(enabled ? "1" : "0");
    Vulkan::Instance instance{s32{-1}, false};
    if (!enabled && instance.GetPipelineBinaryGlobalKey().keySize != 0) {
        std::fputs("FAIL: disabled binary cache initialized a global key\n", stderr);
        return 1;
    }
    std::printf("Real device: binary cache %s, global key size %u\n",
                enabled ? "requested" : "disabled", instance.GetPipelineBinaryGlobalKey().keySize);
    TestFallbacks();
    std::puts("pipeline-binary-initialization-test: passed");
}
