// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/providers/cuda/cuda_kernel.h"

using namespace onnxruntime::cuda;

namespace onnxruntime {
namespace contrib {
namespace cuda {

// S2SModelSplitQuickGelu

class S2SModelSplitQuickGelu final : public CudaKernel {
  public:
    S2SModelSplitQuickGelu(const OpKernelInfo& info) : CudaKernel(info) {}

    Status ComputeInternal(OpKernelContext* context) const override;

  private:
    template <typename T>
    struct KernelLaunchDispatcher {
      void operator()(cudaStream_t stream, const int num_outputs, const Tensor& input_tensor, Tensor& output_tensor) const;
    };
};

}  // namespace cuda
}  // namespace contrib
}  // namespace onnxruntime
