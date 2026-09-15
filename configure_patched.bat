@echo off
setlocal
set "CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.4"
set "CUDA_PATH_V11_4=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.4"
set "CudaToolkitDir=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.4\"
set "CM=C:\BuildTools2019\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
"%CM%" -S "D:/llama_win7_cuda/llama-cpp-win7" -B "D:/llama_win7_cuda/llama-cpp-win7/build-win7-cuda114-v142-patched" -G "Visual Studio 16 2019" -A x64 -T v142 -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=75 -DCUDAToolkit_ROOT="C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v11.4" -DCMAKE_CUDA_COMPILER="C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v11.4/bin/nvcc.exe" -DGGML_NATIVE=OFF -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_OPENMP=ON -DCMAKE_BUILD_TYPE=Release -DLLAMA_BUILD_SERVER=ON -DLLAMA_BUILD_TOOLS=ON -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_APP=OFF -DLLAMA_BUILD_MTMD=OFF -DLLAMA_BUILD_UI=OFF
echo CMAKE_EXIT=%ERRORLEVEL%
