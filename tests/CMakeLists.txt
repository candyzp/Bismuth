# Standalone tests for Bismuth's GD-independent rendering core (Linux + Mesa).
#
#   cmake -S tests -B build-tests -G Ninja && ninja -C build-tests && xvfb-run ./build-tests/bismuth_render_test
#
# Uses llvmpipe (OpenGL) and lavapipe (Vulkan) to render the same synthetic level
# with every backend/culling combination and checks that all results match.
cmake_minimum_required(VERSION 3.21)
project(BismuthTests CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 0af55ccecd98d4e5a8d1fad7de25ba429d60e863)
FetchContent_MakeAvailable(glm)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/Bgfx.cmake)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
add_library(bismuth_core STATIC
    ${CMAKE_CURRENT_LIST_DIR}/DiagnosticsStub.cpp
    ${ROOT}/src/gfx/HostGL.cpp
    ${ROOT}/src/gfx/Device.cpp
    ${ROOT}/src/gfx/Compositor.cpp
    ${ROOT}/src/gfx/InteropVulkan.cpp
    ${ROOT}/src/gfx/Programs.cpp
    ${ROOT}/src/gfx/HostTextures.cpp
    ${ROOT}/src/gfx/HostSpriteRenderer.cpp
    ${ROOT}/src/renderer/CpuCuller.cpp
    ${ROOT}/src/renderer/BatchRenderer.cpp
)
target_include_directories(bismuth_core PUBLIC ${ROOT}/src)
target_compile_definitions(bismuth_core PUBLIC BISMUTH_HOST_GLX)
target_link_libraries(bismuth_core PUBLIC bismuth_bgfx bismuth_vulkan_headers glm::glm X11 GL)
target_compile_options(bismuth_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

add_executable(bismuth_render_test render_test.cpp)
target_link_libraries(bismuth_render_test PRIVATE bismuth_core)

add_executable(bismuth_kernel_test kernel_test.cpp)
target_link_libraries(bismuth_kernel_test PRIVATE bismuth_core)
