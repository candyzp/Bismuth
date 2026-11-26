
cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# CMake reads the target architecture while initializing the toolchain, so
# choose the iOS slice before project() enables the C/C++ compilers.
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

project(Kyanite VERSION 1.0.0)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    # The iOS port is intentionally isolated from the desktop OpenGL 4.3
    # renderer. Geometry Dash's stock renderer remains authoritative while
    # the Metal object-rendering backend is brought up in stages.
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 23)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_BUILD_TYPE Release)

    set(SOURCES
        src/ios/Main.cpp
        src/renderer/ios/MetalBackend.mm
        src/renderer/ios/MetalDiagnosticsOverlay.cpp
        src/renderer/ios/hooks.cpp
    )

    add_library(${PROJECT_NAME} SHARED ${SOURCES})

    set_source_files_properties(
        src/renderer/ios/MetalBackend.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc"
    )

    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(UIKIT_FRAMEWORK UIKit REQUIRED)
    find_library(METAL_KIT_FRAMEWORK MetalKit REQUIRED)
    find_library(OPENGLES_FRAMEWORK OpenGLES REQUIRED)
    # Keep the plain signature because setup_geode_mod() adds plain
    # target_link_libraries() entries to this target as well.
    target_link_libraries(${PROJECT_NAME}
        ${METAL_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
        ${UIKIT_FRAMEWORK}
        ${METAL_KIT_FRAMEWORK}
        ${OPENGLES_FRAMEWORK}
    )
    target_include_directories(${PROJECT_NAME} PRIVATE src)
else()
    file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
    add_library(${PROJECT_NAME} SHARED ${SOURCES})

    if (MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE /Ob3 /Oi /Ot /Gw /Gy /Zc:inline)
    endif()

    # Link-time code generation is intentionally disabled: /GL can remove
    # inline assembly stubs used by Geode bindings.
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
    set(ENABLE_OPT 0)
    set(ENABLE_GLSLANG_BINARIES OFF)

    include(FetchContent)
    FetchContent_Declare(
        glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG 0af55ccecd98d4e5a8d1fad7de25ba429d60e863
    )
    FetchContent_Declare(
        glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG vulkan-sdk-1.4.335
    )
    FetchContent_Declare(
        OpenCL-Headers
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenCL-Headers.git
        GIT_TAG v2026.05.29
    )
    FetchContent_MakeAvailable(glm glslang OpenCL-Headers)
    target_link_libraries(${PROJECT_NAME} glm::glm glslang::glslang OpenCL::Headers)
    target_include_directories(${PROJECT_NAME} PRIVATE src)
endif()

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode SDK: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})
