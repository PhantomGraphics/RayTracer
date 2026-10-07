@echo off
setlocal
if not defined VULKAN_SDK (
  echo VULKAN_SDK is not set.
  exit /b 1
)
"%VULKAN_SDK%\Bin\glslc.exe" "%~dp0gltf_pbvr.frag" -o "%~dp0gltf_pbvr.frag.spv"
exit /b %ERRORLEVEL%
