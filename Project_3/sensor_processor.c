/*
 * sensor_processor.c - Python C extension for filtering and analysing
 *                      sensor readings.
 *
 * Functions exposed to Python:
 *   filter_readings(data, minimum, maximum)  -> list[float]
 *   count_outliers(data, minimum, maximum)   -> int
 *   moving_average(data, window)             -> list[float]
 *   maximum_difference(data)                 -> float
 *   sensor_summary(data)                     -> dict
 *
 * Build:  python3 setup.py build_ext --inplace
 *
 * ---------------------------------------------------------------------------
 * Input rules (shared by all functions)
 *   - data must be a list or tuple; anything else raises TypeError.
 *   - Each reading must be an int or a float (subclasses allowed, bool is
 *     rejected); anything else raises TypeError naming the bad index.
 *   - Each reading must be finite: NaN or +/-inf raises ValueError, because
 *     min/max/mean are undefined for them.
 *   - Empty data: filter_readings returns [] and count_outliers returns 0
 *     (both have a natural answer); moving_average, maximum_difference and
 *     sensor_summary raise ValueError (no meaningful result exists).
 *
 * ---------------------------------------------------------------------------
 * How Python objects are accessed (no copying)
 *   A list or tuple stores its elements as a C array of PyObject pointers.
 *   PySequence_Fast_ITEMS() returns that array directly, so the code reads
 *   element i as items[i] with no function call, no new reference and no
 *   temporary copy of the data. The pointers are BORROWED references: the
 *   container owns them and keeps them alive for the duration of the call.
 *
 *   Each element is converted to a C double by to_double():
 *     float  -> PyFloat_AS_DOUBLE()   (reads the stored double directly)
 *     int    -> PyLong_AsDouble()     (raises OverflowError if too large)
 *   Neither conversion runs Python code, so the container cannot be changed
 *   while it is being traversed (the GIL is held throughout).
 *
 * How references and memory are managed
 *   - Arguments from PyArg_ParseTuple are borrowed: never DECREF'd.
 *   - Every object created here (result lists, floats, ints, the dict) is a
 *     NEW reference that is either returned to Python (ownership passes to
 *     the caller) or released with Py_DECREF on an error path.
 *   - PyList_SET_ITEM "steals" the reference to the item it stores, so the
 *     float created for each slot needs no separate DECREF.
 *   - If an error happens after a result list was created, Py_DECREF(list)
 *     frees the list and every item already stored in it. Unfilled slots
 *     are NULL, which list deallocation skips safely.
 *   - No malloc/PyMem_Malloc is used at all: values are read straight from
 *     the input container, and result lists are created once at their
 *     exact final size (no repeated resizing).
 *   - filter_readings reuses the input's float objects (floats are
 *     immutable) with Py_INCREF instead of creating copies; only int
 *     readings need a new float object.
 *
 * Time complexity (n = number of readings, w = window size)
 *   filter_readings      O(n) time  - two passes (count, then fill)
 *   count_outliers       O(n) time  - one pass
 *   moving_average       O(n) time  - sliding window sum (not O(n*w))
 *   maximum_difference   O(n) time  - one pass
 *   sensor_summary       O(n) time  - two passes (stats, then above-mean)
 *   Extra memory is O(1) apart from the returned result objects.
 *
 * Numerical accuracy
 *   All arithmetic uses double. Sums use Neumaier compensated summation,
 *   which tracks the rounding error lost by each addition and adds it back,
 *   so long sums (and the add/subtract steps of the sliding window) do not
 *   drift. Results that overflow the double range raise OverflowError
 *   instead of silently returning inf.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <math.h>

/* ========================================================================
 * Helper functions
 * ===================================================================== */

/*
 * get_items - check that data is a list or tuple and expose its elements.
 *   On success stores the borrowed element array in *items and the length
 *   in *n, and returns 0. Otherwise sets TypeError and returns -1.
 *   PySequence_Fast_ITEMS accepts a list or tuple directly, so no
 *   PySequence_Fast() wrapper (and no extra reference) is needed.
 */
static int
get_items(PyObject *data, PyObject ***items, Py_ssize_t *n)
{
    if (!PyList_Check(data) && !PyTuple_Check(data)) {
        PyErr_Format(PyExc_TypeError,
                     "data must be a list or tuple, not %.100s",
                     Py_TYPE(data)->tp_name);
        return -1;
    }
    *items = PySequence_Fast_ITEMS(data);
    *n = PySequence_Fast_GET_SIZE(data);
    return 0;
}

/*
 * to_double - convert one reading to a finite C double.
 *   Returns 0 on success, -1 with an exception set on failure.
 */
static int
to_double(PyObject *item, Py_ssize_t index, double *out)
{
    double value;

    if (PyFloat_Check(item)) {
        value = PyFloat_AS_DOUBLE(item);          /* fast path: no call */
    }
    else if (PyLong_Check(item) && !PyBool_Check(item)) {
        value = PyLong_AsDouble(item);            /* exact for |x| < 2^53 */
        if (value == -1.0 && PyErr_Occurred())
            return -1;                            /* OverflowError */
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "data[%zd] must be an int or float, not %.100s",
                     index, Py_TYPE(item)->tp_name);
        return -1;
    }

    if (!isfinite(value)) {
        PyErr_Format(PyExc_ValueError,
                     "data[%zd] is not a finite number (NaN or infinity)",
                     index);
        return -1;
    }
    *out = value;
    return 0;
}

/*
 * check_range - validate the minimum/maximum arguments.
 *   Both must be finite and minimum <= maximum (equal is allowed: a range
 *   containing exactly one value).
 */
static int
check_range(double minimum, double maximum)
{
    if (!isfinite(minimum) || !isfinite(maximum)) {
        PyErr_SetString(PyExc_ValueError,
                        "minimum and maximum must be finite numbers");
        return -1;
    }
    if (minimum > maximum) {
        /* PyErr_Format has no %g, so format the numbers with snprintf */
        char msg[128];
        snprintf(msg, sizeof msg,
                 "minimum (%g) must not be greater than maximum (%g)",
                 minimum, maximum);
        PyErr_SetString(PyExc_ValueError, msg);
        return -1;
    }
    return 0;
}

/*
 * Neumaier compensated summation.
 *   A plain "sum += x" loses the low-order bits of x whenever sum is much
 *   larger. The lost part is (sum - t) + x (or (x - t) + sum when x is the
 *   larger term); it is collected in comp and added back at the end:
 *       total = sum + comp
 */
typedef struct {
    double sum;
    double comp;
} CompSum;

static inline void
compsum_add(CompSum *s, double x)
{
    double t = s->sum + x;
    if (fabs(s->sum) >= fabs(x))
        s->comp += (s->sum - t) + x;
    else
        s->comp += (x - t) + s->sum;
    s->sum = t;
}

static inline double
compsum_value(const CompSum *s)
{
    return s->sum + s->comp;
}

/* Raise OverflowError if a computed result is not finite. */
static int
check_result(double value, const char *what)
{
    if (!isfinite(value)) {
        PyErr_Format(PyExc_OverflowError,
                     "%s is too large to represent as a float", what);
        return -1;
    }
    return 0;
}

/* ========================================================================
 * 1. filter_readings(data, minimum, maximum)
 *    Keeps readings x with minimum <= x <= maximum.
 *    Pass 1 validates every reading and counts the matches; pass 2 creates
 *    the result list at its exact size and fills it. Validating first means
 *    no result is built for invalid input, and the list is never resized.
 *    Matching float objects are shared rather than copied.
 *    Time O(n), extra memory O(1) besides the result.
 * ===================================================================== */
PyDoc_STRVAR(filter_readings_doc,
"filter_readings(data, minimum, maximum) -> list\n\n"
"Return a new list of the readings within [minimum, maximum] (inclusive),\n"
"as floats, in their original order.");

static PyObject *
filter_readings(PyObject *Py_UNUSED(self), PyObject *args)
{
    PyObject *data, **items, *result;
    double minimum, maximum, value;
    Py_ssize_t n, i, count = 0, pos = 0;

    /* "Odd": data as an object, minimum and maximum converted to double.
     * A non-numeric minimum/maximum raises TypeError here. */
    if (!PyArg_ParseTuple(args, "Odd:filter_readings",
                          &data, &minimum, &maximum))
        return NULL;
    if (check_range(minimum, maximum) < 0 || get_items(data, &items, &n) < 0)
        return NULL;

    /* Pass 1: validate and count readings inside the range */
    for (i = 0; i < n; i++) {
        if (to_double(items[i], i, &value) < 0)
            return NULL;
        if (value >= minimum && value <= maximum)
            count++;
    }

    result = PyList_New(count);                  /* new reference */
    if (result == NULL)
        return NULL;

    /* Pass 2: store the matching readings (already validated) */
    for (i = 0; i < n && pos < count; i++) {
        to_double(items[i], i, &value);          /* cannot fail now */
        if (value >= minimum && value <= maximum) {
            PyObject *f;
            if (PyFloat_CheckExact(items[i])) {
                /* Floats are immutable, so the existing object can be
                 * shared instead of allocating an identical new one.
                 * INCREF turns the borrowed reference into one the
                 * result list owns. */
                f = items[i];
                Py_INCREF(f);
            }
            else {
                f = PyFloat_FromDouble(value);   /* int -> new float */
                if (f == NULL) {
                    Py_DECREF(result);           /* frees stored items too */
                    return NULL;
                }
            }
            PyList_SET_ITEM(result, pos++, f);   /* list steals reference */
        }
    }
    return result;                               /* ownership to caller */
}

/* ========================================================================
 * 2. count_outliers(data, minimum, maximum)
 *    Counts readings x with x < minimum or x > maximum, i.e.
 *    outliers = n - (number of readings inside the range).
 *    Time O(n), extra memory O(1).
 * ===================================================================== */
PyDoc_STRVAR(count_outliers_doc,
"count_outliers(data, minimum, maximum) -> int\n\n"
"Return how many readings fall outside [minimum, maximum].");

static PyObject *
count_outliers(PyObject *Py_UNUSED(self), PyObject *args)
{
    PyObject *data, **items;
    double minimum, maximum, value;
    Py_ssize_t n, i, outliers = 0;

    if (!PyArg_ParseTuple(args, "Odd:count_outliers",
                          &data, &minimum, &maximum))
        return NULL;
    if (check_range(minimum, maximum) < 0 || get_items(data, &items, &n) < 0)
        return NULL;

    for (i = 0; i < n; i++) {
        if (to_double(items[i], i, &value) < 0)
            return NULL;
        if (value < minimum || value > maximum)
            outliers++;
    }
    return PyLong_FromSsize_t(outliers);         /* new reference */
}

/* ========================================================================
 * 3. moving_average(data, window)
 *    For each position k = 0 .. n-w:
 *        avg[k] = (x[k] + x[k+1] + ... + x[k+w-1]) / w
 *    giving n - w + 1 values.
 *
 *    Sliding window: instead of re-adding w values for every output
 *    (O(n*w)), keep a running sum. Moving one step right adds the reading
 *    entering the window and subtracts the one leaving it:
 *        S[k+1] = S[k] + x[k+w] - x[k]
 *    so each reading is added once and subtracted once: O(n) total.
 *    The running sum is compensated (Neumaier) so repeated add/subtract
 *    does not accumulate rounding drift.
 *    Readings are converted on demand from the input container instead of
 *    being copied into a temporary C array, so no buffer is allocated.
 * ===================================================================== */
PyDoc_STRVAR(moving_average_doc,
"moving_average(data, window) -> list\n\n"
"Return the averages of every run of `window` consecutive readings\n"
"(len(data) - window + 1 values). window must be an int with\n"
"1 <= window <= len(data).");

static PyObject *
moving_average(PyObject *Py_UNUSED(self), PyObject *args)
{
    PyObject *data, *window_obj, **items, *result;
    Py_ssize_t n, i, window, outputs;
    CompSum s = {0.0, 0.0};
    double value, old, avg;

    if (!PyArg_ParseTuple(args, "OO:moving_average", &data, &window_obj))
        return NULL;
    if (get_items(data, &items, &n) < 0)
        return NULL;

    /* window must be a real int (2.0, "2" and True are rejected) */
    if (!PyLong_Check(window_obj) || PyBool_Check(window_obj)) {
        PyErr_Format(PyExc_TypeError, "window must be an int, not %.100s",
                     Py_TYPE(window_obj)->tp_name);
        return NULL;
    }
    window = PyLong_AsSsize_t(window_obj);
    if (window == -1 && PyErr_Occurred())
        return NULL;

    if (n == 0) {
        PyErr_SetString(PyExc_ValueError,
                        "moving_average() requires at least one reading");
        return NULL;
    }
    if (window < 1 || window > n) {
        PyErr_Format(PyExc_ValueError,
                     "window must be between 1 and %zd (number of "
                     "readings), got %zd", n, window);
        return NULL;
    }

    outputs = n - window + 1;
    result = PyList_New(outputs);                /* new reference */
    if (result == NULL)
        return NULL;

    /* Fill the first window: S[0] = x[0] + ... + x[w-1] */
    for (i = 0; i < window; i++) {
        if (to_double(items[i], i, &value) < 0)
            goto error;
        compsum_add(&s, value);
    }

    /* Slide across the data, writing one average per position */
    for (i = 0; i < outputs; i++) {
        PyObject *f;

        if (i > 0) {
            Py_ssize_t enter = i + window - 1;   /* reading entering */
            if (to_double(items[enter], enter, &value) < 0)
                goto error;
            to_double(items[i - 1], i - 1, &old);  /* leaving; validated */
            compsum_add(&s, value);
            compsum_add(&s, -old);
        }
        avg = compsum_value(&s) / (double)window;
        if (check_result(avg, "moving average") < 0)
            goto error;
        f = PyFloat_FromDouble(avg);
        if (f == NULL)
            goto error;
        PyList_SET_ITEM(result, i, f);           /* steals reference */
    }
    return result;

error:
    Py_DECREF(result);                           /* releases filled slots */
    return NULL;
}

/* ========================================================================
 * 4. maximum_difference(data)
 *    range = max(x) - min(x), found in a single pass that updates both the
 *    running minimum and maximum. Time O(n), extra memory O(1).
 * ===================================================================== */
PyDoc_STRVAR(maximum_difference_doc,
"maximum_difference(data) -> float\n\n"
"Return max(data) - min(data).");

static PyObject *
maximum_difference(PyObject *Py_UNUSED(self), PyObject *args)
{
    PyObject *data, **items;
    Py_ssize_t n, i;
    double value, lo, hi, diff;

    if (!PyArg_ParseTuple(args, "O:maximum_difference", &data))
        return NULL;
    if (get_items(data, &items, &n) < 0)
        return NULL;
    if (n == 0) {
        PyErr_SetString(PyExc_ValueError,
                        "maximum_difference() requires at least one reading");
        return NULL;
    }

    if (to_double(items[0], 0, &value) < 0)
        return NULL;
    lo = hi = value;                             /* start from x[0] */
    for (i = 1; i < n; i++) {
        if (to_double(items[i], i, &value) < 0)
            return NULL;
        if (value < lo) lo = value;
        if (value > hi) hi = value;
    }

    diff = hi - lo;                              /* can overflow, e.g. */
    if (check_result(diff, "maximum difference") < 0)   /* 1e308-(-1e308) */
        return NULL;
    return PyFloat_FromDouble(diff);
}

/* ========================================================================
 * 5. sensor_summary(data)
 *    Pass 1: n, minimum, maximum and the compensated sum.
 *            mean = (x[0] + ... + x[n-1]) / n
 *    Pass 2: count readings strictly greater than the mean (the mean is
 *            only known after pass 1, so a second pass is required).
 *    Time O(n) (2n steps), extra memory O(1).
 * ===================================================================== */
PyDoc_STRVAR(sensor_summary_doc,
"sensor_summary(data) -> dict\n\n"
"Return {'samples', 'minimum', 'maximum', 'average', 'above_average'}.\n"
"'above_average' counts readings strictly greater than the mean.");

static PyObject *
sensor_summary(PyObject *Py_UNUSED(self), PyObject *args)
{
    PyObject *data, **items;
    Py_ssize_t n, i, above = 0;
    CompSum s = {0.0, 0.0};
    double value, lo, hi, mean;

    if (!PyArg_ParseTuple(args, "O:sensor_summary", &data))
        return NULL;
    if (get_items(data, &items, &n) < 0)
        return NULL;
    if (n == 0) {
        PyErr_SetString(PyExc_ValueError,
                        "sensor_summary() requires at least one reading");
        return NULL;
    }

    /* Pass 1: validate, track min/max, accumulate the sum */
    if (to_double(items[0], 0, &value) < 0)
        return NULL;
    lo = hi = value;
    compsum_add(&s, value);
    for (i = 1; i < n; i++) {
        if (to_double(items[i], i, &value) < 0)
            return NULL;
        if (value < lo) lo = value;
        if (value > hi) hi = value;
        compsum_add(&s, value);
    }
    mean = compsum_value(&s) / (double)n;
    if (check_result(mean, "average") < 0)
        return NULL;

    /* Pass 2: count readings above the mean (all already validated) */
    for (i = 0; i < n; i++) {
        to_double(items[i], i, &value);
        if (value > mean)
            above++;
    }

    /* Py_BuildValue creates the dict and its int/float values in one call
     * and manages their references: it returns one new reference (the
     * dict), or NULL with everything already released on failure.
     * "n" = Py_ssize_t -> int, "d" = double -> float. */
    return Py_BuildValue("{s:n,s:d,s:d,s:d,s:n}",
                         "samples", n,
                         "minimum", lo,
                         "maximum", hi,
                         "average", mean,
                         "above_average", above);
}

/* ========================================================================
 * Method table: maps each Python name to its C function. METH_VARARGS
 * means the arguments arrive as a tuple, unpacked with PyArg_ParseTuple.
 * ===================================================================== */
static PyMethodDef SensorProcessorMethods[] = {
    {"filter_readings",    filter_readings,    METH_VARARGS,
     filter_readings_doc},
    {"count_outliers",     count_outliers,     METH_VARARGS,
     count_outliers_doc},
    {"moving_average",     moving_average,     METH_VARARGS,
     moving_average_doc},
    {"maximum_difference", maximum_difference, METH_VARARGS,
     maximum_difference_doc},
    {"sensor_summary",     sensor_summary,     METH_VARARGS,
     sensor_summary_doc},
    {NULL, NULL, 0, NULL}                        /* sentinel */
};

/* ========================================================================
 * Module definition
 * ===================================================================== */
PyDoc_STRVAR(module_doc,
"Fast filtering and statistics for sensor readings, implemented in C.");

static struct PyModuleDef sensor_processor_module = {
    PyModuleDef_HEAD_INIT,
    "sensor_processor",      /* module name */
    module_doc,              /* module docstring */
    -1,                      /* no per-module state */
    SensorProcessorMethods,  /* method table */
    NULL, NULL, NULL, NULL   /* slots, traverse, clear, free: unused */
};

/* ========================================================================
 * Module initialisation: called by "import sensor_processor".
 * The name must be PyInit_<module name>.
 * ===================================================================== */
PyMODINIT_FUNC
PyInit_sensor_processor(void)
{
    return PyModule_Create(&sensor_processor_module);
}