# KV cache regression fixtures

These tiny ONNX graphs concatenate past and new keys/values in BSHD, BHSD,
and BHDS layouts. Each layout has FP32, FP16, and INT8 variants. They require
neither a tokenizer nor a downloaded language model.

The C++ regression compares the production KVCacheManager's separate
past/present buffers with independently allocated ORT reference outputs,
including prefill, repeated decode, reset, and a completely full cache.
Capacity and failed-run checks verify that active data is not published early.

Build and run from the repository root:

```sh
cmake --build cmake_build --target llm_kv_cache-t -j 4
cmake_build/runtime_output_directory/llm_kv_cache-t
```

The checked-in models use opset 17 and IR version 9. Regenerate them with
`python generate.py` from this directory (requires the `onnx` Python package;
generated using ONNX 1.20.0). Python is not required to run the C++ tests.

This regression validates KV storage and binding with Concat graphs. It does
not replace per-token logits validation of a full generation model.
