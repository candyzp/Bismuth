cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(Bismuth VERSION 1.0.0)

# The iOS backend contains Objective-C++ for Metal/CoreVideo interop.
set(BISMUTH_IOS_BUILD FALSE)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(BISMUTH_IOS_BUILD TRUE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 23)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

# I want to use `geode build` with clang but this is the only way to do it :(
message(STATUS "Found Geode: $ENV{CMAKE_C_COMPILER}")
if (DEFINED ENV{CMAKE_C_COMPILER})
    set(CMAKE_C_COMPILER $ENV{CMAKE_C_COMPILER})
endif()
if (DEFINED ENV{CMAKE_CXX_COMPILER})
    set(CMAKE_CXX_COMPILER $ENV{CMAKE_CXX_COMPILER})
endif()

# Add all source files inside src (recursively)
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
if (BISMUTH_IOS_BUILD)
    # OpenCL's WGL loader is Windows-only. The iOS backend uses Metal instead.
    list(FILTER SOURCES EXCLUDE REGEX ".*/cl\\.cpp$")
    file(GLOB_RECURSE OBJCXX_SOURCES CONFIGURE_DEPENDS src/*.mm)
    list(APPEND SOURCES ${OBJCXX_SOURCES})
endif()

# Set up the mod binary
add_library(${PROJECT_NAME} SHARED ${SOURCES})

# Not sure about this...............
set(CMAKE_BUILD_TYPE RelWithDebInfo)
# set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# Libraries
set(ENABLE_OPT 0)
set(ENABLE_GLSLANG_BINARIES OFF)

include(FetchContent)

FetchContent_Declare(
	glm
	GIT_REPOSITORY	https://github.com/g-truc/glm.git
	GIT_TAG 	0af55ccecd98d4e5a8d1fad7de25ba429d60e863
)
FetchContent_Declare(glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git GIT_TAG vulkan-sdk-1.4.335)
if (NOT BISMUTH_IOS_BUILD)
    FetchContent_Declare(
        OpenCL-Headers
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenCL-Headers.git
        GIT_TAG v2026.05.29
    )
    FetchContent_MakeAvailable(glm glslang OpenCL-Headers)
    target_link_libraries(${PROJECT_NAME} glm::glm glslang::glslang OpenCL::Headers)
else()
    FetchContent_MakeAvailable(glm glslang)
    target_link_libraries(${PROJECT_NAME} glm::glm glslang::glslang)

    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
    find_library(OPENGLES_FRAMEWORK OpenGLES REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${FOUNDATION_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${COREVIDEO_FRAMEWORK}
        ${OPENGLES_FRAMEWORK}
    )
    target_compile_options(${PROJECT_NAME} PRIVATE
        "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
    )
endif()
target_include_directories(${PROJECT_NAME} PRIVATE src)

# Other Geode stuff

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# Set up dependencies, resources, and link Geode.
setup_geode_mod(${PROJECT_NAME})
