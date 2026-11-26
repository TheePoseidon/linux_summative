"""
Build configuration for the sensor_processor C extension.

Build in place (creates sensor_processor.*.so next to this file):
    python3 setup.py build_ext --inplace

Or install into the current environment:
    python3 -m pip install .
"""

from setuptools import Extension, setup

sensor_processor = Extension(
    name="sensor_processor",
    sources=["sensor_processor.c"],
    extra_compile_args=["-O2", "-Wall", "-Wextra"],
    libraries=["m"],                 # C math library (fabs, isfinite)
)

setup(
    name="sensor_processor",
    version="1.0.0",
    description="C extension for filtering and analysing sensor readings",
    ext_modules=[sensor_processor],
    python_requires=">=3.8",
)