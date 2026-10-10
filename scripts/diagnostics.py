#!/usr/bin/env python3
import os
import sys
import json
import argparse
import fnmatch
import hashlib
import math
from pathlib import Path
import platform
import re
import stat
import subprocess

UPDATER_EVIDENCE_FILES = ("install_policy.json", "vehicle_status.json",
                          "vehicle_profiles.json", "safety_thresholds.json")
UPDATER_EVIDENCE_PREFIX = "DS_UPDATER_EVIDENCE_V1:"


def _protected_evidence_path(path, directory=False):
    """Reject evidence writable or replaceable by another local account.

    A nonroot private anchor below sticky /tmp or /var/tmp may stop ancestor
    traversal. Root execution checks every ancestor and trusts root only.
    """
    path = Path(path).absolute()
    if path.resolve() != path:
        raise ValueError("Diagnostic evidence contains a symlink/noncanonical path")
    if (directory and not path.is_dir()) or (not directory and not path.is_file()):
        raise ValueError("Required diagnostic evidence is missing: " + str(path))
    owner = os.geteuid()
    info = path.lstat()
    if info.st_uid != owner:
        raise ValueError("Diagnostic evidence is owned by an unauthorized account: " + str(path))
    if info.st_mode & 0o022:
        raise ValueError("Diagnostic evidence is writable by unauthorized accounts: " + str(path))
    child = path
    for parent in path.parents:
        info = parent.lstat()
        if not stat.S_ISDIR(info.st_mode):
            raise ValueError("Diagnostic evidence parent is not a safe directory: " + str(parent))
        if parent in (Path("/tmp"), Path("/var/tmp")) and info.st_mode & stat.S_ISVTX:
            child_info = child.lstat()
            system_owner = Path("/").lstat().st_uid if owner != 0 else 0
            if (info.st_uid in (0, system_owner) and child_info.st_uid == owner
                    and stat.S_ISDIR(child_info.st_mode) and not child_info.st_mode & 0o077):
                if owner != 0:
                    return path
                # Sticky protection permits this temporary boundary, but root
                # must still verify the ownership/mode of higher ancestors.
                child = parent
                continue
        if info.st_uid not in (0, owner):
            raise ValueError("Diagnostic evidence parent is owned by an unauthorized account: " + str(parent))
        if info.st_mode & 0o022:
            raise ValueError("Diagnostic evidence parent is writable by unauthorized accounts: " + str(parent))
        child = parent
    return path


def _load_updater_evidence(config_dir, nonce):
    """Load one protected source; neither fixtures nor overrides participate."""
    config = _protected_evidence_path(config_dir, directory=True)
    documents = {}
    digests = {}
    roots = {"install_policy.json": "install_policy", "vehicle_profiles.json": "supported_vehicles",
             "safety_thresholds.json": "safety_thresholds"}
    for name in UPDATER_EVIDENCE_FILES:
        path = _protected_evidence_path(config / name)
        data = path.read_bytes()
        document = json.loads(data)
        if not isinstance(document, dict):
            raise ValueError("Required diagnostic evidence must be an object: " + name)
        if name in roots and (not isinstance(document.get(roots[name]), dict) or not document[roots[name]]):
            raise ValueError("Required diagnostic configuration is empty/invalid: " + name)
        documents[name] = document
        digests[name] = hashlib.sha256(data).hexdigest()
    status = documents["vehicle_status.json"]
    if not all(isinstance(status.get(field), str) and status[field]
               for field in ("model", "firmware_version")):
        raise ValueError("Required vehicle model/firmware evidence is missing")
    for field in ("battery_soh", "temperature_c", "voltage_v", "soc"):
        value = status.get(field)
        if type(value) not in (int, float) or not math.isfinite(value):
            raise ValueError("Required finite vehicle evidence is missing: " + field)
    receipt = {"contract": "ds-ev-updater-evidence-v1", "config_dir": str(config),
               "nonce": nonce, "sha256": digests}
    return documents, receipt

def log_message(msg, log_paths=None):
    """Logs a message to stdout and specified log files."""
    print(msg)
    if log_paths is None:
        log_paths = [
            "/opt/ds_enhancement/logs/install.log",
            "logs/install.log",
            "./install.log"
        ]
    for path in log_paths:
        try:
            # Create directories if they don't exist
            dir_name = os.path.dirname(path)
            if dir_name and not os.path.exists(dir_name):
                os.makedirs(dir_name, exist_ok=True)
            with open(path, "a") as f:
                f.write(msg + "\n")
        except Exception:
            # If we don't have permission or write fails, ignore and proceed
            pass

def find_file(filename, search_dirs):
    """Searches for a file in a list of directories and returns its absolute path."""
    for directory in search_dirs:
        full_path = os.path.join(directory, filename)
        if os.path.exists(full_path):
            return os.path.abspath(full_path)
    return None

def check_hardware_interfaces(policy):
    """Checks for the presence of configured hardware interfaces."""
    required_any = policy.get("hardware_interfaces", {}).get("required_any", ["simulation"])

    # Always allow simulation or mock hardware check
    if "simulation" in required_any:
        return True, "Simulation interface available (Simulation Mode)"

    # Check if we can run ip link to find CAN interfaces
    try:
        result = subprocess.run(["ip", "link"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=2)
        if result.returncode == 0:
            for iface in required_any:
                if iface in result.stdout:
                    return True, f"Found physical hardware interface: {iface}"
    except Exception:
        pass

    # On macOS or system without `ip link`, check if network interfaces match
    try:
        import socket
        for iface in required_any:
            # Check SPI / sysfs nodes
            if iface.startswith("spi") and os.path.exists(f"/dev/{iface}"):
                return True, f"Found SPI node: /dev/{iface}"
    except Exception:
        pass

    if policy.get("allow_unsupported_hardware_override", True):
        return True, "No active physical hardware interfaces found; falling back to simulation interface."

    return False, f"None of the required hardware interfaces {required_any} were detected on the system."

def load_json(filepath):
    try:
        with open(filepath, "r") as f:
            return json.load(f)
    except Exception as e:
        sys.stderr.write(f"Error loading JSON {filepath}: {e}\n")
        return None

def main():
    parser = argparse.ArgumentParser(description="DS EV Battery Enhancement - Pre-Flight Diagnostics")
    parser.add_argument("--model", type=str, help="Override vehicle model")
    parser.add_argument("--firmware", type=str, help="Override firmware version")
    parser.add_argument("--soh", type=float, help="Override battery State of Health (%%)")
    parser.add_argument("--temp", type=float, help="Override temperature (°C)")
    parser.add_argument("--voltage", type=float, help="Override battery voltage (V)")
    parser.add_argument("--soc", type=float, help="Override State of Charge (%%)")
    parser.add_argument("--log-dir", type=str, help="Alternative log directory")
    parser.add_argument("--config-dir", type=str,
                        help="Use only this diagnostics configuration and vehicle status directory")
    parser.add_argument("--updater-evidence", action="store_true",
                        help="Validate protected top-level config evidence only; ignore DS_* and presets")
    parser.add_argument("--evidence-nonce", type=str,
                        help="Updater evidence-contract challenge (32 hexadecimal characters)")
    args = parser.parse_args()
    evidence_documents = evidence_receipt = None
    if args.updater_evidence:
        if not args.config_dir or not args.log_dir:
            parser.error("--updater-evidence requires explicit --config-dir and --log-dir")
        if not args.evidence_nonce or not re.fullmatch(r"[0-9a-f]{32}", args.evidence_nonce):
            parser.error("--updater-evidence requires a valid --evidence-nonce")
        if any(getattr(args, field) is not None for field in ("model", "firmware", "soh", "temp", "voltage", "soc")):
            parser.error("Vehicle overrides are prohibited by the updater evidence contract")
        try:
            evidence_documents, evidence_receipt = _load_updater_evidence(args.config_dir, args.evidence_nonce)
        except (OSError, ValueError) as exc:
            parser.error("Untrustworthy updater diagnostic evidence: " + str(exc))
    elif args.evidence_nonce is not None:
        parser.error("--evidence-nonce requires --updater-evidence")

    # Determine paths and search directories
    base_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    search_dirs = [
        os.path.abspath("."),
        base_dir,
        "/opt/ds_enhancement",
        "/opt/ds_enhancement/config",
        os.path.join(base_dir, "config")
    ]
    if args.config_dir:
        search_dirs = [os.path.abspath(args.config_dir)]

    log_paths = [
        "/opt/ds_enhancement/logs/install.log",
        os.path.join(base_dir, "logs/install.log"),
        "./logs/install.log",
        "./install.log"
    ]
    if args.log_dir:
        # An explicit destination must keep staged checks out of live logs.
        log_paths = [os.path.join(args.log_dir, "install.log")]

    log_message("=== Starting Pre-Flight Diagnostics ===", log_paths)

    # 1. OS & Python Environment Verification
    current_os = platform.system().lower()
    python_version = platform.python_version()
    python_major, python_minor = sys.version_info[:2]

    log_message(f"Host OS detected: {platform.system()} {platform.release()}", log_paths)
    log_message(f"Python Version detected: {python_version}", log_paths)

    policy = {}
    if evidence_documents is not None:
        policy = evidence_documents["install_policy.json"]["install_policy"]
    else:
        policy_path = find_file("install_policy.json", search_dirs)
        if not policy_path:
            # Fallback if policy is not yet deployed
            policy_path = find_file("config/install_policy.json", search_dirs)
        if policy_path:
            policy_data = load_json(policy_path)
            if policy_data:
                policy = policy_data.get("install_policy", {})

    # Check target OS
    allowed_os = policy.get("target_os", ["linux", "darwin"])
    if current_os not in allowed_os:
        msg = f"CRITICAL: OS '{current_os}' is not supported. Supported: {allowed_os}"
        log_message(msg, log_paths)
        sys.exit(1)

    # Check python version
    min_py = policy.get("python_version", {}).get("min", "3.8")
    min_major, min_minor = map(int, min_py.split("."))
    if (python_major < min_major) or (python_major == min_major and python_minor < min_minor):
        msg = f"CRITICAL: Python version {python_version} is too old. Required: >= {min_py}"
        log_message(msg, log_paths)
        sys.exit(1)

    log_message("✓ Environment and OS verification PASSED.", log_paths)

    # 2. Hardware Interface Availability Verification
    hw_ok, hw_msg = check_hardware_interfaces(policy)
    if not hw_ok:
        log_message(f"CRITICAL: Hardware check FAILED. {hw_msg}", log_paths)
        sys.exit(1)
    log_message(f"✓ Hardware check: {hw_msg}", log_paths)

    # 3. Read Vehicle Status (Simulated or Real)
    # Priority: /tmp/vehicle_status.json -> config/vehicle_status.json in search dirs
    status = {}
    if evidence_documents is not None:
        status_file = os.path.join(os.path.abspath(args.config_dir), "vehicle_status.json")
        status = evidence_documents["vehicle_status.json"]
        log_message(f"Loaded protected updater vehicle status from: {status_file}", log_paths)
    else:
        status_file = (os.path.join(os.path.abspath(args.config_dir), "vehicle_status.json")
                       if args.config_dir else "/tmp/vehicle_status.json")
        if not os.path.exists(status_file):
            status_file = find_file("vehicle_status.json", search_dirs)
        if not status_file:
            status_file = find_file("config/vehicle_status.json", search_dirs)
        if status_file:
            status_data = load_json(status_file)
            if status_data:
                status = status_data
                log_message(f"Loaded vehicle status from: {status_file}", log_paths)

    # First determine the target model (arg takes priority, then env, then top-level status json)
    overrides = {} if args.updater_evidence else os.environ
    model = args.model or overrides.get("DS_MODEL") or status.get("model", "Unknown")

    # If the chosen model exists in simulated_presets, use its preset as the base status!
    presets = {} if args.updater_evidence else status.get("simulated_presets", {})
    if model in presets:
        log_message(f"Applying simulated status preset for model: {model}", log_paths)
        status_base = presets[model]
    else:
        status_base = status

    # Apply overrides (Env variables -> base status values)
    firmware_version = overrides.get("DS_FIRMWARE", status_base.get("firmware_version", status_base.get("firmware", "Unknown")))
    battery_soh = status_base.get("battery_soh", 0.0)
    if "DS_SOH" in overrides:
        try: battery_soh = float(overrides["DS_SOH"])
        except ValueError: pass
    temperature_c = status_base.get("temperature_c", 0.0)
    if "DS_TEMP_C" in overrides:
        try: temperature_c = float(overrides["DS_TEMP_C"])
        except ValueError: pass
    voltage_v = status_base.get("voltage_v", 0.0)
    if "DS_VOLTAGE_V" in overrides:
        try: voltage_v = float(overrides["DS_VOLTAGE_V"])
        except ValueError: pass
    soc = status_base.get("soc", 0.0)
    if "DS_SOC" in overrides:
        try: soc = float(overrides["DS_SOC"])
        except ValueError: pass

    # CLI args override everything
    if args.firmware: firmware_version = args.firmware
    if args.soh is not None: battery_soh = args.soh
    if args.temp is not None: temperature_c = args.temp
    if args.voltage is not None: voltage_v = args.voltage
    if args.soc is not None: soc = args.soc

    log_message(f"Vehicle parameters to evaluate:\n"
                f"  Model:            {model}\n"
                f"  Firmware Version: {firmware_version}\n"
                f"  Battery SOH:      {battery_soh}%\n"
                f"  Temperature:      {temperature_c}°C\n"
                f"  Voltage:          {voltage_v}V\n"
                f"  SOC:              {soc}%", log_paths)

    # 4. Load vehicle profiles and thresholds
    if evidence_documents is not None:
        profiles_data = evidence_documents["vehicle_profiles.json"]
        thresholds_data = evidence_documents["safety_thresholds.json"]
    else:
        profiles_path = find_file("vehicle_profiles.json", search_dirs) or find_file("config/vehicle_profiles.json", search_dirs)
        thresholds_path = find_file("safety_thresholds.json", search_dirs) or find_file("config/safety_thresholds.json", search_dirs)
        if not profiles_path or not thresholds_path:
            log_message("CRITICAL: Configuration files (profiles/thresholds) could not be located.", log_paths)
            sys.exit(1)
        profiles_data = load_json(profiles_path)
        thresholds_data = load_json(thresholds_path)
    if profiles_data is None or thresholds_data is None:
        log_message("CRITICAL: Configuration files could not be parsed.", log_paths)
        sys.exit(1)

    profiles = profiles_data.get("supported_vehicles", {})
    thresholds = thresholds_data.get("safety_thresholds", {})

    # Validate model support
    if model not in profiles:
        msg = f"CRITICAL: Vehicle model '{model}' is not supported. See vehicle_profiles.json for a list of supported models."
        log_message(msg, log_paths)
        sys.exit(2)

    profile = profiles[model]
    log_message(f"✓ Model '{model}' is verified and fully supported ({profile['model_name']}).", log_paths)

    # Validate firmware version matching
    firmware_matched = False
    supported_fw_patterns = profile.get("supported_firmware_versions", [])
    for pattern in supported_fw_patterns:
        if fnmatch.fnmatch(firmware_version, pattern):
            firmware_matched = True
            break

    if not firmware_matched:
        msg = f"CRITICAL: Firmware version '{firmware_version}' is unsupported for model '{model}'. Supported patterns: {supported_fw_patterns}"
        log_message(msg, log_paths)
        if not policy.get("allow_unsupported_firmware_override", False):
            sys.exit(3)
        else:
            log_message("WARNING: Installation policy permits overriding firmware warning. Proceeding with caution.", log_paths)
    else:
        log_message(f"✓ Firmware '{firmware_version}' matches supported profile requirements.", log_paths)

    # 5. Safety Thresholds Verification
    failures = []

    # NaN values compare false against every bound, so reject all non-finite
    # measurements explicitly rather than accidentally treating them as safe.
    measurements = {
        "Battery SOH": battery_soh,
        "Battery temperature": temperature_c,
        "Battery voltage": voltage_v,
        "Battery State of Charge": soc,
    }
    for name, value in measurements.items():
        if not math.isfinite(value):
            failures.append(f"{name} must be a finite number (received: {value})")

    # State of Health
    min_soh = thresholds.get("battery_soh", {}).get("min_percent", 80.0)
    if battery_soh < min_soh:
        failures.append(f"Battery SOH ({battery_soh}%) is below safe enhancement threshold (min: {min_soh}%)")

    # Temperature bounds
    temp_min = thresholds.get("temperature_c", {}).get("min", 0.0)
    temp_max = thresholds.get("temperature_c", {}).get("max", 45.0)
    if temperature_c < temp_min or temperature_c > temp_max:
        failures.append(f"Battery temperature ({temperature_c}°C) is outside safe range (allowed: {temp_min}°C to {temp_max}°C)")

    # SOC bounds
    soc_min = thresholds.get("soc_percent", {}).get("min", 20.0)
    soc_max = thresholds.get("soc_percent", {}).get("max", 90.0)
    if soc < soc_min or soc > soc_max:
        failures.append(f"Battery State of Charge ({soc}%) is outside ideal installation range (allowed: {soc_min}% to {soc_max}%)")

    # Voltage range (checked against vehicle profile specifics)
    v_limits = profile.get("voltage_range", {})
    v_min = v_limits.get("min", 300.0)
    v_max = v_limits.get("max", 450.0)
    if voltage_v < v_min or voltage_v > v_max:
        failures.append(f"Measured battery voltage ({voltage_v}V) is outside nominal profile bounds (allowed: {v_min}V to {v_max}V)")

    # Report results
    if failures:
        log_message("CRITICAL: Diagnostics failed with the following safety violations:", log_paths)
        for fail in failures:
            log_message(f"  - {fail}", log_paths)
        log_message("Installation or execution blocked for vehicle safety.", log_paths)
        sys.exit(4)

    if evidence_receipt is not None:
        try:
            _, current_receipt = _load_updater_evidence(args.config_dir, args.evidence_nonce)
            if current_receipt != evidence_receipt:
                raise ValueError("Diagnostic evidence changed during validation")
        except (OSError, ValueError) as exc:
            log_message("CRITICAL: Untrustworthy updater diagnostic evidence: " + str(exc), log_paths)
            sys.exit(1)
    log_message("✓ All health, temperature, voltage, and SOC metrics are within safe boundaries.", log_paths)
    log_message("=== Pre-Flight Diagnostics Completed Successfully ===", log_paths)
    if evidence_receipt is not None:
        print(UPDATER_EVIDENCE_PREFIX + json.dumps(evidence_receipt, sort_keys=True))
    sys.exit(0)

if __name__ == "__main__":
    main()
