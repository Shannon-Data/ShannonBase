# Classifier safety fixtures

These synthetic ONNX graphs contain only a constant output and one declared
input. They contain no trained weights or external data. `rapid_ml-t` loads them
through the public classifier API; Python is not required to run the tests.

To regenerate them, use Python with `onnx==1.17.0` and run `generate.py`.
The models use IR version 9 and opset 17.

The invalid INT64 output deliberately has FLOAT 0.9 in its low 32 bits, and the
invalid shape contains scores above the offload threshold. Reading those
outputs without validating their metadata would incorrectly select Rapid.
The wrong-input-type model loads successfully but raises a real ONNX Runtime
exception when inference receives the classifier's FLOAT feature tensor.
