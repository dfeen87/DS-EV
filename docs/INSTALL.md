# DS EV Battery Enhancement Software - Installation Guide

Welcome to the production-grade installation and deployment manual for the Dual-State (DS) EV Battery Enhancement Software. This system deploys advanced dual-state (Ψ, Φ) physics-based algorithms directly alongside your vehicle's existing Battery Management System (BMS) to optimize battery life, fast charging profiles, and regenerative braking.

---

## 📋 Table of Contents
- [Prerequisites](#prerequisites)
- [System Requirements](#system-requirements)
- [Target Directory Layout](#target-directory-layout)
- [Makefile Workflows](#makefile-workflows)
- [Safety Gates & Failure Gating](#safety-gates--failure-gating)
- [Static & Runtime Analysis](#static--runtime-analysis)
- [Valgrind Memory Leak Analysis](#valgrind-memory-leak-analysis)
- [Backup & Revert Rollback](#backup--revert-rollback)

---

## 🔧 Prerequisites

Before starting, ensure that the vehicle computer has:
1. **Operating System**: A POSIX-compliant system (Ubuntu Linux 20.04+ or macOS 12+ recommended).
2. **C++ Compiler**: A compiler supporting **C++17** or newer (`g++ >= 9` or `clang >= 11`).
3. **Build Tools**: `make` and `cmake` (version 3.15+).
4. **Python**: **Python >= 3.8** with standard library modules.

---

## 💻 System Requirements

The DS software stack has a lightweight footprint suitable for automotive ECU-adjacent integration:
* **Memory**: ~50KB per battery pack instance.
* **CPU**: <3% overhead at 100Hz BMS loop update frequency.
* **Execution Latency**: sub-100µs (C++ core).

---

## 📂 Target Directory Layout

When installed, the package is deployed to the system root under:
`/opt/ds_enhancement/`

The directories are organized as follows:
* `/opt/ds_enhancement/bin` — Compiled high-performance C++ CLI tool (`ds_enhancer`) and helper scripts.
* `/opt/ds_enhancement/lib` — Shared library files (`libds_enhancer.so`) and high-performance Python bindings (`ds_enhancer_pybind.so`).
* `/opt/ds_enhancement/python` — Python `ctypes` bindings for C++ library integrations.
* `/opt/ds_enhancement/python_fallback` — Pure-Python mathematical fallback module.
* `/opt/ds_enhancement/config` — Vehicle profiles, safety thresholds, and installation policies.
* `/opt/ds_enhancement/logs` — Comprehensive log files including `install.log`.
* `/opt/ds_enhancement/backups` — Timestamped, historical system rollback states.

---

## 🛠️ Makefile Workflows

The entire installation and maintenance lifecycle is driven by the top-level `Makefile`.

### 1. `make static-analysis`
Runs static analysis checks using `cppcheck` and `clang-tidy` to identify code quality or safety issues in compile time:
```bash
make static-analysis
```

### 2. `make verify`
Runs full host OS and compiler compatibility checks. It automatically calls `make static-analysis` first:
```bash
make verify
```

### 3. `make diagnostics`
Executes pre-flight battery, temperature, SOC, voltage, model, and firmware verification checks on the vehicle status:
```bash
make diagnostics
```

### 4. `sudo make install`
Safe, root-gated installation that compiles and deploys the entire stack to `/opt/ds_enhancement/`:
```bash
sudo make install
```

### 5. `sudo make update`
Before updating a legacy installation, complete the
[trusted legacy maintenance migration](UPDATER_LEGACY_MIGRATION.md). Legacy
diagnostics and timestamped backups cannot satisfy v8.0.2's evidence and verified
recovery contracts. The migration requires independent source provenance and
explicit administrator action; it never replaces operational configuration or
repairs unsafe installed permissions. A missing trusted release digest still
blocks the updater after migration.

Validates an independently SHA-pinned HTTPS release, stages its build and tests,
checks diagnostics and a verified backup, then performs recoverable file copying:
```bash
sudo make update
```

### 6. `sudo make rollback`
Restores the most recent timestamped backup from `/opt/ds_enhancement/backups/`:
```bash
sudo make rollback
```

---

## 🛑 Safety Gates & Failure Gating

To ensure extreme vehicle safety, `make install` and `make update` incorporate an **impenetrable safety gate**:
1. Before compiling or copying any file, the installer executes `scripts/diagnostics.py`.
2. If any parameter (such as SOH < 80%, Temperature outside 0-45°C, SOC outside 20-90%, or voltage out of range) fails to satisfy its threshold, the diagnostics script exits with a non-zero code.
3. The Makefile catches this code and **IMMEDIATELY ABORTS** the installation, writing a critical failure message to the logs and leaving the active system fully unaltered.

---

## 🔍 Static & Runtime Analysis

### Enforced Compiler Flags
All C++ targets are built with strict compiler warnings:
`-Wall -Wextra -Wpedantic -Werror`

### Sanitizers
When configuring the project in `Debug` mode, AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan) are automatically activated:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```
Any memory out-of-bounds or undefined behaviors will trigger immediate runtime crashes with trace dumps.

---

## 🧠 Valgrind Memory Leak Analysis

To run Valgrind on the CLI tool and find any potential memory leaks or illegal accesses:

1. Build in Debug or Release with symbols:
   ```bash
   cmake -B build -DCMAKE_BUILD_TYPE=Debug
   cmake --build build
   ```

2. Run the `ds_enhancer` CLI tool under Valgrind's leak checker:
   ```bash
   valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all --track-origins=yes ./ds_core/bin/ds_enhancer -v 355.0 -c 50.0 -t 25.0 -s 0.65 -d 0.1
   ```

3. Review the outputs:
   - Ensure that "definitely lost", "indirectly lost", and "possibly lost" are all 0 bytes.
   - Any "invalid read" or "invalid write" indicates memory safety issues that must be addressed immediately.

---

## 🔄 Backup & Revert Rollback

Before any file in `/opt/ds_enhancement` is altered or updated, the system triggers `/opt/ds_enhancement/bin/rollback.sh backup`.
* Backups are stored as a timestamped directory in `/opt/ds_enhancement/backups/YYYYMMDD_HHMMSS`.
* To revert to the previous version, run:
  ```bash
  sudo make rollback
  ```
  This cleanly wipes active binaries, configurations, and scripts, copies the backup files back into place, and writes a success event to the `/opt/ds_enhancement/logs/install.log` file.
