# SPDX-License-Identifier: MIT
"""Single-step recurrent lowering. No opaque GRU/LSTM operation survives.

Weights use ONNX ordering: GRU z,r,h; LSTM i,o,f,c. State is explicit
input/output. Each graph is stateless; a caller commits state only on success.
The emitted MIL uses fp16 weights, matmul/add/mul/sigmoid/tanh/relu and reshapes.
"""
from pathlib import Path
import struct

import numpy as np


class Graph:
    def __init__(self, inputs):
        self.inputs = inputs
        self.shapes = dict(inputs)
        self.constants = {}
        self.ops = []
        self.outputs = []

    def constant(self, name, value):
        value = np.asarray(value, dtype=np.float32)
        if value.ndim == 1:
            value = value[None]
        self.constants[name] = value
        self.shapes[name] = value.shape
        return name

    def op(self, name, op, a, b=None):
        self.shapes[name] = ((self.shapes[a][0], self.shapes[b][1]) if op == "matmul" else self.shapes[a])
        self.ops.append((name, op, a, b))
        return name

    def evaluate(self, values):
        v = {**self.constants, **values}
        for name, op, a, b in self.ops:
            x = v[a]
            if op == "matmul": v[name] = x @ v[b]
            elif op == "add": v[name] = x + v[b]
            elif op == "mul": v[name] = x * v[b]
            elif op == "sigmoid": v[name] = 1 / (1 + np.exp(-x))
            elif op == "tanh": v[name] = np.tanh(x)
            elif op == "relu": v[name] = np.maximum(x, 0)
            else: raise ValueError(op)
        return {name: v[name] for name in self.outputs}

    def write(self, directory):
        directory = Path(directory)
        directory.mkdir(parents=True, exist_ok=True)
        def typ(shape):
            return "tensor<fp16, [" + ",".join(map(str, shape)) + "]>"
        args = ", ".join(f"{typ(shape)} {name}" for name, shape in self.inputs.items())
        lines = ["program(1.3)", "[buildInfo = dict<string, string>({})]", "{", f"  func main<ios18>({args}) {{",
                 '    bool no_transpose = const()[name = string("no_transpose"), val = bool(false)];']
        blob = bytearray(64)
        for name, value in self.constants.items():
            while len(blob) % 64: blob.append(0)
            offset = len(blob)
            payload = np.asarray(value, dtype="<f2").tobytes()
            blob.extend(struct.pack("<IIQQ", 0xDEADBEEF, 0, len(payload), offset + 64))
            blob.extend(bytes(40))
            blob.extend(payload)
            t = typ(value.shape)
            lines.append(f'    {t} {name} = const()[name = string("{name}"), val = {t}(BLOBFILE(path = string("@model_path/weights.bin"), offset = uint64({offset})))];')
        for name, op, a, b in self.ops:
            t = typ(self.shapes[name])
            if op in ("sigmoid", "tanh", "relu"):
                h = self.shapes[name][1]
                lines.extend([
                    f'    tensor<int32, [4]> {name}_s4 = const()[name = string("{name}_s4"), val = tensor<int32, [4]>([1,{h},1,1])];',
                    f'    tensor<fp16, [1,{h},1,1]> {name}_in = reshape(shape = {name}_s4, x = {a})[name = string("{name}_in")];',
                    f'    tensor<fp16, [1,{h},1,1]> {name}_act = {op}(x = {name}_in)[name = string("{name}_act")];',
                    f'    tensor<int32, [2]> {name}_s2 = const()[name = string("{name}_s2"), val = tensor<int32, [2]>([1,{h}])];',
                    f'    {t} {name} = reshape(shape = {name}_s2, x = {name}_act)[name = string("{name}")];'])
            else:
                params = f"x = {a}, y = {b}"
                if op == "matmul": params = "transpose_x = no_transpose, transpose_y = no_transpose, " + params
                lines.append(f'    {t} {name} = {op}({params})[name = string("{name}")];')
        lines.extend(["  } -> (" + ", ".join(self.outputs) + ");", "}"])
        (directory / "cell.mil").write_text("\n".join(lines) + "\n")
        (directory / "weights.bin").write_bytes(blob)
        np.savez(directory / "weights.npz", **self.constants)


def gru(W, R, B=None, *, linear_before_reset=1, candidate="tanh"):
    W, R = np.asarray(W, np.float32), np.asarray(R, np.float32)
    H, I = R.shape[1], W.shape[1]
    if W.shape != (3*H, I) or R.shape != (3*H, H) or candidate not in ("tanh", "relu"):
        raise ValueError("invalid GRU weights/activation")
    B = np.zeros(6*H, np.float32) if B is None else np.asarray(B, np.float32)
    if B.shape != (6*H,) or linear_before_reset not in (0, 1):
        raise ValueError("invalid GRU bias/reset mode")
    g = Graph({"x": (1, I), "h": (1, H)})
    for index, gate in enumerate(("z", "r", "n")):
        g.constant("W"+gate, W[index*H:(index+1)*H].T)
        g.constant("R"+gate, R[index*H:(index+1)*H].T)
        g.constant("B"+gate, B[index*H:(index+1)*H])
        g.constant("C"+gate, B[(index+3)*H:(index+4)*H])
        g.op("x"+gate, "matmul", "x", "W"+gate)
        g.op("xb"+gate, "add", "x"+gate, "B"+gate)
    for gate in ("z", "r"):
        g.op("h"+gate, "matmul", "h", "R"+gate)
        g.op("hb"+gate, "add", "h"+gate, "C"+gate)
        g.op("a"+gate, "add", "xb"+gate, "hb"+gate)
        g.op(gate, "sigmoid", "a"+gate)
    if linear_before_reset:
        g.op("hn", "matmul", "h", "Rn")
        g.op("hbn", "add", "hn", "Cn")
        g.op("reset", "mul", "r", "hbn")
    else:
        g.op("rh", "mul", "r", "h")
        g.op("hn", "matmul", "rh", "Rn")
        g.op("reset", "add", "hn", "Cn")
    g.op("an", "add", "xbn", "reset")
    g.op("n", candidate, "an")
    g.constant("minus_one", -np.ones((1, H), np.float32))
    g.constant("one", np.ones((1, H), np.float32))
    g.op("neg_z", "mul", "z", "minus_one")
    g.op("one_minus_z", "add", "one", "neg_z")
    g.op("candidate_part", "mul", "one_minus_z", "n")
    g.op("history_part", "mul", "z", "h")
    g.op("h_next", "add", "candidate_part", "history_part")
    g.outputs = ["h_next"]
    return g


def lstm(W, R, B=None):
    W, R = np.asarray(W, np.float32), np.asarray(R, np.float32)
    H, I = R.shape[1], W.shape[1]
    B = np.zeros(8*H, np.float32) if B is None else np.asarray(B, np.float32)
    if W.shape != (4*H, I) or R.shape != (4*H, H) or B.shape != (8*H,):
        raise ValueError("invalid LSTM weights")
    g = Graph({"x": (1, I), "h": (1, H), "c": (1, H)})
    for index, gate in enumerate(("i", "o", "f", "g")):
        g.constant("W"+gate, W[index*H:(index+1)*H].T)
        g.constant("R"+gate, R[index*H:(index+1)*H].T)
        g.constant("B"+gate, B[index*H:(index+1)*H] + B[(index+4)*H:(index+5)*H])
        g.op("x"+gate, "matmul", "x", "W"+gate)
        g.op("h"+gate, "matmul", "h", "R"+gate)
        g.op("a"+gate, "add", "x"+gate, "h"+gate)
        g.op("b"+gate, "add", "a"+gate, "B"+gate)
        g.op(gate, "tanh" if gate == "g" else "sigmoid", "b"+gate)
    g.op("forget", "mul", "f", "c")
    g.op("update", "mul", "i", "g")
    g.op("c_next", "add", "forget", "update")
    g.op("cell_tanh", "tanh", "c_next")
    g.op("h_next", "mul", "o", "cell_tanh")
    g.outputs = ["h_next", "c_next"]
    return g
