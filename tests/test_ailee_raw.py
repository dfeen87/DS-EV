# Copyright (c) Don Michael Feeney Jr.
# Licensed under the MIT License.

"""Regressions for the embedded, public raw-evidence trust boundary."""

import math
import sys
import unittest

from ds_core.python.ailee.core_min import (
    AileeConfig,
    AileeTrustPipeline,
    ConsensusStatus,
    GraceStatus,
    SafetyStatus,
)


class TestAileeRawEvidence(unittest.TestCase):
    def assert_rejected_unchanged(self, pipeline, raw_value, raw_confidence):
        history = list(pipeline._history)
        result = pipeline.process_raw(raw_value, raw_confidence)
        self.assertEqual(result.safety_status, SafetyStatus.REJECTED)
        self.assertEqual(result.value, 0.0)
        self.assertEqual(result.confidence_score, 0.0)
        self.assertTrue(math.isfinite(result.value))
        self.assertTrue(math.isfinite(result.confidence_score))
        self.assertTrue(result.used_fallback)
        self.assertEqual(result.grace_status, GraceStatus.NOT_APPLIED)
        self.assertEqual(result.consensus_status, ConsensusStatus.NO_QUORUM)
        self.assertTrue(result.reasons)
        self.assertEqual(pipeline._history, history)

    def populated_pipeline(self):
        pipeline = AileeTrustPipeline()
        pipeline.process_raw(1.25, 1.0)
        return pipeline

    def test_nonfinite_confidence_does_not_authorize_or_commit(self):
        pipeline = self.populated_pipeline()
        for confidence in (math.nan, math.inf, -math.inf):
            with self.subTest(confidence=confidence):
                self.assert_rejected_unchanged(pipeline, 2.0, confidence)

    def test_nonfinite_values_are_rejected_at_every_confidence_level(self):
        pipeline = self.populated_pipeline()
        for value in (math.nan, math.inf, -math.inf):
            for confidence in (0.1, 0.75, 1.0):
                with self.subTest(value=value, confidence=confidence):
                    self.assert_rejected_unchanged(pipeline, value, confidence)

    def test_malformed_numeric_evidence_is_rejected(self):
        pipeline = self.populated_pipeline()
        for invalid in (None, "bad", [], True, False, 10 ** 1000):
            with self.subTest(field="value", invalid=repr(invalid)):
                self.assert_rejected_unchanged(pipeline, invalid, 1.0)
            with self.subTest(field="confidence", invalid=repr(invalid)):
                self.assert_rejected_unchanged(pipeline, 1.0, invalid)

    def test_out_of_domain_confidence_is_rejected(self):
        pipeline = self.populated_pipeline()
        for confidence in (-0.01, 1.01, sys.float_info.max, -sys.float_info.max):
            with self.subTest(confidence=confidence):
                self.assert_rejected_unchanged(pipeline, 1.0, confidence)

    def test_valid_threshold_equalities_and_clamps_are_preserved(self):
        pipeline = AileeTrustPipeline()
        # Fixed expected values are independent of the implementation's clamp.
        cases = (
            (2.0, 0.0, 0.0, SafetyStatus.FALLBACK),
            (2.0, 0.65, 2.0, SafetyStatus.BORDERLINE),
            (2.0, 0.85, 2.0, SafetyStatus.ACCEPTED),
            (2.0, 1.0, 2.0, SafetyStatus.ACCEPTED),
            (sys.float_info.max, 1.0, 3.0, SafetyStatus.ACCEPTED),
            (-sys.float_info.max, 1.0, 0.0, SafetyStatus.ACCEPTED),
        )
        for value, confidence, expected_value, expected_status in cases:
            with self.subTest(value=value, confidence=confidence):
                result = pipeline.process_raw(value, confidence)
                self.assertEqual(result.value, expected_value)
                self.assertEqual(result.confidence_score, confidence)
                self.assertEqual(result.safety_status, expected_status)

    def test_custom_finite_policy_and_bounded_history_are_preserved(self):
        pipeline = AileeTrustPipeline(cfg=AileeConfig(
            hard_min=-2.0, hard_max=4.0,
            fallback_clamp_min=-1.0, fallback_clamp_max=2.0,
            history_window=2,
        ))
        self.assertEqual(pipeline.process_raw(3.0, 1.0).value, 3.0)
        self.assertEqual(pipeline.process_raw(3.0, 0.75).value, 2.0)
        self.assertEqual(pipeline.process_raw(3.0, 0.1).value, -1.0)
        self.assertEqual(pipeline._history, [2.0, -1.0])
        self.assert_rejected_unchanged(pipeline, math.nan, 1.0)
        self.assertEqual(pipeline.process_raw(1.5, 1.0).value, 1.5)
        self.assertEqual(pipeline._history, [-1.0, 1.5])

    def test_numeric_string_conversion_and_metadata_are_preserved(self):
        pipeline = AileeTrustPipeline()
        context = {"source": "software regression"}
        result = pipeline.process_raw("1.5", "0.9", context=context)
        self.assertEqual(result.value, 1.5)
        self.assertEqual(result.confidence_score, 0.9)
        self.assertEqual(result.safety_status, SafetyStatus.ACCEPTED)
        self.assertEqual(result.metadata, context)
        self.assertEqual(pipeline._history, [1.5])


if __name__ == "__main__":
    unittest.main()
