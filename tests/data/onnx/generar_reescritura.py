# Genera reescritura.onnx, el modelo de prueba de ai/OnnxRewrite (requiere el paquete `onnx`):
#   python generar_reescritura.py
# Contiene los tres casos que se reescriben para la GPU: un Split con 17 salidas, un Sum de tres
# entradas y un ConvTranspose 1D de núcleo grande (128, paso 32) con sesgo.
import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

rng = np.random.default_rng(7)
parts = [f"p{i}" for i in range(17)]
nodes = [
    helper.make_node("Constant", [], ["sizes"], value=numpy_helper.from_array(np.full(17, 2, np.int64))),
    helper.make_node("Split", ["x", "sizes"], parts, axis=1, name="trocear"),
    helper.make_node("Concat", parts, ["unido"], axis=1, name="unir"),
    helper.make_node("Sum", ["p0", "p1", "p2"], ["suma"], name="sumar"),
    helper.make_node("ConvTranspose", ["suma", "w", "b"], ["istft"], kernel_shape=[128], strides=[32], name="istft"),
]
graph = helper.make_graph(
    nodes,
    "reescritura",
    [helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 34, 5])],
    [
        helper.make_tensor_value_info("unido", TensorProto.FLOAT, [1, 34, 5]),
        helper.make_tensor_value_info("p16", TensorProto.FLOAT, [1, 2, 5]),
        helper.make_tensor_value_info("istft", TensorProto.FLOAT, [1, 3, 256]),
    ],
    [
        numpy_helper.from_array(rng.standard_normal((2, 3, 128)).astype(np.float32), "w"),
        numpy_helper.from_array(rng.standard_normal(3).astype(np.float32), "b"),
    ],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], ir_version=8)
onnx.checker.check_model(model)
onnx.save(model, "reescritura.onnx")
