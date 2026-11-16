# Copyright (c) 2026, Shannon Data AI and/or its affiliates.
# SPDX-License-Identifier: GPL-2.0-only
"""Regenerate dynamic KV Concat fixtures with onnx; not needed to run tests."""
from pathlib import Path
import onnx
from onnx import TensorProto, helper

root = Path(__file__).resolve().parent
for layout, axis in [('bshd', 1), ('bhsd', 2), ('bhds', 3)]:
    def shape(seq):
        return {'bshd': [1, seq, 8, 64], 'bhsd': [1, 8, seq, 64],
                'bhds': [1, 8, 64, seq]}[layout]
    for dtype, tag in [(TensorProto.FLOAT, 'fp32'), (TensorProto.FLOAT16, 'fp16'),
                       (TensorProto.INT8, 'int8')]:
        inputs, outputs, nodes = [], [], []
        for kind in ['key', 'value']:
            past, new, present = 'past_' + kind, 'new_' + kind, 'present_' + kind
            inputs.extend([helper.make_tensor_value_info(past, dtype, shape('past_seq')),
                           helper.make_tensor_value_info(new, dtype, shape('new_seq'))])
            outputs.append(helper.make_tensor_value_info(present, dtype, shape('total_seq')))
            nodes.append(helper.make_node('Concat', [past, new], [present], axis=axis))
        model = helper.make_model(helper.make_graph(nodes, 'kv_concat', inputs, outputs),
                                  opset_imports=[helper.make_opsetid('', 17)], ir_version=9)
        onnx.checker.check_model(model)
        onnx.save(model, root / f'{layout}_{tag}.onnx')
