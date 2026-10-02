# Copyright (c) 2026, Shannon Data AI and/or its affiliates.
# SPDX-License-Identifier: GPL-2.0-only
"""Regenerate the tiny test models with onnx==1.17.0; not needed to run tests."""
from pathlib import Path
import onnx
from onnx import TensorProto, helper

root = Path(__file__).resolve().parent

def constant(name, dtype, shape, values, input_type=TensorProto.FLOAT):
    tensor = helper.make_tensor("score", dtype, shape, values)
    node = helper.make_node("Constant", [], ["output"], value=tensor)
    graph = helper.make_graph(
        [node], name,
        [helper.make_tensor_value_info("input", input_type, [1, 18])],
        [helper.make_tensor_value_info("output", dtype, shape)])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], ir_version=9)
    onnx.checker.check_model(model)
    onnx.save(model, root / (name + ".onnx"))

constant("scalar", TensorProto.FLOAT, [1, 1], [0.9])
constant("classes", TensorProto.FLOAT, [1, 2], [0.1, 0.9])
# The low 32 bits encode FLOAT 0.9, so an unchecked reinterpretation would offload.
constant("wrong_type", TensorProto.INT64, [1, 1], [0x3F666666])
constant("wrong_shape", TensorProto.FLOAT, [3], [0.9, 0.9, 0.9])
constant("nan_score", TensorProto.FLOAT, [1, 1], [float("nan")])
constant("inf_score", TensorProto.FLOAT, [1, 1], [float("inf")])
# Loading succeeds, but Run() must reject the classifier's FLOAT input tensor.
constant("wrong_input_type", TensorProto.FLOAT, [1, 1], [0.9], TensorProto.INT64)
