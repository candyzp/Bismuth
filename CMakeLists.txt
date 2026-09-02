cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BISMUTH_IOS OFF)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(BISMUTH_IOS ON)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(Bismuth VERSION 1.0.0)

message(STATUS "Found Geode: $ENV{CMAKE_C_COMPILER}")
if (DEFINED ENV{CMAKE_C_COMPILER})
    set(CMAKE_C_COMPILER $ENV{CMAKE_C_COMPILER})
endif()
if (DEFINED ENV{CMAKE_CXX_COMPILER})
    set(CMAKE_CXX_COMPILER $ENV{CMAKE_CXX_COMPILER})
endif()

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

# iOS uses a dedicated OpenGL ES 2 backend. Keep the original desktop
# renderer untouched and replace only the platform-specific implementation.
if (BISMUTH_IOS)
    list(REMOVE_ITEM SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/Buffer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/Shader.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/Renderer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/DifferenceMode.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/OverdrawView.cpp"
    )
endif()

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

if (BISMUTH_IOS)
    target_include_directories(${PROJECT_NAME} BEFORE PRIVATE libs/ios-shims)
    find_library(OPENGLES_FRAMEWORK OpenGLES)
    if (NOT OPENGLES_FRAMEWORK)
        message(FATAL_ERROR "OpenGLES framework not found")
    endif()
    target_link_libraries(${PROJECT_NAME} ${OPENGLES_FRAMEWORK})
else()
    FetchContent_Declare(
        glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG vulkan-sdk-1.4.335
    )
    FetchContent_MakeAvailable(glslang)
    target_link_libraries(${PROJECT_NAME} glslang::glslang)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE src)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})
