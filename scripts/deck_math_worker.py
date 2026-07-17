#!/usr/bin/env python3
"""Hardened SymPy worker. Protocol: action NUL expression NUL min NUL max."""

import ast
import math
import signal
import sys


def fail(message):
    sys.stdout.write("error\0" + message)
    raise SystemExit(0)


try:
    import sympy as sp
    from sympy.parsing.sympy_parser import convert_xor, parse_expr, standard_transformations
except Exception:
    fail("SymPy is not installed in the selected Python environment")


raw = sys.stdin.buffer.read(4096).decode("utf-8", "replace").split("\0")
action = raw[0] if raw else "calc"
expression = raw[1].strip() if len(raw) > 1 else ""
if not expression or len(expression) > 4096:
    fail("Expression is empty or too large")


def evaluation_timeout(_signum, _frame):
    fail("Evaluation timed out after 2 seconds")


signal.signal(signal.SIGALRM, evaluation_timeout)
signal.setitimer(signal.ITIMER_REAL, 2.0)

symbols = {name: sp.Symbol(name) for name in ("x", "y", "z", "t", "n")}
allowed = {
    **symbols,
    "sin": sp.sin, "cos": sp.cos, "tan": sp.tan, "exp": sp.exp, "log": sp.log,
    "sqrt": sp.sqrt, "abs": sp.Abs, "factor": sp.factor, "expand": sp.expand,
    "simplify": sp.simplify, "solve": sp.solve, "diff": sp.diff,
    "integrate": sp.integrate, "limit": sp.limit, "Matrix": sp.Matrix,
    "det": sp.det, "pi": sp.pi, "E": sp.E, "oo": sp.oo,
}
allowed_nodes = (
    ast.Expression, ast.Call, ast.Name, ast.Load, ast.Constant, ast.List, ast.Tuple,
    ast.BinOp, ast.UnaryOp, ast.Add, ast.Sub, ast.Mult, ast.Div, ast.Pow, ast.BitXor,
    ast.Mod, ast.USub, ast.UAdd,
)
try:
    tree = ast.parse(expression, mode="eval")
except SyntaxError as exc:
    fail("Invalid expression: " + str(exc))
for node in ast.walk(tree):
    if not isinstance(node, allowed_nodes):
        fail("Unsafe or unsupported syntax: " + type(node).__name__)
    if isinstance(node, ast.Name) and node.id not in allowed:
        fail("Unknown name: " + node.id)
    if isinstance(node, ast.Call) and not isinstance(node.func, ast.Name):
        fail("Only direct calls to supported math functions are allowed")

globals_dict = {
    "__builtins__": {}, "Integer": sp.Integer, "Float": sp.Float, "Rational": sp.Rational,
}
try:
    result = parse_expr(
        expression, local_dict=allowed, global_dict=globals_dict,
        transformations=standard_transformations + (convert_xor,), evaluate=True,
    )
except Exception as exc:
    fail("Evaluation failed: " + str(exc))


def braille_plot(expr, variable, low, high):
    width, height = 80, 32
    values = []
    for index in range(160):
        x_value = low + (high - low) * index / 159
        try:
            y_value = float(sp.N(expr.subs(variable, x_value)))
            values.append((x_value, y_value if math.isfinite(y_value) else None))
        except Exception:
            values.append((x_value, None))
    finite = [value for _, value in values if value is not None]
    if not finite:
        return "No finite samples in plot range"
    y_low, y_high = min(finite), max(finite)
    if y_low == y_high:
        y_low -= 1
        y_high += 1
    pixels = [[False] * width for _ in range(height)]
    for index, (_, value) in enumerate(values):
        if value is None:
            continue
        px = min(width - 1, int(index * width / len(values)))
        py = min(height - 1, max(0, int((y_high - value) / (y_high - y_low) * (height - 1))))
        pixels[py][px] = True
    dots = ((0, 0, 1), (0, 1, 2), (0, 2, 4), (1, 0, 8),
            (1, 1, 16), (1, 2, 32), (0, 3, 64), (1, 3, 128))
    rows = []
    for top in range(0, height, 4):
        row = []
        for left in range(0, width, 2):
            code = 0
            for dx, dy, bit in dots:
                if pixels[top + dy][left + dx]:
                    code |= bit
            row.append(chr(0x2800 + code))
        rows.append("".join(row))
    rows.append(f"{variable}: [{low:g}, {high:g}]  y: [{y_low:g}, {y_high:g}]  160 samples")
    return "\n".join(rows)


if action == "plot":
    free = sorted(result.free_symbols, key=str)
    variable = free[0] if free else symbols["x"]
    try:
        low = float(raw[2]) if len(raw) > 2 else -10.0
        high = float(raw[3]) if len(raw) > 3 else 10.0
    except ValueError:
        fail("Invalid plot range")
    if not low < high:
        fail("Plot minimum must be less than maximum")
    rendered = braille_plot(result, variable, low, high)
    signal.setitimer(signal.ITIMER_REAL, 0)
    sys.stdout.write("ok\0" + rendered)
else:
    plain = str(result)
    pretty = sp.pretty(result, use_unicode=True)
    signal.setitimer(signal.ITIMER_REAL, 0)
    sys.stdout.write("ok\0" + plain + "\0" + pretty)
