#!/usr/bin/env python3
"""Offline migration gates use private fixtures, real evidence and real snapshots.

Only this test's temporary installations are copied or restored. Privileged
metadata and the administrative CLI are mocked; /opt is never accessed.
"""

import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))


def load_module(name, filename):
    specification = importlib.util.spec_from_file_location(name, filename)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


migration = load_module("ds_legacy_migration_under_test", SCRIPTS / "legacy_migration.py")
backup = load_module("ds_migration_fixture_backup", SCRIPTS / "backup_restore.py")
update = load_module("ds_migration_fixture_update", SCRIPTS / "update.py")
existing_update_tests = load_module("ds_migration_existing_update_tests", ROOT / "tests/test_update.py")
COMMIT = "a" * 40


def digest(data):
    return hashlib.sha256(data).hexdigest()


def authenticate_fixture(source, manifest):
    """Fixture-only independent trust record; no release digest is invented in production."""
    for name in migration.SOURCE_FILES:
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes((ROOT / name).read_bytes())
        path.chmod(0o755)
    record = {
        "format": "ds-ev-maintenance-v1", "version": "8.0.2", "commit": COMMIT,
        "files": {name: digest((source / name).read_bytes()) for name in migration.SOURCE_FILES},
    }
    manifest.write_text(json.dumps(record, sort_keys=True), encoding="utf-8")
    manifest.chmod(0o600)
    return digest(manifest.read_bytes())


def filesystem_state(root):
    """Check bytes and metadata, including source, evidence, backups and empty directories."""
    result = {}
    for path in [root] + sorted(root.rglob("*")):
        information = path.lstat()
        content = None
        if stat.S_ISREG(information.st_mode):
            content = path.read_bytes()
        elif stat.S_ISLNK(information.st_mode):
            content = os.readlink(path)
        result[path.relative_to(root).as_posix()] = (
            stat.S_IMODE(information.st_mode), information.st_uid, information.st_gid, content)
    return result


class MigrationFixture(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ds-legacy-migration-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "trusted-source"
        self.source.mkdir(mode=0o700)
        self.manifest = self.root / "maintenance-manifest.json"
        self.manifest_sha256 = authenticate_fixture(self.source, self.manifest)
        self.live = self.root / "installation"
        self.live.mkdir(mode=0o700)
        for name in backup.MANAGED_DIRS:
            (self.live / name).mkdir(mode=0o755)
        for name in update.REQUIRED_CONFIGS:
            shutil.copy2(ROOT / "config" / name, self.live / "config" / name)
        # Use the actual vehicle profiles, thresholds and healthy installed
        # evidence. This HTTPS digest is a test fixture, never a real release.
        policy = json.loads((self.live / "config/update_source.json").read_text())
        policy["update_policy"].update(
            fallback_url="https://operator-selected.example/releases/payload.tar.gz",
            sha256=digest(b"independently authenticated fixture release"))
        (self.live / "config/update_source.json").write_text(json.dumps(policy), encoding="utf-8")
        self.legacy = {
            "diagnostics.py": b"print('Legacy diagnostic success without protected evidence')\n",
            "backup_restore.py": b"raise RuntimeError('Never execute the legacy backup helper')\n",
            "rollback.sh": b"#!/bin/sh\nexit 99 # Unverified legacy rollback must never execute\n",
            "update.py": b"raise RuntimeError('Never execute the legacy updater')\n",
        }
        for name, content in self.legacy.items():
            (self.live / "bin" / name).write_bytes(content)
            (self.live / "bin" / name).chmod(0o755)
        (self.live / "bin/application").write_bytes(b"Existing deployed application")
        (self.live / "lib/operator-library.so").write_bytes(b"Existing deployed library")
        (self.live / "docs/OPERATOR.md").write_bytes(b"Existing operator documentation")
        self.commands = []
        self.recovery = None
        self.recovery_sha256 = None

    def run_real(self, command, **options):
        arguments = [str(part) for part in command]
        self.commands.append((arguments, options))
        self.assertNotIn("sudo", arguments)
        self.assertNotIn("git", arguments)
        self.assertFalse(any("/opt/ds_enhancement" in part for part in arguments))
        self.assertIn("-I", arguments)
        self.assertIn("-B", arguments)
        return update.run_command(arguments, **options)

    def check(self, mode="inspect", **changes):
        arguments = {
            "mode": mode, "source": self.source, "manifest": self.manifest,
            "manifest_sha256": self.manifest_sha256, "commit": COMMIT, "install": self.live,
            "snapshot": self.recovery, "snapshot_sha256": self.recovery_sha256,
            "owner": os.geteuid(), "group": os.getegid(), "runner": self.run_real,
            "which": lambda name: "/fixture-tools/" + name,
        }
        arguments.update(changes)
        return migration.check(**arguments)

    def recovery_point(self):
        self.recovery = backup.create_backup(
            self.live, self.root / "recovery", "selected", required_owner=os.geteuid())
        self.recovery_sha256 = digest((self.recovery / "manifest.json").read_bytes())
        return self.recovery

    def replace_helpers(self, names=migration.HELPERS):
        """Simulate the runbook's explicitly authorized replacement inside the fixture only."""
        for name in names:
            target = self.live / "bin" / name
            shutil.copyfile(self.source / "scripts" / name, target)
            target.chmod(0o755)

    def assert_read_only_block(self, text, mode="inspect", **changes):
        before = filesystem_state(self.root)
        with self.assertRaisesRegex(Exception, text):
            self.check(mode, **changes)
        self.assertEqual(filesystem_state(self.root), before)


class SourceTrustTests(MigrationFixture):
    def test_missing_invalid_and_mismatched_source_pin_block_before_execution(self):
        for value in (None, "", "0" * 64, "b" * 64, "not-a-digest"):
            with self.subTest(pin=value):
                self.assert_read_only_block("SHA-256|pin", manifest_sha256=value)
        self.assertEqual(self.commands, [])

    def test_wrong_commit_and_missing_source_manifest_block(self):
        for commit in ("b" * 40, "a" * 39, "A" * 40):
            with self.subTest(commit=commit):
                self.assert_read_only_block("identity/manifest", commit=commit)
        self.assert_read_only_block("No such file", manifest=self.root / "missing.json")
        self.assertEqual(self.commands, [])

    def test_tampered_helper_is_rejected_before_loading_any_code(self):
        (self.source / "scripts/backup_restore.py").write_bytes(b"raise RuntimeError('untrusted')\n")
        with mock.patch.object(migration, "_modules") as loader:
            self.assert_read_only_block("Trusted source hash mismatch")
        loader.assert_not_called()
        self.assertEqual(self.commands, [])

    def test_missing_helper_is_rejected(self):
        (self.source / "scripts/rollback.sh").unlink()
        self.assert_read_only_block("No such file")
        self.assertEqual(self.commands, [])

    def test_manifest_must_describe_exact_helper_set_and_version(self):
        original = json.loads(self.manifest.read_text())
        records = []
        for field, value in (("format", "legacy"), ("version", "8.0.1"), ("commit", "b" * 40)):
            changed = dict(original)
            changed[field] = value
            records.append(changed)
        records.append(dict(original, files={name: value for name, value in original["files"].items()
                                           if name != "scripts/legacy_migration.py"}))
        records.append(dict(original, files=dict(original["files"], unexpected="c" * 64)))
        for record in records:
            with self.subTest(record=record):
                self.manifest.write_text(json.dumps(record), encoding="utf-8")
                self.assert_read_only_block("identity/manifest",
                                            manifest_sha256=digest(self.manifest.read_bytes()))
        self.assertEqual(self.commands, [])

    def test_authenticated_source_cannot_substitute_a_different_running_checker(self):
        helper = self.source / "scripts/legacy_migration.py"
        helper.write_bytes(helper.read_bytes() + b"\n# A different checker\n")
        record = json.loads(self.manifest.read_text())
        record["files"]["scripts/legacy_migration.py"] = digest(helper.read_bytes())
        self.manifest.write_text(json.dumps(record), encoding="utf-8")
        self.assert_read_only_block("Running checker differs",
                                    manifest_sha256=digest(self.manifest.read_bytes()))
        self.assertEqual(self.commands, [])

    def test_source_symlink_and_hard_link_are_rejected(self):
        target = self.source / "scripts/rollback.sh"
        saved = target.read_bytes()
        external = self.root / "original-rollback.sh"
        external.write_bytes(saved)
        target.unlink()
        target.symlink_to(external)
        self.assert_read_only_block("Symlink|noncanonical")
        target.unlink()
        os.link(external, target)
        self.assert_read_only_block("hard-linked")


class InstallationPreflightTests(MigrationFixture):
    def test_legacy_zero_exit_is_a_compatibility_gap_and_inspect_is_read_only(self):
        before = filesystem_state(self.root)
        result = self.check()
        self.assertEqual(result["status"], "CHECKS PASSED; NO MIGRATION PERFORMED")
        self.assertEqual(result["matching_helpers"], [])
        self.assertIsNone(result["recovery_manifest_sha256"])
        self.assertEqual(filesystem_state(self.root), before)
        self.assertEqual(len(self.commands), 1)
        self.assertIn(str(self.source / "scripts/diagnostics.py"), self.commands[0][0])
        self.assertNotIn(str(self.live / "bin/diagnostics.py"), self.commands[0][0])

    def test_legacy_argument_rejection_does_not_execute_installed_code(self):
        (self.live / "bin/diagnostics.py").write_bytes(
            b"import argparse\nargparse.ArgumentParser().parse_args()\n")
        before = filesystem_state(self.root)
        self.assertEqual(self.check()["status"], "CHECKS PASSED; NO MIGRATION PERFORMED")
        self.assertEqual(filesystem_state(self.root), before)

    def test_unsafe_write_permissions_are_rejected_without_repair(self):
        for path in (self.live / "bin/diagnostics.py", self.live / "config", self.live, self.root):
            original = stat.S_IMODE(path.stat().st_mode)
            for write_mode in (0o020, 0o002):
                with self.subTest(path=path.name, write_mode=write_mode):
                    path.chmod(original | write_mode)
                    try:
                        self.assert_read_only_block("permissions|writable")
                    finally:
                        path.chmod(original)
        self.assertEqual(self.commands, [])

    def test_unsafe_owner_is_rejected_without_chown(self):
        path = self.live / "bin/diagnostics.py"
        original_stat = Path.lstat

        def different_owner(target, *args, **kwargs):
            result = original_stat(target, *args, **kwargs)
            if target == path:
                values = list(result)
                values[4] = os.geteuid() + 1
                return os.stat_result(values)
            return result

        with mock.patch.object(Path, "lstat", different_owner), \
                mock.patch.object(migration.os, "chown", side_effect=AssertionError("Must not repair ownership")):
            self.assert_read_only_block("ownership")
        self.assertEqual(self.commands, [])

    def test_root_preflight_keeps_normal_updater_rejection_of_sticky_tmp_ancestry(self):
        original_lstat, original_stat = Path.lstat, Path.stat

        def root_owned(method):
            def inspect(path, *args, **kwargs):
                information = list(method(path, *args, **kwargs))
                information[4] = 0
                return os.stat_result(information)
            return inspect

        # A protected snapshot may use a private sticky-tmp anchor. The root
        # active-installation contract remains the updater's stricter ancestry
        # rule; making this fixture root-owned must not turn /tmp into safe /opt.
        with mock.patch.object(Path, "lstat", root_owned(original_lstat)), \
                mock.patch.object(Path, "stat", root_owned(original_stat)):
            self.assert_read_only_block("root-owned and protected from other writers", owner=0, group=0)
        self.assertEqual(self.commands, [])

    def test_unsafe_updater_lock_is_rejected_even_in_recovery_mode(self):
        self.recovery_point()
        lock = self.live.parent / ("." + self.live.name + ".update.lock")
        lock.write_bytes(b"existing updater lock")
        lock.chmod(0o666)
        for mode in ("inspect", "recovery"):
            with self.subTest(mode=mode):
                self.assert_read_only_block("permissions", mode)
        lock.unlink()
        lock.symlink_to(self.manifest)
        self.assert_read_only_block("Symlink|noncanonical", "recovery")
        self.assertEqual(self.commands, [])

    def test_privileged_modes_and_installed_links_are_rejected(self):
        program = self.live / "bin/application"
        program.chmod(0o4755)
        self.assert_read_only_block("Privileged software mode")
        program.chmod(0o644)
        (self.live / "bin/link").symlink_to(program)
        self.assert_read_only_block("link|Unsafe")

    def test_existing_helper_directory_blocks_preflight_without_copying_inside_it(self):
        target = self.live / "bin/update.py"
        target.unlink()
        target.mkdir(mode=0o755)
        self.assert_read_only_block("Unsafe type", "inspect")
        self.assertEqual(list(target.iterdir()), [])
        self.assertEqual(self.commands, [])

    def test_missing_managed_directory_or_diagnostics_is_not_migratable(self):
        shutil.rmtree(self.live / "python_fallback")
        self.assert_read_only_block("No such file")
        (self.live / "python_fallback").mkdir()
        (self.live / "bin/diagnostics.py").unlink()
        self.assert_read_only_block("No such file")

    def test_missing_toolchain_and_compiler_are_reported_without_execution(self):
        for command in ("cmake", "ctest", "bash", "python3", "compiler"):
            with self.subTest(command=command):
                def which(name):
                    if name == command or (command == "compiler" and name in ("c++", "g++", "clang++")):
                        return None
                    return "/fixture-tools/" + name
                self.assert_read_only_block("Missing updater prerequisite", which=which)
        self.assertEqual(self.commands, [])

    def test_missing_operator_release_provenance_cannot_be_replaced_by_source_trust(self):
        policy = json.loads((self.live / "config/update_source.json").read_text())
        policy["update_policy"]["sha256"] = None
        (self.live / "config/update_source.json").write_text(json.dumps(policy), encoding="utf-8")
        self.assert_read_only_block("independently configured SHA-256")
        self.assertEqual(self.commands, [])

    def test_legacy_backups_are_reported_but_are_not_recovery_evidence(self):
        storage = self.live / "backups"
        storage.mkdir(mode=0o700)
        (storage / "v8.0.1.tar.gz").write_bytes(b"Unverified old archive")
        result = self.check()
        self.assertEqual(result["unverified_legacy_backups"], ["v8.0.1.tar.gz"])
        self.assertNotEqual(result["status"], "MIGRATION VERIFIED")
        legacy = storage / "old-directory"
        legacy.mkdir(mode=0o700)
        self.assert_read_only_block("No such file", "before", snapshot=legacy, snapshot_sha256="a" * 64)

    def test_interrupted_backup_requires_manual_investigation(self):
        interrupted = self.live / "backups/.incomplete-interrupted"
        interrupted.mkdir(parents=True, mode=0o700)
        self.assert_read_only_block("Interrupted backup requires manual investigation")
        self.assertTrue(interrupted.exists())

    def test_corrupt_existing_verified_format_backup_blocks_even_inspection(self):
        snapshot = backup.create_backup(self.live, self.live / "backups", "old", required_owner=os.geteuid())
        (snapshot / "payload/bin/application").write_bytes(b"corrupted old recovery data")
        self.assert_read_only_block("contents do not match")


class RecoveryAndVerificationTests(MigrationFixture):
    def test_intact_snapshot_with_original_helper_directory_is_not_safe_helper_recovery(self):
        target = self.live / "bin/update.py"
        target.unlink()
        target.mkdir(mode=0o755)
        snapshot = self.recovery_point()
        verified = backup.verify_backup(snapshot, required_owner=os.geteuid())
        self.assertEqual(verified["bin/update.py"]["type"], "directory")
        self.assert_read_only_block("Recovery helper must be an original regular file or absent", "recovery")
        self.assertEqual(list(target.iterdir()), [])
        self.assertEqual(self.commands, [])

    def test_recovery_checks_only_pinned_snapshot_when_live_diagnostics_are_damaged(self):
        self.recovery_point()
        self.replace_helpers(("diagnostics.py",))
        (self.live / "bin/diagnostics.py").chmod(0o777)
        (self.live / "config/vehicle_status.json").unlink()
        before = filesystem_state(self.root)
        result = self.check("recovery", which=lambda _: None,
                            runner=mock.Mock(side_effect=AssertionError("Recovery must not run damaged diagnostics")))
        self.assertEqual(result["status"], "RECOVERY SNAPSHOT VERIFIED; NO MIGRATION SUCCESS")
        self.assertEqual(result["recovery_manifest_sha256"], self.recovery_sha256)
        self.assertNotIn("matching_helpers", result)
        self.assertEqual(filesystem_state(self.root), before)
        self.assertEqual(self.commands, [])
        self.assert_read_only_block("permissions", "verify")

    def test_recovery_requires_source_trust_and_intact_selected_pinned_snapshot(self):
        self.assert_read_only_block("selected, pinned recovery snapshot", "recovery")
        snapshot = self.recovery_point()
        for pin in (None, "0" * 64, "b" * 64):
            with self.subTest(pin=pin):
                self.assert_read_only_block("SHA-256|pin", "recovery", snapshot_sha256=pin)
        self.assert_read_only_block("Source manifest SHA-256 mismatch", "recovery", manifest_sha256="b" * 64)
        (snapshot / "payload/bin/application").write_bytes(b"corrupted recovery snapshot")
        self.assert_read_only_block("contents do not match", "recovery")
        self.assertEqual(self.commands, [])

    def test_manual_helper_only_recovery_preserves_operational_files_and_allows_before(self):
        snapshot = self.recovery_point()
        original = backup.tree_manifest(self.live)
        self.replace_helpers(("diagnostics.py", "backup_restore.py"))
        self.assert_read_only_block("Partial/unexpected migration", "verify")
        self.assertEqual(self.check("recovery")["status"], "RECOVERY SNAPSHOT VERIFIED; NO MIGRATION SUCCESS")
        # Restore only the four recorded maintenance paths after authenticating
        # the recovery snapshot, exactly as the runbook directs. Installed old
        # rollback and backup helpers are never executed.
        expected = backup.verify_backup(snapshot, required_owner=os.geteuid())
        operational = filesystem_state(self.live / "config")
        for name in migration.HELPERS:
            target = self.live / "bin" / name
            shutil.copyfile(snapshot / "payload/bin" / name, target)
            target.chmod(expected["bin/" + name]["mode"])
        self.assertEqual(filesystem_state(self.live / "config"), operational)
        self.assertEqual(backup.tree_manifest(self.live), original)
        self.assertEqual(self.check("before")["status"], "CHECKS PASSED; NO MIGRATION PERFORMED")
        self.assert_read_only_block("Partial/unexpected migration", "verify")

    def test_before_requires_explicit_selected_pinned_recovery(self):
        self.assert_read_only_block("selected, pinned recovery snapshot", "before")
        self.recovery_point()
        for pin in (None, "", "0" * 64, "b" * 64):
            with self.subTest(pin=pin):
                self.assert_read_only_block("SHA-256|pin", "before", snapshot_sha256=pin)

    def test_corrupted_snapshot_blocks_before_any_replacement(self):
        snapshot = self.recovery_point()
        (snapshot / "payload/bin/application").write_bytes(b"corrupted snapshot")
        self.assert_read_only_block("contents do not match", "before")
        self.assertEqual((self.live / "bin/diagnostics.py").read_bytes(), self.legacy["diagnostics.py"])

    def test_recovery_manifest_tampering_does_not_accept_consistent_payload(self):
        snapshot = self.recovery_point()
        (snapshot / "payload/bin/application").write_bytes(b"different recoverable installation")
        changed = backup.tree_manifest(snapshot / "payload")
        (snapshot / "manifest.json").write_text(json.dumps({"format": 1, "files": changed}), encoding="utf-8")
        self.assert_read_only_block("Recovery manifest SHA-256 mismatch", "before")

    def test_missing_snapshot_and_unexpected_live_change_block_before(self):
        self.assert_read_only_block("No such file", "before", snapshot=self.root / "missing", snapshot_sha256="a" * 64)
        self.recovery_point()
        (self.live / "bin/application").write_bytes(b"concurrent live software change")
        self.assert_read_only_block("differs from live software", "before")

    def test_repeated_before_checks_are_read_only_and_do_not_claim_migration(self):
        self.recovery_point()
        before = filesystem_state(self.root)
        for _ in range(2):
            result = self.check("before")
            self.assertEqual(result["status"], "CHECKS PASSED; NO MIGRATION PERFORMED")
            self.assertEqual(result["recovery_manifest_sha256"], self.recovery_sha256)
        self.assertEqual(filesystem_state(self.root), before)

    def test_partial_migration_is_detected_and_trusted_snapshot_restores_fixture(self):
        snapshot = self.recovery_point()
        original = backup.tree_manifest(self.live)
        self.assertEqual(self.check("before")["status"], "CHECKS PASSED; NO MIGRATION PERFORMED")
        self.replace_helpers(("diagnostics.py",))
        self.assert_read_only_block("Partial/unexpected migration", "verify")
        self.assert_read_only_block("differs from live software", "before")
        self.assertTrue(backup.restore_backup(self.live, snapshot, required_owner=os.geteuid()))
        self.assertEqual(backup.tree_manifest(self.live), original)
        self.assertEqual(self.check("before")["status"], "CHECKS PASSED; NO MIGRATION PERFORMED")

    def test_success_requires_exact_helpers_real_diagnostic_contract_and_preserved_evidence(self):
        operational = {name: (self.live / name).read_bytes()
                       for name in backup.tree_manifest(self.live)
                       if (self.live / name).is_file() and not name.startswith("bin/")}
        self.recovery_point()
        self.check("before")
        self.replace_helpers()
        before = filesystem_state(self.root)
        with mock.patch.dict(os.environ, {"DS_SOH": "0", "DS_MODEL": "false-ambient-model"}):
            result = self.check("verify")
        self.assertEqual(result["status"], "MIGRATION VERIFIED")
        self.assertEqual(result["source_commit"], COMMIT)
        self.assertEqual(set(result["matching_helpers"]), set(migration.HELPERS))
        self.assertEqual(result["configuration_sha256"], {
            name: digest((self.live / "config" / name).read_bytes()) for name in update.REQUIRED_CONFIGS})
        self.assertIn("normal remote updater gates remain required", result["scope"])
        self.assertEqual(filesystem_state(self.root), before)
        for name, data in operational.items():
            self.assertEqual((self.live / name).read_bytes(), data)
        arguments, options = self.commands[-1]
        self.assertIn(str(self.live / "bin/diagnostics.py"), arguments)
        self.assertIn("--updater-evidence", arguments)
        self.assertEqual(arguments[arguments.index("--config-dir") + 1], str(self.live / "config"))
        self.assertFalse(any(name.startswith("DS_") for name in options["env"]))

    def test_repeated_verification_is_read_only_and_never_resumes_or_reinstalls(self):
        self.recovery_point()
        self.replace_helpers()
        before = filesystem_state(self.root)
        for _ in range(2):
            self.assertEqual(self.check("verify")["status"], "MIGRATION VERIFIED")
        self.assertEqual(filesystem_state(self.root), before)
        self.assert_read_only_block("differs from live software", "before")

    def test_wrong_installed_hash_mode_or_group_has_no_success(self):
        self.recovery_point()
        self.replace_helpers()
        helper = self.live / "bin/update.py"
        original = helper.read_bytes()
        helper.write_bytes(original + b"\n# Unexpected bytes\n")
        self.assert_read_only_block("Partial/unexpected migration", "verify")
        helper.write_bytes(original)
        helper.chmod(0o700)
        self.assert_read_only_block("Partial/unexpected migration", "verify")
        helper.chmod(0o755)
        self.assert_read_only_block("Partial/unexpected migration", "verify", group=os.getegid() + 1)

    def test_operational_configuration_and_safety_evidence_changes_cannot_be_hidden(self):
        self.recovery_point()
        self.replace_helpers()
        for name in ("vehicle_status.json", "safety_thresholds.json", "update_source.json"):
            path = self.live / "config" / name
            original = path.read_bytes()
            path.write_bytes(original + b"\n")
            try:
                with self.subTest(name=name):
                    self.assert_read_only_block("Partial/unexpected migration", "verify")
            finally:
                path.write_bytes(original)

    def test_unsafe_authoritative_status_fails_real_diagnostics_despite_healthy_preset(self):
        status_path = self.live / "config/vehicle_status.json"
        status = json.loads(status_path.read_text())
        status["battery_soh"] = 20.0
        status_path.write_text(json.dumps(status), encoding="utf-8")
        self.recovery_point()
        self.replace_helpers()
        with mock.patch.dict(os.environ, {"DS_SOH": "99", "DS_TEMP_C": "25", "DS_VOLTAGE_V": "355"}):
            self.assert_read_only_block("Maintenance evidence failed", "verify")

    def test_real_diagnostic_receipt_mutation_and_nonzero_exit_do_not_become_success(self):
        self.recovery_point()
        self.replace_helpers()
        for failure in ("nonce", "hash", "source", "duplicate", "missing", "nonzero"):
            with self.subTest(failure=failure):
                def dishonest_runner(command, **options):
                    ok, output, error = self.run_real(command, **options)
                    self.assertTrue(ok, error)
                    if failure == "nonzero":
                        return False, output, "injected failed subprocess"
                    if failure == "missing":
                        return True, "Legacy success without a receipt", ""
                    lines = output.splitlines()
                    receipt_line = next(line for line in lines if line.startswith(update.DIAGNOSTIC_EVIDENCE_PREFIX))
                    receipt = json.loads(receipt_line[len(update.DIAGNOSTIC_EVIDENCE_PREFIX):])
                    if failure == "nonce":
                        receipt["nonce"] = "0" * 32
                    elif failure == "hash":
                        receipt["sha256"]["vehicle_status.json"] = "0" * 64
                    elif failure == "source":
                        receipt["config_dir"] = str(self.source / "config")
                    altered = update.DIAGNOSTIC_EVIDENCE_PREFIX + json.dumps(receipt)
                    if failure == "duplicate":
                        altered += "\n" + altered
                    return True, output.replace(receipt_line, altered), ""
                self.assert_read_only_block("Maintenance evidence failed", "verify", runner=dishonest_runner)

    def test_changes_during_diagnostics_are_detected_without_additional_writes(self):
        self.recovery_point()
        self.replace_helpers()
        status = self.live / "config/vehicle_status.json"
        changed_state = []

        def interrupted_runner(command, **options):
            result = self.run_real(command, **options)
            status.write_bytes(status.read_bytes() + b"\n")
            changed_state.append(filesystem_state(self.root))
            return result

        with self.assertRaisesRegex(Exception, "selected evidence changed"):
            self.check("verify", runner=interrupted_runner)
        self.assertEqual(filesystem_state(self.root), changed_state[-1])

    def test_source_and_recovery_are_reauthenticated_after_diagnostics(self):
        self.recovery_point()
        self.replace_helpers()
        for target, expected in ((self.source / "scripts/rollback.sh", "Trusted source hash mismatch"),
                                 (self.recovery / "payload/bin/application", "contents do not match")):
            original = target.read_bytes()
            with self.subTest(target=target.name):
                def changed_runner(command, **options):
                    result = self.run_real(command, **options)
                    target.write_bytes(original + b"\nchanged during check")
                    return result
                with self.assertRaisesRegex(Exception, expected):
                    self.check("verify", runner=changed_runner)
            target.write_bytes(original)


class AdministrativeInterfaceTests(unittest.TestCase):
    def test_nonadministrator_cli_is_rejected_before_check(self):
        with mock.patch.object(migration.os, "geteuid", return_value=1000), \
                mock.patch.object(migration, "check") as checker, contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as result:
                migration.main(["inspect", "--source", "/fixture/source", "--manifest", "/fixture/manifest",
                                "--manifest-sha256", "a" * 64, "--commit", COMMIT,
                                "--install-dir", "/fixture/install"])
        self.assertEqual(result.exception.code, 2)
        checker.assert_not_called()

    def test_failed_cli_has_nonzero_status_no_success_and_explicit_recovery_reference(self):
        output, error = io.StringIO(), io.StringIO()
        with mock.patch.object(migration.os, "geteuid", return_value=0), \
                mock.patch.object(migration, "check", side_effect=migration.MigrationError("failed prerequisite")), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(error):
            result = migration.main(["verify", "--source", "/fixture/source", "--manifest", "/fixture/manifest",
                                     "--manifest-sha256", "a" * 64, "--commit", COMMIT,
                                     "--install-dir", "/fixture/install"])
        self.assertEqual(result, 1)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("BLOCKED: failed prerequisite", error.getvalue())
        self.assertIn("docs/UPDATER_LEGACY_MIGRATION.md", error.getvalue())


class NormalUpdaterGatesAfterMigrationTests(existing_update_tests.TemporaryUpdaterCase):
    def migrate_fixture(self):
        manifest = self.root / "maintenance-manifest.json"
        source_sha256 = authenticate_fixture(self.source, manifest)
        # All setup and replacements are confined to the existing updater fixture.
        self.write_file(self.live / "bin/update.py", b"# old updater\n")
        snapshot = backup.create_backup(self.live, self.root / "recovery", "selected", required_owner=os.geteuid())
        pin = digest((snapshot / "manifest.json").read_bytes())
        parameters = {
            "source": self.source, "manifest": manifest, "manifest_sha256": source_sha256,
            "commit": COMMIT, "install": self.live, "snapshot": snapshot, "snapshot_sha256": pin,
            "owner": os.geteuid(), "group": os.getegid(), "which": lambda name: "/fixture-tools/" + name,
        }
        migration.check("before", **parameters)
        for name in migration.HELPERS:
            shutil.copyfile(self.source / "scripts" / name, self.live / "bin" / name)
            (self.live / "bin" / name).chmod(0o755)
        self.assertEqual(migration.check("verify", **parameters)["status"], "MIGRATION VERIFIED")

    def test_migration_success_does_not_bypass_normal_release_provenance(self):
        self.migrate_fixture()
        self.policy["sha256"] = None
        self.save_policy()
        self.assert_blocked("independently configured SHA-256")
        self.opener.assert_not_called()

    def test_migration_success_does_not_bypass_normal_current_diagnostic_contract(self):
        self.migrate_fixture()
        self.write_file(self.live / "bin/diagnostics.py", b"print('Legacy success without evidence')\n")

        def actual_current_diagnostics(command, **options):
            if str(self.live / "bin/diagnostics.py") in command:
                return update.run_command(command, **options)
            return self.runner(command, **options)

        self.assert_blocked("missing or mismatched evidence receipt", runner=actual_current_diagnostics)
        self.opener.assert_not_called()

    def test_migration_success_does_not_bypass_normal_verified_backup_gate(self):
        self.migrate_fixture()
        self.backup_mode = "empty"
        before = self.snapshot()
        deployer = mock.Mock(side_effect=AssertionError("A bad backup must not permit deployment"))
        self.assertNotEqual(self.run_update(deployer=deployer), 0)
        self.assertEqual(self.snapshot(), before)
        deployer.assert_not_called()
        self.assertFalse(any(line.startswith("SUCCESS:") for line in self.logs))
        self.assertTrue(any("backup" in command for command, _ in self.commands))


if __name__ == "__main__":
    unittest.main()
