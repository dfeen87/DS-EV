#!/usr/bin/env python3
"""Fail-closed updater regressions; all installation paths are temporary.

Network requests and external commands are injected. No test invokes an
installer, the live rollback shell script, or a privileged operation.
"""

import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import socket
import shutil
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import urllib.error


ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))
SPEC = importlib.util.spec_from_file_location("ds_update_under_test", SCRIPTS / "update.py")
update = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(update)


class Response(io.BytesIO):
    """Minimal HTTPS response supporting streamed download tests."""

    def __init__(self, data, final_url="https://releases.example/payload.tar.gz", headers=None):
        super().__init__(data)
        self.final_url = final_url
        self.headers = headers or {}

    def geturl(self):
        return self.final_url


def archive_bytes(files=None, members=None):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w:gz") as archive:
        for name, content in (files or {}).items():
            info = tarfile.TarInfo(name)
            info.size = len(content)
            info.mode = 0o644
            archive.addfile(info, io.BytesIO(content))
        for info, content in members or []:
            archive.addfile(info, io.BytesIO(content) if content is not None else None)
    return output.getvalue()


class TemporaryUpdaterCase(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ds-update-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "checkout"
        self.live = self.root / "installation"
        self.source.mkdir()
        self.live.mkdir()
        self.logs = []
        self.commands = []
        self.diagnostic_environments = []
        self.fail_command = None
        self.backup_mode = "valid"
        self.restore_mode = "valid"
        self.test_count = 1
        self.cli_executable = True
        self.write_file(self.source / "checkout-marker", b"unchanged checkout")
        for name in ("rollback.sh", "backup_restore.py"):
            content = (SCRIPTS / name).read_bytes()
            self.write_file(self.source / "scripts" / name, content)
            self.write_file(self.live / "bin" / name, content)
        self.write_file(self.live / "bin/diagnostics.py", b"# current diagnostics\n")
        self.write_file(self.live / "bin/old-program", b"old program")
        self.write_file(self.live / "lib/libold.so", b"old library")
        self.write_file(self.live / "python/old.py", b"old wrapper")
        self.write_file(self.live / "python_fallback/old.py", b"old fallback")
        self.write_file(self.live / "docs/OLD.md", b"old docs")
        self.files = {
            "CMakeLists.txt": b"cmake_minimum_required(VERSION 3.16)\n",
            "scripts/diagnostics.py": b"# staged diagnostics\n",
            "scripts/rollback.sh": b"#!/bin/sh\nexit 0\n",
            "scripts/backup_restore.py": b"# staged backup utility\n",
            "scripts/update.py": b"# staged updater\n",
            "ds_core/python/ds_enhancer.py": b"# new wrapper\n",
            "ds_core/python_fallback/ds_enhancer_fallback.py": b"# new fallback\n",
            "docs/RELEASE_8.0.2.md": b"# Updater regression fixture\n",
        }
        for name in ("vehicle_profiles", "vehicle_status", "safety_thresholds", "install_policy"):
            content = (ROOT / "config" / f"{name}.json").read_bytes()
            self.files[f"config/{name}.json"] = content
            self.write_file(self.live / f"config/{name}.json", content)
        self.config = self.source / "config/update_source.json"
        # A release's next-update configuration cannot contain its own hash.
        # The externally trusted configuration below pins this whole archive.
        self.files["config/update_source.json"] = json.dumps(
            {"update_policy": self.policy_for(b"future-release-not-this-archive")}
        ).encode()
        self.payload = archive_bytes(self.files)
        self.policy = self.policy_for(self.payload)
        self.save_policy()
        self.opener = mock.Mock(spec=[], side_effect=lambda *args, **kwargs: Response(self.payload))

    @staticmethod
    def write_file(path, content):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)

    @staticmethod
    def policy_for(payload):
        return {
            "prefer_git": False,
            "fallback_url": "https://releases.example/payload.tar.gz",
            "sha256": hashlib.sha256(payload).hexdigest(),
            "timeout_seconds": 5,
            "max_download_bytes": 1024 * 1024,
            "max_extracted_bytes": 1024 * 1024,
            "max_archive_members": 100,
            "auto_rollback_on_failure": True,
        }

    def save_policy(self):
        content = json.dumps({"update_policy": self.policy}).encode()
        self.write_file(self.config, content)
        self.write_file(self.live / "config/update_source.json", content)

    def set_payload(self, payload):
        self.payload = payload
        self.policy["sha256"] = hashlib.sha256(payload).hexdigest()
        self.save_policy()

    def snapshot(self):
        """Inspect managed installation files, excluding backup/log metadata."""
        return {
            str(path.relative_to(self.live)): path.read_bytes()
            for directory in ("bin", "lib", "config", "python", "python_fallback", "docs")
            if (self.live / directory).is_dir()
            for path in (self.live / directory).rglob("*")
            if path.is_file()
        }

    def runner(self, command, cwd=None, env=None):
        command = [str(value) for value in command]
        self.commands.append((command, cwd))
        self.assertFalse(any(value == "sudo" for value in command))
        self.assertFalse(any("/opt/ds_enhancement" in value for value in command))
        self.assertNotIn("git", command)
        if self.fail_command and self.fail_command(command, cwd):
            return False, "", "injected command failure"
        if any(value.endswith("diagnostics.py") for value in command):
            self.diagnostic_environments.append(env)
            context = Path(command[command.index("--config-dir") + 1])
            nonce = command[command.index("--evidence-nonce") + 1]
            receipt = update._diagnostic_receipt(context, nonce)
            return True, update.DIAGNOSTIC_EVIDENCE_PREFIX + json.dumps(receipt, sort_keys=True), ""
        if "backup" in command:
            if self.backup_mode == "failed":
                return False, "", "backup refused"
            if self.backup_mode == "empty":
                return True, "backup allegedly succeeded", ""
            import backup_restore
            install = Path(command[command.index("--install-dir") + 1])
            backup_dir = Path(command[command.index("--backup-dir") + 1])
            snapshot_id = command[command.index("--snapshot-id") + 1]
            backup_restore.create_backup(install, backup_dir, snapshot_id)
        elif "restore" in command or "rollback" in command:
            if self.restore_mode == "failed":
                return False, "", "restore refused"
            if self.restore_mode == "empty":
                return True, "restoration allegedly succeeded", ""
            import backup_restore
            install = Path(command[command.index("--install-dir") + 1])
            backup_dir = Path(command[command.index("--backup-dir") + 1])
            snapshot_id = command[command.index("--snapshot-id") + 1]
            backup_restore.restore_backup(install, backup_dir / snapshot_id)
        elif command[:2] == ["cmake", "--build"]:
            stage = Path(cwd)
            self.write_file(stage / "ds_core/bin/ds_enhancer", b"new binary")
            (stage / "ds_core/bin/ds_enhancer").chmod(0o755 if self.cli_executable else 0o644)
            self.write_file(stage / "ds_core/lib/libds_enhancer.so", b"new library")
        elif command == ["ctest", "-N"]:
            return True, f"Total Tests: {self.test_count}\n", ""
        return True, "", ""

    def run_update(self, **kwargs):
        arguments = {
            "install_dir": str(self.live), "source_dir": str(self.source), "config_path": str(self.config),
            "runner": self.runner, "opener": self.opener, "logger": self.logs.append,
            "require_root": False, "which": lambda name: f"/test-tools/{name}",
        }
        arguments.update(kwargs)
        return update.run_update(**arguments)

    def assert_blocked(self, expected=None, **kwargs):
        before = self.snapshot()
        status = self.run_update(**kwargs)
        self.assertNotEqual(status, 0, self.logs)
        self.assertEqual(self.snapshot(), before)
        self.assertFalse(any(line.startswith("SUCCESS:") for line in self.logs), self.logs)
        self.assertFalse(any("backup" in cmd or "restore" in cmd for cmd, _ in self.commands))
        self.assertEqual((self.source / "checkout-marker").read_bytes(), b"unchanged checkout")
        message = "\n".join(self.logs)
        if expected is not None:
            self.assertIn(expected.lower(), message.lower())
        return message


class DownloadTests(TemporaryUpdaterCase):
    def download(self, policy=None, opener=None):
        destination = self.root / "download.tar.gz"
        update.download_archive(policy or self.policy, str(destination), opener=opener or self.opener)
        return destination

    def test_missing_release_asset_is_clear_failure_without_fallback(self):
        self.opener.side_effect = urllib.error.HTTPError(
            self.policy["fallback_url"], 404, "Not Found", {}, None)
        message = self.assert_blocked()
        self.assertIn("404", message)
        self.assertNotIn("SUCCESS:", message)
        self.assertFalse((self.source / "remote_payload").exists())

    def test_network_timeout_blocks_update(self):
        self.opener.side_effect = socket.timeout("timed out")
        self.assertIn("timed out", self.assert_blocked().lower())
        _, kwargs = self.opener.call_args
        self.assertEqual(kwargs["timeout"], self.policy["timeout_seconds"])

    def test_stream_timeout_blocks_update(self):
        response = Response(self.payload)
        response.read = mock.Mock(side_effect=socket.timeout("stream timed out"))
        response.read1 = mock.Mock(side_effect=socket.timeout("stream timed out"))
        self.opener.side_effect = None
        self.opener.return_value = response
        self.assertIn("timed out", self.assert_blocked().lower())

    def test_trickling_response_cannot_extend_total_download_deadline(self):
        elapsed = [0.0]
        response = Response(self.payload)

        def slow_read(size):
            elapsed[0] += 3.0
            return b"x"

        response.read1 = mock.Mock(side_effect=slow_read)
        response.read = mock.Mock(side_effect=AssertionError("read1 should bound each transport read"))
        with mock.patch.object(update.time, "monotonic", side_effect=lambda: elapsed[0]):
            with self.assertRaises(update.UpdateError) as result:
                self.download(opener=mock.Mock(spec=[], return_value=response))
        self.assertIn("timed out", str(result.exception).lower())
        self.assertEqual(response.read1.call_count, 2)
        response.read.assert_not_called()

    def test_valid_digest_downloads_exact_payload_with_timeout(self):
        destination = self.download()
        self.assertEqual(destination.read_bytes(), self.payload)
        self.assertEqual(self.opener.call_args.kwargs["timeout"], 5)

    def test_checksum_mismatch_rejects_archive(self):
        self.policy["sha256"] = "f" * 64
        self.save_policy()
        self.assertIn("sha", self.assert_blocked().lower())

    def test_download_size_is_bounded_even_without_content_length(self):
        self.policy["max_download_bytes"] = 16
        self.save_policy()
        self.assertIn("size", self.assert_blocked().lower())

    def test_content_length_over_limit_is_rejected(self):
        response = Response(self.payload, headers={"Content-Length": str(2 * 1024 * 1024)})
        response.read = mock.Mock(wraps=response.read)
        response.read1 = mock.Mock(wraps=response.read1)
        with self.assertRaises(update.UpdateError):
            self.download(opener=mock.Mock(spec=[], return_value=response))
        response.read.assert_not_called()
        response.read1.assert_not_called()

    def test_http_source_is_rejected_before_request(self):
        self.policy["fallback_url"] = "http://releases.example/payload.tar.gz"
        self.save_policy()
        self.assert_blocked("https")
        self.opener.assert_not_called()

    def test_https_redirect_to_http_is_rejected(self):
        self.opener.side_effect = lambda *args, **kwargs: Response(self.payload, "http://unsafe.example/archive")
        self.assert_blocked("https")

    def test_http_redirect_is_rejected_before_transport_is_opened(self):
        request = update.urllib.request.Request(self.policy["fallback_url"])
        redirect = update.HTTPSOnlyRedirect()
        with mock.patch.object(update.urllib.request.HTTPRedirectHandler, "redirect_request") as parent:
            with self.assertRaises(update.UpdateError):
                redirect.redirect_request(request, None, 302, "Found", {}, "http://unsafe.example/archive")
        parent.assert_not_called()

    def test_missing_digest_blocks_download(self):
        del self.policy["sha256"]
        self.save_policy()
        self.assert_blocked("sha-256")
        self.opener.assert_not_called()

    def test_invalid_digest_blocks_download(self):
        self.policy["sha256"] = "not-a-sha256-digest"
        self.save_policy()
        self.assert_blocked("sha-256")
        self.opener.assert_not_called()


class ArchiveTests(TemporaryUpdaterCase):
    def assert_rejected(self, payload, **kwargs):
        archive = self.root / "archive.tar.gz"
        archive.write_bytes(payload)
        stage = Path(tempfile.mkdtemp(prefix="stage-", dir=self.root))
        with self.assertRaises(update.UpdateError):
            update.extract_archive(str(archive), str(stage), **kwargs)
        self.assertEqual(list(stage.rglob("*")), [], "Extraction happened before all members were validated")

    def test_corrupt_archive_blocks_deployment(self):
        self.set_payload(b"not a gzip or tar archive")
        self.assert_blocked("archive")

    def test_traversal_is_rejected_before_extracting_safe_member(self):
        self.assert_rejected(archive_bytes({"safe.txt": b"safe", "../escape.txt": b"escape"}))
        self.assertFalse((self.root / "escape.txt").exists())

    def test_absolute_member_is_rejected(self):
        self.assert_rejected(archive_bytes({str(self.root / "escaped"): b"escape"}))
        self.assertFalse((self.root / "escaped").exists())

    def test_symlink_is_rejected(self):
        member = tarfile.TarInfo("unsafe-link")
        member.type = tarfile.SYMTYPE
        member.linkname = "../../outside"
        self.assert_rejected(archive_bytes({"safe.txt": b"safe"}, [(member, None)]))

    def test_hardlink_is_rejected(self):
        member = tarfile.TarInfo("unsafe-link")
        member.type = tarfile.LNKTYPE
        member.linkname = "../outside"
        self.assert_rejected(archive_bytes(members=[(member, None)]))

    def test_fifo_is_rejected(self):
        member = tarfile.TarInfo("pipe")
        member.type = tarfile.FIFOTYPE
        self.assert_rejected(archive_bytes(members=[(member, None)]))

    def test_sparse_and_contiguous_member_types_are_rejected(self):
        for member_type in (tarfile.GNUTYPE_SPARSE, tarfile.CONTTYPE):
            with self.subTest(member_type=member_type):
                member = tarfile.TarInfo("unsupported")
                member.type = member_type
                self.assert_rejected(archive_bytes(members=[(member, None)]))

    def test_duplicate_member_is_rejected(self):
        member = tarfile.TarInfo("same.txt")
        member.size = 1
        self.assert_rejected(archive_bytes({"same.txt": b"a"}, [(member, b"b")]))

    def test_file_parent_conflict_is_rejected(self):
        self.assert_rejected(archive_bytes({"parent": b"file", "parent/child": b"child"}))

    def test_expanded_size_is_bounded(self):
        self.assert_rejected(archive_bytes({"large.txt": b"a" * 100}), max_bytes=16)

    def test_member_count_is_bounded(self):
        self.assert_rejected(archive_bytes({"one": b"1", "two": b"2"}), max_members=1)

    def test_valid_regular_archive_extracts(self):
        archive = self.root / "valid.tar.gz"
        archive.write_bytes(archive_bytes({"folder/file.txt": b"valid"}))
        stage = self.root / "valid-stage"
        update.extract_archive(str(archive), str(stage))
        self.assertEqual((stage / "folder/file.txt").read_bytes(), b"valid")


class UpdateGateTests(TemporaryUpdaterCase):
    def test_absent_installation_cannot_bypass_required_backup(self):
        shutil.rmtree(self.live)
        self.assert_blocked("installation")
        self.assertFalse(self.live.exists())
        self.opener.assert_not_called()

    def test_no_source_configuration_blocks_deployment(self):
        self.config.unlink()
        self.assert_blocked("update-source configuration")
        self.opener.assert_not_called()

    def test_no_source_url_blocks_deployment(self):
        self.policy.pop("fallback_url")
        self.save_policy()
        self.assert_blocked("release source")
        self.opener.assert_not_called()

    def test_missing_active_diagnostics_cannot_use_checkout_diagnostics(self):
        (self.live / "bin/diagnostics.py").unlink()
        self.write_file(self.source / "scripts/diagnostics.py", b"# cannot substitute for installed diagnostic\n")
        self.assert_blocked("current diagnostics")

    def test_failing_active_diagnostics_blocks_deployment(self):
        self.fail_command = lambda cmd, cwd: str(self.live / "bin/diagnostics.py") in cmd
        self.assert_blocked("current diagnostics failed")

    def test_missing_staged_diagnostics_blocks_deployment(self):
        del self.files["scripts/diagnostics.py"]
        self.set_payload(archive_bytes(self.files))
        self.assert_blocked("diagnostics.py")

    def test_missing_staged_updater_blocks_deployment(self):
        del self.files["scripts/update.py"]
        self.set_payload(archive_bytes(self.files))
        self.assert_blocked("update.py")

    def test_failing_staged_diagnostics_blocks_deployment(self):
        self.fail_command = lambda cmd, cwd: any(value.endswith("/scripts/diagnostics.py") for value in cmd)
        self.assert_blocked("staged diagnostics failed")

    def test_missing_backup_utility_blocks_deployment(self):
        del self.files["scripts/rollback.sh"]
        self.set_payload(archive_bytes(self.files))
        self.assert_blocked("rollback.sh")

    def test_missing_backup_helper_blocks_deployment(self):
        del self.files["scripts/backup_restore.py"]
        self.set_payload(archive_bytes(self.files))
        self.assert_blocked("backup_restore.py")

    def test_missing_current_rollback_utility_blocks_deployment(self):
        (self.source / "scripts/rollback.sh").unlink()
        (self.live / "bin/rollback.sh").unlink()
        self.assert_blocked("backup/rollback")
        self.opener.assert_not_called()

    def test_missing_current_backup_helper_blocks_deployment(self):
        (self.source / "scripts/backup_restore.py").unlink()
        (self.live / "bin/backup_restore.py").unlink()
        self.assert_blocked("backup helper")
        self.opener.assert_not_called()

    def test_missing_build_command_blocks_deployment(self):
        before = self.snapshot()
        with mock.patch.object(update.shutil, "which", return_value=None):
            result = update.run_update(
                install_dir=str(self.live), source_dir=str(self.source), config_path=str(self.config),
                runner=self.runner, opener=self.opener, logger=self.logs.append,
                require_root=False, which=lambda name: None,
            )
        self.assertNotEqual(result, 0)
        self.assertEqual(self.snapshot(), before)
        self.assertFalse(any("backup" in cmd for cmd, _ in self.commands))

    def test_git_update_is_disabled_without_mutating_checkout(self):
        self.write_file(self.source / ".git/HEAD", b"ref: refs/heads/main\n")
        self.policy["prefer_git"] = True
        self.save_policy()
        self.assertIn("git", self.assert_blocked().lower())
        self.opener.assert_not_called()
        self.assertEqual((self.source / ".git/HEAD").read_bytes(), b"ref: refs/heads/main\n")

    def test_production_rejects_local_mock_even_if_payload_exists(self):
        mock_dir = self.root / "remote_payload"
        self.write_file(mock_dir / "payload.tar.gz", self.payload)
        self.policy["local_mock_source"] = str(mock_dir)
        self.save_policy()
        self.assertIn("mock", self.assert_blocked().lower())
        self.opener.assert_not_called()

    def test_production_never_generates_payload_after_download_failure(self):
        self.opener.side_effect = urllib.error.URLError("source unavailable")
        with mock.patch.object(update.tarfile, "open", wraps=tarfile.open) as tar_open:
            self.assert_blocked()
        self.assertFalse(any("w" in str(call.args[1:]) for call in tar_open.call_args_list))

    def test_invalid_source_policy_types_are_rejected(self):
        for key, value in (("prefer_git", "false"), ("timeout_seconds", True),
                           ("max_download_bytes", -1), ("auto_rollback_on_failure", False)):
            with self.subTest(key=key):
                original = self.policy[key]
                self.policy[key] = value
                self.save_policy()
                self.assert_blocked()
                self.policy[key] = original

    def test_invalid_configuration_json_blocks_deployment(self):
        self.config.write_text("{invalid json", encoding="utf-8")
        self.assert_blocked("update-source configuration")


class DeploymentTests(TemporaryUpdaterCase):
    def assert_failed_without_success(self, result):
        self.assertNotEqual(result, 0, self.logs)
        self.assertFalse(any(line.startswith("SUCCESS:") for line in self.logs), self.logs)

    def test_failing_backup_blocks_live_mutation(self):
        self.backup_mode = "failed"
        before = self.snapshot()
        self.assert_failed_without_success(self.run_update())
        self.assertEqual(self.snapshot(), before)
        self.assertTrue(any("backup" in cmd for cmd, _ in self.commands))
        self.assertFalse(any("restore" in cmd for cmd, _ in self.commands))

    def test_backup_zero_exit_without_valid_snapshot_blocks_live_mutation(self):
        self.backup_mode = "empty"
        before = self.snapshot()
        self.assert_failed_without_success(self.run_update())
        self.assertEqual(self.snapshot(), before)
        self.assertTrue(any("backup" in cmd for cmd, _ in self.commands))

    def test_cmake_configuration_failure_blocks_deployment(self):
        self.fail_command = lambda cmd, cwd: cmd[0] == "cmake" and "--build" not in cmd
        self.assert_blocked("cmake configuration failed")

    def test_build_failure_leaves_active_installation_unchanged(self):
        self.fail_command = lambda cmd, cwd: cmd[:2] == ["cmake", "--build"]
        self.assert_blocked("staged build")

    def test_staged_ctest_failure_blocks_deployment(self):
        self.fail_command = lambda cmd, cwd: cmd == ["ctest", "--output-on-failure"]
        self.assert_blocked("staged tests failed")

    def test_empty_staged_ctest_suite_blocks_deployment(self):
        self.test_count = 0
        self.assert_blocked("tests")

    def test_non_executable_built_cli_blocks_deployment(self):
        self.cli_executable = False
        self.assert_blocked("executable")

    def test_deployment_failure_restores_exact_snapshot_and_still_fails(self):
        before = self.snapshot()

        def fail_after_partial_deployment(stage, install):
            self.write_file(Path(install) / "bin/old-program", b"partially replaced")
            self.write_file(Path(install) / "bin/new-partial-program", b"partial")
            raise OSError("injected copy failure")

        self.assert_failed_without_success(self.run_update(deployer=fail_after_partial_deployment))
        self.assertEqual(self.snapshot(), before)
        self.assertTrue(any("restore" in cmd for cmd, _ in self.commands))
        self.assertTrue(any(line.startswith("RESTORATION VERIFIED:") for line in self.logs), self.logs)

    def test_failed_rollback_is_reported_without_claiming_restoration(self):
        self.restore_mode = "failed"

        def fail_after_partial_deployment(stage, install):
            self.write_file(Path(install) / "bin/old-program", b"partially replaced")
            raise OSError("injected copy failure")

        self.assert_failed_without_success(self.run_update(deployer=fail_after_partial_deployment))
        self.assertEqual((self.live / "bin/old-program").read_bytes(), b"partially replaced")
        self.assertTrue(any("restore" in cmd for cmd, _ in self.commands))
        self.assertTrue(any(line.startswith("RESTORATION FAILED:") for line in self.logs), self.logs)
        self.assertFalse(any(line.startswith("RESTORATION VERIFIED:") for line in self.logs))

    def test_restore_zero_exit_without_matching_snapshot_is_not_success(self):
        self.restore_mode = "empty"

        def fail_after_partial_deployment(stage, install):
            self.write_file(Path(install) / "bin/old-program", b"partially replaced")
            raise OSError("injected copy failure")

        self.assert_failed_without_success(self.run_update(deployer=fail_after_partial_deployment))
        self.assertTrue(any(line.startswith("RESTORATION FAILED:") for line in self.logs), self.logs)
        self.assertFalse(any(line.startswith("RESTORATION VERIFIED:") for line in self.logs))

    def test_valid_trusted_update_deploys_only_after_validation_and_backup(self):
        status = self.run_update()
        self.assertEqual(status, 0, self.logs)
        self.assertEqual((self.live / "bin/ds_enhancer").read_bytes(), b"new binary")
        self.assertEqual((self.live / "lib/libds_enhancer.so").read_bytes(), b"new library")
        self.assertEqual((self.live / "python/ds_enhancer.py").read_bytes(), b"# new wrapper\n")
        self.assertFalse((self.live / "bin/old-program").exists())
        self.assertEqual((self.source / "checkout-marker").read_bytes(), b"unchanged checkout")
        backup_index = next(i for i, (cmd, _) in enumerate(self.commands) if "backup" in cmd)
        diagnostic_indices = [i for i, (cmd, _) in enumerate(self.commands)
                              if any(value.endswith("diagnostics.py") for value in cmd)]
        self.assertEqual(len(diagnostic_indices), 3)
        self.assertTrue(all(index < backup_index for index in diagnostic_indices[:2]))
        self.assertGreater(diagnostic_indices[-1], backup_index)
        self.assertTrue(any(line.startswith("SUCCESS:") for line in self.logs))
        self.assertFalse(any("simulat" in line.lower() for line in self.logs))

    def test_valid_update_preserves_operational_configuration_and_digest(self):
        self.write_file(self.live / "config/site-settings.json", b'{"site": "current-operational-settings"}')
        configuration_before = {path.name: path.read_bytes() for path in (self.live / "config").iterdir()}
        # The payload's status differs from the active installation and must
        # never replace vehicle evidence or the operator's trusted source pin.
        payload_status = json.loads(self.files["config/vehicle_status.json"])
        payload_status["battery_soh"] = 99.0
        self.files["config/vehicle_status.json"] = json.dumps(payload_status).encode()
        self.set_payload(archive_bytes(self.files))
        configuration_before["update_source.json"] = (self.live / "config/update_source.json").read_bytes()
        self.assertEqual(self.run_update(), 0, self.logs)
        configuration_after = {path.name: path.read_bytes() for path in (self.live / "config").iterdir()}
        self.assertEqual(configuration_after, configuration_before)
        diagnostics_commands = [(command, cwd) for command, cwd in self.commands
                                if any(value.endswith("diagnostics.py") for value in command)]
        self.assertEqual(len(diagnostics_commands), 3)
        for command, cwd in diagnostics_commands:
            self.assertIn("--updater-evidence", command)
            self.assertIn("--evidence-nonce", command)
            self.assertIn("--config-dir", command)
            configuration = Path(command[command.index("--config-dir") + 1])
            self.assertEqual(configuration, Path(cwd))

    def test_installed_tool_layout_can_perform_verified_update(self):
        self.write_file(self.live / "bin/update.py", (SCRIPTS / "update.py").read_bytes())
        self.assertEqual(self.run_update(source_dir=str(self.live)), 0, self.logs)
        self.assertEqual((self.live / "bin/ds_enhancer").read_bytes(), b"new binary")


class DiagnosticEvidenceTests(TemporaryUpdaterCase):
    def test_successful_legacy_diagnostics_without_receipt_require_migration(self):
        def legacy_runner(command, cwd=None, env=None):
            if any(value.endswith("diagnostics.py") for value in command):
                return True, "All pre-flight diagnostics passed", ""
            return self.runner(command, cwd=cwd, env=env)

        message = self.assert_blocked("migrate legacy installed diagnostics", runner=legacy_runner)
        self.assertIn("missing or mismatched evidence receipt", message)
        self.opener.assert_not_called()

    def test_legacy_script_rejecting_contract_fails_before_download(self):
        self.write_file(self.live / "bin/diagnostics.py", b"import argparse\nargparse.ArgumentParser().parse_args()\n")

        def legacy_runner(command, cwd=None, env=None):
            if str(self.live / "bin/diagnostics.py") in command:
                return update.run_command(command, cwd=cwd, env=env)
            return self.runner(command, cwd=cwd, env=env)

        self.assert_blocked("migrate legacy installed diagnostics", runner=legacy_runner)
        self.opener.assert_not_called()

    def test_wrong_receipt_source_nonce_hash_and_duplicate_receipts_are_rejected(self):
        for change in ("source", "nonce", "digest", "malformed", "duplicate"):
            with self.subTest(change=change):
                def dishonest_runner(command, cwd=None, env=None):
                    ok, output, error = self.runner(command, cwd=cwd, env=env)
                    if not any(value.endswith("diagnostics.py") for value in command):
                        return ok, output, error
                    receipt = json.loads(output[len(update.DIAGNOSTIC_EVIDENCE_PREFIX):])
                    if change == "source":
                        receipt["config_dir"] = str(self.live / "config")
                    elif change == "nonce":
                        receipt["nonce"] = "0" * 32
                    elif change == "digest":
                        receipt["sha256"]["vehicle_status.json"] = "0" * 64
                    if change == "malformed":
                        output = update.DIAGNOSTIC_EVIDENCE_PREFIX + "invalid json"
                    else:
                        output = update.DIAGNOSTIC_EVIDENCE_PREFIX + json.dumps(receipt)
                        if change == "duplicate":
                            output += "\n" + output
                    return ok, output, error

                self.assert_blocked("current diagnostics failed", runner=dishonest_runner)
                self.opener.assert_not_called()

    def test_selected_evidence_changed_during_diagnostics_blocks_deployment(self):
        def changing_runner(command, cwd=None, env=None):
            result = self.runner(command, cwd=cwd, env=env)
            if any(value.endswith("diagnostics.py") for value in command):
                (Path(cwd) / "vehicle_status.json").write_bytes(b'{"changed": true}')
            return result

        self.assert_blocked("selected evidence changed", runner=changing_runner)
        self.opener.assert_not_called()

    def test_all_diagnostic_processes_receive_environment_without_ds_overrides(self):
        with mock.patch.dict(os.environ, {"DS_SOH": "99", "DS_MODEL": "healthy-fixture",
                                         "DS_CUSTOM_OVERRIDE": "ignore", "KEEP_DIAGNOSTIC_ENV": "keep"}):
            self.assertEqual(self.run_update(), 0, self.logs)
        self.assertEqual(len(self.diagnostic_environments), 3)
        for environment in self.diagnostic_environments:
            self.assertIsInstance(environment, dict)
            self.assertFalse(any(name.startswith("DS_") for name in environment))
            self.assertEqual(environment["KEEP_DIAGNOSTIC_ENV"], "keep")

    def test_current_real_diagnostics_reject_unsafe_installation_despite_environment_and_preset(self):
        self.write_file(self.live / "bin/diagnostics.py", (SCRIPTS / "diagnostics.py").read_bytes())
        unsafe = json.loads((self.live / "config/vehicle_status.json").read_text())
        unsafe["simulated_presets"] = {unsafe["model"]: dict(unsafe, battery_soh=99.0)}
        unsafe["battery_soh"] = 20.0
        self.write_file(self.live / "config/vehicle_status.json", json.dumps(unsafe).encode())

        def actual_diagnostic_runner(command, cwd=None, env=None):
            if str(self.live / "bin/diagnostics.py") in command:
                return update.run_command(command, cwd=cwd, env=env)
            return self.runner(command, cwd=cwd, env=env)

        with mock.patch.dict(os.environ, {"DS_SOH": "99", "DS_TEMP_C": "25", "DS_VOLTAGE_V": "355"}):
            message = self.assert_blocked("current diagnostics failed", runner=actual_diagnostic_runner)
        self.assertIn("20.0%", message)
        self.opener.assert_not_called()

    def test_staged_missing_receipt_blocks_before_backup(self):
        def legacy_stage_runner(command, cwd=None, env=None):
            result = self.runner(command, cwd=cwd, env=env)
            if any(value.endswith("/scripts/diagnostics.py") for value in command):
                return True, "legacy staged success", ""
            return result

        self.assert_blocked("staged diagnostics failed", runner=legacy_stage_runner)

    def test_deployed_missing_receipt_restores_verified_snapshot_and_returns_failure(self):
        diagnostic_calls = 0

        def legacy_deployed_runner(command, cwd=None, env=None):
            nonlocal diagnostic_calls
            result = self.runner(command, cwd=cwd, env=env)
            if any(value.endswith("diagnostics.py") for value in command):
                diagnostic_calls += 1
                if diagnostic_calls == 3:
                    return True, "legacy deployed success", ""
            return result

        before = self.snapshot()
        self.assertNotEqual(self.run_update(runner=legacy_deployed_runner), 0)
        self.assertEqual(self.snapshot(), before)
        self.assertTrue(any(line.startswith("RESTORATION VERIFIED:") for line in self.logs))
        self.assertFalse(any(line.startswith("SUCCESS:") for line in self.logs))


class BackupVerificationTests(TemporaryUpdaterCase):
    def test_updater_rejects_writable_existing_backup_storage_without_deployment(self):
        backups = self.live / "backups"
        backups.mkdir()
        backups.chmod(0o777)
        before = self.snapshot()
        self.assertNotEqual(self.run_update(), 0)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(list(backups.iterdir()), [])
        self.assertIn("writable", "\n".join(self.logs))
        self.assertFalse(any(line.startswith("SUCCESS:") for line in self.logs))
        self.assertFalse(any("restore" in command for command, _ in self.commands))

    def test_updater_rechecks_backup_permissions_after_backup_command(self):
        def unprotected_backup_runner(command, cwd=None, env=None):
            result = self.runner(command, cwd=cwd, env=env)
            if "backup" in command:
                (self.live / "backups").chmod(0o777)
            return result

        before = self.snapshot()
        self.assertNotEqual(self.run_update(runner=unprotected_backup_runner), 0)
        self.assertEqual(self.snapshot(), before)
        self.assertIn("writable", "\n".join(self.logs))
        self.assertFalse(any(line.startswith("SUCCESS:") for line in self.logs))
        self.assertFalse(any("restore" in command for command, _ in self.commands))

    def test_corrupt_snapshot_is_rejected_before_restore_mutates_installation(self):
        import backup_restore
        snapshot = backup_restore.create_backup(self.live, self.live / "backups", "verified")
        self.write_file(snapshot / "payload/bin/old-program", b"tampered backup")
        before = self.snapshot()
        with self.assertRaises(backup_restore.BackupError):
            backup_restore.restore_backup(self.live, snapshot)
        self.assertEqual(self.snapshot(), before)

    def test_snapshot_must_match_original_installation_manifest(self):
        import backup_restore
        original = backup_restore.tree_manifest(self.live)
        snapshot = backup_restore.create_backup(self.live, self.live / "backups", "verified")
        # Both bytes and record have changed: internal consistency alone is
        # insufficient evidence that this is the pre-update installation.
        changed = dict(original)
        changed.pop("bin/old-program")
        with self.assertRaises(backup_restore.BackupError):
            backup_restore.verify_backup(snapshot, changed)

    def test_backup_rejects_installed_symlinks(self):
        import backup_restore
        (self.live / "bin/link").symlink_to(self.root / "outside")
        with self.assertRaises(backup_restore.BackupError):
            backup_restore.create_backup(self.live, self.live / "backups", "unsafe")
        self.assertFalse((self.live / "backups/unsafe").exists())

    def test_failed_snapshot_copy_cleans_incomplete_backup(self):
        import backup_restore
        before = self.snapshot()
        with mock.patch.object(backup_restore.shutil, "copytree", side_effect=OSError("copy failed")):
            with self.assertRaises(OSError):
                backup_restore.create_backup(self.live, self.live / "backups", "failed")
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(list((self.live / "backups").iterdir()), [])

    def test_snapshot_top_level_regular_file_is_rejected_before_restoration(self):
        import backup_restore
        original = backup_restore.tree_manifest(self.live)
        snapshot = backup_restore.create_backup(self.live, self.live / "backups", "verified")
        shutil.rmtree(snapshot / "payload/bin")
        replacement = b"a file cannot stand in for the bin directory"
        self.write_file(snapshot / "payload/bin", replacement)
        entry = dict(original["bin/old-program"])
        entry.update(size=len(replacement), sha256=hashlib.sha256(replacement).hexdigest())
        (snapshot / "manifest.json").write_text(
            json.dumps({"format": 1, "files": {"bin": entry}}), encoding="utf-8")
        before = self.snapshot()
        with self.assertRaises(backup_restore.BackupError):
            backup_restore.restore_backup(self.live, snapshot)
        self.assertEqual(self.snapshot(), before)

    def test_backup_directory_cannot_be_nested_in_managed_software(self):
        import backup_restore
        before = self.snapshot()
        with self.assertRaises(backup_restore.BackupError):
            backup_restore.create_backup(self.live, self.live / "bin/nested-backups", "unsafe")
        self.assertEqual(self.snapshot(), before)
        self.assertFalse((self.live / "bin/nested-backups").exists())

    def test_backup_directory_rejects_symlink_parent(self):
        import backup_restore
        external = self.root / "external"
        external.mkdir()
        alias = self.root / "alias"
        alias.symlink_to(external, target_is_directory=True)
        with self.assertRaises(backup_restore.BackupError):
            backup_restore.create_backup(self.live, alias / "backups", "unsafe")
        self.assertFalse((external / "backups").exists())

    def test_restoration_removes_managed_directories_absent_from_snapshot(self):
        import backup_restore
        shutil.rmtree(self.live / "docs")
        before = self.snapshot()
        snapshot = backup_restore.create_backup(self.live, self.live / "backups", "verified")
        self.write_file(self.live / "docs/new.md", b"new release documentation")
        self.write_file(self.live / "bin/new-program", b"new release program")
        self.assertTrue(backup_restore.restore_backup(self.live, snapshot))
        self.assertEqual(self.snapshot(), before)
        self.assertFalse((self.live / "docs").exists())


class DiagnosticLoggingTests(unittest.TestCase):
    def test_explicit_diagnostic_log_directory_is_exclusive(self):
        import diagnostics
        with tempfile.TemporaryDirectory(prefix="ds-diagnostic-log-test-") as directory:
            expected = str(Path(directory) / "install.log")
            with mock.patch.object(sys, "argv", [str(SCRIPTS / "diagnostics.py"), "--log-dir", directory]), \
                    mock.patch.object(diagnostics, "log_message", wraps=diagnostics.log_message) as logger, \
                    mock.patch.object(diagnostics.platform, "system", return_value="Linux"), \
                    mock.patch("builtins.print"):
                with self.assertRaises(SystemExit) as result:
                    diagnostics.main()
            self.assertEqual(result.exception.code, 0)
            self.assertGreater(logger.call_count, 0)
            for call in logger.call_args_list:
                self.assertEqual(call.args[1], [expected])
            self.assertTrue(Path(expected).is_file())

    def test_authoritative_config_rejects_unsafe_status_despite_healthy_tmp_fixture(self):
        import diagnostics
        with tempfile.TemporaryDirectory(prefix="ds-diagnostic-context-test-") as directory:
            context = Path(directory) / "current-config"
            context.mkdir()
            for name in ("install_policy.json", "vehicle_profiles.json", "safety_thresholds.json"):
                (context / name).write_bytes((ROOT / "config" / name).read_bytes())
            unsafe = {
                "model": "Tesla_Model_3_LR", "firmware_version": "v2024.12.15",
                "battery_soh": 20.0, "temperature_c": 25.0, "voltage_v": 355.0, "soc": 65.0,
            }
            healthy = dict(unsafe, battery_soh=99.0)
            unsafe["simulated_presets"] = {unsafe["model"]: healthy}
            (context / "vehicle_status.json").write_text(json.dumps(unsafe), encoding="utf-8")
            original_load = diagnostics.load_json
            original_exists = diagnostics.os.path.exists
            original_read = Path.read_bytes

            def load(path):
                if str(path) == "/tmp/vehicle_status.json":
                    return healthy
                return original_load(path)

            def exists(path):
                return str(path) == "/tmp/vehicle_status.json" or original_exists(path)

            arguments = [str(SCRIPTS / "diagnostics.py"), "--updater-evidence", "--config-dir", str(context),
                         "--evidence-nonce", "1" * 32, "--log-dir", str(Path(directory) / "logs")]
            with mock.patch.object(sys, "argv", arguments), \
                    mock.patch.object(diagnostics, "load_json", side_effect=load) as loader, \
                    mock.patch.object(diagnostics.os.path, "exists", side_effect=exists), \
                    mock.patch.dict(diagnostics.os.environ, {"DS_SOH": "99", "DS_MODEL": unsafe["model"],
                                                            "DS_FIRMWARE": unsafe["firmware_version"]}), \
                    mock.patch.object(Path, "read_bytes", autospec=True, side_effect=original_read) as reader, \
                    mock.patch.object(diagnostics.platform, "system", return_value="Linux"), \
                    mock.patch("builtins.print"):
                with self.assertRaises(SystemExit) as result:
                    diagnostics.main()
            self.assertEqual(result.exception.code, 4)
            self.assertTrue(any(call.args[0] == context / "vehicle_status.json" for call in reader.call_args_list))
            self.assertFalse(any(str(call.args[0]) == "/tmp/vehicle_status.json"
                                 for call in loader.call_args_list))

    def test_strict_diagnostics_emit_receipt_for_exact_selected_source(self):
        import diagnostics
        with tempfile.TemporaryDirectory(prefix="ds-evidence-success-") as directory:
            context = Path(directory) / "config"
            context.mkdir(mode=0o700)
            for name in update.DIAGNOSTIC_EVIDENCE_FILES:
                (context / name).write_bytes((ROOT / "config" / name).read_bytes())
            nonce = "2" * 32
            arguments = [str(SCRIPTS / "diagnostics.py"), "--updater-evidence", "--config-dir", str(context),
                         "--evidence-nonce", nonce, "--log-dir", str(Path(directory) / "logs")]
            with mock.patch.object(sys, "argv", arguments), \
                    mock.patch.dict(os.environ, {"DS_SOH": "0", "DS_MODEL": "unknown-environment-model"}), \
                    mock.patch.object(diagnostics.platform, "system", return_value="Linux"), \
                    mock.patch("builtins.print") as printer:
                with self.assertRaises(SystemExit) as result:
                    diagnostics.main()
            self.assertEqual(result.exception.code, 0)
            receipts = [call.args[0] for call in printer.call_args_list
                        if call.args and str(call.args[0]).startswith(update.DIAGNOSTIC_EVIDENCE_PREFIX)]
            self.assertEqual(len(receipts), 1)
            self.assertEqual(json.loads(receipts[0][len(update.DIAGNOSTIC_EVIDENCE_PREFIX):]),
                             update._diagnostic_receipt(context, nonce))

    def test_strict_diagnostics_reject_measurement_cli_overrides(self):
        import diagnostics
        for option in ("--model", "--firmware", "--soh", "--temp", "--voltage", "--soc"):
            with self.subTest(option=option):
                arguments = [str(SCRIPTS / "diagnostics.py"), "--updater-evidence", "--config-dir", "/unused",
                             "--evidence-nonce", "1" * 32, "--log-dir", "/unused", option, "99"]
                with mock.patch.object(sys, "argv", arguments), mock.patch.object(sys, "stderr", io.StringIO()) as errors:
                    with self.assertRaises(SystemExit) as result:
                        diagnostics.main()
                self.assertEqual(result.exception.code, 2)
                self.assertIn("overrides are prohibited", errors.getvalue())

    def test_strict_evidence_rejects_writable_config_file_directory_and_parent(self):
        import diagnostics
        with tempfile.TemporaryDirectory(prefix="ds-evidence-permissions-") as directory:
            parent = Path(directory) / "parent"
            context = parent / "config"
            context.mkdir(parents=True, mode=0o700)
            evidence = context / "vehicle_status.json"
            evidence.write_bytes(b"{}")
            for path in (evidence, context, parent):
                original_mode = path.stat().st_mode & 0o7777
                for extra_write in (0o020, 0o002):
                    with self.subTest(path=path.name, extra_write=extra_write):
                        path.chmod(original_mode | extra_write)
                        try:
                            with self.assertRaisesRegex(ValueError, "writable"):
                                diagnostics._protected_evidence_path(evidence)
                        finally:
                            path.chmod(original_mode)

    def test_strict_evidence_rejects_symlink_source(self):
        import diagnostics
        with tempfile.TemporaryDirectory(prefix="ds-evidence-symlink-") as directory:
            target = Path(directory) / "status.json"
            target.write_bytes(b"{}")
            link = Path(directory) / "vehicle_status.json"
            link.symlink_to(target)
            with self.assertRaisesRegex(ValueError, "symlink"):
                diagnostics._protected_evidence_path(link)

    def test_root_evidence_contract_checks_ancestors_above_sticky_tmp(self):
        import diagnostics
        with tempfile.TemporaryDirectory(prefix="ds-evidence-root-contract-") as directory:
            evidence = Path(directory) / "vehicle_status.json"
            evidence.write_bytes(b"{}")
            original_stat = Path.stat

            def root_owned_stat(path, *args, **kwargs):
                values = list(original_stat(path, *args, **kwargs))
                values[4] = 12345 if path == Path("/") else 0
                return os.stat_result(values)

            with mock.patch.object(diagnostics.os, "geteuid", return_value=0), \
                    mock.patch.object(Path, "stat", root_owned_stat):
                with self.assertRaisesRegex(ValueError, "unauthorized account"):
                    diagnostics._protected_evidence_path(evidence)


if __name__ == "__main__":
    unittest.main()
