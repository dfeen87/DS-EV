"""PB-15 regressions for the separate approximate Python battery model."""

import math
import sys
import unittest

from ds_core.python_fallback.ds_enhancer_fallback import DSEnhancerFallback


class TestFallbackIntegrity(unittest.TestCase):
    @staticmethod
    def normal_inputs():
        return dict(voltage=360.0, current=10.0, temperature=25.0, soc=0.8, dt=1.0)

    def assert_rejected_unchanged(self, model, **overrides):
        before = dict(vars(model))
        with self.assertRaises(ValueError):
            model.enhance_cycle(**dict(self.normal_inputs(), **overrides))
        self.assertEqual(vars(model), before)

    def assert_finite_result(self, result):
        self.assertTrue(result["numerical_stability"])
        for field, value in result.items():
            self.assertTrue(math.isfinite(value), field)

    def test_negative_timestep_preserves_fresh_state(self):
        self.assert_rejected_unchanged(DSEnhancerFallback(), dt=-1.0)

    def test_nan_soc_is_rejected_before_clamping(self):
        self.assert_rejected_unchanged(DSEnhancerFallback(), soc=float("nan"))

    def test_invalid_timestep_rejected_after_accepted_cycle(self):
        for timestep in (0.0, -1.0, float("nan"), float("inf"), -float("inf"),
                         None, "1", [], True):
            with self.subTest(timestep=timestep):
                model = DSEnhancerFallback()
                model.enhance_cycle(**self.normal_inputs())
                self.assert_rejected_unchanged(model, dt=timestep)

    def test_nonfinite_or_malformed_consumed_inputs(self):
        for field in ("voltage", "current", "temperature", "soc"):
            for value in (float("nan"), float("inf"), -float("inf"),
                          None, "360", [], {}, True, 10**400):
                with self.subTest(field=field, value=value):
                    model = DSEnhancerFallback()
                    model.enhance_cycle(**self.normal_inputs())
                    self.assert_rejected_unchanged(model, **{field: value})

    def test_invalid_physical_inputs_rejected(self):
        for overrides in ({"voltage": 0.0}, {"voltage": -360.0},
                          {"temperature": -273.15001}):
            with self.subTest(overrides=overrides):
                self.assert_rejected_unchanged(DSEnhancerFallback(), **overrides)

    def test_constructor_rejects_invalid_nominal_configuration(self):
        for field in ("capacity_ah", "nominal_voltage_v"):
            for value in (0.0, -1.0, float("nan"), float("inf"), -float("inf"),
                          None, "75", [], True, 10**400, sys.float_info.max):
                with self.subTest(field=field, value=value):
                    with self.assertRaises(ValueError):
                        DSEnhancerFallback(**{field: value})

    def test_finite_current_temperature_and_soc_clamping_preserved(self):
        for raw, clamped in (
                (dict(current=1000.0, temperature=100.0, soc=2.0),
                 dict(current=500.0, temperature=60.0, soc=1.0)),
                (dict(current=-1000.0, temperature=-100.0, soc=-2.0),
                 dict(current=-500.0, temperature=-20.0, soc=0.0))):
            with self.subTest(raw=raw):
                observed, expected = DSEnhancerFallback(), DSEnhancerFallback()
                result = observed.enhance_cycle(**dict(self.normal_inputs(), **raw))
                reference = expected.enhance_cycle(**dict(self.normal_inputs(), **clamped))
                self.assertTrue(result["input_clamped"])
                self.assertEqual(dict(result, input_clamped=False), reference)
                self.assertEqual(vars(observed), vars(expected))
                self.assert_finite_result(result)

    def test_extreme_finite_current_clamping_stays_finite(self):
        for current in (sys.float_info.max, -sys.float_info.max):
            with self.subTest(current=current):
                model = DSEnhancerFallback()
                result = model.enhance_cycle(**dict(self.normal_inputs(), current=current))
                self.assertTrue(result["input_clamped"])
                self.assert_finite_result(result)
                self.assertAlmostEqual(model.charge_throughput_ah, 500.0 / 3600.0)

    def test_finite_soc_clamping_preserves_legacy_flag(self):
        for soc, temperature in ((-sys.float_info.max, 25.0),
                                 (sys.float_info.max, 30.0)):
            with self.subTest(soc=soc):
                result = DSEnhancerFallback().enhance_cycle(
                    **dict(self.normal_inputs(), soc=soc))
                self.assertEqual(result["recommended_temperature"], temperature)
                self.assertFalse(result["input_clamped"])
                self.assert_finite_result(result)

    def test_extreme_timestep_overflow_is_atomic(self):
        self.assert_rejected_unchanged(
            DSEnhancerFallback(), current=500.0, temperature=60.0,
            dt=sys.float_info.max)

    def test_cumulative_time_overflow_is_atomic(self):
        model = DSEnhancerFallback()
        model.time = sys.float_info.max
        self.assert_rejected_unchanged(model, current=0.0, dt=sys.float_info.max)

    def test_cumulative_throughput_overflow_is_atomic(self):
        model = DSEnhancerFallback()
        model.charge_throughput_ah = sys.float_info.max
        model.cycle_count = model.charge_throughput_ah / (2.0 * model.capacity_ah)
        self.assert_rejected_unchanged(model, current=1.0, dt=sys.float_info.max)

    def test_derived_metric_overflow_is_atomic(self):
        model = DSEnhancerFallback(capacity_ah=sys.float_info.min)
        self.assert_rejected_unchanged(model, current=500.0, dt=100.0)

    def test_returned_charge_time_overflow_is_atomic(self):
        model = DSEnhancerFallback(capacity_ah=sys.float_info.max / 4.0)
        self.assert_rejected_unchanged(model, soc=0.0)

    def test_finite_unstable_metric_does_not_commit(self):
        self.assert_rejected_unchanged(
            DSEnhancerFallback(capacity_ah=1e-16), current=500.0, dt=100.0)

    def test_invalid_existing_state_cannot_manufacture_success(self):
        for field, value in (("time", -1.0), ("entropy", -0.05),
                             ("cycle_count", -1.0), ("charge_throughput_ah", -1.0),
                             ("degradation", 1.1)):
            with self.subTest(field=field):
                model = DSEnhancerFallback()
                setattr(model, field, value)
                self.assert_rejected_unchanged(model)

    def test_mutated_invalid_configuration_is_rejected_atomically(self):
        for field, value in (("capacity_ah", 0.0), ("nominal_voltage_v", float("nan")),
                             ("max_temperature", 0.0), ("lambda_val", float("inf"))):
            with self.subTest(field=field):
                model = DSEnhancerFallback()
                setattr(model, field, value)
                self.assert_rejected_unchanged(model)

    def test_normal_multi_cycle_accumulation_and_current_sign_compatibility(self):
        positive, negative = DSEnhancerFallback(), DSEnhancerFallback()
        for index in range(10):
            result = positive.enhance_cycle(**self.normal_inputs())
            opposite = negative.enhance_cycle(**dict(self.normal_inputs(), current=-10.0))
            self.assertEqual(result, opposite)
            self.assert_finite_result(result)
            self.assertEqual(positive.time, index + 1.0)
        self.assertAlmostEqual(positive.charge_throughput_ah, 100.0 / 3600.0)
        self.assertAlmostEqual(positive.cycle_count, (100.0 / 3600.0) / 150.0)
        self.assertAlmostEqual(positive.entropy, 0.5)
        self.assertGreater(positive.degradation, 0.0)
        self.assertEqual(vars(positive), vars(negative))

    def test_valid_custom_nominal_configuration(self):
        model = DSEnhancerFallback(capacity_ah=150.0, nominal_voltage_v=800.0)
        result = model.enhance_cycle(**self.normal_inputs())
        self.assertAlmostEqual(model.cycle_count, (10.0 / 3600.0) / 300.0)
        self.assertAlmostEqual(result["recommended_voltage_limit"], 880.0, places=6)
        self.assert_finite_result(result)

    def test_valid_cycle_after_rejection_preserves_history(self):
        model, control = DSEnhancerFallback(), DSEnhancerFallback()
        for _ in range(3):
            model.enhance_cycle(**self.normal_inputs())
            control.enhance_cycle(**self.normal_inputs())
        self.assert_rejected_unchanged(model, dt=-1.0)
        self.assert_rejected_unchanged(model, soc=float("nan"))
        self.assert_rejected_unchanged(model, current=500.0, dt=sys.float_info.max)
        self.assertEqual(model.enhance_cycle(**self.normal_inputs()),
                         control.enhance_cycle(**self.normal_inputs()))
        self.assertEqual(vars(model), vars(control))

    def test_previously_valid_single_cycle_output_preserved(self):
        model = DSEnhancerFallback()
        result = model.enhance_cycle(**self.normal_inputs())
        expected = {
            "degradation": 2.00018518528519e-05,
            "remaining_capacity_percent": 99.99799981481472,
            "cycles_to_80_percent": 999.8999906907385,
            "ds_confidence": 0.9999799980481447,
            "recommended_current_limit": 99.99799980481447,
            "recommended_voltage_limit": 439.9999999959999,
            "recommended_temperature": 29.0,
            "estimated_charge_time": 9.00018002116747,
        }
        for field, value in expected.items():
            self.assertAlmostEqual(result[field], value, places=8, msg=field)
        self.assertFalse(result["degradation_warning"])
        self.assertFalse(result["input_clamped"])
        self.assert_finite_result(result)


if __name__ == "__main__":
    unittest.main()
