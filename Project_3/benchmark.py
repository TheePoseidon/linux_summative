"""
benchmark.py - compare sensor_processor (C) with equivalent pure-Python code.

Run after building:
    python3 benchmark.py
"""

import random
import time

import sensor_processor as sp

N = 1_000_000
WINDOW = 60
LOW, HIGH = 15.0, 35.0


# --- Pure-Python reference implementations (same results) -----------------
def py_filter(data, lo, hi):
    return [float(x) for x in data if lo <= x <= hi]


def py_outliers(data, lo, hi):
    return sum(1 for x in data if x < lo or x > hi)


def py_moving_average(data, w):            # sliding sum, O(n)
    total = sum(data[:w])
    out = [total / w]
    for i in range(w, len(data)):
        total += data[i] - data[i - w]
        out.append(total / w)
    return out


def py_max_difference(data):
    return max(data) - min(data)


def py_summary(data):
    n = len(data)
    mean = sum(data) / n
    return {"samples": n, "minimum": min(data), "maximum": max(data),
            "average": mean, "above_average": sum(1 for x in data if x > mean)}


def best_time(func, *args, repeat=3):
    best = float("inf")
    for _ in range(repeat):
        start = time.perf_counter()
        func(*args)
        best = min(best, time.perf_counter() - start)
    return best


random.seed(42)
data = [random.gauss(25.0, 6.0) for _ in range(N)]

cases = [
    ("filter_readings", py_filter, sp.filter_readings, (data, LOW, HIGH)),
    ("count_outliers", py_outliers, sp.count_outliers, (data, LOW, HIGH)),
    ("moving_average", py_moving_average, sp.moving_average, (data, WINDOW)),
    ("maximum_difference", py_max_difference, sp.maximum_difference, (data,)),
    ("sensor_summary", py_summary, sp.sensor_summary, (data,)),
]

print(f"{N:,} readings, window = {WINDOW}, best of 3 runs\n")
print(f"{'Function':<20}{'Python (ms)':>13}{'C (ms)':>10}{'Speed-up':>11}")
print("-" * 54)
for name, py_func, c_func, args in cases:
    t_py = best_time(py_func, *args)
    t_c = best_time(c_func, *args)
    print(f"{name:<20}{t_py * 1000:>13.1f}{t_c * 1000:>10.1f}"
          f"{t_py / t_c:>10.1f}x")