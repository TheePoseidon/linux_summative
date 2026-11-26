#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <math.h>

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

static int
to_double(PyObject *item, Py_ssize_t index, double *out)
{
    double value;

    if (PyFloat_Check(item)) {
        value = PyFloat_AS_DOUBLE(item);
    }
    else if (PyLong_Check(item) && !PyBool_Check(item)) {
        value = PyLong_AsDouble(item);
        if (value == -1.0 && PyErr_Occurred())
            return -1;
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

static int
check_range(double minimum, double maximum)
{
    if (!isfinite(minimum) || !isfinite(maximum)) {
        PyErr_SetString(PyExc_ValueError,
                        "minimum and maximum must be finite numbers");
        return -1;
    }
    if (minimum > maximum) {
        char msg[128];
        snprintf(msg, sizeof msg,
                 "minimum (%g) must not be greater than maximum (%g)",
                 minimum, maximum);
        PyErr_SetString(PyExc_ValueError, msg);
        return -1;
    }
    return 0;
}

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

    if (!PyArg_ParseTuple(args, "Odd:filter_readings",
                          &data, &minimum, &maximum))
        return NULL;
    if (check_range(minimum, maximum) < 0 || get_items(data, &items, &n) < 0)
        return NULL;

    for (i = 0; i < n; i++) {
        if (to_double(items[i], i, &value) < 0)
            return NULL;
        if (value >= minimum && value <= maximum)
            count++;
    }

    result = PyList_New(count);
    if (result == NULL)
        return NULL;

    for (i = 0; i < n && pos < count; i++) {
        to_double(items[i], i, &value);
        if (value >= minimum && value <= maximum) {
            PyObject *f;
            if (PyFloat_CheckExact(items[i])) {
                f = items[i];
                Py_INCREF(f);
            }
            else {
                f = PyFloat_FromDouble(value);
                if (f == NULL) {
                    Py_DECREF(result);
                    return NULL;
                }
            }
            PyList_SET_ITEM(result, pos++, f);
        }
    }
    return result;
}

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
    return PyLong_FromSsize_t(outliers);
}

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
    result = PyList_New(outputs);
    if (result == NULL)
        return NULL;

    for (i = 0; i < window; i++) {
        if (to_double(items[i], i, &value) < 0)
            goto error;
        compsum_add(&s, value);
    }

    for (i = 0; i < outputs; i++) {
        PyObject *f;

        if (i > 0) {
            Py_ssize_t enter = i + window - 1;
            if (to_double(items[enter], enter, &value) < 0)
                goto error;
            to_double(items[i - 1], i - 1, &old);
            compsum_add(&s, value);
            compsum_add(&s, -old);
        }
        avg = compsum_value(&s) / (double)window;
        if (check_result(avg, "moving average") < 0)
            goto error;
        f = PyFloat_FromDouble(avg);
        if (f == NULL)
            goto error;
        PyList_SET_ITEM(result, i, f);
    }
    return result;

error:
    Py_DECREF(result);
    return NULL;
}

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
    lo = hi = value;
    for (i = 1; i < n; i++) {
        if (to_double(items[i], i, &value) < 0)
            return NULL;
        if (value < lo) lo = value;
        if (value > hi) hi = value;
    }

    diff = hi - lo;
    if (check_result(diff, "maximum difference") < 0)
        return NULL;
    return PyFloat_FromDouble(diff);
}

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

    for (i = 0; i < n; i++) {
        to_double(items[i], i, &value);
        if (value > mean)
            above++;
    }

    return Py_BuildValue("{s:n,s:d,s:d,s:d,s:n}",
                         "samples", n,
                         "minimum", lo,
                         "maximum", hi,
                         "average", mean,
                         "above_average", above);
}

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
    {NULL, NULL, 0, NULL}
};

PyDoc_STRVAR(module_doc,
"Fast filtering and statistics for sensor readings, implemented in C.");

static struct PyModuleDef sensor_processor_module = {
    PyModuleDef_HEAD_INIT,
    "sensor_processor",
    module_doc,
    -1,
    SensorProcessorMethods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC
PyInit_sensor_processor(void)
{
    return PyModule_Create(&sensor_processor_module);
}