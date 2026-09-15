@echo off
setlocal
set "CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.4"
set "CUDA_PATH_V11_4=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.4"
set "CudaToolkitDir=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.4\"
echo BUILD_START %DATE% %TIME% > "D:\llama_win7_cuda\build_patched.log"
"C:\BuildTools2019\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "D:\llama_win7_cuda\llama-cpp-win7\build-win7-cuda114-v142-patched" --config Release --target llama-server -- /m:8 >> "D:\llama_win7_cuda\build_patched.log" 2>&1
echo BUILD_EXIT=%ERRORLEVEL% >> "D:\llama_win7_cuda\build_patched.log"
