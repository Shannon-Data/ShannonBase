# LLM Models Directory

This directory contains local LLM models in **ONNX format** used by ShannonBase
for embedding and inference via ONNX Runtime.

---

## Rules

- Model names are **case-sensitive**.
- All `.onnx` files must be placed in an `onnx/` subdirectory under each model folder.

```
extra/llm-models/
├── multilingual-e5-small/
│   └── onnx/
│       └── model.onnx
└── all-MiniLM-L12-v2/
    └── onnx/
        └── model.onnx
```

---

## Supported Models

| Model | Source | Purpose |
|---|---|---|
| `multilingual-e5-small` | [shannondata/multilingual-e5-small](https://huggingface.co/shannondata/multilingual-e5-small) | Multilingual embedding |
| `all-MiniLM-L12-v2` | [shannondata/all-MiniLM-L12-v2](https://huggingface.co/shannondata/all-MiniLM-L12-v2) | English embedding |
| `Qwen3.5-2B-ONNX` | [shannondata/Qwen3.5-2B-ONNX](https://huggingface.co/shannondata/Qwen3.5-2B-ONNX) | Inference |

> **Note:** ONNX files are not bundled in this repository (too large).
> You must download or convert them manually as described below.

---

## Option 1 — Download Pre-converted ONNX Models

Use the `hf` CLI to download directly:

```bash
# Install
pip install huggingface_hub

# Example: multilingual-e5-small
hf download shannondata/multilingual-e5-small \
  --local-dir extra/llm-models/multilingual-e5-small
```

Pre-converted models are also available from the community:
[https://huggingface.co/onnx-community](https://huggingface.co/onnx-community)

---

## Option 2 — Convert from PyTorch / TensorFlow / JAX

Use [Optimum](https://github.com/huggingface/optimum) to export to ONNX:

```bash
# 1. Create virtual environment
python3 -m venv ~/onnx_venv && source ~/onnx_venv/bin/activate

# 2. Install dependencies
pip install "optimum[exporters]" transformers torch onnxruntime onnx

# 3. Export
optimum-cli export onnx \
  --model sentence-transformers/all-MiniLM-L12-v2 \
  extra/llm-models/all-MiniLM-L12-v2/onnx/
```

Reference: [Optimum ONNX Export Guide](https://huggingface.co/docs/optimum/exporters/onnx/usage_guides/export_a_model)