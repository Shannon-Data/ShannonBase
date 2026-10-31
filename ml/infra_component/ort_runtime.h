/**
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

   The fundmental code for imcs.
   Copyright (c) 2023 -, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_ML_ORT_RUNTIME_H__
#define __SHANNONBASE_ML_ORT_RUNTIME_H__

#include <cstddef>
#include <string>

#include "onnxruntime_cxx_api.h"

namespace ShannonBase {
namespace ML {
namespace OrtRuntime {

// What this machine offers, probed once per process.
struct Capability {
  bool cuda_built{false};      // SHANNONBASE_ONNX_CUDA_EP
  bool cuda_available{false};  // driver + device present
  int cuda_devices{0};
  std::string gpu_name;
  std::string cpu_tier;  // x86-64-v3 / v8.2 / ...
  std::string backend;   // .so the OrtApi table comes from
  std::string reason;    // why CUDA is not used
};

// Cached probe of the current process.
const Capability &probe();

// Appends the best execution provider; returns "CUDA" or "CPU".
// model_bytes < SHANNON_ORT_GPU_MIN_BYTES keeps small models on the CPU.
std::string execute(Ort::SessionOptions &options, const char *tag, size_t model_bytes = 0);

// One-line summary for logs.
std::string describe();

// Size of a model file, 0 when it cannot be read.
size_t file_size(const std::string &path);

}  // namespace OrtRuntime
}  // namespace ML
}  // namespace ShannonBase

#endif  // __SHANNONBASE_ML_ORT_RUNTIME_H__
