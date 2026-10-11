"""Parse real C++ learning audit output, including denied hostile evidence."""

import json
import math
from pathlib import Path
import subprocess
import tempfile
import unittest


class LearningAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="ds-learning-audit-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        root = Path(__file__).resolve().parents[1]
        binary = directory / "test_trust_learning_integrity"
        subprocess.run(
            ["g++", "-std=c++17", "-Wall", "-Wextra", "-Wpedantic",
             "-I", str(root / "include"), str(root / "tests/test_trust_learning_integrity.cpp"),
             "-o", str(binary)],
            check=True, capture_output=True, text=True,
        )
        cls.audit = directory / "audit.jsonl"
        subprocess.run([str(binary), "emit-audit", str(cls.audit)],
                       check=True, capture_output=True, text=True)
        cls.lines = cls.audit.read_text(encoding="utf-8").splitlines()
        cls.records = [json.loads(line, parse_constant=cls.reject_nonstandard_constant)
                       for line in cls.lines]
        byte_audit = directory / "byte-audit.jsonl"
        subprocess.run([str(binary), "emit-byte-audit", str(byte_audit)],
                       check=True, capture_output=True, text=True)
        cls.byte_records = [json.loads(line, parse_constant=cls.reject_nonstandard_constant)
                            for line in byte_audit.read_text(encoding="utf-8").splitlines()]

    @staticmethod
    def reject_nonstandard_constant(value):
        raise ValueError("invalid JSON numeric token: " + value)

    def test_each_operation_has_one_valid_json_line(self):
        self.assertEqual(len(self.records), 12)
        self.assertEqual(len(self.lines), 12)
        self.assertEqual([record["audit_id"] for record in self.records], list(range(1, 13)))

    def test_strings_preserve_quotes_backslashes_all_controls_and_nul(self):
        prefix = 'charger"identity\\\n\r\t\b\f\x01'
        self.assertEqual(self.records[0]["compartment"], prefix)
        self.assertEqual(self.records[1]["compartment"], prefix + "\x00tail")
        self.assertEqual(self.records[0]["param"], 'regen"limit\\\n\r\t\b\f\x01')
        self.assertFalse(self.records[1]["approved"])

    def test_nonfinite_numbers_are_null_with_explicit_evidence(self):
        classifications = set()
        for record in self.records:
            evidence = record["nonfinite_evidence"]
            for field in ("previous", "proposed", "applied", "trust_score"):
                value = record[field]
                if value is None:
                    self.assertIn(field, evidence)
                    classifications.add(evidence[field])
                else:
                    self.assertTrue(math.isfinite(value))
                    self.assertNotIn(field, evidence)
        self.assertEqual(classifications, {"NaN", "+Infinity", "-Infinity"})

    def test_rejected_evidence_has_no_committed_snapshot_or_rollback_handle(self):
        accepted = [record for record in self.records if record["approved"]]
        denied = [record for record in self.records if not record["approved"]]
        self.assertEqual(len(accepted), 3)
        self.assertEqual(len(denied), 9)
        self.assertTrue(all(record["snapshot_id"] > 0 for record in accepted))
        self.assertEqual(len({record["snapshot_id"] for record in accepted}), 3)
        self.assertTrue(all(record["snapshot_id"] == 0 for record in denied))
        denied_rollback = [record for record in denied if record["operation"] == "rollback"]
        self.assertEqual(len(denied_rollback), 1)
        self.assertEqual(denied_rollback[0]["rollback_source"], accepted[0]["snapshot_id"])

    def test_older_handle_rollback_reports_actual_pre_rollback_value(self):
        rollback = next(record for record in self.records
                        if record["operation"] == "rollback" and record["approved"])
        self.assertEqual(rollback["previous"], 0.58)
        self.assertEqual(rollback["proposed"], 0.5)
        self.assertEqual(rollback["applied"], 0.5)
        self.assertEqual(rollback["target_identity"], self.records[0]["target_identity"])

    def test_automotive_log_preserves_one_line_per_attempt(self):
        lines = Path(str(self.audit) + ".log").read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(lines), 12)
        prefix = "[AILEE_LEARNING] "
        self.assertTrue(all(line.startswith(prefix) for line in lines))
        automotive = [json.loads(line[len(prefix):], parse_constant=self.reject_nonstandard_constant)
                      for line in lines]
        self.assertEqual(automotive, self.records)

    def test_valid_utf8_preserves_original_unicode_text(self):
        record = self.byte_records[0]
        self.assertTrue(record["approved"])
        self.assertEqual(record["compartment"], "充電é")
        self.assertEqual(record["param"], "regén_充電")
        self.assertEqual(record["invalid_utf8_evidence"], {})

    def test_invalid_utf8_records_original_bytes_as_hex(self):
        self.assertEqual(len(self.byte_records), 7)
        raw = self.byte_records[1]
        self.assertEqual(raw["compartment"], "raw\xff\xc0")
        self.assertEqual(bytes.fromhex(raw["invalid_utf8_evidence"]["compartment"]), b"raw\xff\xc0")
        self.assertEqual(bytes.fromhex(self.byte_records[2]["invalid_utf8_evidence"]["param"]), b"r\xc2.")
        expected = [b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xe0\x80\x80", b"\xf0\x80\x80\x80"]
        for record, original in zip(self.byte_records[3:], expected):
            self.assertFalse(record["approved"])
            self.assertEqual(bytes.fromhex(record["invalid_utf8_evidence"]["compartment"]), original)


if __name__ == "__main__":
    unittest.main()
