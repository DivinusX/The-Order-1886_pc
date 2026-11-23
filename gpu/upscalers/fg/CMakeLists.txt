cmake_minimum_required(VERSION 3.22)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(order1886_fg LANGUAGES C CXX)
endif()
find_package(Vulkan REQUIRED)
option(ORDER1886_FG_BUILD_TESTS "Build the CPU provider contract checks" OFF)

# Pull in the vendored FidelityFX Vulkan stack. The frame-generation backend lives
# behind the 3.1.5 scaffold gate; it owns the SDK 2.3 core closure and MUST NOT be
# linked beside the 3.1.5 upscaler in the same binary (duplicate private symbols),
# which is exactly why this is its own dlopen'd provider module.
set(FFX_VK_PORTABLE_BUILD_TESTS OFF)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF)
set(FFX_VK_PORTABLE_BUILD_FSR3_HOST OFF)
set(FFX_VK_PORTABLE_BUILD_FSR3_VK_BACKEND OFF)
set(FFX_VK_PORTABLE_BUILD_FSR3_3_1_5_SCAFFOLD ON)
set(FFX_VK_PORTABLE_BUILD_FSR4_V07_VULKAN OFF)
set(FFX_VK_PORTABLE_INSTALL OFF)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/fsr-vulkan"
                 "${CMAKE_CURRENT_BINARY_DIR}/fidelityfx" EXCLUDE_FROM_ALL)
set_target_properties(ffx_vulkan_portable PROPERTIES POSITION_INDEPENDENT_CODE ON)
foreach (ffg IN ITEMS ffx_vulkan_fsr3_framegen_3_1_6_scaffold
                      ffx_vulkan_fsr3_sdk_core_3_1_5_scaffold
                      ffx_vulkan_fsr3_vk_backend_3_1_5
                      ffx_vulkan_fsr3_vk_framegeneration_3_1_6)
    if (TARGET ${ffg})
        set_target_properties(${ffg} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endforeach()

add_library(order1886_fg SHARED order1886_fg.cpp)
target_compile_features(order1886_fg PRIVATE cxx_std_17)
target_include_directories(order1886_fg PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../shim")
target_link_libraries(order1886_fg PRIVATE ffx-vulkan::fsr3-vk-framegeneration-3.1.6)
set_target_properties(order1886_fg PROPERTIES PREFIX "" OUTPUT_NAME "order1886_fg")

if (MINGW)
    set(ORDER1886_FFX_COMPAT "${CMAKE_CURRENT_SOURCE_DIR}/../ffx_mingw_compat.h")
    foreach (ffd IN ITEMS ffx_vulkan_portable ffx_vulkan_fsr3_framegen_3_1_6_scaffold
                          ffx_vulkan_fsr3_sdk_core_3_1_5_scaffold
                          ffx_vulkan_fsr3_vk_framegeneration_3_1_6 order1886_fg)
        if (TARGET ${ffd})
            target_compile_options(${ffd} PRIVATE "SHELL:-include ${ORDER1886_FFX_COMPAT}")
        endif()
    endforeach()
endif()
