#!/usr/bin/env python3
"""Offline, check-only gates for the explicit v8.0.2 maintenance runbook.

Authenticate this checker before executing it with python3 -I -B. It never
installs, downloads, repairs metadata, creates a backup, or restores software.
Only verified source modules are loaded; installed legacy code is never run.
"""
import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import tempfile
import types

HELPERS = ("update.py", "diagnostics.py", "backup_restore.py", "rollback.sh")
SOURCE_FILES = tuple("scripts/" + name for name in HELPERS + ("legacy_migration.py",))


class MigrationError(RuntimeError):
    pass


def _sha256(data):
    return hashlib.sha256(data).hexdigest()


def _pin(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", value) or value == "0" * 64:
        raise MigrationError("Missing or invalid independently recorded SHA-256 pin")
    return value.lower()


def _protected(path, owner, directory=False):
    path = Path(path).absolute()
    if path.resolve() != path:
        raise MigrationError("Symlink or noncanonical path: " + str(path))
    info = path.lstat()
    expected = stat.S_ISDIR if directory else stat.S_ISREG
    if not expected(info.st_mode) or (not directory and info.st_nlink != 1):
        raise MigrationError("Unsafe type or hard-linked file: " + str(path))
    if info.st_uid != owner or info.st_mode & 0o022:
        raise MigrationError("Unsafe ownership or permissions: " + str(path))
    child = path
    trusted_system_owner = 0
    for parent in path.parents:
        info = parent.lstat()
        if not stat.S_ISDIR(info.st_mode):
            raise MigrationError("Unsafe parent: " + str(parent))
        if parent in (Path("/tmp"), Path("/var/tmp")) and info.st_mode & stat.S_ISVTX:
            anchor = child.lstat()
            system_owner = Path("/").lstat().st_uid if owner != 0 else 0
            if info.st_uid in (0, system_owner) and anchor.st_uid == owner and stat.S_ISDIR(anchor.st_mode) and not anchor.st_mode & 0o077:
                trusted_system_owner = system_owner
                child = parent
                continue
        if info.st_uid not in (0, owner, trusted_system_owner) or info.st_mode & 0o022:
            raise MigrationError("Unsafe parent ownership or permissions: " + str(parent))
        child = parent
    return path


def verified_source(source, manifest, manifest_sha256, commit, owner):
    """Verify every byte before compiling/loading either trusted Python helper."""
    source = _protected(source, owner, directory=True)
    data = _protected(manifest, owner).read_bytes()
    if not hmac.compare_digest(_sha256(data), _pin(manifest_sha256)):
        raise MigrationError("Source manifest SHA-256 mismatch")
    record = json.loads(data)
    if (not isinstance(record, dict) or record.get("format") != "ds-ev-maintenance-v1"
            or record.get("version") != "8.0.2" or not re.fullmatch(r"[0-9a-f]{40}", commit)
            or record.get("commit") != commit or not isinstance(record.get("files"), dict)
            or set(record["files"]) != set(SOURCE_FILES)):
        raise MigrationError("Source identity/manifest does not describe the exact v8.0.2 maintenance set")
    contents = {}
    for name in SOURCE_FILES:
        contents[name] = _protected(source / name, owner).read_bytes()
        if not hmac.compare_digest(_sha256(contents[name]), _pin(record["files"][name])):
            raise MigrationError("Trusted source hash mismatch: " + name)
    if Path(__file__).read_bytes() != contents["scripts/legacy_migration.py"]:
        raise MigrationError("Running checker differs from authenticated source")
    return source, contents


def _modules(source, contents):
    # Compile verified bytes, never a cached .pyc or an installed legacy module.
    def load(name, filename):
        module = types.ModuleType(name)
        module.__file__ = str(source / "scripts" / filename)
        exec(compile(contents["scripts/" + filename], module.__file__, "exec"), module.__dict__)
        return module

    backup = load("ds_migration_backup", "backup_restore.py")
    previous = sys.modules.get("backup_restore")
    search = sys.path[:]
    try:
        sys.modules["backup_restore"] = backup
        updater = load("ds_migration_update", "update.py")
    finally:
        sys.path[:] = search
        if previous is None:
            sys.modules.pop("backup_restore", None)
        else:
            sys.modules["backup_restore"] = previous
    return backup, updater


def _storage_inventory(install, owner, backup):
    storage = install / "backups"
    legacy = []
    if not os.path.lexists(storage):
        return legacy
    _protected(storage, owner, directory=True)
    for path in sorted(storage.rglob("*")):
        _protected(path, owner, directory=path.is_dir())
    for path in sorted(storage.iterdir()):
        if path.name.startswith(".incomplete-"):
            raise MigrationError("Interrupted backup requires manual investigation: " + str(path))
        if path.is_dir() and (path / "manifest.json").exists():
            backup.verify_backup(path, required_owner=owner)
        else:
            legacy.append(path.name)
    return legacy


def _recovery_snapshot(install, source, snapshot, pin, owner, backup):
    if snapshot is None:
        raise MigrationError("An explicitly selected, pinned recovery snapshot is required")
    snapshot = _protected(snapshot, owner, directory=True)
    backup._safe_backup_location(install, snapshot)
    if source == snapshot or source in snapshot.parents or snapshot in source.parents:
        raise MigrationError("Recovery snapshot must be separate from trusted source")
    data = _protected(snapshot / "manifest.json", owner).read_bytes()
    if not hmac.compare_digest(_sha256(data), _pin(pin)):
        raise MigrationError("Recovery manifest SHA-256 mismatch; stop and recover manually")
    baseline = backup.verify_backup(snapshot, required_owner=owner)
    for name in HELPERS:
        entry = baseline.get("bin/" + name)
        if entry is not None and entry.get("type") != "file":
            raise MigrationError("Recovery helper must be an original regular file or absent: " + name)
    return snapshot, baseline


def check(mode, source, manifest, manifest_sha256, commit, install, snapshot=None,
          snapshot_sha256=None, *, owner=0, group=0, runner=None, which=None):
    """Test seam permits a fixture owner; the administrative CLI always uses 0."""
    if mode not in ("inspect", "before", "verify", "recovery"):
        raise MigrationError("Unknown check mode")
    source, contents = verified_source(source, manifest, manifest_sha256, commit, owner)
    backup, updater = _modules(source, contents)
    install = _protected(install, owner, directory=True)
    _protected(install.parent, owner, directory=True)
    if owner == 0:
        # Unlike recovery storage, the updater's active installation/config
        # contract has no sticky temporary-directory ancestry exception.
        updater._trusted_owned_path(install, "Active installation")
    lock = install.parent / ("." + install.name + ".update.lock")
    if os.path.lexists(lock):
        _protected(lock, owner)
    if source == install or install in source.parents or source in install.parents:
        raise MigrationError("Trusted source must be outside the active installation")
    if mode == "recovery":
        snapshot, baseline = _recovery_snapshot(install, source, snapshot, snapshot_sha256, owner, backup)
        verified_source(source, manifest, manifest_sha256, commit, owner)
        _recovery_snapshot(install, source, snapshot, snapshot_sha256, owner, backup)
        return {"status": "RECOVERY SNAPSHOT VERIFIED; NO MIGRATION SUCCESS",
                "source_commit": commit, "recovery_manifest_sha256": _pin(snapshot_sha256),
                "original_helpers": {name: baseline.get("bin/" + name) for name in HELPERS}}
    for name in HELPERS:
        target = install / "bin" / name
        if os.path.lexists(target):
            _protected(target, owner)
    original = backup.tree_manifest(install)
    for name, entry in original.items():
        _protected(install / name, owner, directory=entry["type"] == "directory")
        if entry["mode"] & 0o7000:
            raise MigrationError("Privileged software mode requires manual investigation: " + name)
    for name in backup.MANAGED_DIRS:
        _protected(install / name, owner, directory=True)
    _protected(install / "bin/diagnostics.py", owner)
    updater._validate_configs(install)
    updater.load_policy(install / "config/update_source.json")
    which = which or shutil.which
    for name in ("cmake", "ctest", "bash", "python3"):
        if not which(name):
            raise MigrationError("Missing updater prerequisite command: " + name)
    if not any(which(name) for name in ("c++", "g++", "clang++")):
        raise MigrationError("Missing updater prerequisite C++ compiler")
    legacy = _storage_inventory(install, owner, backup)
    expected = None
    if mode != "inspect":
        snapshot, expected = _recovery_snapshot(install, source, snapshot, snapshot_sha256, owner, backup)
        if mode == "before":
            if expected != original:
                raise MigrationError("Recovery snapshot differs from live software; interrupted or changed installation")
        else:
            expected = dict(expected)
            for name in HELPERS:
                data = contents["scripts/" + name]
                expected["bin/" + name] = {"type": "file", "mode": 0o755, "uid": owner,
                                          "gid": group, "size": len(data), "sha256": _sha256(data)}
            if expected != original:
                raise MigrationError("Partial/unexpected migration or changed configuration; no migration success; recover selected snapshot")
    diagnostic = install / "bin/diagnostics.py" if mode == "verify" else source / "scripts/diagnostics.py"
    base_runner = runner or updater.run_command

    def isolated_runner(command, **options):
        # Keep the existing nonce/receipt/hash gate; isolate only Python imports.
        return base_runner([command[0], "-I", "-B"] + command[1:], **options)

    with tempfile.TemporaryDirectory(prefix="ds-maintenance-check-") as temporary:
        updater._check_diagnostics(isolated_runner, diagnostic, install / "config",
                                   Path(temporary) / "logs", "Maintenance evidence", lambda _: None)
    # Recheck source, storage, snapshot and live bytes after all subprocess work.
    verified_source(source, manifest, manifest_sha256, commit, owner)
    _storage_inventory(install, owner, backup)
    if mode != "inspect":
        backup.verify_backup(snapshot, required_owner=owner)
        if _sha256((snapshot / "manifest.json").read_bytes()) != _pin(snapshot_sha256):
            raise MigrationError("Recovery evidence changed during checks")
    if backup.tree_manifest(install) != original:
        raise MigrationError("Installation changed during checks; no migration success")
    matches = [name for name in HELPERS if original.get("bin/" + name, {}).get("sha256") == _sha256(contents["scripts/" + name])]
    return {"status": "MIGRATION VERIFIED" if mode == "verify" else "CHECKS PASSED; NO MIGRATION PERFORMED",
            "source_commit": commit, "matching_helpers": matches,
            "unverified_legacy_backups": legacy,
            "configuration_sha256": {name: original["config/" + name]["sha256"] for name in updater.REQUIRED_CONFIGS},
            "recovery_manifest_sha256": snapshot_sha256 if mode != "inspect" else None,
            "scope": "offline maintenance prerequisites only; normal remote updater gates remain required"}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("inspect", "before", "verify", "recovery"))
    parser.add_argument("--source", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--manifest-sha256", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--install-dir", required=True)
    parser.add_argument("--snapshot")
    parser.add_argument("--snapshot-sha256")
    args = parser.parse_args(argv)
    if os.geteuid() != 0:
        parser.error("Explicit administrator execution as root is required")
    try:
        result = check(args.mode, args.source, args.manifest, args.manifest_sha256,
                       args.commit, args.install_dir, args.snapshot, args.snapshot_sha256)
        print(json.dumps(result, sort_keys=True))
        return 0
    except Exception as exc:
        # A failed step never turns into readiness or migration success.
        print("BLOCKED: " + str(exc) + "; see docs/UPDATER_LEGACY_MIGRATION.md", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
