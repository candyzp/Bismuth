cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Bismuth is now an iOS-only renderer experiment. The desktop renderer was the
# original architecture; keeping a second platform backend made the new
# GD-authoritative/GPU-assist design harder to reason about and test.
if (NOT ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS))
    message(FATAL_ERROR "Bismuth now targets iOS only")
endif()

set(CMAKE_OSX_ARCHITECTURES "arm64")
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(Bismuth VERSION 1.0.0)

if (DEFINED ENV{CMAKE_C_COMPILER})
    set(CMAKE_C_COMPILER $ENV{CMAKE_C_COMPILER})
endif()
if (DEFINED ENV{CMAKE_CXX_COMPILER})
    set(CMAKE_CXX_COMPILER $ENV{CMAKE_CXX_COMPILER})
endif()

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

# Keep only the OpenGL ES 2 implementations. These desktop source files remain
# in history for reference, but are no longer part of the supported product.
list(REMOVE_ITEM SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/Buffer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/Shader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/Renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/DifferenceMode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/OverdrawView.cpp"
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

find_library(OPENGLES_FRAMEWORK OpenGLES)
if (NOT OPENGLES_FRAMEWORK)
    message(FATAL_ERROR "OpenGLES framework not found")
endif()
target_link_libraries(${PROJECT_NAME} ${OPENGLES_FRAMEWORK})

target_include_directories(${PROJECT_NAME} PRIVATE src)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode SDK: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})
