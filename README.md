# llama.cpp Win7 + CUDA 11.4 适配版

基于 [llama.cpp](https://github.com/ggml-org/llama.cpp) master 分支修改，专为 **Windows 7 x64 + NVIDIA CUDA 11.4** 环境适配。

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://opensource.org/licenses/MIT)

## 背景

llama.cpp 官方已不再支持 Windows 7，且 CUDA 11.4 的 WDDM 1.1 驱动与新版 CUDA Toolkit 存在兼容性问题。本项目针对这些限制做了大量适配，使其能在老旧系统上正常运行。

源码基线约为上游 2026-10-09 的 master（b11527 前后），本仓库在其上共 15 个提交（含文档整理）。需要说明的是：**追平上游并不会更快**。用同一套工具链编译上游最新 master（b12155）做锁频对比，在本卡上 MTP 模式反而慢约 12%，而本仓库的 MMVQ Turing 补丁是上游没有的。

## 测试环境

| 项目 | 详情 |
|------|------|
| **GPU** | NVIDIA RTX 2080 Ti（**魔改 22GB 显存**，sm_75，标称带宽 616 GB/s） |
| **系统** | Windows 7 x64 SP1，驱动 475.xx |
| **CUDA** | 11.4.4（WDDM 1.1） |
| **编译器** | Visual Studio 2019 Build Tools / MSVC v142 |
| **运行时** | `/MT` 静态链接 CRT |
| **测试模型** | `Qwen3.8-27B-UD-Q4_K_M.gguf`（15.33 GB，架构 qwen35）+ `FastMTP-32K` 草稿模块（0.84 GB）+ `mmproj-Q8_0` |

涉及三台机器，性能读数均在 Win7 上取得：

- **开发机**：i5-14400F / 32 GB / 2080 Ti 22G / Win10（双启 Win7）
- **生产机**：E5-2666 v3 / X99 / DDR3 ECC 32 GB / 同款卡 / Win7 / 装有云桌面客户端
- **对照机**：E5-2678 v3 / X99 / 64 GB / 同款卡 / Win7

**仅测试了上述环境。** 其他 GPU、驱动版本、模型未做测试，可能存在各种 bug。

## 使用方式

```powershell
# 启动服务器（fit_params 会自动根据显存调整上下文大小）
C:\llama-server\llama-server.exe -m F:\models\model.gguf --host 0.0.0.0 --port 8080

# 如果显存不足，手动指定上下文
C:\llama-server\llama-server.exe -m F:\models\model.gguf --host 0.0.0.0 --port 8080 -c 8192

# 纯 CPU 推理（不推荐，仅用于调试）
C:\llama-server\llama-server.exe -m F:\models\model.gguf -ngl 0 --host 0.0.0.0 --port 8080

# 启用 MTP 推测解码加速（Qwen3.5/3.6/3.8 等自带 MTP 头的模型）
C:\llama-server\llama-server.exe -m F:\models\model.gguf --spec-type draft-mtp --spec-draft-n-max 2 --host 0.0.0.0 --port 8080

# 如遇 CUDA VMM 兼容问题，可禁用 VMM
$env:GGML_CUDA_NO_VMM = '1'
```

### 22GB 卡的实测推荐配置

这是目前在 2080 Ti 22G + Qwen3.8-27B Q4_K_M 上跑下来的最优组合，逐条都有实测依据（见[性能实测](#性能实测)）：

```powershell
llama-server.exe `
    -m  C:\models\Qwen3.8\Qwen3.8-27B-UD-Q4_K_M.gguf `
    -md C:\models\Qwen3.8\Qwen3.8-27B-FastMTP-32K.gguf `
    --host 127.0.0.1 --port 8080 `
    --ctx-size 16384 `
    -ngl 99 `
    --flash-attn on `
    --spec-type draft-mtp,ngram-mod `
    --spec-draft-n-max 2 `
    --parallel 1 `
    --batch-size 2048 --ubatch-size 512 `
    --threads 8 --threads-batch 10 `
    --no-mmap `
    --jinja
```

按用途微调：

| 场景 | 改法 | 依据 |
|------|------|------|
| 纯聊天、大量非重复散文 | `--spec-type draft-mtp`（去掉 ngram） | ngram 对散文无收益，徒增提案开销 |
| 输出高度重复（改代码、填表、模板） | `--spec-type draft-mtp,ngram-simple` | 代码更快，但散文慢约 2% |
| 上下文 > 32k、显存吃紧 | 加 `--cache-type-k q8_0 --cache-type-v q8_0` | 16k 以内 f16 KV 更快，超过才值得换 q8_0 |
| 显存装不下目标模型 | 换 IQ4_XS 量化 | 省约 2.1 GiB，解码只慢约 1% |

`--spec-draft-n-max` 对单 MTP 头模型的最优值是 **2**；叠加 ngram 后 `n=3` 反而更差。

> 推测解码是**验证式**的：输出与不开投机逐字节一致，加速不牺牲质量。

## 主要改动

### 1. Win7 API 兼容
- 目标 `_WIN32_WINNT=0x0601` / `WINVER=0x0601`
- 替换 Win8+ API：移除 `PrefetchVirtualMemory`、`GetFileInformationByHandleEx`、`CreateFile2` 等静态导入
- cpp-httplib 网络库适配 Win7（`select()` 替代 `WSAPoll`，`WSAStringToAddressA` 替代 `inet_pton`）
- 替换 `std::filesystem` 调用为 Win32 API（避免 `GetFileInformationByHandleEx` 导入）

### 2. `/MT` 跨 DLL CRT 冲突修复
- **问题**：静态链接 CRT 时，`ggml-base.dll` 用 `_wfopen` 创建的 `FILE*`，被 `llama.dll` 的 `_get_osfhandle` 调用时，因各 DLL 有独立的 CRT 句柄表而崩溃
- **修复**：`llama_file::impl` 改用 `CreateFileW` 直接获取 HANDLE，再通过 `_open_osfhandle` + `_fdopen` 转 CRT `FILE*`，全程在 `llama.dll` 自己的 CRT 内完成

### 3. CUDA 11.4 兼容
- 多显卡架构编译：默认构建 Compute Capability 6.1 / 7.5 / 8.6 三档原生 SASS，详见[多显卡架构编译](#多显卡架构编译推荐)
- CUDA VMM 驱动层虚拟内存管理：`cuMemAddressReserve` + `cuMemCreate`（每次 ≤3GB）+ `cuMemMap`
- VMM API 运行期动态解析（`GetProcAddress`），避免静态导入不兼容符号
- `GGML_CUDA_NO_VMM` 环境变量可运行时禁用 VMM

### 4. `fit_params` 自动显存适配
- 默认开启，自动根据设备可用显存调整 `n_gpu_layers` 和 `n_ctx`
- 避免手动猜测上下文大小

### 5. `llama_model_load` 防内联
- 添加 `__declspec(noinline)` 防止 MSVC Release 优化导致的大栈帧崩溃

### 6. 未处理异常捕获
- `common_init()` 注册 `SetUnhandledExceptionFilter`，崩溃时输出异常码和地址

### 7. 推测解码（MTP / DSpark / FastMTP）
- **MTP 增强**：新增 `llama_model_params.load_mtp` 按需加载 MTP 头；`qwen35` / `qwen35moe` 支持 `TENSOR_SKIP` 跳过 MTP 张量；启用 `--spec-type draft-mtp` 时按需加载 MTP 层
- **DSpark 推测解码**：新增 `draft-dspark` 类型，DFlash 骨干 + 半自回归 Markov head + Confidence head；通过 `--spec-type draft-dspark` 显式启用
- **FastMTP 草稿模块**：`qwen35` 支持 GGUF 里的 `d2t` 张量做 draft-vocab trim——草稿头只在小词表上出 logits，再映射回全词表（`src/models/qwen35.cpp`）。配套的 `HauhauCS-FastMTP-llama.cpp.patch` 保留在仓库根目录，供同步上游后重新套用
- **省显存**：给了 `-md` 草稿模型时不再加载目标模型的 MTP 头。drafting 全在 `ctx_dft` 里跑，目标的 nextn 层根本不会执行，之前白占约 330 MiB 常驻显存（`21397 -> 21069 MiB`，输出与速度不变）

### 8. MMVQ 量化解码提速（Turing 专项，上游没有）
投机解码一步要验证多列激活，而 MMVQ kernel 原本每列都把**权重侧**重算一遍（nibble 提取 + 6-bit scale/min 解包），这部分其实与列无关。

- **权重反量化外提**：改成每行算一次、各列共享。先对 q4_K / q5_K / q6_K / iq4_XS 做了，随后复测把范围收敛到 **q4_K 和 q5_K**：q6_K 的权重侧只有一次 `dp4a`，外提反而慢约 1%；iq4_XS 连激活侧一起外提会多占寄存器，慢 6%。两条负收益路径都已回退。`ncols_dst == 1`（非投机）仍走原代码路径
- **Turing 每 block 处理 4 行**：参数表原本在 2~4 列时用 2 rows/block，导致权重矩阵的突发读取不够长——q4_K 在 n=3 时只有 381 GB/s，而单列能跑到 469 GB/s。改为 4 rows/block 后各档全面提升（q4_K +14%/+21%、iq4_XS +15%/+24%、q5_K +9%/+15%、iq3_S +12%、q6_K +5%/+13%）；再往上试 8 rows 又变慢，所以停在 4
- kernel 层 q4_K（m=4096, k=14336）：n=3 `87.2 -> 74.9 us`，n=4 `107.4 -> 84.7 us`
- 端到端：**Q4_K_M +5.9%、IQ3_S +11.6%、Q4_K_XL +4.6%**
- `test-backend-ops` MUL_MAT 1132/1134，2 个失败是既有的 `iq1_m`（那些 shape 不走 MMVQ）

> 复测注意：显存空闲时会掉到 810 MHz，单次对比的读数没有意义。要么锁显存频率，要么让 GPU 除基准外完全空闲。

### 9. 移植的上游正确性修复
以下几条**不是提速**，是修复会在长会话里咬人的缺陷：

- `ggml-cuda`：正在使用的计算图不再被 eviction sweep 淘汰（上游 `00f113ec`）
- `speculative`：补上 MTP hidden state 的 get/set，slot 存取档时草稿状态才能正确恢复（`0f35d6d7`）
- `server`：按 draft 长度而不是 rollback size 选 spec checkpoint（`7df8b876`）
- `delta-net`：conv 回滚槽改为一次 `set_rows` 写入，替代 K 次独立拷贝（`040ce199`）
- `ggml`：去掉基于 shape 的 view 边界断言，它会误拒合法的 strided view

移植前后输出与 draft 接受率完全一致（锁 1500 MHz：34.52 vs 34.53 t/s）。

### 10. 代码审计修复
- **top-k**：修正 CUDA 侧的派发；`GGML_CUDA_USE_CUB` 路径同样上报宽行 top-k 支持，并在 `ggml_cuda_top_k_supported` 拒绝非 f32 / 非连续输入
- **DFlash2 不等长 draft block**：批次折叠成 `[tokens_per_block, n_seqs]` 时，多个槽因剩余上下文不同而要求不同 `n_max`，批次会被切成 block 边界错乱的 ubatch，**草稿质量静默劣化**。现按所有 drafting 序列夹紧 block 长度。单草稿槽不受影响，MTP 不用 block 折叠
- `nextn` 层的 reserve、`fit` 的 MTP 层计数、Win7 下的文件写入路径各修一处
- `conversion`：GGUF 缺 `head_dim` 时回退用 `hidden_size / n_head`
- 回归：`test-backend-ops` TOP_K 375/375、ARGSORT 46/46、SET_ROWS 通过；2~3 槽 DFlash2 混合 `max_tokens`（含 1492 token 长 prompt）跑干净

## 性能实测

机器：RTX 2080 Ti 22G / i5-14400F / Win7 / CUDA 11.4。
口径：`llama-server`，`-c 8192 -ngl 99 -fa on --parallel 1 -b 2048 -ub 512 --no-mmap --jinja --temp 0`，取 `/v1/chat/completions` 的 `timings.predicted_per_second`，默认 boost，单次运行，**噪声约 ±5%**。

### 投机解码各配置（Qwen3.8-27B Q4_K_M，t/s）

| 配置 | 散文 | 代码（128 tok） | 代码（256 tok） |
|------|-----:|-----:|-----:|
| 内置 MTP `n=2` | 34.5 | 44.8 | 42.8 |
| FastMTP 模块 `n=2` | 37.1 | 50.0 | 42.3 |
| **FastMTP + ngram-mod `n=2`** | **37.3** | **75.1** | **50.0** |

- `--spec-type` 支持逗号分隔叠加，多个 speculator 按优先级串联，ngram 系优先于 MTP
- ngram 对**代码 / 结构化输出**收益最大（+18% ~ +50%，相对内置 MTP 最高 +72%），因为重复片段只需一次哈希查找就能命中；**散文持平**
- FastMTP 模块本身散文 +7% ~ +12%，长代码输出在噪声内持平
- `n=3` 只在纯内置 MTP 下略有优势，叠加 ngram 后更差
- 生产配置（`-c 110592` + KV q8_0 + FastMTP + `draft-mtp,ngram-mod`）稳定在 **44 t/s 左右**，比只用内置 MTP 的 33 t/s 高约 33%

### 上下文长度不惩罚解码

同一配置，`-c 32768` 填约 13k token 对比 `-c 8192` 空上下文：**37.5 vs 37.1 t/s**。该混合模型 64 层里有 48 层是 GDN 线性注意力，其循环状态不随长度变贵，所以 13k 以内不必为速度砍上下文。32k 以上才变贵（全注意力那 16 层的 KV 读写随长度线性增长）。

显存吃紧时优先降 KV 精度（`-ctk q8_0 -ctv q8_0`，代价 0~3%），而不是降上下文。

### 量化的性价比

同一 27B 模型五个量化文件，`llama-bench -p 512 -n 128` 下的每 GiB 解码成本（2026-11-08 测量）：

| 文件 | GiB | tg128 t/s | ms/GiB |
|------|----:|----:|----:|
| IQ3_XXS | 9.72 | 18.0 | 5.71 |
| IQ3_S | 10.95 | 19.3 | 4.73 |
| IQ4_XS | 13.26 | 20.3 | 3.72 |
| **Q4_K_M** | **15.32** | **20.5** | **3.19** |
| Q4_K_XL | 16.68 | 19.0 | 3.15 |

- 曲线在 4.2~4.8 bpw 见底，**Q4_K_M 是默认选择**：再往低比特走，省下的字节会被 i-quant 的反量化 ALU 成本吃掉
- IQ4_XS 只在显存不够开长上下文时才换：省 2.1 GiB，解码慢约 1%
- IQ3_S 这类 i-quant 为主的模型（IQ3_S 30% + IQ4_XS 25% + IQ3_XXS 17%）**吃不到**第 8 节的权重外提优化，那些类型仍走原路径；但能吃到 4 rows/block 的 +11.6%

### 为什么到不了 50 t/s

不是「代码不够新」。拆开看：MTP 每步 59.1 ms、每步平均产出 2.35 token，即 37 t/s。要 50 t/s 需要每步 ≤ 47 ms，只有两条路：

1. 单 token 从 43 ms 压到 33 ms，对应有效带宽 329 -> 470 GB/s（约等于 MMVQ 单内核实测的 469 GB/s）
2. draft 接受率从 57% 提到 85%，每步 3.85 token

根因之一是 **IQ4_XS 在 Turing 上是算力/反量化受限，不是纯带宽受限**：把 SM 锁到 1500 MHz，解码从 23.1 掉到 17.0 t/s（-26%），而显存频率没动。纯带宽受限的负载不该对 SM 频率这么敏感。

## 编译

### 前置要求
- Visual Studio 2019 Build Tools（MSVC v142）
- CMake 3.16+
- CUDA Toolkit 11.4
- Python 3.8+（用于 gguf-py 和 UI 构建）

### 构建命令

```powershell
cmake -B build -G "Visual Studio 16 2019" -A x64 -T v142 `
  -DCMAKE_CUDA_ARCHITECTURES=61-real;75-real;86-real `
  -DBUILD_SHARED_LIBS=ON `
  -DGGML_CUDA=ON `
  -DGGML_STATIC=OFF `
  -DCMAKE_CXX_FLAGS="/MT" `
  -DCMAKE_C_FLAGS="/MT"

cmake --build build --config Release
```

### 多显卡架构编译（推荐）

仓库根目录提供三个现成脚本，只构建 `llama-server`，默认产出 **Compute Capability 6.1 / 7.5 / 8.6** 三档原生 SASS：

```powershell
# 方式一：Ninja 生成器（任何环境都能用，推荐）
.\build_win7_multiarch_ninja.bat

# 方式二：Visual Studio 生成器（需在普通命令行运行，原因见下）
.\configure_win7_multiarch.bat
.\build_win7_multiarch.bat
```

产物位于 `build-win7-cuda114-ninja\bin\`（方式一）或 `build-win7-cuda114-v142-multiarch\bin\Release\`（方式二）。

**架构选择依据**

| 架构 | 覆盖显卡 |
|------|----------|
| `61-real` | GTX 1050 ~ 1080 Ti、Titan Xp、MX150/250（同时覆盖同大版本的 sm_60 / sm_62） |
| `75-real` | RTX 20xx、GTX 16xx（同时覆盖 sm_72） |
| `86-real` | RTX 30xx、A10 / A40（同时覆盖 sm_80 / sm_87 / sm_89） |

CUDA 保证同一大版本内**向高小版本**的二进制兼容，因此每档取该大版本的最低小版本即可覆盖整档——例如 sm_86 的 cubin 可以直接跑在 sm_89 上，无需单独编译。CUDA 11.4 最高只能生成 sm_87 的原生代码，**无法为 sm_89 / sm_90 编译原生 SASS**（需要 CUDA 11.8+，而 CUDA 11.5 起已放弃 Win7）。

如需支持其他显卡，编辑脚本中的 `ARCHS` 变量（每增加一个架构，`ggml-cuda.dll` 约增大 75~90 MB）：

- 加 `60-real`：Tesla P40 / P100
- 加 `70-real`：V100 / Titan V
- 加 `80-real`：A100 / A30
- 加 `50-real`：Maxwell（GTX 750 / 9xx，LLM 推理性能较弱）
- 加 `86-virtual`：让 sm_90 及更新显卡通过驱动 JIT 运行（Win7 无对应驱动，一般不需要）

> **注意**：不要只加一个较低的 `XX-virtual` 当兜底。ggml-cuda 依据 `__CUDA_ARCH_LIST__`（包含 virtual 项）决定启用哪些 kernel，若只放 `50-virtual`，宿主会按 Pascal/Turing 特性选择 kernel，而驱动实际 JIT 的是 compute_50 代码，两者不一致。要加就整档加。

**为什么另外提供 Ninja 脚本**

CUDA 11.4 的 MSBuild 集成通过 XAML 任务工厂定义 `CudaCompile` 任务，该任务工厂在低完整性（Low IL）令牌下无法创建，会报 `MSB4175 ... 客户端没有所需的特权`。Ninja 生成器由 CMake 直接调用 `nvcc`，不经过 MSBuild，因此在受限环境下同样可用；在普通命令行中两种方式都可以。

**部署（只用 llama-server）**

`llama-server` 不依赖其他工具 exe，运行时需要同一构建目录产出的以下 9 个文件（1 个 exe + 8 个 DLL）：

```
llama-server.exe
ggml-base.dll   ggml-cpu.dll   ggml-cuda.dll   ggml.dll
llama.dll       llama-common.dll   llama-server-impl.dll   mtmd.dll
```

构建目录里同名的 `.manifest` 文件是链接器副产品，manifest 已嵌入 PE，**无需部署**。

必须**整套替换**，不要只替换 `ggml-cuda.dll`：各 DLL 之间存在 ABI 依赖。

### 其他构建脚本与补丁

| 文件 | 作用与注意事项 |
|------|------|
| `configure_patched.bat`<br>`build_patched.bat` | 单架构（sm_75）构建，输出到 `build-win7-cuda114-v142-patched`，日志写到构建脚本同级目录的 `build_patched.log`。脚本里的 `D:/llama_win7_cuda/...` 和 `C:/BuildTools2019/...` 是**硬编码**的，用前需改成自己的路径 |
| `HauhauCS-FastMTP-llama.cpp.patch` | FastMTP 草稿模块所需的 `d2t` draft-vocab trim。**当前源码树已经包含这些改动，直接构建即可，不要 `git apply`**（现在 apply 会报 `patch does not apply`）。保留此文件是为了同步上游后能重新套用 |

## 已修复的 Bug

| # | 症状 | 根因 | 修复 |
|---|------|------|------|
| 1 | 启动即崩溃 "已停止工作" | `fit_params=true` 导致模型双加载 + `/MT` 跨 DLL CRT 冲突 | `CreateFileW` 替代 `_get_osfhandle` |
| 2 | `cudaMalloc failed: out of memory` | 默认 `n_ctx` 太大导致 KV Cache 超出显存 | `fit_params=true` 自动适配上下文 |
| 3 | VMM 初始化失败 | `cuMem*` API 静态导入在老旧驱动上不存在 | 运行期动态解析 + `GGML_CUDA_NO_VMM` 开关 |
| 4 | Win7 上 `GetFileInformationByHandleEx` 导入失败 | `std::filesystem` 间接引入 Win8+ API | Win32 API 替代 |
| 5 | cpp-httplib `WSAPoll` 在 Win7 上不可用 | HTTP 库使用了 Win8+ 网络 API | `select()` 回退 |
| 6 | 多模态模型（如 Qwen3-ASR）无法识别音频，只输出 `language None<asr_text>` | cuBLAS 对 f16 权重使用 `CUBLAS_COMPUTE_16F`（fp16 累加），本工具链下普通 `ggml_mul_mat` 产出 `inf`；而 mmproj 的卷积 / `conv_out` 权重正是 f16，导致音频嵌入变成垃圾。量化权重走另一条路径，所以纯文本推理看起来正常 | `ggml-cuda.cu`：当 `src0->type == GGML_TYPE_F16` 时改走既有 `prefer_f32_output` 分支，用 f32 累加。量化权重不受影响，LLM 推理速度不变。修复后 `test-backend-ops` 的 CUDA 失败数由 201 降到 4（剩余为 `iq1_m`，另一独立问题） |
| 7 | 以 `w` / `a` 模式打开的文件写不出内容 | `llama_file::impl` 用 `CreateFileW` 拿到 HANDLE 后，`_open_osfhandle` 固定传 flags `0`，CRT 侧始终是只读句柄，`fwrite` 不生效 | 按 fopen 模式映射 `_O_WRONLY` / `_O_APPEND`，并让 CRT fd 接管 HANDLE 的生命周期，避免重复 `CloseHandle` |
| 8 | 多草稿槽并发时 DFlash2 提案质量静默下降（不报错，只影响加速比） | 各槽剩余上下文不同导致 `n_max` 不一致，折叠成 `[tokens_per_block, n_seqs]` 的批次被切成 block 边界错乱的 ubatch | 跨所有 drafting 序列夹紧 draft block 长度；单草稿槽与 MTP 不受影响 |
| 9 | 宽行 top-k 在 CUDA 上不可用，或结果不对 | top-k 的能力判断没覆盖 `GGML_CUDA_USE_CUB` 路径，也没校验输入的 dtype 与内存连续性 | 该路径同样上报支持；非 f32 或非连续输入直接判为不支持。`test-backend-ops` TOP_K 375/375 |
| 10 | 挂了 `-md` 草稿模型后，显存比预期多约 330 MiB | `load_mtp` 只由 `--spec-type` 决定，完全忽略 `-md`。此时 drafting 在 `ctx_dft` 里跑，目标模型的 nextn 层永不执行却常驻权重 | `load_mtp` 加上 `!has_dft()` 条件，并对 draft 上下文强制开启。输出逐字节不变 |

## 已知风险与未解决问题

### 长生成时黑屏（TDR）：生产机上仍未定论

- **现象**：长时间生成时突然黑屏 -> 显示恢复 -> WER 弹窗提示 `llama-server` -> 进程残留需手杀。
- **判读**：典型的 TDR（`TdrDelay` 默认 2 秒）。kernel 超时被驱动杀掉，CUDA context 随即失效触发 `GGML_ABORT`，再撞上 WER 弹窗把进程挂住。
- **能缩短链条的配置**：`TdrDelay=60`，关闭 WER 弹窗，用 `/health` 看门狗自动重启残留进程。
- **头号嫌疑**：`82e4764` 那 3 行多列 MMVQ（投机验证批正好落在 2~4 列）。回退它是最稳的选择，**代价是解码 -13%**（本机 A/B：44.91 -> 39.13 t/s）。
- **但本机复现不了**：Win7 双启后，同一版二进制 10 轮草稿式长生成全部通过（40~48 t/s，`draft_n` 约 1800~2000），关 VMM、关投机也都正常。
- **余下差异指向环境**：该配置下显存只剩约 0.75 GB（21771/22528 MiB），而出问题的机器同时装着云桌面客户端。最省事的验证方向是把上下文降一档（`-c 110592` 降到 `-c 65536`）再压三轮：若不再黑屏，说明是显存余量问题，那 13% 就得让出去。

### 其他遗留

- `iq1_m` 在 `test-backend-ops` 里有 2 个 MUL_MAT 用例失败，属独立遗留问题，本分支未处理，也不是 MMVQ 改动引入的。
- 曾被怀疑但已排除的方向：`--spec-draft-p-min` 剪枝（五档草稿数完全相同，无效）、换回内置 MTP 提高接受率（慢 9%）、iq4_XS 权重外提（端到端 -0.75%，已回退）、q4_K 尾部越界读（该模型所有 q4_K 张量行数都是 4 的倍数）。

## 辅助工具

| 脚本 | 用途 |
|------|------|
| `dump_gguf_meta.py <file.gguf>` | 打印 GGUF 头（version / n_tensors / n_kv）、全部 KV 与张量表。用来确认 MTP / `d2t` 张量在不在、量化档位对不对 |
| `read_gguf_meta.py` | 同上的早期一次性版本，**模型路径硬编码在脚本里**，日常用 `dump_gguf_meta.py` |

## 免责声明

- 本项目仅在自己的测试环境（Win7 x64 + 2080 Ti 22GB + CUDA 11.4）验证通过
- **未在其他 GPU、驱动版本、模型上测试**
- 多架构构建中的 sm_61 / sm_86 等架构仅验证了**编译产物正确包含对应 cubin**，未在对应显卡上做过运行时验证
- 所有 t/s 都是默认 boost 下单次运行的采样，噪声约 ±5%；跨机器、跨驱动的差异可能大于这些数字之间的差距。涉及显存带宽的对比必须锁频，否则空闲显存会掉到 810 MHz，读数没有意义
- 长生成 TDR 黑屏在部分机器上仍未定论，见[已知风险与未解决问题](#已知风险与未解决问题)
- 可能存在未发现的兼容性问题，生产环境使用请自行评估风险
- 欢迎提交 Issue 反馈其他环境下的问题

## 许可

与原项目一致，采用 MIT 许可证。详见 [LICENSE](LICENSE)。

## 上游

https://github.com/ggml-org/llama.cpp

[Manifesto](https://github.com/ggml-org/llama.cpp/discussions/205) / [ggml](https://github.com/ggml-org/ggml) / [ops](https://github.com/ggml-org/llama.cpp/blob/master/docs/ops.md) / [maintainer PRs](https://github.com/ggml-org/llama.cpp/issues?q=is%3Apr%20is%3Aopen%20draft%3AFalse%20(author%3Argerganov%20OR%20author%3AKitaitiMakoto%20OR%20author%3Adanbev%20OR%20author%3Aaldehir%20OR%20author%3Amax-krasnyansky%20OR%20author%3ACISC%20OR%20author%3Aggerganov%20OR%20author%3Aam17an%20OR%20author%3Abartowski1182%20OR%20author%3Ahipudding%20OR%20author%3AServeurpersoCom%20OR%20author%3Apwilkin%20OR%20author%3Areeselevine%20OR%20author%3Angxson%20OR%20author%3Ajeffbolznv%20OR%20author%3A0cc4m%20OR%20author%3Aangt%20OR%20author%3AIMbackK%20OR%20author%3Aarthw%20OR%20author%3AJohannesGaessler%20OR%20author%3AORippler%20OR%20author%3Aruixiang63%20OR%20author%3Axctan%20OR%20author%3Aallozaur%20OR%20author%3Ayomaytk%20OR%20author%3Aaendk%20OR%20author%3Agaugarg-nv%20OR%20author%3Ataronaeo%20OR%20author%3Aforforever73%20OR%20author%3Alhez%20OR%20author%3Anetrunnereve%20OR%20author%3Afairydreaming)%20sort%3Aupdated-desc)

LLM inference in C/C++

## Recent API changes

- [Changelog for `libllama` API](https://github.com/ggml-org/llama.cpp/issues/9289)
- [Changelog for `llama-server` REST API](https://github.com/ggml-org/llama.cpp/issues/9291)

## Hot topics

- **Hugging Face cache migration: models downloaded with `-hf` are now stored in the standard Hugging Face cache directory, enabling sharing with other HF tools.**
- **[guide : using the new WebUI of llama.cpp](https://github.com/ggml-org/llama.cpp/discussions/16938)**
- [guide : running gpt-oss with llama.cpp](https://github.com/ggml-org/llama.cpp/discussions/15396)
- [[FEEDBACK] Better packaging for llama.cpp to support downstream consumers 🤗](https://github.com/ggml-org/llama.cpp/discussions/15313)
- Support for the `gpt-oss` model with native MXFP4 format has been added | [PR](https://github.com/ggml-org/llama.cpp/pull/15091) | [Collaboration with NVIDIA](https://blogs.nvidia.com/blog/rtx-ai-garage-openai-oss) | [Comment](https://github.com/ggml-org/llama.cpp/discussions/15095)
- Multimodal support arrived in `llama-server`: [#12898](https://github.com/ggml-org/llama.cpp/pull/12898) | [documentation](./docs/multimodal.md)
- VS Code extension for FIM completions: https://github.com/ggml-org/llama.vscode
- Vim/Neovim plugin for FIM completions: https://github.com/ggml-org/llama.vim
- Hugging Face Inference Endpoints now support GGUF out of the box! https://github.com/ggml-org/llama.cpp/discussions/9669
- Hugging Face GGUF editor: [discussion](https://github.com/ggml-org/llama.cpp/discussions/9268) | [tool](https://huggingface.co/spaces/CISCai/gguf-editor)
- WebGPU support is now available in the browser, see a blog/demo introducing it [here](https://reeselevine.github.io/llamas-on-the-web/).

----

## Quick start

Getting started with llama.cpp is straightforward. Here are several ways to install it on your machine:

- Install `llama.cpp` using [brew, nix, winget, or conda-forge](docs/install.md)
- Run with Docker - see our [Docker documentation](docs/docker.md)
- Download pre-built binaries from the [releases page](https://github.com/ggml-org/llama.cpp/releases)
- Build from source by cloning this repository - check out [our build guide](docs/build.md)

Once installed, you'll need a model to work with. Head to the [Obtaining and quantizing models](#obtaining-and-quantizing-models) section to learn more.

Example command:

```sh
# Use a local model file
llama-cli -m my_model.gguf

# Or download and run a model directly from Hugging Face
llama-cli -hf ggml-org/gemma-3-1b-it-GGUF

# Launch OpenAI-compatible API server
llama-server -hf ggml-org/gemma-3-1b-it-GGUF
```

## Description

The main goal of `llama.cpp` is to enable LLM inference with minimal setup and state-of-the-art performance on a wide
range of hardware - locally and in the cloud.

- Plain C/C++ implementation without any dependencies
- Apple silicon is a first-class citizen - optimized via ARM NEON, Accelerate and Metal frameworks
- AVX, AVX2, AVX512 and AMX support for x86 architectures
- RVV, ZVFH, ZFH, ZICBOP and ZIHINTPAUSE support for RISC-V architectures
- 1.5-bit, 2-bit, 3-bit, 4-bit, 5-bit, 6-bit, and 8-bit integer quantization for faster inference and reduced memory use
- Custom CUDA kernels for running LLMs on NVIDIA GPUs (support for AMD GPUs via HIP and Moore Threads GPUs via MUSA)
- Vulkan and SYCL backend support
- CPU+GPU hybrid inference to partially accelerate models larger than the total VRAM capacity

The `llama.cpp` project is the main playground for developing new features for the [ggml](https://github.com/ggml-org/ggml) library.

<details>
<summary>Models</summary>

Typically finetunes of the base models below are supported as well.

Instructions for adding support for new models: [HOWTO-add-model.md](docs/development/HOWTO-add-model.md)

#### Text-only

- [X] LLaMA 🦙
- [x] LLaMA 2 🦙🦙
- [x] LLaMA 3 🦙🦙🦙
- [X] [Mistral 7B](https://huggingface.co/mistralai/Mistral-7B-v0.1)
- [x] [Mixtral MoE](https://huggingface.co/models?search=mistral-ai/Mixtral)
- [x] [DBRX](https://huggingface.co/databricks/dbrx-instruct)
- [x] [Jamba](https://huggingface.co/ai21labs)
- [X] [Falcon](https://huggingface.co/models?search=tiiuae/falcon)
- [X] [Chinese LLaMA / Alpaca](https://github.com/ymcui/Chinese-LLaMA-Alpaca) and [Chinese LLaMA-2 / Alpaca-2](https://github.com/ymcui/Chinese-LLaMA-Alpaca-2)
- [X] [Vigogne (French)](https://github.com/bofenghuang/vigogne)
- [X] [BERT](https://github.com/ggml-org/llama.cpp/pull/5423)
- [X] [Koala](https://bair.berkeley.edu/blog/2023/04/03/koala/)
- [X] [Baichuan 1 & 2](https://huggingface.co/models?search=baichuan-inc/Baichuan) + [derivations](https://huggingface.co/hiyouga/baichuan-7b-sft)
- [X] [Aquila 1 & 2](https://huggingface.co/models?search=BAAI/Aquila)
- [X] [Starcoder models](https://github.com/ggml-org/llama.cpp/pull/3187)
- [X] [Refact](https://huggingface.co/smallcloudai/Refact-1_6B-fim)
- [X] [MPT](https://github.com/ggml-org/llama.cpp/pull/3417)
- [X] [Bloom](https://github.com/ggml-org/llama.cpp/pull/3553)
- [x] [Yi models](https://huggingface.co/models?search=01-ai/Yi)
- [X] [StableLM models](https://huggingface.co/stabilityai)
- [x] [Deepseek models](https://huggingface.co/models?search=deepseek-ai/deepseek)
- [x] [Qwen models](https://huggingface.co/models?search=Qwen/Qwen)
- [x] [PLaMo-13B](https://github.com/ggml-org/llama.cpp/pull/3557)
- [x] [Phi models](https://huggingface.co/models?search=microsoft/phi)
- [x] [PhiMoE](https://github.com/ggml-org/llama.cpp/pull/11003)
- [x] [GPT-2](https://huggingface.co/gpt2)
- [x] [Orion 14B](https://github.com/ggml-org/llama.cpp/pull/5118)
- [x] [InternLM2](https://huggingface.co/models?search=internlm2)
- [x] [CodeShell](https://github.com/WisdomShell/codeshell)
- [x] [Gemma](https://ai.google.dev/gemma)
- [x] [Mamba](https://github.com/state-spaces/mamba)
- [x] [Grok-1](https://huggingface.co/keyfan/grok-1-hf)
- [x] [Xverse](https://huggingface.co/models?search=xverse)
- [x] [Command-R models](https://huggingface.co/models?search=CohereForAI/c4ai-command-r)
- [x] [SEA-LION](https://huggingface.co/models?search=sea-lion)
- [x] [GritLM-7B](https://huggingface.co/GritLM/GritLM-7B) + [GritLM-8x7B](https://huggingface.co/GritLM/GritLM-8x7B)
- [x] [OLMo](https://allenai.org/olmo)
- [x] [OLMo 2](https://allenai.org/olmo)
- [x] [OLMoE](https://huggingface.co/allenai/OLMoE-1B-7B-0924)
- [x] [Granite models](https://huggingface.co/collections/ibm-granite/granite-code-models-6624c5cec322e4c148c8b330)
- [x] [GPT-NeoX](https://github.com/EleutherAI/gpt-neox) + [Pythia](https://github.com/EleutherAI/pythia)
- [x] [Snowflake-Arctic MoE](https://huggingface.co/collections/Snowflake/arctic-66290090abe542894a5ac520)
- [x] [Smaug](https://huggingface.co/models?search=Smaug)
- [x] [Poro 34B](https://huggingface.co/LumiOpen/Poro-34B)
- [x] [Bitnet b1.58 models](https://huggingface.co/1bitLLM)
- [x] [Flan T5](https://huggingface.co/models?search=flan-t5)
- [x] [Open Elm models](https://huggingface.co/collections/apple/openelm-instruct-models-6619ad295d7ae9f868b759ca)
- [x] [ChatGLM3-6b](https://huggingface.co/THUDM/chatglm3-6b) + [ChatGLM4-9b](https://huggingface.co/THUDM/glm-4-9b) + [GLMEdge-1.5b](https://huggingface.co/THUDM/glm-edge-1.5b-chat) + [GLMEdge-4b](https://huggingface.co/THUDM/glm-edge-4b-chat)
- [x] [GLM-4-0414](https://huggingface.co/collections/THUDM/glm-4-0414-67f3cbcb34dd9d252707cb2e)
- [x] [SmolLM](https://huggingface.co/collections/HuggingFaceTB/smollm-6695016cad7167254ce15966)
- [x] [EXAONE-3.0-7.8B-Instruct](https://huggingface.co/LGAI-EXAONE/EXAONE-3.0-7.8B-Instruct)
- [x] [FalconMamba Models](https://huggingface.co/collections/tiiuae/falconmamba-7b-66b9a580324dd1598b0f6d4a)
- [x] [Jais](https://huggingface.co/inceptionai/jais-13b-chat)
- [x] [Bielik-11B-v2.3](https://huggingface.co/collections/speakleash/bielik-11b-v23-66ee813238d9b526a072408a)
- [x] [RWKV-7](https://huggingface.co/collections/shoumenchougou/rwkv7-gxx-gguf)
- [x] [RWKV-6](https://github.com/BlinkDL/RWKV-LM)
- [x] [QRWKV-6](https://huggingface.co/recursal/QRWKV6-32B-Instruct-Preview-v0.1)
- [x] [GigaChat-20B-A3B](https://huggingface.co/ai-sage/GigaChat-20B-A3B-instruct)
- [X] [Trillion-7B-preview](https://huggingface.co/trillionlabs/Trillion-7B-preview)
- [x] [Ling models](https://huggingface.co/collections/inclusionAI/ling-67c51c85b34a7ea0aba94c32)
- [x] [Liquid LFM2 models](https://huggingface.co/collections/LiquidAI/lfm2)
- [x] [Liquid LFM2.5 models](https://huggingface.co/collections/LiquidAI/lfm25)
- [x] [Liquid Nanos](https://huggingface.co/collections/LiquidAI/liquid-nanos)
- [x] [Hunyuan models](https://huggingface.co/collections/tencent/hunyuan-dense-model-6890632cda26b19119c9c5e7)
- [x] [BailingMoeV2 (Ring/Ling 2.0) models](https://huggingface.co/collections/inclusionAI/ling-v2-68bf1dd2fc34c306c1fa6f86)
- [x] [Mellum models](https://huggingface.co/JetBrains/models?search=mellum)

#### Multimodal

- [x] [LLaVA 1.5 models](https://huggingface.co/collections/liuhaotian/llava-15-653aac15d994e992e2677a7e), [LLaVA 1.6 models](https://huggingface.co/collections/liuhaotian/llava-16-65b9e40155f60fd046a5ccf2)
- [x] [BakLLaVA](https://huggingface.co/models?search=SkunkworksAI/Bakllava)
- [x] [Obsidian](https://huggingface.co/NousResearch/Obsidian-3B-V0.5)
- [x] [ShareGPT4V](https://huggingface.co/models?search=Lin-Chen/ShareGPT4V)
- [x] [MobileVLM 1.7B/3B models](https://huggingface.co/models?search=mobileVLM)
- [x] [Yi-VL](https://huggingface.co/models?search=Yi-VL)
- [x] [Mini CPM](https://huggingface.co/models?search=MiniCPM)
- [x] [Moondream](https://huggingface.co/vikhyatk/moondream2)
- [x] [Bunny](https://github.com/BAAI-DCAI/Bunny)
- [x] [GLM-EDGE](https://huggingface.co/models?search=glm-edge)
- [x] [Qwen2-VL](https://huggingface.co/collections/Qwen/qwen2-vl-66cee7455501d7126940800d)
- [x] [LFM2-VL](https://huggingface.co/collections/LiquidAI/lfm2-vl-68963bbc84a610f7638d5ffa)

</details>

<details>
<summary>Bindings</summary>

- Python: [ddh0/easy-llama](https://github.com/ddh0/easy-llama)
- Python: [abetlen/llama-cpp-python](https://github.com/abetlen/llama-cpp-python)
- Go: [go-skynet/go-llama.cpp](https://github.com/go-skynet/go-llama.cpp)
- Node.js: [withcatai/node-llama-cpp](https://github.com/withcatai/node-llama-cpp)
- JS/TS (llama.cpp server client): [lgrammel/modelfusion](https://modelfusion.dev/integration/model-provider/llamacpp)
- JS/TS (Programmable Prompt Engine CLI): [offline-ai/cli](https://github.com/offline-ai/cli)
- JavaScript/Wasm (works in browser): [tangledgroup/llama-cpp-wasm](https://github.com/tangledgroup/llama-cpp-wasm)
- Typescript/Wasm (nicer API, available on npm): [ngxson/wllama](https://github.com/ngxson/wllama)
- Ruby: [yoshoku/llama_cpp.rb](https://github.com/yoshoku/llama_cpp.rb)
- Ruby: [docusealco/rllama](https://github.com/docusealco/rllama)
- Rust (more features): [edgenai/llama_cpp-rs](https://github.com/edgenai/llama_cpp-rs)
- Rust (nicer API): [mdrokz/rust-llama.cpp](https://github.com/mdrokz/rust-llama.cpp)
- Rust (more direct bindings): [utilityai/llama-cpp-rs](https://github.com/utilityai/llama-cpp-rs)
- Rust (automated build from crates.io): [ShelbyJenkins/llm_client](https://github.com/ShelbyJenkins/llm_client)
- C#/.NET: [SciSharp/LLamaSharp](https://github.com/SciSharp/LLamaSharp)
- C#/VB.NET (more features - community license): [LM-Kit.NET](https://docs.lm-kit.com/lm-kit-net/index.html)
- Scala 3: [donderom/llm4s](https://github.com/donderom/llm4s)
- Clojure: [phronmophobic/llama.clj](https://github.com/phronmophobic/llama.clj)
- React Native: [mybigday/llama.rn](https://github.com/mybigday/llama.rn)
- Java: [kherud/java-llama.cpp](https://github.com/kherud/java-llama.cpp)
- Java: [QuasarByte/llama-cpp-jna](https://github.com/QuasarByte/llama-cpp-jna)
- Zig: [deins/llama.cpp.zig](https://github.com/Deins/llama.cpp.zig)
- Flutter/Dart: [netdur/llama_cpp_dart](https://github.com/netdur/llama_cpp_dart)
- Flutter: [xuegao-tzx/Fllama](https://github.com/xuegao-tzx/Fllama)
- PHP (API bindings and features built on top of llama.cpp): [distantmagic/resonance](https://github.com/distantmagic/resonance) [(more info)](https://github.com/ggml-org/llama.cpp/pull/6326)
- Guile Scheme: [guile_llama_cpp](https://savannah.nongnu.org/projects/guile-llama-cpp)
- Swift [srgtuszy/llama-cpp-swift](https://github.com/srgtuszy/llama-cpp-swift)
- Swift [ShenghaiWang/SwiftLlama](https://github.com/ShenghaiWang/SwiftLlama)
- Delphi [Embarcadero/llama-cpp-delphi](https://github.com/Embarcadero/llama-cpp-delphi)
- Go (no CGo needed): [hybridgroup/yzma](https://github.com/hybridgroup/yzma)
- Android: [llama.android](/examples/llama.android)

</details>

<details>
<summary>UIs</summary>

*(to have a project listed here, it should clearly state that it depends on `llama.cpp`)*

- [AI Sublime Text plugin](https://github.com/yaroslavyaroslav/OpenAI-sublime-text) (MIT)
- [BonzAI App](https://apps.apple.com/us/app/bonzai-your-local-ai-agent/id6752847988) (proprietary)
- [cztomsik/ava](https://github.com/cztomsik/ava) (MIT)
- [Dot](https://github.com/alexpinel/Dot) (GPL)
- [eva](https://github.com/ylsdamxssjxxdd/eva) (MIT)
- [iohub/collama](https://github.com/iohub/coLLaMA) (Apache-2.0)
- [janhq/jan](https://github.com/janhq/jan) (AGPL)
- [johnbean393/Sidekick](https://github.com/johnbean393/Sidekick) (MIT)
- [KanTV](https://github.com/zhouwg/kantv?tab=readme-ov-file) (Apache-2.0)
- [KodiBot](https://github.com/firatkiral/kodibot) (GPL)
- [llama.vim](https://github.com/ggml-org/llama.vim) (MIT)
- [LARS](https://github.com/abgulati/LARS) (AGPL)
- [Llama Assistant](https://github.com/vietanhdev/llama-assistant) (GPL)
- [LlamaLib](https://github.com/undreamai/LlamaLib) (Apache-2.0)
- [LLMFarm](https://github.com/guinmoon/LLMFarm?tab=readme-ov-file) (MIT)
- [LLMUnity](https://github.com/undreamai/LLMUnity) (MIT)
- [LMStudio](https://lmstudio.ai/) (proprietary)
- [LocalAI](https://github.com/mudler/LocalAI) (MIT)
- [LostRuins/koboldcpp](https://github.com/LostRuins/koboldcpp) (AGPL)
- [MindMac](https://mindmac.app) (proprietary)
- [MindWorkAI/AI-Studio](https://github.com/MindWorkAI/AI-Studio) (FSL-1.1-MIT)
- [Mobile-Artificial-Intelligence/maid](https://github.com/Mobile-Artificial-Intelligence/maid) (MIT)
- [Mozilla-Ocho/llamafile](https://github.com/Mozilla-Ocho/llamafile) (Apache-2.0)
- [nat/openplayground](https://github.com/nat/openplayground) (MIT)
- [nomic-ai/gpt4all](https://github.com/nomic-ai/gpt4all) (MIT)
- [ollama/ollama](https://github.com/ollama/ollama) (MIT)
- [oobabooga/text-generation-webui](https://github.com/oobabooga/text-generation-webui) (AGPL)
- [PocketPal AI](https://github.com/a-ghorbani/pocketpal-ai) (MIT)
- [psugihara/FreeChat](https://github.com/psugihara/FreeChat) (MIT)
- [ptsochantaris/emeltal](https://github.com/ptsochantaris/emeltal) (MIT)
- [pythops/tenere](https://github.com/pythops/tenere) (AGPL)
- [ramalama](https://github.com/containers/ramalama) (MIT)
- [semperai/amica](https://github.com/semperai/amica) (MIT)
- [withcatai/catai](https://github.com/withcatai/catai) (MIT)
- [Autopen](https://github.com/blackhole89/autopen) (GPL)

</details>

<details>
<summary>Tools</summary>

- [akx/ggify](https://github.com/akx/ggify) – download PyTorch models from Hugging Face Hub and convert them to GGML
- [akx/ollama-dl](https://github.com/akx/ollama-dl) – download models from the Ollama library to be used directly with llama.cpp
- [crashr/gppm](https://github.com/crashr/gppm) – launch llama.cpp instances utilizing NVIDIA Tesla P40 or P100 GPUs with reduced idle power consumption
- [gpustack/gguf-parser](https://github.com/gpustack/gguf-parser-go/tree/main/cmd/gguf-parser) - review/check the GGUF file and estimate the memory usage
- [Styled Lines](https://marketplace.unity.com/packages/tools/generative-ai/styled-lines-llama-cpp-model-292902) (proprietary licensed, async wrapper of inference part for game development in Unity3d with pre-built Mobile and Web platform wrappers and a model example)
- [unslothai/unsloth](https://github.com/unslothai/unsloth) – 🦥 exports/saves fine-tuned and trained models to GGUF (Apache-2.0)

</details>

<details>
<summary>Infrastructure</summary>

- [Paddler](https://github.com/intentee/paddler) - Open-source LLMOps platform for hosting and scaling AI in your own infrastructure
- [GPUStack](https://github.com/gpustack/gpustack) - Manage GPU clusters for running LLMs
- [llama_cpp_canister](https://github.com/onicai/llama_cpp_canister) - llama.cpp as a smart contract on the Internet Computer, using WebAssembly
- [llama-swap](https://github.com/mostlygeek/llama-swap) - transparent proxy that adds automatic model switching with llama-server
- [Kalavai](https://github.com/kalavai-net/kalavai-client) - Crowdsource end to end LLM deployment at any scale
- [llmaz](https://github.com/InftyAI/llmaz) - ☸️ Easy, advanced inference platform for large language models on Kubernetes.
- [LLMKube](https://github.com/defilantech/llmkube) - Kubernetes operator for llama.cpp with multi-GPU and Apple Silicon Metal
  support"
</details>

<details>
<summary>Games</summary>

- [Lucy's Labyrinth](https://github.com/MorganRO8/Lucys_Labyrinth) - A simple maze game where agents controlled by an AI model will try to trick you.

</details>


## Supported backends

| Backend | Target devices |
| --- | --- |
| [Metal](docs/build.md#metal-build) | Apple Silicon |
| [BLAS](docs/build.md#blas-build) | All |
| [BLIS](docs/backend/BLIS.md) | All |
| [SYCL](docs/backend/SYCL.md) | Intel GPU |
| [OpenVINO [In Progress]](docs/backend/OPENVINO.md) | Intel CPUs, GPUs, and NPUs |
| [MUSA](docs/build.md#musa) | Moore Threads GPU |
| [CUDA](docs/build.md#cuda) | Nvidia GPU |
| [HIP](docs/build.md#hip) | AMD GPU |
| [ZenDNN](docs/build.md#zendnn) | AMD CPU |
| [Vulkan](docs/build.md#vulkan) | GPU |
| [CANN](docs/build.md#cann) | Ascend NPU |
| [OpenCL](docs/backend/OPENCL.md) | Adreno GPU |
| [IBM zDNN](docs/backend/zDNN.md) | IBM Z & LinuxONE |
| [WebGPU](docs/build.md#webgpu) | All |
| [RPC](https://github.com/ggml-org/llama.cpp/tree/master/tools/rpc) | All |
| [Hexagon [In Progress]](docs/backend/snapdragon/README.md) | Snapdragon |
| [VirtGPU](docs/backend/VirtGPU.md) | VirtGPU APIR |

## Obtaining and quantizing models

The [Hugging Face](https://huggingface.co) platform hosts a [number of LLMs](https://huggingface.co/models?library=gguf&sort=trending) compatible with `llama.cpp`:

- [Trending](https://huggingface.co/models?library=gguf&sort=trending)
- [LLaMA](https://huggingface.co/models?sort=trending&search=llama+gguf)

You can either manually download the GGUF file or directly use any `llama.cpp`-compatible models from [Hugging Face](https://huggingface.co/) or other model hosting sites, by using this CLI argument: `-hf <user>/<model>[:quant]`. For example:

```sh
llama-cli -hf ggml-org/gemma-3-1b-it-GGUF
```

By default, the CLI would download from Hugging Face, you can switch to other options with the environment variable `MODEL_ENDPOINT`. The `MODEL_ENDPOINT` must point to a Hugging Face compatible API endpoint.

After downloading a model, use the CLI tools to run it locally - see below.

`llama.cpp` requires the model to be stored in the [GGUF](https://github.com/ggml-org/ggml/blob/master/docs/gguf.md) file format. Models in other data formats can be converted to GGUF using the `convert_*.py` Python scripts in this repo.

The Hugging Face platform provides a variety of online tools for converting, quantizing and hosting models with `llama.cpp`:

- Use the [GGUF-my-repo space](https://huggingface.co/spaces/ggml-org/gguf-my-repo) to convert to GGUF format and quantize model weights to smaller sizes
- Use the [GGUF-my-LoRA space](https://huggingface.co/spaces/ggml-org/gguf-my-lora) to convert LoRA adapters to GGUF format (more info: https://github.com/ggml-org/llama.cpp/discussions/10123)
- Use the [GGUF-editor space](https://huggingface.co/spaces/CISCai/gguf-editor) to edit GGUF meta data in the browser (more info: https://github.com/ggml-org/llama.cpp/discussions/9268)
- Use the [Inference Endpoints](https://ui.endpoints.huggingface.co/) to directly host `llama.cpp` in the cloud (more info: https://github.com/ggml-org/llama.cpp/discussions/9669)

To learn more about model quantization, [read this documentation](tools/quantize/README.md)

## [`llama-cli`](tools/cli)

#### A CLI tool for accessing and experimenting with most of `llama.cpp`'s functionality.

- <details open>
    <summary>Run in conversation mode</summary>

    Models with a built-in chat template will automatically activate conversation mode. If this doesn't occur, you can manually enable it by adding `-cnv` and specifying a suitable chat template with `--chat-template NAME`

    ```bash
    llama-cli -m model.gguf

    # > hi, who are you?
    # Hi there! I'm your helpful assistant! I'm an AI-powered chatbot designed to assist and provide information to users like you. I'm here to help answer your questions, provide guidance, and offer support on a wide range of topics. I'm a friendly and knowledgeable AI, and I'm always happy to help with anything you need. What's on your mind, and how can I assist you today?
    #
    # > what is 1+1?
    # Easy peasy! The answer to 1+1 is... 2!
    ```

    </details>

- <details>
    <summary>Run in conversation mode with custom chat template</summary>

    ```bash
    # use the "chatml" template (use -h to see the list of supported templates)
    llama-cli -m model.gguf -cnv --chat-template chatml

    # use a custom template
    llama-cli -m model.gguf -cnv --in-prefix 'User: ' --reverse-prompt 'User:'
    ```

    </details>

- <details>
    <summary>Constrain the output with a custom grammar</summary>

    ```bash
    llama-cli -m model.gguf -n 256 --grammar-file grammars/json.gbnf -p 'Request: schedule a call at 8pm; Command:'

    # {"appointmentTime": "8pm", "appointmentDetails": "schedule a a call"}
    ```

    The [grammars/](grammars/) folder contains a handful of sample grammars. To write your own, check out the [GBNF Guide](grammars/README.md).

    For authoring more complex JSON grammars, check out https://grammar.intrinsiclabs.ai/

    </details>


## [`llama-server`](tools/server)

#### A lightweight, [OpenAI API](https://github.com/openai/openai-openapi) compatible, HTTP server for serving LLMs.

- <details open>
    <summary>Start a local HTTP server with default configuration on port 8080</summary>

    ```bash
    llama-server -m model.gguf --port 8080

    # Basic web UI can be accessed via browser: http://localhost:8080
    # Chat completion endpoint: http://localhost:8080/v1/chat/completions
    ```

    </details>

- <details>
    <summary>Support multiple-users and parallel decoding</summary>

    ```bash
    # up to 4 concurrent requests, each with 4096 max context
    llama-server -m model.gguf -c 16384 -np 4
    ```

    </details>

- <details>
    <summary>Enable speculative decoding</summary>

    ```bash
    # the draft.gguf model should be a small variant of the target model.gguf
    llama-server -m model.gguf -md draft.gguf
    ```

    </details>

- <details>
    <summary>Serve an embedding model</summary>

    ```bash
    # use the /embedding endpoint
    llama-server -m model.gguf --embedding --pooling cls -ub 8192
    ```

    </details>

- <details>
    <summary>Serve a reranking model</summary>

    ```bash
    # use the /reranking endpoint
    llama-server -m model.gguf --reranking
    ```

    </details>

- <details>
    <summary>Constrain all outputs with a grammar</summary>

    ```bash
    # custom grammar
    llama-server -m model.gguf --grammar-file grammar.gbnf

    # JSON
    llama-server -m model.gguf --grammar-file grammars/json.gbnf
    ```

    </details>


## [`llama-perplexity`](tools/perplexity)

#### A tool for measuring the [perplexity](tools/perplexity/README.md) [^1] (and other quality metrics) of a model over a given text.

- <details open>
    <summary>Measure the perplexity over a text file</summary>

    ```bash
    llama-perplexity -m model.gguf -f file.txt

    # [1]15.2701,[2]5.4007,[3]5.3073,[4]6.2965,[5]5.8940,[6]5.6096,[7]5.7942,[8]4.9297, ...
    # Final estimate: PPL = 5.4007 +/- 0.67339
    ```

    </details>

- <details>
    <summary>Measure KL divergence</summary>

    ```bash
    # TODO
    ```

    </details>

[^1]: [https://huggingface.co/docs/transformers/perplexity](https://huggingface.co/docs/transformers/perplexity)

## [`llama-bench`](tools/llama-bench)

#### Benchmark the performance of the inference for various parameters.

- <details open>
    <summary>Run default benchmark</summary>

    ```bash
    llama-bench -m model.gguf

    # Output:
    # | model               |       size |     params | backend    | threads |          test |                  t/s |
    # | ------------------- | ---------: | ---------: | ---------- | ------: | ------------: | -------------------: |
    # | qwen2 1.5B Q4_0     | 885.97 MiB |     1.54 B | Metal,BLAS |      16 |         pp512 |      5765.41 ± 20.55 |
    # | qwen2 1.5B Q4_0     | 885.97 MiB |     1.54 B | Metal,BLAS |      16 |         tg128 |        197.71 ± 0.81 |
    #
    # build: 3e0ba0e60 (4229)
    ```

    </details>

## [`llama-simple`](examples/simple)

#### A minimal example for implementing apps with `llama.cpp`. Useful for developers.

- <details>
    <summary>Basic text completion</summary>

    ```bash
    llama-simple -m model.gguf

    # Hello my name is Kaitlyn and I am a 16 year old girl. I am a junior in high school and I am currently taking a class called "The Art of
    ```

    </details>


## Contributing

- Contributors can open PRs
- Collaborators will be invited based on contributions
- Maintainers can push to branches in the `llama.cpp` repo and merge PRs into the `master` branch
- Any help with managing issues, PRs and projects is very appreciated!
- See [good first issues](https://github.com/ggml-org/llama.cpp/issues?q=is%3Aissue+is%3Aopen+label%3A%22good+first+issue%22) for tasks suitable for first contributions
- Read the [CONTRIBUTING.md](CONTRIBUTING.md) for more information
- Make sure to read this: [Inference at the edge](https://github.com/ggml-org/llama.cpp/discussions/205)
- A bit of backstory for those who are interested: [Changelog podcast](https://changelog.com/podcast/532)

## Other documentation

- [cli](tools/cli/README.md)
- [completion](tools/completion/README.md)
- [server](tools/server/README.md)
- [GBNF grammars](grammars/README.md)

#### Development documentation

- [How to build](docs/build.md)
- [Running on Docker](docs/docker.md)
- [Build on Android](docs/android.md)
- [Multi-GPU usage](docs/multi-gpu.md)
- [Performance troubleshooting](docs/development/token_generation_performance_tips.md)
- [GGML tips & tricks](https://github.com/ggml-org/llama.cpp/wiki/GGML-Tips-&-Tricks)

#### Seminal papers and background on the models

If your issue is with model generation quality, then please at least scan the following links and papers to understand the limitations of LLaMA models. This is especially important when choosing an appropriate model size and appreciating both the significant and subtle differences between LLaMA models and ChatGPT:
- LLaMA:
    - [Introducing LLaMA: A foundational, 65-billion-parameter large language model](https://ai.facebook.com/blog/large-language-model-llama-meta-ai/)
    - [LLaMA: Open and Efficient Foundation Language Models](https://arxiv.org/abs/2302.13971)
- GPT-3
    - [Language Models are Few-Shot Learners](https://arxiv.org/abs/2005.14165)
- GPT-3.5 / InstructGPT / ChatGPT:
    - [Aligning language models to follow instructions](https://openai.com/research/instruction-following)
    - [Training language models to follow instructions with human feedback](https://arxiv.org/abs/2203.02155)

## XCFramework
The XCFramework is a precompiled version of the library for iOS, visionOS, tvOS,
and macOS. It can be used in Swift projects without the need to compile the
library from source. For example:
```swift
// swift-tools-version: 5.10
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "MyLlamaPackage",
    targets: [
        .executableTarget(
            name: "MyLlamaPackage",
            dependencies: [
                "LlamaFramework"
            ]),
        .binaryTarget(
            name: "LlamaFramework",
            url: "https://github.com/ggml-org/llama.cpp/releases/download/b5046/llama-b5046-xcframework.zip",
            checksum: "c19be78b5f00d8d29a25da41042cb7afa094cbf6280a225abe614b03b20029ab"
        )
    ]
)
```
The above example is using an intermediate build `b5046` of the library. This can be modified
to use a different version by changing the URL and checksum.

## Completions
Command-line completion is available for some environments.

#### Bash Completion
```bash
$ build/bin/llama-cli --completion-bash > ~/.llama-completion.bash
$ source ~/.llama-completion.bash
```
Optionally this can be added to your `.bashrc` or `.bash_profile` to load it
automatically. For example:
```console
$ echo "source ~/.llama-completion.bash" >> ~/.bashrc
```

## Dependencies

- [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) - Single-header HTTP server, used by `llama-server` - MIT license
- [stb-image](https://github.com/nothings/stb) - Single-header image format decoder, used by multimodal subsystem - Public domain
- [nlohmann/json](https://github.com/nlohmann/json) - Single-header JSON library, used by various tools/examples - MIT License
- [miniaudio.h](https://github.com/mackron/miniaudio) - Single-header audio format decoder, used by multimodal subsystem - Public domain
- [subprocess.h](https://github.com/sheredom/subprocess.h) - Single-header process launching solution for C and C++ - Public domain
