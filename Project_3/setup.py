from setuptools import Extension, setup

sensor_processor = Extension(
    name="sensor_processor",
    sources=["sensor_processor.c"],
    extra_compile_args=["-O2", "-Wall", "-Wextra"],
    libraries=["m"],
)

setup(
    name="sensor_processor",
    version="1.0.0",
    description="C extension for filtering and analysing sensor readings",
    ext_modules=[sensor_processor],
    python_requires=">=3.8",
)