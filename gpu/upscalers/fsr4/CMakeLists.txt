cmake_minimum_required(VERSION 3.22)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(order1886_fsr4 LANGUAGES C CXX)
endif()
find_package(Vulkan REQUIRED)

# Keep the SDK's other providers, probes, test workloads and install rules out;
# the v07 FSR4 provider and its asset contract are all this module needs.
set(FFX_VK_PORTABLE_BUILD_TESTS OFF)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF)
set(FFX_VK_PORTABLE_BUILD_FSR3_HOST OFF)
set(FFX_VK_PORTABLE_BUILD_FSR3_VK_BACKEND OFF)
set(FFX_VK_PORTABLE_BUILD_FSR3_3_1_5_SCAFFOLD OFF)
set(FFX_VK_PORTABLE_BUILD_FSR4_V07_VULKAN ON)
set(FFX_VK_PORTABLE_INSTALL OFF)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/fsr-vulkan"
                 "${CMAKE_CURRENT_BINARY_DIR}/fidelityfx" EXCLUDE_FROM_ALL)
set_target_properties(ffx_vulkan_fsr4_v07_vulkan
                      ffx_vulkan_fsr4_v07_assets PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(order1886_fsr4 SHARED order1886_fsr4.cpp)
target_compile_features(order1886_fsr4 PRIVATE cxx_std_17)
target_include_directories(order1886_fsr4 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shim"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/fsr-vulkan/include")
target_link_libraries(order1886_fsr4 PRIVATE ffx-vulkan::fsr4-v07-vulkan ffx-vulkan::fsr4-v07-assets Vulkan::Vulkan)
if(NOT WIN32)
    target_link_libraries(order1886_fsr4 PRIVATE ${CMAKE_DL_LIBS})
endif()
set_target_properties(order1886_fsr4 PROPERTIES PREFIX "" OUTPUT_NAME "order1886_fsr4")

if (MINGW)
    set(ORDER1886_FFX_COMPAT "${CMAKE_CURRENT_SOURCE_DIR}/../ffx_mingw_compat.h")
    foreach (ffd ffx_vulkan_portable ffx_vulkan_fsr4_v07_vulkan ffx_vulkan_fsr4_v07_assets order1886_fsr4)
        if (TARGET ${ffd})
            target_compile_options(${ffd} PRIVATE "SHELL:-include ${ORDER1886_FFX_COMPAT}")
        endif()
    endforeach()
endif()

option(ORDER1886_FSR4_BUILD_GPU_SMOKE "Build manual opt-in offscreen Vulkan smoke (never registered with CTest)" OFF)
if(ORDER1886_FSR4_BUILD_GPU_SMOKE)
    add_executable(order1886_fsr4_gpu_smoke EXCLUDE_FROM_ALL tests/provider_gpu_smoke.cpp)
    target_compile_features(order1886_fsr4_gpu_smoke PRIVATE cxx_std_17)
    target_include_directories(order1886_fsr4_gpu_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../shim")
    target_link_libraries(order1886_fsr4_gpu_smoke PRIVATE order1886_fsr4 Vulkan::Vulkan)
endif()
