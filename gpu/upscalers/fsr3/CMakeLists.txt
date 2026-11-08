cmake_minimum_required(VERSION 3.22)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(order1886_fsr3 LANGUAGES C CXX)
endif()
find_package(Vulkan REQUIRED)
option(ORDER1886_FSR3_BUILD_TESTS "Build CPU provider contract checks" OFF)
option(ORDER1886_FSR3_BUILD_GPU_SMOKE "Build manual opt-in offscreen Vulkan smoke (never registered with CTest)" OFF)

# Keep the SDK's other providers, probes, test workloads and install rules out.
set(FFX_VK_PORTABLE_BUILD_TESTS OFF)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF)
set(FFX_VK_PORTABLE_BUILD_FSR3_HOST ON)
set(FFX_VK_PORTABLE_BUILD_FSR3_VK_BACKEND ON)
set(FFX_VK_PORTABLE_BUILD_FSR3_3_1_5_SCAFFOLD OFF)
set(FFX_VK_PORTABLE_BUILD_FSR4_V07_VULKAN OFF)
set(FFX_VK_PORTABLE_INSTALL OFF)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/fsr-vulkan"
                 "${CMAKE_CURRENT_BINARY_DIR}/fidelityfx" EXCLUDE_FROM_ALL)
set_target_properties(ffx_vulkan_portable ffx_vulkan_fsr3_vk_backend_1_1_4
                      ffx_vulkan_fsr3_host_1_1_4 PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(order1886_fsr3 SHARED order1886_fsr3.cpp)
target_compile_features(order1886_fsr3 PRIVATE cxx_std_17)
target_include_directories(order1886_fsr3 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../shim")
target_link_libraries(order1886_fsr3 PRIVATE ffx-vulkan::portable)
set_target_properties(order1886_fsr3 PROPERTIES PREFIX "" OUTPUT_NAME "order1886_fsr3")

# mingw-w64 lacks the MSVC spellings the FFX 1.1.4 host uses (_countof; swprintf_s needs
# <wchar.h> that the SDK translation units do not include). Force-include a guarded compat
# header on the FFX targets and this provider only - upstream sources stay untouched.
if (MINGW)
    set(ORDER1886_FFX_COMPAT "${CMAKE_CURRENT_SOURCE_DIR}/../ffx_mingw_compat.h")
    foreach (ffd ffx_vulkan_portable ffx_vulkan_fsr3_vk_backend_1_1_4 ffx_vulkan_fsr3_host_1_1_4 order1886_fsr3)
        if (TARGET ${ffd})
            target_compile_options(${ffd} PRIVATE "SHELL:-include ${ORDER1886_FFX_COMPAT}")
        endif()
    endforeach()
endif()

if(ORDER1886_FSR3_BUILD_GPU_SMOKE)
    add_executable(order1886_fsr3_gpu_smoke EXCLUDE_FROM_ALL tests/provider_gpu_smoke.cpp)
    target_compile_features(order1886_fsr3_gpu_smoke PRIVATE cxx_std_17)
    target_include_directories(order1886_fsr3_gpu_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../shim")
    target_link_libraries(order1886_fsr3_gpu_smoke PRIVATE order1886_fsr3 Vulkan::Vulkan)
endif()

if(ORDER1886_FSR3_BUILD_TESTS)
    enable_testing()
    add_executable(order1886_fsr3_contract tests/provider_contract.cpp order1886_fsr3.cpp)
    target_compile_features(order1886_fsr3_contract PRIVATE cxx_std_17)
    target_include_directories(order1886_fsr3_contract PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/fsr-vulkan/include"
        "${Vulkan_INCLUDE_DIRS}")
    add_test(NAME order1886_fsr3_contract COMMAND order1886_fsr3_contract)
    add_executable(order1886_fsr3_load tests/provider_load.cpp)
    target_include_directories(order1886_fsr3_load PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../shim" "${Vulkan_INCLUDE_DIRS}")
    target_link_libraries(order1886_fsr3_load PRIVATE order1886_fsr3)
    add_test(NAME order1886_fsr3_load COMMAND order1886_fsr3_load)
endif()
