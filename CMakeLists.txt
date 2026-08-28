# Cross-platform CMake build for the RayTracer core library (PathTracer),
# its GoogleTest suite (RayTracerTest), and RayTracerView (Vulkan app).
#
# This is a standalone CMake build, separate from the MSBuild/.vcxproj-based
# Windows build (RayTracer/RayTracer/RayTracer.vcxproj + RayTracer/
# RayTracerTest/RayTracerTest.vcxproj, driven by Phantom2026.sln).
# MathCore/GraphicsCore/SpaceCore/GltfRendererCore/etc. are now defined once
# in ../cmake/CrystalCoreLibs.cmake and ../cmake/CrystalVulkanApp.cmake
# (Phase 4 §5.4 item 1 dedup, docs/todo/PLAN_crossplatform_non_cgapp_build.md)
# -- this file just calls them (RayTracerCore itself links against MathCore/
# GraphicsCore/SpaceCore instead of bundling their raw sources, same
# behavior, cleaner) and adds RayTracerTest/RayTracerView on top, so it also
# works standalone (`cmake -S RayTracer -B ...`) exactly as before, and
# identically when add_subdirectory()'d from the root CMakeLists.txt.
#
# Usage (from WSL, in a Linux-native checkout -- avoid /mnt/c/... for speed):
#   cmake -S RayTracer -B RayTracer/build_linux \
#       -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
#       -DCMAKE_BUILD_TYPE=Debug
#   cmake --build RayTracer/build_linux -j
#   ctest --test-dir RayTracer/build_linux --output-on-failure
#
# Prerequisite: libgtest-dev (provides libgtest.a/libgtest_main.a + the GTest
# CMake package config under /usr/lib/x86_64-linux-gnu/cmake/GTest).
#
# NOTE on RayTracer.vcxproj vs RayTracerLib.vcxproj: RayTracer/RayTracer/
# contains two nearly-identical .vcxproj files (RayTracer.vcxproj and
# RayTracerLib.vcxproj), both StaticLibrary configs compiling the exact same
# two sources (pch.cpp + PathTracer.cpp). RayTracerTest.vcxproj and
# RayTracerView.vcxproj both ProjectReference "..\RayTracer\RayTracer.vcxproj"
# (the one actually consumed), so that's the one this file mirrors as
# RayTracerCore. (RayTracerConsole/RayTracer.vcxproj instead references
# "..\RayTracerLib\RayTracerLib.vcxproj", a path that doesn't exist on disk --
# a pre-existing dangling reference, left untouched; RayTracerConsole itself
# is out of scope.)
#
# RayTracerView.vcxproj's real <ProjectReference> list is: GltfRenderer,
# VkAppBase, VulkanGraphics, UIWidgets, Math, Graphics, Space, RayTracer(Core)
# -- no VkRenderer. GltfSceneBuilder.cpp consumes Crystal::Gltf::
# GltfAccessorView/GltfTypes directly, so this needs the full
# GltfRendererCore chain.

cmake_minimum_required(VERSION 3.20)

project(RayTracerCoreLinux LANGUAGES CXX)

# Matches RayTracer.vcxproj/RayTracerTest.vcxproj: both explicitly override
# <LanguageStandard> to stdcpp20 (unlike Numerics/Scene/Volume, which fall
# back to BuildConfig.props' repo-wide C++17 default).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(RAYTRACER_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(CGLIB_ROOT ${REPO_ROOT}/CGLib)

if(MSVC)
    add_compile_options(/utf-8)
    set(CRYSTAL_WARN_FLAGS /W3)
else()
    set(CRYSTAL_WARN_FLAGS -Wall)
endif()

list(APPEND CMAKE_MODULE_PATH ${REPO_ROOT}/cmake)
include(PhantomGTest)
include(PhantomCoreLibs)
include(PhantomVulkanApp)

crystal_add_math_core()
crystal_add_graphics_core()
crystal_add_space_core()

add_library(RayTracerCore STATIC
    ${RAYTRACER_ROOT}/RayTracer/PathTracer.cpp
)
target_include_directories(RayTracerCore PRIVATE ${REPO_ROOT})
target_link_libraries(RayTracerCore PUBLIC MathCore GraphicsCore SpaceCore)
target_compile_options(RayTracerCore PRIVATE ${CRYSTAL_WARN_FLAGS})

# --- RayTracerTest (GoogleTest) ---

crystal_find_gtest()

if(CRYSTAL_GTEST_FOUND)
    enable_testing()

    add_executable(RayTracerTest
        ${RAYTRACER_ROOT}/RayTracerTest/PathTracerTest.cpp
    )
    target_include_directories(RayTracerTest PRIVATE
        ${REPO_ROOT}
        ${RAYTRACER_ROOT}/RayTracer
        ${RAYTRACER_ROOT}/RayTracerTest
    )
    target_link_libraries(RayTracerTest PRIVATE
        RayTracerCore
        GTest::gtest
        GTest::gtest_main
    )
    target_compile_options(RayTracerTest PRIVATE ${CRYSTAL_WARN_FLAGS})

    include(GoogleTest)
    gtest_discover_tests(RayTracerTest)
else()
    message(WARNING "GTest not found -- skipping RayTracerTest target")
endif()

# ---------------------------------------------------------------------------
# RayTracerView (standalone ImGui + Vulkan viewer) -- Phase 2 item 8.
# ---------------------------------------------------------------------------

crystal_find_vulkan_headers()

if(CRYSTAL_VULKAN_INCLUDE_DIR)

crystal_find_vulkan_loader()
crystal_find_glfw_loader()

if(CRYSTAL_VULKAN_LIBRARY AND CRYSTAL_GLFW_LIBRARY)

crystal_add_vkappbase_core()
crystal_add_gltfrenderer_core()

add_executable(RayTracerView
    ${RAYTRACER_ROOT}/RayTracerView/main.cpp
    ${RAYTRACER_ROOT}/RayTracerView/RayTracerApp.cpp
    ${RAYTRACER_ROOT}/RayTracerView/MenuPanel.cpp
    ${RAYTRACER_ROOT}/RayTracerView/RayTraceResultPanel.cpp
    ${RAYTRACER_ROOT}/RayTracerView/GltfSceneBuilder.cpp
    ${RAYTRACER_ROOT}/RayTracerView/CommandDispatcher.cpp
)
target_include_directories(RayTracerView PRIVATE
    ${CGLIB_ROOT}/ThirdParty/imgui
    ${CGLIB_ROOT}/UIWidgets
    ${RAYTRACER_ROOT}/RayTracer
)
target_link_libraries(RayTracerView PRIVATE
    RayTracerCore
    GltfRendererCore
    VkAppBaseCore
    ${CRYSTAL_VULKAN_LIBRARY}
    ${CRYSTAL_GLFW_LIBRARY}
)
target_compile_options(RayTracerView PRIVATE ${CRYSTAL_WARN_FLAGS})

# RayTracerView has no shaders/ dir of its own -- RayTracerView.vcxproj's
# PostBuildEvent xcopies Crystal/GltfViewer/shaders/*.spv (it reuses
# GltfSceneRenderer's pipeline via GltfSceneBuilder.cpp, not a bespoke shader set).
add_custom_command(TARGET RayTracerView POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CGLIB_ROOT}/GltfViewer/shaders
        $<TARGET_FILE_DIR:RayTracerView>/shaders
)

else()
    message(WARNING "RayTracer: Vulkan and/or GLFW loader not found -- RayTracerView is skipped.")
endif()

else()
    message(WARNING "RayTracer: vulkan/vulkan.h not found -- RayTracerView is skipped.")
endif()
