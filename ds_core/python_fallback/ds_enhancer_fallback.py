# -*- coding: utf-8 -*-
"""
DS EV Battery Enhancement - Pure Python Fallback (Option C)
============================================================

A pure-Python, zero-dependency mathematical approximation of the DS physics
and dual-state battery model. Ideal for low-resource environments or rapid prototyping
where C++ compilation is unavailable.
"""

import math
from numbers import Real

class DSEnhancerFallback:
    """Approximate model with an all-or-nothing, finite-state cycle contract.

    Invalid input, configuration, or derived arithmetic raises ``ValueError``
    before any cycle state changes. Finite current, temperature above absolute
    zero, and SOC retain their historical model clamping. Voltage must be
    positive and finite; it is a compatibility input, not a driver of this
    approximate model. This model does not establish equivalence with C++.
    """

    def __init__(self, capacity_ah=75.0, nominal_voltage_v=400.0):
        capacity_ah, nominal_voltage_v = self._nominal_configuration(
            capacity_ah, nominal_voltage_v)
        self.capacity_ah = capacity_ah
        self.nominal_voltage_v = nominal_voltage_v

        # Internal state variables
        self.entropy = 0.0
        self.cycle_count = 0.0
        self.degradation = 0.0
        self.charge_throughput_ah = 0.0
        self.time = 0.0

        # Simulation parameters matching C++ defaults
        self.lambda_val = 1e-6
        self.phi_decay_rate = 0.001
        self.entropy_weight = 0.5
        self.max_temperature = 60.0
        self.min_temperature = -20.0
        self.max_current = 500.0

    @staticmethod
    def _number(name, value):
        # Do not turn strings, booleans, or nonfinite sensor evidence into
        # apparently valid model input by coercion or ordered clamping.
        if isinstance(value, bool) or not isinstance(value, Real):
            raise ValueError(f"{name} must be a finite real number")
        try:
            value = float(value)
        except (OverflowError, TypeError, ValueError) as error:
            raise ValueError(f"{name} must be a finite real number") from error
        if not math.isfinite(value):
            raise ValueError(f"{name} must be a finite real number")
        return value

    @classmethod
    def _nominal_configuration(cls, capacity_ah, nominal_voltage_v):
        capacity_ah = cls._number("capacity_ah", capacity_ah)
        nominal_voltage_v = cls._number("nominal_voltage_v", nominal_voltage_v)
        if capacity_ah <= 0.0 or nominal_voltage_v <= 0.0:
            raise ValueError("nominal capacity and voltage must be positive")
        cls._require_finite(cycle_capacity=2.0 * capacity_ah,
                            maximum_voltage_recommendation=1.1 * nominal_voltage_v)
        return capacity_ah, nominal_voltage_v

    @staticmethod
    def _require_finite(**values):
        for name, value in values.items():
            if not math.isfinite(value):
                raise ValueError(f"nonfinite fallback arithmetic: {name}")

    def enhance_cycle(self, voltage, current, temperature, soc, dt=0.1):
        """
        Pure Python fallback for the DS update cycle.
        Approximate thermodynamic and information-physical states (Ψ, Φ).

        ``dt`` is positive seconds, current is signed amperes, temperature is
        degrees Celsius, and SOC is a fraction. Both current signs contribute
        absolute ampere-hour throughput and equivalent full cycles. Rejected
        cycles raise ``ValueError`` and preserve all persistent state.
        """
        voltage = self._number("voltage", voltage)
        current = self._number("current", current)
        temperature = self._number("temperature", temperature)
        soc = self._number("soc", soc)
        dt = self._number("dt", dt)
        if voltage <= 0.0 or dt <= 0.0 or temperature < -273.15:
            raise ValueError("voltage and timestep must be positive; temperature must be physical")

        # Public model attributes remain configurable, so validate the actual
        # configuration and existing state as well as constructor arguments.
        capacity_ah, nominal_voltage_v = self._nominal_configuration(
            self.capacity_ah, self.nominal_voltage_v)
        parameters = {name: self._number(name, getattr(self, name)) for name in (
            "lambda_val", "phi_decay_rate", "entropy_weight", "max_temperature",
            "min_temperature", "max_current")}
        if (parameters["lambda_val"] < 0.0 or parameters["phi_decay_rate"] < 0.0
                or parameters["entropy_weight"] < 0.0 or parameters["max_current"] <= 0.0
                or parameters["max_temperature"] <= 0.0
                or parameters["min_temperature"] < -273.15
                or parameters["min_temperature"] > parameters["max_temperature"]):
            raise ValueError("invalid fallback model parameters")
        previous = {name: self._number(name, getattr(self, name)) for name in (
            "time", "entropy", "charge_throughput_ah", "cycle_count", "degradation")}
        if (any(value < 0.0 for value in previous.values())
                or previous["entropy"] > 1.0 or previous["degradation"] > 1.0):
            raise ValueError("invalid existing fallback state")

        # Clamp inputs
        input_clamped = False
        if abs(current) > parameters["max_current"]:
            current = math.copysign(parameters["max_current"], current)
            input_clamped = True

        if temperature > parameters["max_temperature"]:
            temperature = parameters["max_temperature"]
            input_clamped = True
        elif temperature < parameters["min_temperature"]:
            temperature = parameters["min_temperature"]
            input_clamped = True

        clamped_soc = max(0.0, min(1.0, soc))

        # Advance time
        candidate_time = previous["time"] + dt
        self._require_finite(time=candidate_time)

        # 1. Update entropy & cycle count (Ψ -> Φ accumulation)
        temp_contrib = max(0.0, temperature - 25.0) / 35.0
        current_contrib = abs(current) / 100.0

        entropy_delta = dt * parameters["entropy_weight"] * (temp_contrib + current_contrib)
        candidate_entropy = previous["entropy"] + entropy_delta
        self._require_finite(temperature_contribution=temp_contrib,
                             current_contribution=current_contrib,
                             entropy_delta=entropy_delta, entropy=candidate_entropy)
        candidate_entropy = min(candidate_entropy, 1.0)

        # Coulomb counting
        charge_delta = abs(current) * dt / 3600.0
        candidate_throughput = previous["charge_throughput_ah"] + charge_delta
        candidate_cycles = candidate_throughput / (2.0 * capacity_ah)
        self._require_finite(charge_delta=charge_delta, charge_throughput=candidate_throughput,
                             cycle_count=candidate_cycles)

        # 2. DS Geometric Metric Approximation (g^eff trace approximation)
        # Gradient approximation based on state changes
        grad_phi_0 = -parameters["phi_decay_rate"] * math.sqrt(
            candidate_entropy**2 + previous["degradation"]**2)
        grad_phi_1 = candidate_entropy * (1.0 - clamped_soc)
        grad_phi_2 = (temperature / parameters["max_temperature"]) * previous["degradation"]
        grad_phi_3 = math.sqrt(candidate_cycles) * 0.01
        self._require_finite(grad_phi_0=grad_phi_0, grad_phi_1=grad_phi_1,
                             grad_phi_2=grad_phi_2, grad_phi_3=grad_phi_3)

        # Approximated trace of effective metric: tr(g_eff) = 2.0 + lambda * sum(grad_phi_i^2)
        # Multiplication produces a checkable infinity instead of a Python
        # exponentiation OverflowError, without changing ordinary arithmetic.
        grad_sum_sq = sum(value * value for value in (
            grad_phi_0, grad_phi_1, grad_phi_2, grad_phi_3))
        g_eff_trace = 2.0 + parameters["lambda_val"] * grad_sum_sq
        self._require_finite(gradient_sum_squared=grad_sum_sq, metric_trace=g_eff_trace)

        # 3. Update degradation
        cycle_delta = candidate_cycles - previous["cycle_count"]
        base_degradation = cycle_delta * 0.0001
        thermal_degradation = temp_contrib * 0.0005 * dt
        ds_correction = g_eff_trace * 0.00001

        candidate_degradation = (previous["degradation"]
                                 + (base_degradation + thermal_degradation + ds_correction))
        self._require_finite(cycle_delta=cycle_delta, base_degradation=base_degradation,
                             thermal_degradation=thermal_degradation,
                             ds_correction=ds_correction, degradation=candidate_degradation)
        candidate_degradation = min(candidate_degradation, 1.0)
        if cycle_delta < 0.0 or candidate_degradation < previous["degradation"]:
            raise ValueError("fallback accumulators must not decrease")

        # 4. Generate results compatible with DSEnhancer wrapper
        remaining_capacity_percent = (1.0 - candidate_degradation) * 100.0

        # Health calculations
        degradation_rate = g_eff_trace * 0.0001
        cycles_to_80_percent = max(0.0, (0.2 - candidate_degradation) / max(degradation_rate, 1e-8))

        metric_stability = 1.0 / (1.0 + abs(g_eff_trace - 2.0))
        ds_confidence = metric_stability * (1.0 - candidate_degradation)

        # Charging optimization
        recommended_current_limit = 100.0 * metric_stability * (1.0 - candidate_degradation)
        recommended_voltage_limit = nominal_voltage_v * (1.0 + 0.1 * metric_stability)
        recommended_temperature = 25.0 + 5.0 * clamped_soc

        remaining_cap_ah = (1.0 - clamped_soc) * capacity_ah
        estimated_charge_time = 60.0 * remaining_cap_ah / max(recommended_current_limit, 1.0)

        degradation_warning = (degradation_rate > 0.001) or (candidate_degradation > 0.3)
        numerical_stability = (g_eff_trace < 1e6)

        result = {
            "degradation": candidate_degradation,
            "remaining_capacity_percent": remaining_capacity_percent,
            "cycles_to_80_percent": cycles_to_80_percent,
            "ds_confidence": ds_confidence,
            "recommended_current_limit": recommended_current_limit,
            "recommended_voltage_limit": recommended_voltage_limit,
            "recommended_temperature": recommended_temperature,
            "estimated_charge_time": estimated_charge_time,
            "degradation_warning": degradation_warning,
            "input_clamped": input_clamped,
            "numerical_stability": numerical_stability
        }
        self._require_finite(degradation_rate=degradation_rate,
                             metric_stability=metric_stability, remaining_cap_ah=remaining_cap_ah,
                             **result)
        if not numerical_stability:
            raise ValueError("fallback metric failed numerical stability")

        # Every owned calculation and returned value has passed validation.
        # This ordinary object has no callbacks or fallible work in the commit.
        self.__dict__.update(time=candidate_time, entropy=candidate_entropy,
                             charge_throughput_ah=candidate_throughput,
                             cycle_count=candidate_cycles, degradation=candidate_degradation)
        return result

if __name__ == "__main__":
    print("Running Pure Python Fallback Self-Test...")
    enhancer = DSEnhancerFallback(capacity_ah=75.0, nominal_voltage_v=400.0)
    res = enhancer.enhance_cycle(voltage=355.0, current=50.0, temperature=25.0, soc=0.65, dt=0.1)
    print("Fallback self-test SUCCESS! Results:")
    for k, v in res.items():
        print(f"  {k}: {v}")
