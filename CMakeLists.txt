# Cross-platform CMake build for the RayTracer core library (PathTracer / PhotonMapper),
# its GoogleTest suite (RayTracerTest), and RayTracerView (Vulkan app).
#
# This is a standalone CMake build, separate from the MSBuild/.vcxproj-based
# Windows build (RayTracer/RayTracer/RayTracer.vcxproj + RayTracer/
# RayTracerTest/RayTracerTest.vcxproj, driven by Phantom2026.sln).
# MathCore/GraphicsCore/SpaceCore/GltfRendererCore/etc. are now defined once
# in ../cmake/PhantomCoreLibs.cmake and ../cmake/PhantomVulkanApp.cmake
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
# -- no VkRenderer. GltfSceneBuilder.cpp consumes Phantom::Gltf::
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
    set(PHANTOM_WARN_FLAGS /W3)
else()
    set(PHANTOM_WARN_FLAGS -Wall)
endif()

list(APPEND CMAKE_MODULE_PATH ${REPO_ROOT}/cmake)
include(PhantomGTest)
include(PhantomCoreLibs)
include(PhantomVulkanApp)

phantom_add_math_core()
phantom_add_graphics_core()
phantom_add_space_core()

add_library(RayTracerCore STATIC
    ${RAYTRACER_ROOT}/RayTracer/PathTracer.cpp
    ${RAYTRACER_ROOT}/RayTracer/PhotonMapper.cpp
    ${RAYTRACER_ROOT}/RayTracer/PbvrResampler.cpp
)
target_include_directories(RayTracerCore PRIVATE ${REPO_ROOT})
target_link_libraries(RayTracerCore PUBLIC MathCore GraphicsCore SpaceCore)
target_compile_options(RayTracerCore PRIVATE ${PHANTOM_WARN_FLAGS})

# CPU surface-particle transport experiment; no Vulkan dependency.
add_executable(CornellSurfaceTransport
    ${RAYTRACER_ROOT}/CornellSurfaceTransport/main.cpp
)
target_link_libraries(CornellSurfaceTransport PRIVATE RayTracerCore)
target_compile_options(CornellSurfaceTransport PRIVATE ${PHANTOM_WARN_FLAGS})
enable_testing()
add_test(NAME CornellSurfaceTransport.Checks COMMAND CornellSurfaceTransport --check)

# --- RayTracerTest (GoogleTest) ---

phantom_find_gtest()

if(PHANTOM_GTEST_FOUND)
    enable_testing()

    add_executable(RayTracerTest
        ${RAYTRACER_ROOT}/RayTracerTest/PathTracerTest.cpp
        ${RAYTRACER_ROOT}/RayTracerTest/PhotonMapperTest.cpp
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
    target_compile_options(RayTracerTest PRIVATE ${PHANTOM_WARN_FLAGS})

    include(GoogleTest)
    gtest_discover_tests(RayTracerTest)
else()
    message(WARNING "GTest not found -- skipping RayTracerTest target")
endif()

# ---------------------------------------------------------------------------
# RayTracerView (standalone ImGui + Vulkan viewer) -- Phase 2 item 8.
# ---------------------------------------------------------------------------

phantom_find_vulkan_headers()

if(PHANTOM_VULKAN_INCLUDE_DIR)

phantom_find_vulkan_loader()
phantom_find_glfw_loader()

if(PHANTOM_VULKAN_LIBRARY)
    phantom_add_vulkangraphics_core()
    add_library(RayTracerPhotonGpu STATIC
        ${RAYTRACER_ROOT}/PhotonSplatGpu/PhotonSplatGpu.cpp
        ${RAYTRACER_ROOT}/PhotonSplatGpu/DepthPhotonTransport.cpp)
    target_link_libraries(RayTracerPhotonGpu PUBLIC RayTracerCore VulkanGraphicsCore ${PHANTOM_VULKAN_LIBRARY})
    target_compile_options(RayTracerPhotonGpu PRIVATE ${PHANTOM_WARN_FLAGS})
    target_link_libraries(CornellSurfaceTransport PRIVATE RayTracerPhotonGpu)
    target_compile_definitions(CornellSurfaceTransport PRIVATE PHOTON_SPLAT_VULKAN=1)
    add_custom_target(CornellPhotonShaders
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${RAYTRACER_ROOT}/PhotonSplatGpu/shaders
            $<TARGET_FILE_DIR:CornellSurfaceTransport>/photon_splat_shaders)
    add_dependencies(CornellSurfaceTransport CornellPhotonShaders)
    if(PHANTOM_GTEST_FOUND)
        add_executable(PhotonSplatGpuTest ${RAYTRACER_ROOT}/PhotonSplatGpu/PhotonSplatGpuTest.cpp)
        target_link_libraries(PhotonSplatGpuTest PRIVATE RayTracerPhotonGpu GTest::gtest GTest::gtest_main)
        target_compile_definitions(PhotonSplatGpuTest PRIVATE
            PHOTON_SPLAT_TEST_SHADERS="${RAYTRACER_ROOT}/PhotonSplatGpu/shaders")
        target_compile_options(PhotonSplatGpuTest PRIVATE ${PHANTOM_WARN_FLAGS})
        gtest_discover_tests(PhotonSplatGpuTest)
    endif()
endif()

if(PHANTOM_VULKAN_LIBRARY AND PHANTOM_GLFW_LIBRARY)

phantom_add_vkappbase_core()
phantom_add_gltfrenderer_core()

add_executable(RayTracerView
    ${RAYTRACER_ROOT}/RayTracerView/main.cpp
    ${RAYTRACER_ROOT}/RayTracerView/RayTracerApp.cpp
    ${RAYTRACER_ROOT}/RayTracerView/MenuPanel.cpp
    ${RAYTRACER_ROOT}/RayTracerView/RayTraceResultPanel.cpp
    ${RAYTRACER_ROOT}/RayTracerView/PbvrPreviewPanel.cpp
    ${RAYTRACER_ROOT}/RayTracerView/GltfPbvrPreview.cpp
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
    RayTracerPhotonGpu
    GltfRendererCore
    VkAppBaseCore
    ${PHANTOM_VULKAN_LIBRARY}
    ${PHANTOM_GLFW_LIBRARY}
)
target_compile_options(RayTracerView PRIVATE ${PHANTOM_WARN_FLAGS})

# Shared vertex/skybox shaders and viewer-specific PBVR fragment shader.
add_custom_command(TARGET RayTracerView POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CGLIB_ROOT}/GltfViewer/shaders
        $<TARGET_FILE_DIR:RayTracerView>/shaders
)
add_custom_command(TARGET RayTracerView POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${RAYTRACER_ROOT}/PhotonSplatGpu/shaders
        $<TARGET_FILE_DIR:RayTracerView>/photon_splat_shaders)
find_program(RAYTRACER_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(RAYTRACER_GLSLC)
set(RAYTRACER_PBVR_SPV ${CMAKE_CURRENT_BINARY_DIR}/raytracer_shaders/gltf_pbvr.frag.spv)
add_custom_command(OUTPUT ${RAYTRACER_PBVR_SPV}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/raytracer_shaders
    COMMAND ${RAYTRACER_GLSLC} ${RAYTRACER_ROOT}/RayTracerView/shaders/gltf_pbvr.frag -o ${RAYTRACER_PBVR_SPV}
    DEPENDS ${RAYTRACER_ROOT}/RayTracerView/shaders/gltf_pbvr.frag VERBATIM)
add_custom_target(RayTracerViewShaders DEPENDS ${RAYTRACER_PBVR_SPV})
add_dependencies(RayTracerView RayTracerViewShaders)
else()
    # Ship SPIR-V so end users do not need the shader compiler.
    set(RAYTRACER_PBVR_SPV ${RAYTRACER_ROOT}/RayTracerView/shaders/gltf_pbvr.frag.spv)
endif()
add_custom_command(TARGET RayTracerView POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:RayTracerView>/raytracer_shaders
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RAYTRACER_PBVR_SPV} $<TARGET_FILE_DIR:RayTracerView>/raytracer_shaders/gltf_pbvr.frag.spv
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${RAYTRACER_ROOT}/RayTracerView/samples $<TARGET_FILE_DIR:RayTracerView>/samples)

else()
    message(WARNING "RayTracer: Vulkan and/or GLFW loader not found -- RayTracerView is skipped.")
endif()

else()
    message(WARNING "RayTracer: vulkan/vulkan.h not found -- RayTracerView is skipped.")
endif()

# Precompiled headers (cmake/PhantomPch.cmake): RayTracerView's own pch.h. RayTracerCore
# and RayTracerTest have too few TUs for a PCH to pay for itself.
include(PhantomPch)
phantom_use_pch(RayTracerView ${RAYTRACER_ROOT}/RayTracerView/pch.h)
