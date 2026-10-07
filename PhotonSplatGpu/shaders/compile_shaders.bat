@echo off
setlocal
if not defined VULKAN_SDK (
    echo VULKAN_SDK is not set.
    exit /b 1
)
pushd "%~dp0"
for %%f in (gbuffer.vert gbuffer.frag splat.vert splat.frag) do (
    "%VULKAN_SDK%\Bin\glslc.exe" -I . "%%f" -o "%%f.spv"
    if errorlevel 1 (
        popd
        exit /b 1
    )
)
popd
