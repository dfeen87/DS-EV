#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Fail-closed DS-EV software updater.

An HTTPS release archive is accepted only with an independently provisioned,
trusted SHA-256 digest. Source, diagnostics, build and verified backup gates all
precede deployment. Recoverable copy failures attempt a verified restoration;
file copying is not atomic and cannot guarantee recovery after power loss.
"""
import gzip
import fcntl
import hashlib
import hmac
import io
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import socket
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

# Import the helper beside this running updater, never the downloaded copy.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import backup_restore

INSTALL_DIR = "/opt/ds_enhancement"
REQUIRED_CONFIGS = ("install_policy.json", "vehicle_status.json", "vehicle_profiles.json",
                    "safety_thresholds.json", "update_source.json")
SOURCE_FILES = ("CMakeLists.txt", "scripts/update.py", "scripts/diagnostics.py", "scripts/rollback.sh",
                "scripts/backup_restore.py", "ds_core/python/ds_enhancer.py",
                "ds_core/python_fallback/ds_enhancer_fallback.py")
MAX_DOWNLOAD_BYTES = 512 * 1024 * 1024
MAX_EXTRACTED_BYTES = 2 * 1024 * 1024 * 1024
DIAGNOSTIC_EVIDENCE_FILES = ("install_policy.json", "vehicle_status.json",
                             "vehicle_profiles.json", "safety_thresholds.json")
DIAGNOSTIC_EVIDENCE_PREFIX = "DS_UPDATER_EVIDENCE_V1:"


class UpdateError(RuntimeError):
    """A mandatory update gate failed; deployment must not succeed."""


def log_message(message):
    # Do not create an installation/log directory before the backup gate.
    print(message, flush=True)


def run_command(cmd, cwd=None, env=None):
    try:
        result = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, timeout=600)
        return result.returncode == 0, result.stdout, result.stderr
    except (OSError, subprocess.TimeoutExpired) as exc:
        return False, "", str(exc)


def _regular_file(path, description):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise UpdateError("Missing or unsafe " + description + ": " + str(path))
    return path


def _trusted_owned_path(path, description):
    path = Path(path).absolute()
    if path.resolve() != path:
        raise UpdateError(description + " contains a symlink/noncanonical component")
    for component in (path,) + tuple(path.parents):
        info = component.stat()
        if info.st_uid != 0 or info.st_mode & 0o022:
            raise UpdateError(description + " and its parents must be root-owned and protected from other writers")


def _https_url(url):
    if not isinstance(url, str) or not url:
        raise UpdateError("No HTTPS release source is configured")
    parsed = urllib.parse.urlsplit(url)
    if (parsed.scheme.lower() != "https" or not parsed.hostname
            or parsed.username is not None or parsed.password is not None
            or parsed.fragment):
        raise UpdateError("Release source and redirects must use HTTPS without credentials/fragments")
    return url


def load_policy(path):
    _regular_file(path, "update-source configuration")
    try:
        document = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise UpdateError("Invalid update-source configuration: " + str(exc)) from exc
    policy = document.get("update_policy") if isinstance(document, dict) else None
    if not isinstance(policy, dict):
        raise UpdateError("update_policy must be a configuration object")
    if type(policy.get("prefer_git", False)) is not bool:
        raise UpdateError("prefer_git must be a boolean")
    if policy.get("prefer_git", False):
        raise UpdateError("Git update is disabled: git pull would mutate the active tree before backup")
    if policy.get("local_mock_source") or policy.get("simulate") or policy.get("test_mode"):
        raise UpdateError("Simulated/local mock payloads are prohibited in production updates")
    if policy.get("auto_rollback_on_failure", True) is not True:
        raise UpdateError("Verified automatic rollback cannot be disabled")
    _https_url(policy.get("fallback_url"))
    checksum = policy.get("sha256")
    if (not isinstance(checksum, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", checksum)
            or checksum == "0" * 64):
        raise UpdateError("A trustworthy, independently configured SHA-256 release digest is required")
    validated = dict(policy)
    validated["sha256"] = checksum.lower()
    limits = {"timeout_seconds": (5, 1, 60),
              "max_download_bytes": (64 * 1024 * 1024, 1, MAX_DOWNLOAD_BYTES),
              "max_extracted_bytes": (256 * 1024 * 1024, 1, MAX_EXTRACTED_BYTES),
              "max_archive_members": (10000, 1, 100000)}
    for name, (default, lower, upper) in limits.items():
        value = policy.get(name, default)
        if type(value) is not int or not lower <= value <= upper:
            raise UpdateError("Invalid bounded update setting: " + name)
        validated[name] = value
    return validated


class HTTPSOnlyRedirect(urllib.request.HTTPRedirectHandler):
    def __init__(self, deadline=None):
        self.deadline = deadline

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        _https_url(newurl)  # Validate before urllib opens the redirected transport.
        if self.deadline is not None:
            remaining = self.deadline - time.monotonic()
            if remaining <= 0:
                raise UpdateError("Release download timed out during redirect")
            req.timeout = remaining
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def download_archive(policy, destination, opener=None):
    """Download bounded bytes with TLS verification and a configured digest."""
    url = _https_url(policy.get("fallback_url"))
    digest_expected = policy.get("sha256")
    if (not isinstance(digest_expected, str)
            or not re.fullmatch(r"[0-9a-fA-F]{64}", digest_expected)
            or digest_expected == "0" * 64):
        raise UpdateError("A trustworthy SHA-256 release digest is required before download")
    timeout = policy.get("timeout_seconds", 5)
    maximum = policy.get("max_download_bytes", 64 * 1024 * 1024)
    if type(timeout) is not int or not 1 <= timeout <= 60 or type(maximum) is not int or not 1 <= maximum <= MAX_DOWNLOAD_BYTES:
        raise UpdateError("Invalid download limits")
    deadline = time.monotonic() + timeout
    opener = opener or urllib.request.build_opener(HTTPSOnlyRedirect(deadline))
    open_request = opener.open if hasattr(opener, "open") else opener
    request = urllib.request.Request(url, headers={"User-Agent": "DS-EV-updater/9.0.0",
                                                  "Accept-Encoding": "identity"})
    destination = Path(destination)
    try:
        with open_request(request, timeout=timeout) as response:
            _https_url(response.geturl())
            status = getattr(response, "status", 200)
            if status != 200:
                raise UpdateError("Release asset is unavailable (HTTP " + str(status) + ")")
            length = response.headers.get("Content-Length")
            if length is not None:
                if not str(length).isdigit() or int(length) > maximum:
                    raise UpdateError("Release payload has an invalid or oversized Content-Length")
            digest = hashlib.sha256()
            total = 0
            with destination.open("xb") as output:
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise UpdateError("Release download timed out")
                    # urllib's timeout bounds connect/read operations. Bound the
                    # aggregate deadline as well, adjusting CPython's socket
                    # budget to the remaining time when available.
                    sock = getattr(getattr(getattr(response, "fp", None), "raw", None), "_sock", None)
                    if sock is not None:
                        sock.settimeout(remaining)
                    # read1 returns available bytes rather than trying to fill
                    # a large buffer forever under a slow byte trickle.
                    read_block = getattr(response, "read1", response.read)
                    block = read_block(min(65536, maximum - total + 1))
                    if time.monotonic() > deadline:
                        raise UpdateError("Release download timed out")
                    if not block:
                        break
                    total += len(block)
                    if total > maximum:
                        raise UpdateError("Release payload exceeds the download size limit")
                    digest.update(block)
                    output.write(block)
            if not total or (length is not None and total != int(length)):
                raise UpdateError("Release payload is empty or truncated")
            if not hmac.compare_digest(digest.hexdigest(), digest_expected.lower()):
                raise UpdateError("Release payload SHA-256 checksum mismatch")
    except urllib.error.HTTPError as exc:
        raise UpdateError("Release asset is unavailable (HTTP " + str(exc.code) + "); no fallback is permitted") from exc
    except (urllib.error.URLError, TimeoutError, socket.timeout, OSError) as exc:
        raise UpdateError("Release download failed: " + str(exc)) from exc
    except UpdateError:
        raise
    return destination


class _LimitedReader:
    """Cap all gzip-expanded tar bytes, including metadata and padding."""
    def __init__(self, source, maximum):
        self.source = source
        self.maximum = maximum
        self.total = 0

    def read(self, size=-1):
        if size < 0:
            size = self.maximum - self.total + 1
        block = self.source.read(min(size, self.maximum - self.total + 1))
        self.total += len(block)
        if self.total > self.maximum:
            raise UpdateError("Archive exceeds the expanded size limit")
        return block


def _member_name(member):
    raw = member.name
    path = PurePosixPath(raw)
    if (not raw or "\\" in raw or "\x00" in raw or path.is_absolute()
            or ".." in path.parts or re.match(r"^[A-Za-z]:", raw)):
        raise UpdateError("Unsafe archive path: " + repr(raw))
    name = path.as_posix()
    if name == ".":
        if member.type != tarfile.DIRTYPE:
            raise UpdateError("Invalid archive root member")
        return None
    if member.type not in (tarfile.REGTYPE, tarfile.AREGTYPE, tarfile.DIRTYPE) or member.sparse is not None:
        raise UpdateError("Unsafe link or unsupported archive member: " + name)
    if member.mode & 0o7000:
        raise UpdateError("Privileged archive permissions are prohibited: " + name)
    return name


def _scan_archive(archive_path, max_bytes, max_members, destination=None):
    names = {}
    required_directories = set()
    count = 0
    with gzip.open(archive_path, "rb") as expanded:
        limited = _LimitedReader(expanded, max_bytes)
        with tarfile.open(fileobj=limited, mode="r|") as archive:
            for member in archive:
                count += 1
                if count > max_members:
                    raise UpdateError("Archive exceeds the member count limit")
                name = _member_name(member)
                if name is None:
                    continue
                if name in names:
                    raise UpdateError("Duplicate archive member: " + name)
                parts = PurePosixPath(name).parts
                for index in range(1, len(parts)):
                    parent = "/".join(parts[:index])
                    if names.get(parent) == "file":
                        raise UpdateError("Archive file/directory collision: " + name)
                    required_directories.add(parent)
                kind = "directory" if member.type == tarfile.DIRTYPE else "file"
                if kind == "file" and name in required_directories:
                    raise UpdateError("Archive file/directory collision: " + name)
                names[name] = kind
                if member.size < 0 or member.size > max_bytes:
                    raise UpdateError("Invalid/oversized archive member: " + name)
                if destination is not None:
                    target = Path(destination).joinpath(*parts)
                    if kind == "directory":
                        target.mkdir(parents=True, exist_ok=True)
                    else:
                        target.parent.mkdir(parents=True, exist_ok=True)
                        source = archive.extractfile(member)
                        if source is None:
                            raise UpdateError("Unreadable archive member: " + name)
                        with source, target.open("xb") as output:
                            shutil.copyfileobj(source, output, length=65536)
                        target.chmod(0o755 if member.mode & 0o111 else 0o644)
        # Consume gzip footer/trailing blocks to check CRC/truncation even when
        # tar iteration has reached its end marker.
        while limited.read(65536):
            pass
    if not names:
        raise UpdateError("Archive contains no release files")
    return names


def extract_archive(archive_path, stage_dir, max_bytes=256 * 1024 * 1024, max_members=10000):
    """Validate the complete archive before writing any staged member."""
    if (type(max_bytes) is not int or not 1 <= max_bytes <= MAX_EXTRACTED_BYTES
            or type(max_members) is not int or not 1 <= max_members <= 100000):
        raise UpdateError("Invalid archive limits")
    destination = Path(stage_dir)
    if destination.is_symlink() or (destination.exists() and any(destination.iterdir())):
        raise UpdateError("Staging must be an empty independent directory")
    try:
        _scan_archive(archive_path, max_bytes, max_members)
        destination.mkdir(parents=True, exist_ok=True, mode=0o700)
        _scan_archive(archive_path, max_bytes, max_members, destination)
    except (tarfile.TarError, EOFError, OSError, ValueError) as exc:
        raise UpdateError("Invalid/corrupt release archive: " + str(exc)) from exc
    return destination


def _validate_configs(root):
    root = Path(root)
    keys = {"install_policy.json": "install_policy", "vehicle_profiles.json": "supported_vehicles",
            "safety_thresholds.json": "safety_thresholds", "update_source.json": "update_policy"}
    for name in REQUIRED_CONFIGS:
        path = _regular_file(root / "config" / name, "required diagnostics configuration")
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise UpdateError("Invalid configuration " + name + ": " + str(exc)) from exc
        if not isinstance(document, dict) or (name in keys and not isinstance(document.get(keys[name]), dict)):
            raise UpdateError("Invalid configuration structure: " + name)
        if name in keys and name != "update_source.json" and not document[keys[name]]:
            raise UpdateError("Required configuration is empty: " + name)
        if name == "vehicle_status.json":
            if not all(isinstance(document.get(field), str) and document[field]
                       for field in ("model", "firmware_version")):
                raise UpdateError("Required vehicle model/firmware evidence is missing")
            for field in ("battery_soh", "temperature_c", "voltage_v", "soc"):
                value = document.get(field)
                if type(value) not in (int, float) or not math.isfinite(value):
                    raise UpdateError("Required finite vehicle evidence is missing: " + field)


def _validate_source(stage):
    for name in SOURCE_FILES:
        _regular_file(stage / name, "required release file")
    _validate_configs(stage)
    docs = stage / "docs"
    if docs.is_symlink() or not docs.is_dir() or not list(docs.glob("*.md")):
        raise UpdateError("Release documentation is missing")


def _check_command(runner, command, cwd, gate, logger, env=None):
    try:
        options = {"cwd": str(cwd)}
        if env is not None:
            options["env"] = env
        ok, stdout, stderr = runner([str(part) for part in command], **options)
    except Exception as exc:
        raise UpdateError(gate + " failed: " + str(exc)) from exc
    if not ok:
        logger(gate + " output: " + (stderr or stdout or "no output"))
        raise UpdateError(gate + " failed")
    return stdout, stderr


def _diagnostic_receipt(config_dir, nonce):
    """Expected runtime acknowledgement of the exact selected evidence."""
    config = Path(config_dir).absolute()
    return {"contract": "ds-ev-updater-evidence-v1", "config_dir": str(config), "nonce": nonce,
            "sha256": {name: hashlib.sha256(_regular_file(config / name, "diagnostic evidence").read_bytes()).hexdigest()
                       for name in DIAGNOSTIC_EVIDENCE_FILES}}


def _check_diagnostics(runner, diagnostics, context, log_dir, gate, logger):
    """Require evidence-aware diagnostics; legacy zero exit is insufficient."""
    nonce = uuid.uuid4().hex
    expected = _diagnostic_receipt(context, nonce)
    environment = {name: value for name, value in os.environ.items() if not name.startswith("DS_")}
    command = [sys.executable, diagnostics, "--updater-evidence", "--config-dir", context,
               "--evidence-nonce", nonce, "--log-dir", log_dir]
    migration = ("protected updater-evidence v1 is required; migrate legacy installed diagnostics "
                 "through a trusted maintenance procedure before updating")
    try:
        stdout, _ = _check_command(runner, command, context, gate, logger, env=environment)
    except UpdateError as exc:
        raise UpdateError(str(exc) + "; " + migration) from exc
    receipts = [line[len(DIAGNOSTIC_EVIDENCE_PREFIX):] for line in (stdout or "").splitlines()
                if line.startswith(DIAGNOSTIC_EVIDENCE_PREFIX)]
    try:
        if len(receipts) != 1 or json.loads(receipts[0]) != expected:
            raise ValueError("missing or mismatched evidence receipt")
        if _diagnostic_receipt(context, nonce) != expected:
            raise ValueError("selected evidence changed during diagnostics")
    except (ValueError, OSError) as exc:
        raise UpdateError(gate + " failed: " + str(exc) + "; " + migration) from exc


def _prepare_deployment(stage, destination, active_config=None):
    cli = _regular_file(stage / "ds_core/bin/ds_enhancer", "compiled CLI")
    if not cli.stat().st_size or not os.access(cli, os.X_OK):
        raise UpdateError("Compiled CLI is empty or not executable")
    library_dir = stage / "ds_core/lib"
    if library_dir.is_symlink() or not library_dir.is_dir():
        raise UpdateError("Compiled shared library directory is missing")
    libraries = list(library_dir.glob("libds_enhancer.*"))
    if not libraries:
        raise UpdateError("Compiled shared library is missing")
    for library in libraries:
        _regular_file(library, "compiled shared library")
        if not library.stat().st_size:
            raise UpdateError("Compiled shared library is empty")
    destination.mkdir()
    (destination / "bin").mkdir()
    shutil.copy2(cli, destination / "bin/ds_enhancer")
    for script in (stage / "scripts").iterdir():
        if script.suffix in (".py", ".sh"):
            _regular_file(script, "release script")
            target = destination / "bin" / script.name
            shutil.copy2(script, target)
            target.chmod(0o755)
    shutil.copytree(library_dir, destination / "lib", symlinks=True)
    for source, name in ((stage / "ds_core/python", "python"),
                         (stage / "ds_core/python_fallback", "python_fallback"),
                         (active_config or stage / "config", "config"), (stage / "docs", "docs")):
        shutil.copytree(source, destination / name, symlinks=True)
    expected = backup_restore.tree_manifest(destination)
    if not expected:
        raise UpdateError("Prepared deployment is empty")
    return expected


def deploy_tree(stage, install):
    """Replace managed software directories after all validation/backup gates."""
    install = Path(install)
    if install.is_symlink():
        raise UpdateError("Installation root is a symlink")
    install.mkdir(parents=True, exist_ok=True)
    for name in backup_restore.MANAGED_DIRS:
        target = install / name
        if os.path.lexists(target):
            if target.is_symlink() or not target.is_dir():
                raise UpdateError("Unsafe deployment target: " + str(target))
            shutil.rmtree(target)
        shutil.copytree(Path(stage) / name, target, symlinks=True)


def _run_update_locked(install_dir=None, source_dir=None, config_path=None, *, runner=None,
               opener=None, logger=None, require_root=True, which=None, deployer=None):
    """Return zero only after genuine validated deployment; injectable for tests."""
    install = Path(install_dir or INSTALL_DIR).absolute()
    source = Path(source_dir or Path(__file__).resolve().parent.parent).absolute()
    config = Path(config_path) if config_path is not None else (
        install / "config/update_source.json" if install.exists() else source / "config/update_source.json")
    runner = runner or run_command
    logger = logger or log_message
    which = which or shutil.which
    deployer = deployer or deploy_tree
    logger("=== Initiating fail-closed software update ===")
    mutated = False
    try:
        if require_root and os.geteuid() != 0:
            raise UpdateError("Update requires root privileges")
        if install.resolve() != install or install.is_symlink() or (install.exists() and not install.is_dir()):
            raise UpdateError("Installation root is unsafe")
        if not install.is_dir():
            raise UpdateError("No existing installation is available; updater cannot deploy without a viable backup")
        if require_root:
            _trusted_owned_path(config, "Release digest configuration")
        policy = load_policy(config)
        for name in ("cmake", "ctest", "bash", "python3"):
            if not which(name):
                raise UpdateError("Required command is missing: " + name)
        if not any(which(name) for name in ("c++", "g++", "clang++")):
            raise UpdateError("Required C++ compiler is missing")
        interpreter = _regular_file(Path(sys.executable).resolve(), "Python executable")
        if not os.access(interpreter, os.X_OK):
            raise UpdateError("Python interpreter is not executable")
        active = install
        diagnostics = _regular_file(active / "bin/diagnostics.py",
                                    "current diagnostics")
        _validate_configs(active)
        if require_root:
            _trusted_owned_path(diagnostics, "Current diagnostics")
            for name in REQUIRED_CONFIGS:
                _trusted_owned_path(active / "config" / name, "Current diagnostics configuration")
        tool_dir = source / "scripts" if (source / "scripts").is_dir() else source / "bin"
        rollback = _regular_file(tool_dir / "rollback.sh", "backup/rollback command")
        helper = _regular_file(tool_dir / "backup_restore.py", "verified backup helper")
        if require_root:
            for tool in (rollback, helper):
                _trusted_owned_path(tool, "Backup/rollback helper")
        original = backup_restore.tree_manifest(install)
        with tempfile.TemporaryDirectory(prefix="ds-ev-update-") as temporary:
            temporary = Path(temporary)
            trusted_tools = temporary / "trusted-tools"
            trusted_tools.mkdir()
            shutil.copy2(rollback, trusted_tools / "rollback.sh")
            shutil.copy2(helper, trusted_tools / "backup_restore.py")
            # Every diagnostic gate reads the same protected copy of current
            # evidence. Legacy diagnostics without the strict receipt contract
            # must be migrated separately; ambiguous evidence cannot pass.
            context = temporary / "diagnostics-context"
            context.mkdir(mode=0o700)
            for name in REQUIRED_CONFIGS:
                shutil.copy2(active / "config" / name, context / name)
            _check_diagnostics(runner, diagnostics, context, temporary / "current-logs",
                               "Current diagnostics", logger)
            archive = temporary / "payload.tar.gz"
            download_archive(policy, archive, opener=opener)
            logger("Authenticated release archive downloaded and SHA-256 verified")
            stage = extract_archive(archive, temporary / "staging",
                                    policy["max_extracted_bytes"], policy["max_archive_members"])
            _validate_source(stage)
            _check_diagnostics(runner, stage / "scripts/diagnostics.py", context, temporary / "staged-logs",
                               "Staged diagnostics", logger)
            for generated in (stage / "build", stage / "ds_core/bin", stage / "ds_core/lib"):
                if generated.exists():
                    shutil.rmtree(generated)
            _check_command(runner, ["cmake", "-S", stage, "-B", stage / "build", "-DCMAKE_BUILD_TYPE=Release",
                                   "-DDS_BUILD_TESTS=ON"], stage, "Staged CMake configuration", logger)
            _check_command(runner, ["cmake", "--build", stage / "build", "--parallel", "2"],
                           stage, "Staged build (active installation unchanged; rollback unnecessary)", logger)
            test_listing, _ = _check_command(runner, ["ctest", "-N"], stage / "build",
                                             "Staged test discovery", logger)
            count = re.search(r"Total Tests:\s*(\d+)", test_listing)
            if count is None or int(count.group(1)) == 0:
                raise UpdateError("Staged test discovery found no required tests")
            _check_command(runner, ["ctest", "--output-on-failure"], stage / "build",
                           "Staged tests", logger)
            layout = temporary / "deployment"
            expected = _prepare_deployment(stage, layout, active / "config")
            snapshot_id = uuid.uuid4().hex
            snapshot = install / "backups" / snapshot_id
            rollback_command = ["bash", trusted_tools / "rollback.sh"]
            rollback_options = ["--install-dir", install, "--backup-dir", install / "backups", "--snapshot-id", snapshot_id]
            _check_command(runner, rollback_command + ["backup"] + rollback_options,
                           trusted_tools, "Required backup", logger)
            backup_restore.verify_backup(snapshot, original)
            if backup_restore.tree_manifest(install) != original:
                raise UpdateError("Active installation changed before deployment; update blocked")
            logger("Backup independently verified against current software")
            try:
                mutated = True  # Copying can partially mutate before raising.
                deployer(layout, install)
                if backup_restore.tree_manifest(install) != expected:
                    raise UpdateError("Deployed files failed verification")
                _check_diagnostics(runner, install / "bin/diagnostics.py", context, temporary / "deployed-logs",
                                   "Post-deployment diagnostics", logger)
                if backup_restore.tree_manifest(install) != expected:
                    raise UpdateError("Deployed software changed during diagnostics")
            except Exception as exc:
                logger("CRITICAL: Deployment failed: " + str(exc))
                try:
                    backup_restore.verify_backup(snapshot, original)
                    _check_command(runner, rollback_command + ["restore"] + rollback_options,
                                   trusted_tools, "Rollback command", logger)
                    if backup_restore.tree_manifest(install) != original:
                        raise UpdateError("Restored installation differs from the pre-update snapshot")
                    logger("RESTORATION VERIFIED: original software restored; update failed")
                except Exception as restore_error:
                    logger("RESTORATION FAILED: " + str(restore_error) + "; manual recovery required")
                return 1
            logger("SUCCESS: Genuine authenticated software update deployed and verified")
            return 0
    except (UpdateError, backup_restore.BackupError, OSError, ValueError) as exc:
        logger("CRITICAL: Update blocked: " + str(exc))
        if not mutated:
            logger("No software deployment was performed")
        return 1


def run_update(install_dir=None, source_dir=None, config_path=None, *, runner=None,
               opener=None, logger=None, require_root=True, which=None, deployer=None):
    """Serialize updaters using a lock outside replaced software directories."""
    install = Path(install_dir or INSTALL_DIR).absolute()
    logger = logger or log_message
    lock_fd = None
    try:
        if require_root and os.geteuid() != 0:
            raise UpdateError("Update requires root privileges")
        if install.resolve() != install:
            raise UpdateError("Installation path contains a symlink or noncanonical component")
        lock_path = install.parent / ("." + install.name + ".update.lock")
        lock_fd = os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        info = os.fstat(lock_fd)
        if not stat.S_ISREG(info.st_mode) or (require_root and (info.st_uid != 0 or info.st_mode & 0o022)):
            raise UpdateError("Unsafe updater lock file")
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise UpdateError("Another updater is active; deployment blocked") from exc
        return _run_update_locked(install, source_dir, config_path, runner=runner,
                                  opener=opener, logger=logger, require_root=require_root,
                                  which=which, deployer=deployer)
    except (UpdateError, OSError) as exc:
        logger("CRITICAL: Update blocked: " + str(exc))
        return 1
    finally:
        if lock_fd is not None:
            os.close(lock_fd)


def main():
    return run_update()


if __name__ == "__main__":
    sys.exit(main())
