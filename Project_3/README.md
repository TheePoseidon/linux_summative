# Project 3: Developing a Python C Extension for IoT Sensor Data Filtering and Analysis

`sensor_processor` is a Python C extension written with the Python C API. It filters sensor readings and calculates statistics in C.

## Files

| File | Purpose |
|---|---|
| `sensor_processor.c` | The 5 functions, method table, module definition and `PyInit_sensor_processor` |
| `setup.py` | setuptools build configuration |
| `test_sensor_processor.py` | Test program |
| `test_output.txt` | Output of the test program |

## Environment, Build and Run

Python 3.14.4, setuptools 78.1.1, GCC 15.2.0, Ubuntu 26.04 LTS (x86-64).

```
python3 setup.py build_ext --inplace
python3 test_sensor_processor.py
```

## Functions

| Function | Returns | Formula | Time |
|---|---|---|---|
| `filter_readings(data, minimum, maximum)` | list of floats | keep x where minimum <= x <= maximum | O(n) |
| `count_outliers(data, minimum, maximum)` | int | count x where x < minimum or x > maximum | O(n) |
| `moving_average(data, window)` | list of floats | avg[k] = (x[k] + ... + x[k+w-1]) / w, using S[k+1] = S[k] + x[k+w] - x[k] | O(n) |
| `maximum_difference(data)` | float | max(x) - min(x) | O(n) |
| `sensor_summary(data)` | dict | mean = sum / n, then count x > mean | O(n) |

## Implementation

- **Input:** `data` must be a list or tuple. `PySequence_Fast_ITEMS` reads its elements directly as a C array of borrowed references, without copying. Each element is converted to a `double` with `PyFloat_AS_DOUBLE` or `PyLong_AsDouble`.
- **Validation:**
  - Other container types, non-numeric readings and `bool` raise `TypeError`. NaN or infinity raises `ValueError`.
  - `minimum > maximum` or a non-finite bound raises `ValueError`.
  - A window that is not an int raises `TypeError`, and a window outside 1..len(data) raises `ValueError`.
  - Empty data returns `[]` or `0` for `filter_readings` and `count_outliers`, and raises `ValueError` for the other three.
- **Memory:**
  - No `malloc` is used. Result lists are created once at their final size with `PyList_New`.
  - `PyList_SET_ITEM` takes over each item's reference.
  - On an error the result list is released with `Py_DECREF`.
  - `sensor_summary` builds its dict with `Py_BuildValue`.
- **Accuracy:** sums use Neumaier compensated summation, so long sums and the sliding window do not drift. A result that overflows raises `OverflowError`.

## Performance

- All loops run in C on raw `double` values. A Python loop would create and check a Python object for every reading at every step.
- Every function is a single O(n) pass, or two passes where needed. `moving_average` updates a running sum, which is O(n) and not O(n × w).
- Readings are not copied into a temporary buffer, and result lists are never resized. Memory use stays O(1) apart from the result.
- For large datasets (millions of readings) this removes interpreter overhead from the core calculations.

## Test Results (from `test_output.txt`)

```
filter_readings([10.5, 25.0, 110.0, 35.5], 20.0, 40.0)
    -> [25.0, 35.5]    [PASS]
count_outliers([10.5, 25.0, 110.0, 35.5], 20.0, 40.0)
    -> 2    [PASS]
moving_average([10.0, 20.0, 30.0, 40.0], 2)
    -> [15.0, 25.0, 35.0]    [PASS]
maximum_difference([10.0, 25.0, 40.0, 15.0])
    -> 30.0    [PASS]
sensor_summary([10.0, 20.0, 30.0, 40.0])
    -> {'samples': 4, 'minimum': 10.0, 'maximum': 40.0, 'average': 25.0, 'above_average': 2}    [PASS]
sensor_summary([])
    -> ValueError: sensor_summary() requires at least one reading    [PASS]
filter_readings("25,30", 0, 50)        # string, not a list
    -> TypeError: data must be a list or tuple, not str    [PASS]
filter_readings([1.0, 2.0], 40.0, 20.0)  # minimum > maximum
    -> ValueError: minimum (40) must not be greater than maximum (20)    [PASS]
moving_average([1.0, 2.0, 3.0], 4)      # window > length
    -> ValueError: window must be between 1 and 3 (number of readings), got 4    [PASS]
```

The full run also covers greenhouse sample data, tuples of ints, inclusive bounds, a single reading, all five functions with empty input, and 15 invalid input cases. Result: **30 passed, 0 failed**.
