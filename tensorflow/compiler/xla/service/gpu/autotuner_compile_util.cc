/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/gpu/autotuner_compile_util.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/const_init.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/node_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tensorflow/compiler/xla/autotune_results.pb.h"
#include "tensorflow/compiler/xla/autotuning.pb.h"
#include "tensorflow/compiler/xla/hlo/ir/hlo_clone_context.h"
#include "tensorflow/compiler/xla/hlo/ir/hlo_computation.h"
#include "tensorflow/compiler/xla/hlo/ir/hlo_instruction.h"
#include "tensorflow/compiler/xla/hlo/ir/hlo_module.h"
#include "tensorflow/compiler/xla/service/compiler.h"
#include "tensorflow/compiler/xla/service/executable.h"
#include "tensorflow/compiler/xla/service/gpu/autotuner_util.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_executable_run_options.h"
#include "tensorflow/compiler/xla/service/gpu/ir_emission_utils.h"
#include "tensorflow/compiler/xla/service/hlo_module_config.h"
#include "tensorflow/compiler/xla/service/platform_util.h"
#include "tensorflow/compiler/xla/service/shaped_buffer.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/stream_executor/device_memory.h"
#include "tensorflow/compiler/xla/stream_executor/gpu/gpu_stream.h"
#include "tensorflow/compiler/xla/stream_executor/gpu/gpu_timer.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/compiler/xla/xla.pb.h"
#include "tensorflow/tsl/platform/errors.h"
#include "tensorflow/tsl/platform/statusor.h"

namespace xla {
namespace gpu {

namespace {

struct CompilationKey {
  template <typename H>
  friend H AbslHashValue(H h, const CompilationKey& k) {
    return H::combine(std::move(h), k.autotune_key, k.res.SerializeAsString());
  }

  bool operator==(const CompilationKey& k) const {
    return res.SerializeAsString() == k.res.SerializeAsString() &&
           autotune_key == k.autotune_key;
  }

  std::string ToString() const {
    return absl::StrFormat("<key=%s, res=%s>", autotune_key.ToString(),
                           res.DebugString());
  }

  AutotuneCacheKey autotune_key;
  AutotuneResult res;
};

static absl::Mutex executable_cache_mutex(absl::kConstInit);
// The key is the "standard" AutotuneCacheKey, which encompasses both the device
// type and the code of the HLO. We need this because TritonAutotuner may be
// called with different device types, and an executable compiled for one device
// type may not run on another.
static auto& ABSL_GUARDED_BY(executable_cache_mutex) executable_cache =
    *new absl::node_hash_map<CompilationKey, std::unique_ptr<Executable>>();

std::vector<ExecutionInput> ExecutionInputsFromBuffers(
    Executable* executable, absl::Span<se::DeviceMemoryBase const> buffers) {
  const HloInstruction::InstructionVector& params =
      executable->module().entry_computation()->parameter_instructions();
  CHECK_EQ(params.size(), buffers.size());
  std::vector<ExecutionInput> inputs;
  for (int i = 0; i < params.size(); ++i) {
    inputs.emplace_back(params.at(i)->shape());
    // Our executable doesn't have input-output aliasing, so we can pass
    // unowned input buffers.
    inputs.back().SetUnownedBuffer(
        /*index=*/{}, MaybeOwningDeviceMemory(/*unowned=*/buffers.at(i)));
  }
  return inputs;
}

}  // namespace

StatusOr<std::optional<absl::Duration>>
AutotunerCompileUtil::GenerateAndProfileExecutable(
    const HloComputation& hlo_computation, const AutotuneResult& config,
    const AutotuneCacheKey& cache_key, se::Stream* stream,
    absl::Span<se::DeviceMemoryBase const> input_buffers,
    se::DeviceMemoryBase output_buffer, ExtractModuleFn extractor) {
  TF_ASSIGN_OR_RETURN(
      Executable * executable,
      Compile(hlo_computation, config, cache_key, std::move(extractor)));

  if (!executable) {
    return {std::nullopt};
  }
  {
    std::vector<ExecutionInput> execution_inputs =
        ExecutionInputsFromBuffers(executable, input_buffers);
    // Warmup: in and out buffers are reused while probing different configs,
    // so GPU caches should be in some comparable states during measurements.
    TF_ASSIGN_OR_RETURN(ExecutionOutput execution_output,
                        Execute(*executable, std::move(execution_inputs)));
    TF_RETURN_IF_ERROR(stream->BlockHostUntilDone());
  }
  std::vector<ExecutionInput> execution_inputs =
      ExecutionInputsFromBuffers(executable, input_buffers);
  TF_ASSIGN_OR_RETURN(auto timer,
                      se::gpu::GpuTimer::Create(se::gpu::AsGpuStream(stream)));
  TF_ASSIGN_OR_RETURN(ExecutionOutput execution_output,
                      Execute(*executable, std::move(execution_inputs)));
  TF_ASSIGN_OR_RETURN(absl::Duration timer_duration,
                      timer.GetElapsedDuration());
  ScopedShapedBuffer result = execution_output.ConsumeResult();
  TF_RET_CHECK(output_buffer.size() == result.root_buffer().size());
  // TODO(cheshire): Copying should not be required. Instead, we can add a new
  // aliased parameter.
  stream->ThenMemcpy(&output_buffer, result.root_buffer(),
                     result.root_buffer().size());
  return std::make_optional(timer_duration);
}

StatusOr<Executable*> AutotunerCompileUtil::Compile(
    const HloComputation& hlo_computation, const AutotuneResult& res,
    const AutotuneCacheKey& cache_key, ExtractModuleFn extractor) {
  CompilationKey key{cache_key, res};
  {
    absl::MutexLock lock(&executable_cache_mutex);
    auto it = executable_cache.find(key);
    if (it != executable_cache.end()) {
      VLOG(4) << "Compilation cache hit";
      return it->second.get();
    }
  }

  TF_ASSIGN_OR_RETURN(std::unique_ptr<Executable> executable,
                      CompileNoCache(hlo_computation, std::move(extractor)));
  absl::MutexLock lock(&executable_cache_mutex);
  auto [it, inserted] = executable_cache.emplace(key, std::move(executable));
  return it->second.get();
}

StatusOr<std::unique_ptr<Executable>> AutotunerCompileUtil::CompileNoCache(
    const HloComputation& original_computation,
    ExtractModuleFn module_extractor) {
  StatusOr<std::unique_ptr<HloModule>> new_hlo_module = module_extractor();
  if (new_hlo_module.status().GetPayload(kUncompilableFusion).has_value()) {
    // Incompatible value of split-k is an expected failure.
    return std::unique_ptr<Executable>();
  } else if (!new_hlo_module.status().ok()) {
    return new_hlo_module.status();
  }
  return RunBackend(original_computation, std::move(*new_hlo_module));
}

/*static*/ StatusOr<AutotunerCompileUtil> AutotunerCompileUtil::Create(
    se::Stream& stream, se::DeviceMemoryAllocator& allocator) {
  se::StreamExecutor& stream_executor = *stream.parent();
  TF_ASSIGN_OR_RETURN(Compiler * compiler,
                      Compiler::GetForPlatform(stream_executor.platform()));
  return AutotunerCompileUtil(compiler, stream_executor, stream, allocator);
}

StatusOr<ExecutionOutput> AutotunerCompileUtil::Execute(
    Executable& executable, std::vector<ExecutionInput> arguments) {
  // Require exclusive GPU lock to prevent other runs during autotuning.
  GpuExecutableRunOptions gpu_opts;
  gpu_opts.set_requires_exclusive_lock_on_gpu();

  ExecutableRunOptions run_options;
  run_options.set_device_ordinal(stream_executor_.device_ordinal());
  run_options.set_stream(&stream_);
  run_options.set_allocator(&allocator_);
  run_options.set_gpu_executable_run_options(&gpu_opts);
  ServiceExecutableRunOptions service_run_options(run_options);
  TF_ASSIGN_OR_RETURN(ExecutionOutput output,
                      executable.ExecuteAsyncOnStreamWrapper(
                          &service_run_options, std::move(arguments)));

  return std::move(output);
}

StatusOr<std::unique_ptr<Executable>> AutotunerCompileUtil::RunBackend(
    const HloComputation& original_computation,
    std::unique_ptr<HloModule> module) {
  DebugOptions options =
      original_computation.parent()->config().debug_options();
  // Avoid dumping compilation steps.
  options.set_xla_dump_to("");
  options.set_xla_gpu_dump_autotune_results_to("");
  options.set_xla_gpu_load_autotune_results_from("");
  options.set_xla_gpu_dump_llvmir(false);
  // Avoid using another thread pool.
  options.set_xla_gpu_force_compilation_parallelism(1);
  options.set_xla_gpu_enable_xla_runtime_executable(false);
  module->config().set_debug_options(options);
  StatusOr<std::unique_ptr<Executable>> out =
      compiler_->RunBackend(std::move(module), &stream_executor_, &allocator_);
  if (out.status().code() == absl::StatusCode::kResourceExhausted) {
    // Being out of shared memory budget is an expected failure.
    return std::unique_ptr<Executable>();
  }
  return out;
}

/*static*/ void AutotunerCompileUtil::ClearCompilationCache() {
  absl::MutexLock lock(&executable_cache_mutex);
  executable_cache.clear();
}

}  // namespace gpu
}  // namespace xla
