#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
DS EV Battery Enhancement - Hardened Integration and Pipeline Test Suite
========================================================================

Verifies safety boundaries, limp/derate modes, installer safety gates,
and update/rollback workflows under simulated/hardware-agnostic conditions.
"""

import os
import sys
import unittest
import subprocess
import shutil
import json
import hashlib
import importlib.util
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BACKUP_SPEC = importlib.util.spec_from_file_location(
    "pipeline_backup_restore", ROOT / "scripts" / "backup_restore.py")
backup_restore = importlib.util.module_from_spec(BACKUP_SPEC)
BACKUP_SPEC.loader.exec_module(backup_restore)

# Add ds_core/lib to path so we can import pybind11 module if needed
sys.path.insert(0, str(ROOT / "ds_core" / "lib"))

class TestHardenedPipeline(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        # Repository CMake defaults put the CLI into a tracked source path.
        # Override distribution outputs from an external CMake project, without
        # changing the repository or requiring a privileged installation.
        cls.workspace = tempfile.TemporaryDirectory(prefix="ds-pipeline-test-")
        cls.addClassCleanup(cls.workspace.cleanup)
        cls.work = Path(cls.workspace.name)
        cls.build = cls.work / "build"
        cls.log_dir = cls.work / "diagnostic-logs"
        cls.config_dir = cls.work / "config"
        shutil.copytree(ROOT / "config", cls.config_dir)
        cmake_project = cls.work / "cmake-project"
        cmake_project.mkdir()
        (cmake_project / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.15)\n'
            'project(DS_Pipeline_Test LANGUAGES CXX)\n'
            'add_subdirectory("' + ROOT.as_posix() + '" distribution)\n'
            'set_target_properties(ds_enhancer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "'
            + (cls.build / "bin").as_posix() + '")\n'
            'set_target_properties(ds_enhancer_shared PROPERTIES LIBRARY_OUTPUT_DIRECTORY "'
            + (cls.build / "lib").as_posix() + '")\n',
            encoding="utf-8")
        subprocess.run([
            "cmake", "-S", str(cmake_project), "-B", str(cls.build),
            "-DCMAKE_BUILD_TYPE=Release",
        ], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["cmake", "--build", str(cls.build), "--parallel", "2"],
                       check=True, stdout=subprocess.DEVNULL)

    @staticmethod
    def diagnostic_environment():
        """Keep ambient vehicle overrides out of simulated fixture checks."""
        env = os.environ.copy()
        for name in ("DS_MODEL", "DS_FIRMWARE", "DS_SOH", "DS_TEMP_C", "DS_VOLTAGE_V", "DS_SOC"):
            env.pop(name, None)
        return env

    def run_diag(self, args):
        """Runs the diagnostics script with given arguments and returns returncode and output."""
        cmd = [sys.executable, str(ROOT / "scripts" / "diagnostics.py")] + args + [
            "--config-dir", str(self.config_dir), "--log-dir", str(self.log_dir)]
        res = subprocess.run(cmd, env=self.diagnostic_environment(),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        return res.returncode, res.stdout, res.stderr

    # ========================================================================
    # 1. Safety Boundaries
    # ========================================================================

    def test_soh_safety_boundaries(self):
        # SOH below threshold (79.9%) -> must fail/exit 4
        code, out, err = self.run_diag(["--soh", "79.9"])
        self.assertEqual(code, 4)
        self.assertIn("below safe enhancement threshold", out)

        # SOH at/above threshold (80.0%) -> should pass
        code, out, err = self.run_diag(["--soh", "80.0"])
        self.assertEqual(code, 0)

        # SOH clearly above (90.0%) -> should pass
        code, out, err = self.run_diag(["--soh", "90.0"])
        self.assertEqual(code, 0)

    def test_diagnostics_help(self):
        code, out, err = self.run_diag(["--help"])
        self.assertEqual(code, 0)
        self.assertIn("State of Health (%)", out)

    def test_non_finite_measurement_is_rejected(self):
        code, out, err = self.run_diag(["--soh", "nan"])
        self.assertEqual(code, 4)
        self.assertIn("must be a finite number", out)

    def test_temperature_safety_boundaries(self):
        # Temperature just above limit (45.1 C) -> must fail/exit 4
        code, out, err = self.run_diag(["--temp", "45.1"])
        self.assertEqual(code, 4)
        self.assertIn("outside safe range", out)

        # Temperature just below limit (0.0 C) -> should pass
        code, out, err = self.run_diag(["--temp", "0.0"])
        self.assertEqual(code, 0)

        # Temperature just above limit (45.0 C) -> should pass
        code, out, err = self.run_diag(["--temp", "45.0"])
        self.assertEqual(code, 0)

        # Temperature clearly cold (-0.1 C) -> must fail/exit 4
        code, out, err = self.run_diag(["--temp", "-0.1"])
        self.assertEqual(code, 4)
        self.assertIn("outside safe range", out)

    def test_soc_safety_boundaries(self):
        # SOC just above upper limit (90.1%) -> must fail/exit 4
        code, out, err = self.run_diag(["--soc", "90.1"])
        self.assertEqual(code, 4)
        self.assertIn("outside ideal installation range", out)

        # SOC just below lower limit (19.9%) -> must fail/exit 4
        code, out, err = self.run_diag(["--soc", "19.9"])
        self.assertEqual(code, 4)
        self.assertIn("outside ideal installation range", out)

        # SOC at limits (20.0% and 90.0%) -> should pass
        code, out, err = self.run_diag(["--soc", "20.0"])
        self.assertEqual(code, 0)

        code, out, err = self.run_diag(["--soc", "90.0"])
        self.assertEqual(code, 0)

    def test_voltage_out_of_range(self):
        # For Tesla_Model_3_LR, nominal voltage range is 300V to 410V
        # Voltage below range (299.0V) -> must fail
        code, out, err = self.run_diag(["--model", "Tesla_Model_3_LR", "--voltage", "299.0"])
        self.assertEqual(code, 4)
        self.assertIn("outside nominal profile bounds", out)

        # Voltage above range (411.0V) -> must fail
        code, out, err = self.run_diag(["--model", "Tesla_Model_3_LR", "--voltage", "411.0"])
        self.assertEqual(code, 4)
        self.assertIn("outside nominal profile bounds", out)

        # Voltage in-range (355.0V) -> should pass
        code, out, err = self.run_diag(["--model", "Tesla_Model_3_LR", "--voltage", "355.0"])
        self.assertEqual(code, 0)

    # ========================================================================
    # 2. Limp / Derate Modes
    # ========================================================================

    def test_limp_and_derate_modes(self):
        try:
            import ds_enhancer_pybind as h
        except ImportError:
            self.skipTest("pybind11 module not found on path")

        # Create state with low SOH/degradation
        middleware = h.DSBMSMiddleware()
        middleware.init(75.0, 400.0)

        # A. Critical SOC (Limp Mode)
        # With SOC at 3% (below 5%), overall scaling should be aggressive derating (limp mode)
        enhanced = middleware.enhance_cycle(360.0, 50.0, 25.0, 0.03, 1.0)
        config = h.TorqueConfig()
        tm = h.DSTorqueManager(config)
        result = tm.compute_torque_limit(enhanced, 4000.0, 0.01)
        self.assertTrue(result.limp_mode_active)
        self.assertTrue(result.soc_derate_active)
        self.assertLess(result.overall_scaling, 0.4)

        # B. High Temperature (Thermal Derating)
        # With Temperature at 48 C (above 45 C soft limit), thermal derating must kick in
        enhanced_hot = middleware.enhance_cycle(360.0, 50.0, 48.0, 0.6, 1.0)
        result_hot = tm.compute_torque_limit(enhanced_hot, 4000.0, 0.01)
        self.assertTrue(result_hot.thermal_derate_active)
        self.assertLess(result_hot.thermal_scaling, 1.0)

        # C. Weak Cell Detection
        # Pass a diagnostic report containing a weak cell count and ID
        diag_report = h.DiagnosticReport()
        diag_report.weak_cell_warning = True
        diag_report.weak_cell_ids = [12, 45]
        diag_report.voltage_imbalance_mv = 75.0 # mV imbalance > 50.0mV triggers limiting

        result_weak = tm.compute_torque_limit(enhanced, 4000.0, 0.01, diag_report)
        self.assertTrue(result_weak.weak_cell_derate_active)
        self.assertLess(result_weak.cell_balancing_scaling, 1.0)

    # ========================================================================
    # 3. Installer safety-gating Behavior
    # ========================================================================

    def test_installer_safety_gating(self):
        # Exercise the actual shell recipes in a complete temporary checkout.
        # Only privilege/build/backup commands are shims; diagnostics are real
        # and all deployment/log destinations are explicitly temporary.
        with tempfile.TemporaryDirectory(prefix="ds-installer-test-") as directory:
            work = Path(directory)
            source, installed, shims = work / "source", work / "installed", work / "shims"
            source.mkdir()
            shims.mkdir()
            shutil.copy2(ROOT / "Makefile", source / "Makefile")
            for name in ("scripts", "config", "docs", "ds_core/python", "ds_core/python_fallback"):
                shutil.copytree(ROOT / name, source / name)
            (source / "ds_core/bin").mkdir()
            (source / "ds_core/lib").mkdir()
            shutil.copy2(self.build / "bin/ds_enhancer", source / "ds_core/bin/ds_enhancer")
            libraries = list((self.build / "lib").glob("libds_enhancer.*"))
            self.assertTrue(libraries, "The checked CMake build must produce a shared library")
            for library in libraries:
                shutil.copy2(library, source / "ds_core/lib" / library.name)

            trace = work / "commands.log"
            log_file, local_log = work / "install.log", work / "local-install.log"
            diagnostic_logs = work / "diagnostic-logs"
            scripts = {
                shims / "id": '#!/bin/sh\n[ "$1" = "-u" ] || exit 91\n'
                    'printf "%s\\n" "${DS_TEST_UID:-0}"\n',
                shims / "cmake": '#!/bin/sh\nprintf "cmake %s\\n" "$*" >> "$DS_TEST_TRACE"\n',
                source / "scripts/rollback.sh": '#!/bin/sh\n[ "$1" = "backup" ] || exit 92\n'
                    'printf "backup\\n" >> "$DS_TEST_TRACE"\n',
                shims / "python3": '#!' + sys.executable + '\nimport os, sys\n'
                    'if sys.argv[1:2] != ["scripts/diagnostics.py"]:\n    sys.exit(93)\n'
                    'with open(os.environ["DS_TEST_TRACE"], "a") as stream:\n'
                    '    stream.write("diagnostics\\n")\n'
                    'os.execv(' + repr(sys.executable) + ', [' + repr(sys.executable) + ', '
                    + repr(str(ROOT / "scripts/diagnostics.py")) + '] + sys.argv[2:] + '
                    '["--config-dir", os.environ["DS_TEST_CONFIG"], '
                    '"--log-dir", os.environ["DS_TEST_LOG_DIR"]])\n',
            }
            for path, content in scripts.items():
                path.write_text(content, encoding="utf-8")
                path.chmod(0o755)

            (installed / "bin").mkdir(parents=True)
            existing_program = installed / "bin/previous-program"
            existing_program.write_bytes(b"previous software remains intact")
            original_manifest = backup_restore.tree_manifest(installed)
            env = self.diagnostic_environment()
            env.update({
                "PATH": str(shims) + os.pathsep + env.get("PATH", ""),
                "DS_TEST_TRACE": str(trace), "DS_TEST_CONFIG": str(source / "config"),
                "DS_TEST_LOG_DIR": str(diagnostic_logs), "DS_SOH": "75.0",
            })
            command = ["make", "install", "INSTALL_DIR=" + str(installed),
                       "LOG_FILE=" + str(log_file), "LOCAL_LOG=" + str(local_log)]
            env["DS_TEST_UID"] = "1000"
            unprivileged = subprocess.run(command, cwd=source, env=env,
                                          capture_output=True, text=True)
            self.assertNotEqual(unprivileged.returncode, 0)
            self.assertIn("root privileges", unprivileged.stdout + unprivileged.stderr)
            self.assertEqual(backup_restore.tree_manifest(installed), original_manifest)
            self.assertFalse(trace.exists())
            env["DS_TEST_UID"] = "0"
            rejected = subprocess.run(command, cwd=source, env=env, capture_output=True, text=True)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("diagnostics failed", rejected.stdout + rejected.stderr)
            self.assertEqual(backup_restore.tree_manifest(installed), original_manifest)
            self.assertEqual(trace.read_text().splitlines(), ["diagnostics"])
            self.assertNotIn("successfully deployed", rejected.stdout + rejected.stderr)
            self.assertNotIn("STATUS: SUCCESS", log_file.read_text())
            self.assertNotIn("STATUS: SUCCESS", local_log.read_text())

            env["DS_SOH"] = "92.5"
            accepted = subprocess.run(command, cwd=source, env=env, capture_output=True, text=True)
            self.assertEqual(accepted.returncode, 0, accepted.stdout + accepted.stderr)
            self.assertIn("DS Software Suite successfully deployed", accepted.stdout)
            self.assertEqual(trace.read_text().splitlines(), [
                "diagnostics", "diagnostics", "backup",
                "cmake -B build -DCMAKE_BUILD_TYPE=Release", "cmake --build build"])
            self.assertIn("STATUS: SUCCESS", log_file.read_text())
            self.assertEqual((installed / "bin/ds_enhancer").read_bytes(),
                             (source / "ds_core/bin/ds_enhancer").read_bytes())
            self.assertTrue(os.access(installed / "bin/ds_enhancer", os.X_OK))
            for library in libraries:
                self.assertEqual((installed / "lib" / library.name).read_bytes(), library.read_bytes())
            for original, deployed in (("config/vehicle_profiles.json", "config/vehicle_profiles.json"),
                                       ("ds_core/python/ds_enhancer.py", "python/ds_enhancer.py"),
                                       ("ds_core/python_fallback/ds_enhancer_fallback.py", "python_fallback/ds_enhancer_fallback.py"),
                                       ("docs/SAFETY.md", "docs/SAFETY.md"),
                                       ("scripts/diagnostics.py", "bin/diagnostics.py")):
                self.assertEqual((installed / deployed).read_bytes(), (source / original).read_bytes())

    # ========================================================================
    # 4. Backup & Rollback behavior
    # ========================================================================

    def test_backup_and_rollback_flow(self):
        # Call the real verified backup/restore functions on a temporary tree;
        # never invoke the privileged live rollback command.
        with tempfile.TemporaryDirectory(prefix="ds-rollback-test-") as directory:
            work = Path(directory)
            installed = work / "installed"
            (installed / "config").mkdir(parents=True)
            dummy_file = installed / "config/dummy.json"
            original = '{"test": "rollback"}'
            dummy_file.write_text(original, encoding="utf-8")
            before = backup_restore.tree_manifest(installed)
            snapshot = backup_restore.create_backup(installed, work / "backups", "test-snapshot")
            self.assertEqual(backup_restore.verify_backup(snapshot, before), before)
            record = json.loads((snapshot / "manifest.json").read_text())
            self.assertEqual(record["files"]["config/dummy.json"]["sha256"],
                             hashlib.sha256(original.encode("utf-8")).hexdigest())
            dummy_file.write_text('{"test": "modified"}', encoding="utf-8")
            (installed / "lib").mkdir()
            (installed / "lib/new-file.so").write_bytes(b"new deployment")
            self.assertTrue(backup_restore.restore_backup(installed, snapshot))
            self.assertEqual(dummy_file.read_text(encoding="utf-8"), original)
            self.assertEqual(backup_restore.tree_manifest(installed), before)
            self.assertFalse((installed / "lib").exists())

if __name__ == "__main__":
    unittest.main()
