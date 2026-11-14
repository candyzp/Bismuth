cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Bismuth is now an iOS-only renderer experiment. The desktop backend has been
# retired so the GD-authoritative/GPU-assist architecture has one platform and
# one set of rendering assumptions to maintain.
if (NOT ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS))
    message(FATAL_ERROR "Bismuth now targets iOS only")
endif()

set(CMAKE_OSX_ARCHITECTURES "arm64")
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(Bismuth VERSION 1.0.0)

# The iOS Metal/OpenGL ES bridge is implemented in Objective-C++.
if (CMAKE_SYSTEM_NAME STREQUAL "iOS" OR IOS)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 23)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

if (DEFINED ENV{CMAKE_C_COMPILER})
    set(CMAKE_C_COMPILER $ENV{CMAKE_C_COMPILER})
endif()
if (DEFINED ENV{CMAKE_CXX_COMPILER})
    set(CMAKE_CXX_COMPILER $ENV{CMAKE_CXX_COMPILER})
endif()

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(APPEND SOURCES
    src/renderer/ios_metal/IOSMetalInterop.mm
    src/renderer/ios_metal/IOSRenderMethodProbe.mm
)
set_source_files_properties(
    src/renderer/ios_metal/IOSMetalInterop.mm
    src/renderer/ios_metal/IOSRenderMethodProbe.mm
    PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc"
        COMPILE_DEFINITIONS "GEODE_IS_IOS"
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})
set(CMAKE_BUILD_TYPE Release)

set(ENABLE_OPT 0)
set(ENABLE_GLSLANG_BINARIES OFF)

include(FetchContent)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 0af55ccecd98d4e5a8d1fad7de25ba429d60e863
)
FetchContent_MakeAvailable(glm)
target_link_libraries(${PROJECT_NAME} glm::glm)

# Some shared Bismuth headers still include Cocos' historical Win32 GL include
# path. This shim maps that include surface directly to Apple's OpenGL ES 2 API;
# it is not a Windows backend.
target_include_directories(${PROJECT_NAME} BEFORE PRIVATE libs/ios-shims)

find_library(OPENGLES_FRAMEWORK OpenGLES REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)

target_link_libraries(
    ${PROJECT_NAME}
    ${OPENGLES_FRAMEWORK}
    ${METAL_FRAMEWORK}
    ${COREVIDEO_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
)

target_include_directories(${PROJECT_NAME} PRIVATE src src/renderer)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode SDK: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})

# Geode's 2.2081 iOS codegen emits standalone FMOD wrappers into
# GeneratedSource.cpp, but that translation unit does not include FMOD's
# declarations. The SDK already supplies the canonical header under
# Geode/fmod; force-include it for the generated bindings target.
if (CMAKE_SYSTEM_NAME STREQUAL "iOS" AND TARGET GeodeBindings)
    target_compile_options(GeodeBindings PRIVATE
        "-include"
        "Geode/fmod/fmod.hpp"
    )
endif()
