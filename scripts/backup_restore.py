#!/usr/bin/env python3
"""Verified software snapshots shared by the updater and rollback command.

Only deployed software directories are managed; logs/backups and vehicle state
outside these directories are never replaced. Restoration is recoverable file
copying, not an atomic transaction or protection against power loss.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import sys
import uuid

MANAGED_DIRS = ("bin", "lib", "python", "python_fallback", "config", "docs")


class BackupError(RuntimeError):
    pass


def _canonical(path, description):
    path = Path(path).absolute()
    if path.resolve() != path:
        raise BackupError(description + " contains a symlink or noncanonical component")
    return path


def _safe_backup_location(install, location):
    location = _canonical(location, "Backup location")
    if location == install or location in install.parents:
        raise BackupError("Backup must be separate from the live software tree")
    if install in location.parents:
        relative = location.relative_to(install)
        if not relative.parts or relative.parts[0] != "backups":
            raise BackupError("Backups inside the installation must stay under backups/")
    return location


def _digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def tree_manifest(root):
    """Record exact software paths, file bytes and permissions; reject links."""
    root = _canonical(root, "Software root")
    if root.is_symlink() or not root.is_dir():
        raise BackupError("Installation/snapshot root is missing or is a link")
    result = {}
    for name in MANAGED_DIRS:
        start = root / name
        if not os.path.lexists(start):
            continue
        if start.is_symlink() or not start.is_dir():
            raise BackupError("Managed software path is not a safe directory: " + name)
        for path in [start] + sorted(start.rglob("*")):
            info = path.lstat()
            relative = path.relative_to(root).as_posix()
            mode = stat.S_IMODE(info.st_mode)
            if stat.S_ISDIR(info.st_mode):
                result[relative] = {"type": "directory", "mode": mode, "uid": info.st_uid, "gid": info.st_gid}
            elif stat.S_ISREG(info.st_mode):
                result[relative] = {"type": "file", "mode": mode, "uid": info.st_uid, "gid": info.st_gid,
                                    "size": info.st_size, "sha256": _digest(path)}
            else:
                raise BackupError("Unsafe link or special file in software tree: " + relative)
    return result


def _copytree_preserving_ownership(source, target):
    shutil.copytree(source, target, symlinks=True)
    # copy2 preserves bytes/timestamps/mode, but ownership needs explicit care.
    for original in [source] + sorted(source.rglob("*")):
        copied = target / original.relative_to(source)
        before, after = original.lstat(), copied.lstat()
        if (before.st_uid, before.st_gid) != (after.st_uid, after.st_gid):
            os.chown(copied, before.st_uid, before.st_gid, follow_symlinks=False)


def verify_backup(snapshot_path, expected_manifest=None):
    snapshot = _canonical(snapshot_path, "Snapshot")
    if snapshot.is_symlink() or not snapshot.is_dir():
        raise BackupError("Backup snapshot is missing or is a link")
    manifest_path = snapshot / "manifest.json"
    if manifest_path.is_symlink() or not manifest_path.is_file():
        raise BackupError("Backup manifest is missing or unsafe")
    try:
        record = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise BackupError("Invalid backup manifest: " + str(exc)) from exc
    if (not isinstance(record, dict) or record.get("format") != 1
            or not isinstance(record.get("files"), dict) or not record["files"]):
        raise BackupError("Backup manifest is empty or unsupported")
    actual = tree_manifest(snapshot / "payload")
    if actual != record["files"]:
        raise BackupError("Backup contents do not match their SHA-256 manifest")
    if expected_manifest is not None and actual != expected_manifest:
        raise BackupError("Backup does not match the pre-update installation")
    return actual


def create_backup(install_dir, backup_dir, snapshot_id):
    install = _canonical(install_dir, "Installation")
    destination = _safe_backup_location(install, backup_dir)
    if (not snapshot_id or Path(snapshot_id).name != snapshot_id
            or snapshot_id in (".", "..")):
        raise BackupError("Invalid backup snapshot identifier")
    original = tree_manifest(install)
    if not original:
        raise BackupError("No installed software exists to back up")
    if destination.is_symlink():
        raise BackupError("Backup directory must not be a symlink")
    destination.mkdir(parents=True, exist_ok=True, mode=0o700)
    snapshot = destination / snapshot_id
    if os.path.lexists(snapshot):
        raise BackupError("Backup snapshot already exists")
    temporary = destination / (".incomplete-" + uuid.uuid4().hex)
    temporary.mkdir(mode=0o700)
    try:
        payload = temporary / "payload"
        payload.mkdir(mode=0o700)
        for name in MANAGED_DIRS:
            source = install / name
            if source.exists():
                if not source.is_dir():
                    raise BackupError("Installed software directory is not a directory: " + name)
                _copytree_preserving_ownership(source, payload / name)
        record = {"format": 1, "files": original}
        (temporary / "manifest.json").write_text(json.dumps(record, sort_keys=True), encoding="utf-8")
        verify_backup(temporary, original)
        if tree_manifest(install) != original:
            raise BackupError("Installation changed while backup was being created")
        temporary.rename(snapshot)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    return snapshot


def restore_backup(install_dir, snapshot_path):
    install = _canonical(install_dir, "Installation")
    snapshot_path = _safe_backup_location(install, snapshot_path)
    expected = verify_backup(snapshot_path)
    if install.is_symlink() or not install.is_dir():
        raise BackupError("Installation directory is missing or unsafe")
    # Snapshot verification precedes every destructive operation. A snapshot
    # records absence too, so newly introduced directories are removed.
    payload = Path(snapshot_path) / "payload"
    for name in MANAGED_DIRS:
        target = install / name
        if os.path.lexists(target):
            if target.is_symlink() or not target.is_dir():
                target.unlink()
            else:
                shutil.rmtree(target)
        if (payload / name).exists():
            _copytree_preserving_ownership(payload / name, target)
    if tree_manifest(install) != expected:
        raise BackupError("Restoration failed verification against the original snapshot")
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("backup", "restore", "rollback"))
    parser.add_argument("--install-dir", default="/opt/ds_enhancement")
    parser.add_argument("--backup-dir")
    parser.add_argument("--snapshot-id")
    args = parser.parse_args(argv)
    if os.geteuid() != 0:
        parser.error("Backup/rollback requires root privileges")
    install = Path(args.install_dir).absolute()
    backups = Path(args.backup_dir).absolute() if args.backup_dir else install / "backups"
    try:
        if args.mode == "backup":
            snapshot_id = args.snapshot_id or uuid.uuid4().hex
            snapshot = create_backup(install, backups, snapshot_id)
            print("Backup verified: " + str(snapshot))
        else:
            if args.snapshot_id:
                if Path(args.snapshot_id).name != args.snapshot_id or args.snapshot_id in (".", ".."):
                    raise BackupError("Invalid backup snapshot identifier")
                snapshot = backups / args.snapshot_id
            else:
                if backups.is_symlink() or not backups.is_dir():
                    raise BackupError("No backup directory is available")
                candidates = [path for path in backups.iterdir()
                              if path.is_dir() and not path.is_symlink() and not path.name.startswith(".")]
                if not candidates:
                    raise BackupError("No verified-format backups are available")
                snapshot = max(candidates, key=lambda path: path.stat().st_mtime_ns)
            restore_backup(install, snapshot)
            print("Restoration verified: " + str(snapshot))
        return 0
    except (BackupError, OSError, ValueError) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
