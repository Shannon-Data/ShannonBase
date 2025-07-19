IN THIS DIRECTORY, THE LOCAL LLM MODELS IN ONNX FORMAT ARE HERE. SHANNONBASE WILL USE FOR EMBEDDING OR INFERRENCING.
YOU CAN COPY OR DOWNLOAD YOUR OWN LLM MODELS FOR YOUR PURPOSE.

you can use a converter to convert your pretrained PyTorch, TensorFlow, or JAX models to ONNX using Optimum.
https://github.com/huggingface/optimum.

You can checkout how to convert to ONNX at https://huggingface.co/docs/optimum/exporters/onnx/usage_guides/export_a_model
Also, you can try it on web, to find the manual at https://github.com/huggingface/transformers.js/tree/main

After that, ShannonBase use ONNX-runtime to run the pretrained models.

You can follow the guide as listed below:


Uisng virtual evn.

1: create virtual evn.
```
python3 -m venv ~/minilm_venv
```

2: install the requirements.
```
pip install optimum [exporters]
pip install transformers torch sentence-transformers onnxruntime onnx
```

3: to check the installation.
```
optimum-cli export onnx --help
```

4: to do coverting.
```
optimum-cli export onnx --model all-MiniLM-L12-v2 all-MiniLM-L12-v2-ONNX/

```

