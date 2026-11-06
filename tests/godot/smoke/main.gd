extends Node
# Prints values computed by GDScript so the recompiled engine's output can be checked exactly.

func fib(n: int) -> int:
	return n if n < 2 else fib(n - 1) + fib(n - 2)

func _ready() -> void:
	print("fib(20)=", fib(20))
	var s := 0.0
	for i in range(1, 1001):
		s += 1.0 / (i * i)
	print("basel=%.9f" % s)
	var d := {"a": 1, "b": [1, 2, 3]}
	print("json=", JSON.stringify(d))
	print("sha256=", "wintomac".sha256_text())
	var v := Vector3(1, 2, 3).cross(Vector3(4, 5, 6))
	print("cross=", v)
	print("sorted=", [5, 3, 9, 1].duplicate().map(func(x): return x * 2))
	var a := [5, 3, 9, 1]; a.sort()
	print("sort=", a)
	print("sin=%.6f" % sin(1.0))
	print("done")
