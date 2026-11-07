#include "common.cuh"

void ggml_cuda_op_top_k(ggml_backend_cuda_context & ctx, ggml_tensor * dst);

// whether ggml_cuda_op_top_k can handle this node (wide rows use the tiled path)
bool ggml_cuda_top_k_supported(const ggml_tensor * op);
