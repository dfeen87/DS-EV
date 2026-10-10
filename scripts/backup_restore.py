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


def _storage_owner(required_owner):
    owner = os.geteuid() if required_owner is None else required_owner
    if isinstance(owner, bool) or not isinstance(owner, int) or owner < 0:
        raise BackupError("Invalid required backup-storage owner")
    return owner


def _protected_entry(path, owner, *, directory=None):
    """Reject storage that a different account can write; never repair it."""
    info = path.lstat()
    if directory is True and not stat.S_ISDIR(info.st_mode):
        raise BackupError("Backup storage is not a safe directory: " + str(path))
    if directory is False and not stat.S_ISREG(info.st_mode):
        raise BackupError("Backup storage is not a safe regular file: " + str(path))
    if directory is None and not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
        raise BackupError("Backup storage contains a link or special file: " + str(path))
    if info.st_uid != owner:
        raise BackupError("Backup storage has an unauthorized owner: " + str(path))
    if stat.S_IMODE(info.st_mode) & 0o022:
        raise BackupError("Backup storage is writable by group or other accounts: " + str(path))
    return info


def _protected_parents(path, owner):
    """Prevent substitution of storage through a writable ancestor.

    A nonprivileged helper may use a private, owner-only temporary anchor below
    the system's sticky /tmp or /var/tmp. The production root contract never
    accepts a non-root owner, including at this temporary-directory boundary.
    """
    child = path
    trusted_system_owner = 0
    for parent in path.parents:
        info = parent.lstat()
        if not stat.S_ISDIR(info.st_mode):
            raise BackupError("Backup parent is not a safe directory: " + str(parent))
        mode = stat.S_IMODE(info.st_mode)
        if parent in (Path("/tmp"), Path("/var/tmp")) and mode & stat.S_ISVTX:
            child_info = child.lstat()
            system_owner = Path("/").lstat().st_uid if owner != 0 else 0
            if (info.st_uid in (0, system_owner) and child_info.st_uid == owner
                    and stat.S_ISDIR(child_info.st_mode)
                    and not stat.S_IMODE(child_info.st_mode) & 0o077):
                # Sticky semantics protect this private child's name from
                # other users; ancestors above this boundary must also pass.
                trusted_system_owner = system_owner
                child = parent
                continue
        if info.st_uid not in (0, owner, trusted_system_owner):
            raise BackupError("Backup parent has an unauthorized owner: " + str(parent))
        if mode & 0o022:
            raise BackupError("Backup parent is writable by group or other accounts: " + str(parent))
        child = parent


def _protected_storage_path(path, owner):
    path = _canonical(path, "Backup storage")
    _protected_entry(path, owner, directory=True)
    _protected_parents(path, owner)
    return path


def _prepare_backup_directory(path, owner):
    """Validate existing ancestors before creating any missing directories."""
    missing = []
    existing = path
    while not os.path.lexists(existing):
        missing.append(existing)
        existing = existing.parent
    _protected_storage_path(existing, owner)
    for directory in reversed(missing):
        directory.mkdir(mode=0o700)
        _protected_storage_path(directory, owner)
    return _protected_storage_path(path, owner)


def _protected_snapshot(snapshot, owner):
    _protected_storage_path(snapshot, owner)
    for path in sorted(snapshot.rglob("*")):
        _protected_entry(path, owner)


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


def verify_backup(snapshot_path, expected_manifest=None, *, required_owner=None):
    owner = _storage_owner(required_owner)
    snapshot = _canonical(snapshot_path, "Snapshot")
    if snapshot.is_symlink() or not snapshot.is_dir():
        raise BackupError("Backup snapshot is missing or is a link")
    manifest_path = snapshot / "manifest.json"
    if manifest_path.is_symlink() or not manifest_path.is_file():
        raise BackupError("Backup manifest is missing or unsafe")
    _protected_snapshot(snapshot, owner)
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


def create_backup(install_dir, backup_dir, snapshot_id, *, required_owner=None):
    owner = _storage_owner(required_owner)
    install = _canonical(install_dir, "Installation")
    destination = _safe_backup_location(install, backup_dir)
    if (not snapshot_id or Path(snapshot_id).name != snapshot_id
            or snapshot_id in (".", "..")):
        raise BackupError("Invalid backup snapshot identifier")
    if os.path.lexists(destination):
        _protected_storage_path(destination, owner)
    original = tree_manifest(install)
    if not original:
        raise BackupError("No installed software exists to back up")
    # Copies retain these permissions/owners for exact restoration. Refuse an
    # installation whose metadata cannot produce protected snapshot storage.
    for name, entry in original.items():
        if entry["uid"] != owner:
            raise BackupError("Installed software has an owner incompatible with protected backup storage: " + name)
        if entry["mode"] & 0o022:
            raise BackupError("Installed software is writable by group or other accounts: " + name)
    if destination.is_symlink():
        raise BackupError("Backup directory must not be a symlink")
    _prepare_backup_directory(destination, owner)
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
        manifest_fd = os.open(temporary / "manifest.json", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(manifest_fd, "w", encoding="utf-8") as manifest:
            json.dump(record, manifest, sort_keys=True)
        verify_backup(temporary, original, required_owner=owner)
        if tree_manifest(install) != original:
            raise BackupError("Installation changed while backup was being created")
        temporary.rename(snapshot)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    return snapshot


def restore_backup(install_dir, snapshot_path, *, required_owner=None):
    owner = _storage_owner(required_owner)
    install = _canonical(install_dir, "Installation")
    snapshot_path = _safe_backup_location(install, snapshot_path)
    expected = verify_backup(snapshot_path, required_owner=owner)
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
            snapshot = create_backup(install, backups, snapshot_id, required_owner=0)
            print("Backup verified: " + str(snapshot))
        else:
            if args.snapshot_id:
                if Path(args.snapshot_id).name != args.snapshot_id or args.snapshot_id in (".", ".."):
                    raise BackupError("Invalid backup snapshot identifier")
                snapshot = backups / args.snapshot_id
            else:
                if backups.is_symlink() or not backups.is_dir():
                    raise BackupError("No backup directory is available")
                _protected_storage_path(backups, 0)
                candidates = [path for path in backups.iterdir()
                              if path.is_dir() and not path.is_symlink() and not path.name.startswith(".")]
                if not candidates:
                    raise BackupError("No verified-format backups are available")
                snapshot = max(candidates, key=lambda path: path.stat().st_mtime_ns)
            restore_backup(install, snapshot, required_owner=0)
            print("Restoration verified: " + str(snapshot))
        return 0
    except (BackupError, OSError, ValueError) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
