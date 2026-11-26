import math
import sys

import sensor_processor as sp

passed = 0
failed = 0


def heading(title):
    print()
    print("=" * 70)
    print(title)
    print("=" * 70)


def check(call_text, result, expected):
    global passed, failed
    ok = result == expected
    passed += ok
    failed += not ok
    print(f"{call_text}\n    -> {result!r}    [{'PASS' if ok else 'FAIL'}]")


def show(call_text, result):
    print(f"{call_text}\n    -> {result!r}")


def expect_error(call_text, func, *args, error=Exception):
    global passed, failed
    try:
        result = func(*args)
    except error as exc:
        passed += 1
        print(f"{call_text}\n    -> {type(exc).__name__}: {exc}    [PASS]")
    except Exception as exc:
        failed += 1
        print(f"{call_text}\n    -> {type(exc).__name__}: {exc}    "
              f"[FAIL - expected {error.__name__}]")
    else:
        failed += 1
        print(f"{call_text}\n    -> {result!r}    "
              f"[FAIL - expected {error.__name__}]")


heading("1. Examples from the assignment brief")

check("filter_readings([10.5, 25.0, 110.0, 35.5], 20.0, 40.0)",
      sp.filter_readings([10.5, 25.0, 110.0, 35.5], 20.0, 40.0),
      [25.0, 35.5])
check("count_outliers([10.5, 25.0, 110.0, 35.5], 20.0, 40.0)",
      sp.count_outliers([10.5, 25.0, 110.0, 35.5], 20.0, 40.0),
      2)
check("moving_average([10.0, 20.0, 30.0, 40.0], 2)",
      sp.moving_average([10.0, 20.0, 30.0, 40.0], 2),
      [15.0, 25.0, 35.0])
check("maximum_difference([10.0, 25.0, 40.0, 15.0])",
      sp.maximum_difference([10.0, 25.0, 40.0, 15.0]),
      30.0)
check("sensor_summary([10.0, 20.0, 30.0, 40.0])",
      sp.sensor_summary([10.0, 20.0, 30.0, 40.0]),
      {"samples": 4, "minimum": 10.0, "maximum": 40.0,
       "average": 25.0, "above_average": 2})

heading("2. Sample data: greenhouse temperatures (deg C), range 15-35")

temperatures = [21.4, 22.0, 23.5, -40.0, 24.8, 26.1,
                27.3, 85.0, 25.9, 24.2, 22.7, 21.8]
print(f"temperatures = {temperatures}\n")

show("filter_readings(temperatures, 15.0, 35.0)",
     sp.filter_readings(temperatures, 15.0, 35.0))
show("count_outliers(temperatures, 15.0, 35.0)",
     sp.count_outliers(temperatures, 15.0, 35.0))
show("moving_average(temperatures, 3)",
     [round(x, 3) for x in sp.moving_average(temperatures, 3)])
show("maximum_difference(temperatures)",
     sp.maximum_difference(temperatures))
show("sensor_summary(temperatures)",
     sp.sensor_summary(temperatures))

print("\nCleaned data (outliers removed first):")
clean = sp.filter_readings(temperatures, 15.0, 35.0)
show("sensor_summary(clean)", sp.sensor_summary(clean))

heading("3. Other accepted inputs")

check("filter_readings((5, 15, 25), 10, 30)      # tuple of ints",
      sp.filter_readings((5, 15, 25), 10, 30), [15.0, 25.0])
check("count_outliers([20.0, 30.0], 20.0, 30.0)  # bounds are inclusive",
      sp.count_outliers([20.0, 30.0], 20.0, 30.0), 0)
check("moving_average([4.0, 8.0], 1)             # window 1 = copy",
      sp.moving_average([4.0, 8.0], 1), [4.0, 8.0])
check("moving_average([4.0, 8.0], 2)             # window = length",
      sp.moving_average([4.0, 8.0], 2), [6.0])
check("sensor_summary([7.5])                     # single reading",
      sp.sensor_summary([7.5]),
      {"samples": 1, "minimum": 7.5, "maximum": 7.5,
       "average": 7.5, "above_average": 0})

heading("4. Empty input")

check("filter_readings([], 0.0, 100.0)", sp.filter_readings([], 0.0, 100.0), [])
check("count_outliers((), 0.0, 100.0)", sp.count_outliers((), 0.0, 100.0), 0)
expect_error("moving_average([], 1)",
             sp.moving_average, [], 1, error=ValueError)
expect_error("maximum_difference([])",
             sp.maximum_difference, [], error=ValueError)
expect_error("sensor_summary([])",
             sp.sensor_summary, [], error=ValueError)

heading("5. Invalid input")

expect_error('filter_readings("25,30", 0, 50)        # string, not a list',
             sp.filter_readings, "25,30", 0, 50, error=TypeError)
expect_error("sensor_summary({1.0, 2.0})              # set, not a list",
             sp.sensor_summary, {1.0, 2.0}, error=TypeError)

expect_error('count_outliers([25.0, "hot", 30.0], 0, 50)',
             sp.count_outliers, [25.0, "hot", 30.0], 0, 50, error=TypeError)
expect_error("maximum_difference([10.0, None])",
             sp.maximum_difference, [10.0, None], error=TypeError)
expect_error("sensor_summary([1.0, True])            # bool rejected",
             sp.sensor_summary, [1.0, True], error=TypeError)
expect_error("sensor_summary([1.0, math.nan])",
             sp.sensor_summary, [1.0, math.nan], error=ValueError)
expect_error("maximum_difference([1.0, math.inf])",
             sp.maximum_difference, [1.0, math.inf], error=ValueError)

expect_error("filter_readings([1.0, 2.0], 40.0, 20.0)  # minimum > maximum",
             sp.filter_readings, [1.0, 2.0], 40.0, 20.0, error=ValueError)
expect_error('count_outliers([1.0], "low", 20.0)      # non-numeric bound',
             sp.count_outliers, [1.0], "low", 20.0, error=TypeError)
expect_error("filter_readings([1.0], math.nan, 20.0)  # NaN bound",
             sp.filter_readings, [1.0], math.nan, 20.0, error=ValueError)

expect_error("moving_average([1.0, 2.0, 3.0], 0)",
             sp.moving_average, [1.0, 2.0, 3.0], 0, error=ValueError)
expect_error("moving_average([1.0, 2.0, 3.0], -2)",
             sp.moving_average, [1.0, 2.0, 3.0], -2, error=ValueError)
expect_error("moving_average([1.0, 2.0, 3.0], 4)      # window > length",
             sp.moving_average, [1.0, 2.0, 3.0], 4, error=ValueError)
expect_error("moving_average([1.0, 2.0, 3.0], 2.5)    # not an int",
             sp.moving_average, [1.0, 2.0, 3.0], 2.5, error=TypeError)

expect_error("sensor_summary()",
             sp.sensor_summary, error=TypeError)

heading("Result")
print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)