#!/usr/bin/env python3
"""Backup trust regressions using only unprivileged temporary directories."""

import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest import mock


SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import backup_restore


class BackupStoragePermissionsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ds-backup-permissions-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.install = self.root / "installation"
        (self.install / "bin").mkdir(parents=True)
        (self.install / "bin/program").write_bytes(b"original program")
        self.backups = self.install / "backups"

    def create(self):
        return backup_restore.create_backup(self.install, self.backups, "verified")

    @staticmethod
    def owner_override(path, owner):
        original_lstat = Path.lstat

        def replaced_lstat(current):
            result = original_lstat(current)
            if current == path:
                values = list(result)
                values[4] = owner
                return os.stat_result(values)
            return result

        return mock.patch.object(Path, "lstat", replaced_lstat)

    def assert_restore_rejected(self, snapshot, message):
        before = backup_restore.tree_manifest(self.install)
        with self.assertRaisesRegex(backup_restore.BackupError, message):
            backup_restore.restore_backup(self.install, snapshot)
        self.assertEqual(backup_restore.tree_manifest(self.install), before)

    def test_new_private_backup_storage_supports_exact_restore(self):
        before = backup_restore.tree_manifest(self.install)
        snapshot = self.create()
        self.assertEqual(stat.S_IMODE(self.backups.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(snapshot.stat().st_mode), 0o700)
        (self.install / "bin/program").write_bytes(b"replacement program")
        self.assertTrue(backup_restore.restore_backup(self.install, snapshot))
        self.assertEqual(backup_restore.tree_manifest(self.install), before)

    def test_existing_protected_storage_is_accepted(self):
        self.backups.mkdir(mode=0o755)
        self.backups.chmod(0o755)
        snapshot = self.create()
        self.assertEqual(backup_restore.verify_backup(snapshot), backup_restore.tree_manifest(self.install))
        self.assertEqual(stat.S_IMODE(self.backups.stat().st_mode), 0o755)

    def test_manifest_is_private_even_with_permissive_umask(self):
        previous_umask = os.umask(0)
        try:
            snapshot = self.create()
        finally:
            os.umask(previous_umask)
        self.assertEqual(stat.S_IMODE((snapshot / "manifest.json").stat().st_mode), 0o600)
        self.assertEqual(backup_restore.verify_backup(snapshot), backup_restore.tree_manifest(self.install))

    def test_existing_group_or_world_writable_storage_is_rejected_without_repair(self):
        self.backups.mkdir()
        marker = self.backups / "preserve-me"
        marker.write_bytes(b"existing user backup metadata")
        before = backup_restore.tree_manifest(self.install)
        for mode in (0o770, 0o707, 0o777, 0o1777):
            with self.subTest(mode=oct(mode)):
                self.backups.chmod(mode)
                with self.assertRaisesRegex(backup_restore.BackupError, "writable"):
                    self.create()
                self.assertEqual(stat.S_IMODE(self.backups.stat().st_mode), mode)
                self.assertEqual(marker.read_bytes(), b"existing user backup metadata")
                self.assertFalse((self.backups / "verified").exists())
                self.assertEqual(backup_restore.tree_manifest(self.install), before)

    def test_existing_storage_with_unauthorized_owner_is_rejected(self):
        self.backups.mkdir(mode=0o700)
        with self.owner_override(self.backups, os.geteuid() + 1):
            with self.assertRaisesRegex(backup_restore.BackupError, "unauthorized owner"):
                self.create()
        self.assertEqual(list(self.backups.iterdir()), [])

    def test_root_storage_contract_rejects_nonroot_owner(self):
        self.backups.mkdir(mode=0o700)
        with self.owner_override(self.backups, 12345):
            with self.assertRaisesRegex(backup_restore.BackupError, "unauthorized owner"):
                backup_restore.create_backup(self.install, self.backups, "verified", required_owner=0)
        self.assertEqual(list(self.backups.iterdir()), [])

    def test_group_or_world_writable_parent_blocks_creation(self):
        parent = self.root / "external"
        parent.mkdir()
        for mode in (0o770, 0o707, 0o1777):
            with self.subTest(mode=oct(mode)):
                parent.chmod(mode)
                with self.assertRaisesRegex(backup_restore.BackupError, "writable"):
                    backup_restore.create_backup(self.install, parent / "backups", "verified")
                self.assertFalse((parent / "backups").exists())

    def test_unauthorized_parent_owner_blocks_existing_storage(self):
        parent = self.root / "external"
        storage = parent / "backups"
        storage.mkdir(parents=True, mode=0o700)
        with self.owner_override(parent, os.geteuid() + 1):
            with self.assertRaisesRegex(backup_restore.BackupError, "parent.*unauthorized owner"):
                backup_restore.create_backup(self.install, storage, "verified")
        self.assertEqual(list(storage.iterdir()), [])

    def test_unprotected_anchor_below_sticky_tmp_is_rejected(self):
        original_mode = stat.S_IMODE(self.root.stat().st_mode)
        self.root.chmod(0o755)
        self.addCleanup(self.root.chmod, original_mode)
        with self.assertRaisesRegex(backup_restore.BackupError, "parent.*(writable|owner)"):
            self.create()
        self.assertFalse(self.backups.exists())

    def test_ancestors_above_private_sticky_anchor_are_checked(self):
        original_lstat = Path.lstat

        def replaced_lstat(current):
            result = original_lstat(current)
            if current == Path("/"):
                values = list(result)
                values[0] |= 0o002
                return os.stat_result(values)
            return result

        with mock.patch.object(Path, "lstat", replaced_lstat):
            with self.assertRaisesRegex(backup_restore.BackupError, "parent.*writable"):
                self.create()
        self.assertFalse(self.backups.exists())

    def test_root_contract_checks_ancestors_above_root_owned_sticky_anchor(self):
        original_lstat = Path.lstat
        for root_owner, extra_mode, message in ((12345, 0, "unauthorized owner"),
                                                (0, 0o002, "writable")):
            with self.subTest(root_owner=root_owner, extra_mode=oct(extra_mode)):
                def replaced_lstat(current):
                    values = list(original_lstat(current))
                    values[4] = root_owner if current == Path("/") else 0
                    if current == Path("/"):
                        values[0] |= extra_mode
                    return os.stat_result(values)

                with mock.patch.object(Path, "lstat", replaced_lstat):
                    with self.assertRaisesRegex(backup_restore.BackupError, "parent.*" + message):
                        backup_restore.create_backup(self.install, self.backups, "verified", required_owner=0)
                self.assertFalse(self.backups.exists())

    def test_existing_snapshot_parent_is_rechecked_before_restore(self):
        snapshot = self.create()
        self.backups.chmod(0o777)
        self.assert_restore_rejected(snapshot, "parent.*writable")
        self.assertEqual(stat.S_IMODE(self.backups.stat().st_mode), 0o777)

    def test_snapshot_parent_owner_is_rechecked_before_restore(self):
        snapshot = self.create()
        with self.owner_override(self.backups, os.geteuid() + 1):
            with self.assertRaisesRegex(backup_restore.BackupError, "parent.*unauthorized owner"):
                backup_restore.verify_backup(snapshot)
            self.assert_restore_rejected(snapshot, "parent.*unauthorized owner")

    def test_snapshot_manifest_and_payload_permissions_are_verified(self):
        snapshot = self.create()
        for relative in (".", "manifest.json", "payload", "payload/bin", "payload/bin/program"):
            path = snapshot / relative
            original_mode = stat.S_IMODE(path.stat().st_mode)
            for extra_write in (0o020, 0o002):
                with self.subTest(path=relative, extra_write=oct(extra_write)):
                    path.chmod(original_mode | extra_write)
                    try:
                        with self.assertRaisesRegex(backup_restore.BackupError, "writable"):
                            backup_restore.verify_backup(snapshot)
                        self.assert_restore_rejected(snapshot, "writable")
                    finally:
                        path.chmod(original_mode)

    def test_snapshot_manifest_and_payload_owners_are_verified(self):
        snapshot = self.create()
        for relative in (".", "manifest.json", "payload", "payload/bin", "payload/bin/program"):
            path = snapshot / relative
            with self.subTest(path=relative), self.owner_override(path, os.geteuid() + 1):
                with self.assertRaisesRegex(backup_restore.BackupError, "unauthorized owner"):
                    backup_restore.verify_backup(snapshot)
                self.assert_restore_rejected(snapshot, "unauthorized owner")

    def test_new_backup_rejects_writable_software_before_creating_storage(self):
        (self.install / "bin/program").chmod(0o666)
        before = backup_restore.tree_manifest(self.install)
        with self.assertRaisesRegex(backup_restore.BackupError, "writable"):
            self.create()
        self.assertEqual(backup_restore.tree_manifest(self.install), before)
        self.assertFalse(self.backups.exists())

    def test_new_backup_rejects_untrusted_software_owner_before_copy(self):
        with self.owner_override(self.install / "bin/program", os.geteuid() + 1), \
                mock.patch.object(backup_restore.shutil, "copytree") as copytree:
            with self.assertRaisesRegex(backup_restore.BackupError, "owner incompatible"):
                self.create()
        copytree.assert_not_called()
        self.assertFalse(self.backups.exists())

    def test_snapshot_link_is_rejected_before_live_mutation(self):
        snapshot = self.create()
        (snapshot / "unmanaged-link").symlink_to(self.root / "outside")
        self.assert_restore_rejected(snapshot, "link or special file")

    def test_snapshot_special_file_is_rejected_before_live_mutation(self):
        snapshot = self.create()
        os.mkfifo(snapshot / "unmanaged-fifo")
        self.assert_restore_rejected(snapshot, "link or special file")

    def test_cli_explicitly_requires_root_owned_create_storage(self):
        with mock.patch.object(backup_restore.os, "geteuid", return_value=0), \
                mock.patch.object(backup_restore, "create_backup", return_value=self.backups / "verified") as create, \
                mock.patch("builtins.print"):
            result = backup_restore.main(["backup", "--install-dir", str(self.install),
                                          "--backup-dir", str(self.backups), "--snapshot-id", "verified"])
        self.assertEqual(result, 0)
        create.assert_called_once_with(self.install, self.backups, "verified", required_owner=0)

    def test_cli_explicitly_requires_root_owned_restore_storage(self):
        with mock.patch.object(backup_restore.os, "geteuid", return_value=0), \
                mock.patch.object(backup_restore, "restore_backup", return_value=True) as restore, \
                mock.patch("builtins.print"):
            result = backup_restore.main(["restore", "--install-dir", str(self.install),
                                          "--backup-dir", str(self.backups), "--snapshot-id", "verified"])
        self.assertEqual(result, 0)
        restore.assert_called_once_with(self.install, self.backups / "verified", required_owner=0)

    def test_cli_checks_storage_before_enumerating_latest_snapshot(self):
        self.backups.mkdir(mode=0o700)
        with mock.patch.object(backup_restore.os, "geteuid", return_value=0), \
                self.owner_override(self.backups, 12345), \
                mock.patch.object(Path, "iterdir") as enumerate_storage, \
                mock.patch.object(backup_restore, "restore_backup") as restore, \
                mock.patch("builtins.print"):
            result = backup_restore.main(["rollback", "--install-dir", str(self.install),
                                          "--backup-dir", str(self.backups)])
        self.assertEqual(result, 1)
        enumerate_storage.assert_not_called()
        restore.assert_not_called()


if __name__ == "__main__":
    unittest.main()
