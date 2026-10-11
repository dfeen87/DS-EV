# -*- coding: utf-8 -*-
"""
DS EV Battery Enhancement - Python ctypes Wrapper (Option B)
=============================================================

This module provides a pure-Python wrapper around the high-performance
compiled C++17 DS middleware and physics engine.
"""

import os
import sys
import ctypes

# Wrapper release metadata; the loaded shared library is a separate artifact.
__version__ = "9.0.0"

# Define the C-compatible struct matching ds_core/src/ds_enhancer.cpp
class DSResult(ctypes.Structure):
    _fields_ = [
        ("degradation", ctypes.c_double),
        ("remaining_capacity_percent", ctypes.c_double),
        ("cycles_to_80_percent", ctypes.c_double),
        ("ds_confidence", ctypes.c_double),
        ("recommended_current_limit", ctypes.c_double),
        ("recommended_voltage_limit", ctypes.c_double),
        ("recommended_temperature", ctypes.c_double),
        ("estimated_charge_time", ctypes.c_double),
        ("degradation_warning", ctypes.c_int),
        ("input_clamped", ctypes.c_int),
        ("numerical_stability", ctypes.c_int)
    ]

class DSEnhancer:
    def __init__(self, capacity_ah=75.0, nominal_voltage_v=400.0, lib_path=None):
        self.capacity_ah = capacity_ah
        self.nominal_voltage_v = nominal_voltage_v
        self.lib = None
        self.handle = None

        if lib_path is None:
            # Look in standard build/installation paths
            possible_paths = [
                # In-tree locations
                os.path.join(os.path.dirname(__file__), "../lib/libds_enhancer.so"),
                os.path.join(os.path.dirname(__file__), "../lib/libds_enhancer.dylib"),
                # Installed location
                "/opt/ds_enhancement/lib/libds_enhancer.so",
                "/opt/ds_enhancement/lib/libds_enhancer.dylib",
                # System locations
                "/usr/local/lib/libds_enhancer.so",
                "/usr/lib/libds_enhancer.so"
            ]
            for path in possible_paths:
                if os.path.exists(path):
                    lib_path = path
                    break

        if lib_path is None or not os.path.exists(lib_path):
            raise FileNotFoundError(
                "Could not find compiled shared library libds_enhancer.so. "
                "Ensure that the DS C++ code is built and installed properly."
            )

        # Load library
        self.lib = ctypes.CDLL(lib_path)

        # Declare C-function signatures
        self.lib.ds_create.restype = ctypes.c_void_p
        self.lib.ds_create.argtypes = [ctypes.c_double, ctypes.c_double]

        self.lib.ds_destroy.restype = None
        self.lib.ds_destroy.argtypes = [ctypes.c_void_p]

        self.lib.ds_enhance.restype = ctypes.c_int
        self.lib.ds_enhance.argtypes = [
            ctypes.c_void_p,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.POINTER(DSResult)
        ]

        # Initialize DS instance
        self.handle = self.lib.ds_create(self.capacity_ah, self.nominal_voltage_v)
        if not self.handle:
            raise RuntimeError("Failed to initialize DS Middleware instance in shared library.")

    def enhance_cycle(self, voltage, current, temperature, soc, dt=0.1):
        """
        Updates the DS engine with physical readings and returns the enhanced predictions.
        """
        if not self.handle:
            raise RuntimeError("Enhancer instance has been destroyed.")

        result = DSResult()
        ret = self.lib.ds_enhance(
            self.handle,
            float(voltage),
            float(current),
            float(temperature),
            float(soc),
            float(dt),
            ctypes.byref(result)
        )

        if ret != 0:
            raise RuntimeError(f"DS Core evaluation failed with code {ret}")

        return {
            "degradation": result.degradation,
            "remaining_capacity_percent": result.remaining_capacity_percent,
            "cycles_to_80_percent": result.cycles_to_80_percent,
            "ds_confidence": result.ds_confidence,
            "recommended_current_limit": result.recommended_current_limit,
            "recommended_voltage_limit": result.recommended_voltage_limit,
            "recommended_temperature": result.recommended_temperature,
            "estimated_charge_time": result.estimated_charge_time,
            "degradation_warning": bool(result.degradation_warning),
            "input_clamped": bool(result.input_clamped),
            "numerical_stability": bool(result.numerical_stability)
        }

    def __del__(self):
        if self.lib and self.handle:
            self.lib.ds_destroy(self.handle)
            self.handle = None

# Simple self-test code when run directly
if __name__ == "__main__":
    print("Running DS Python Wrapper Self-Test...")
    try:
        enhancer = DSEnhancer(capacity_ah=75.0, nominal_voltage_v=400.0)
        res = enhancer.enhance_cycle(voltage=355.0, current=50.0, temperature=25.0, soc=0.65, dt=0.1)
        print("Self-test SUCCESS! Results:")
        for k, v in res.items():
            print(f"  {k}: {v}")
    except Exception as e:
        print(f"Self-test failed: {e}")
        sys.exit(1)
