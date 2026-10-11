# DS EV Battery Enhancement - System Architecture

**Release:** 9.0.0 development candidate — BEDROCK REFORGED: ENERGY CONTINUITY.

This document provides a highly detailed technical overview of the Dual-State (DS) EV Battery Enhancement software structure, detailing module boundaries, interfaces, and the closed-loop energy cycle.

The diagrams describe software data flow and advisory integration. Their arrows
are not evidence of physical energy transfers or actuator deployment. The
research software requires independent physical and safety validation.

---

## 🏗️ Core Architecture Overview

The software integrates high-performance physical models (C++17) with robust scripting and distribution layers (Python/Shell) to create a highly reliable, deterministic, and safe automotive-adjacent control loop.

```
+------------------------------------------------------------+
|                       Top-Level CLI / UI                   |
|       (make install / diagnostics.py / ds_enhancer)       |
+------------------------------------------------------------+
                              |
                              v
+------------------------------------------------------------+
|                pybind11 Python Wrapper                     |
|           (ds_core/lib/ds_enhancer_pybind.so)            |
+------------------------------------------------------------+
                              |
                              v (via C++ interfaces)
+------------------------------------------------------------+
|                    Compiled C++17 Core                     |
|          (libds_enhancer.so / ds_enhancer binary)        |
+------------------------------------------------------------+
                              |
                              +---> ds_bms_middleware_v2
                              +---> ds_battery_core
                              +---> torque_enhancement
                              +---> ds_regen_braking_manager_v1
```

---

## 🔁 The Closed-Loop Energy Cycle

The DS stack v9.0.0 preserves its existing software loop between battery state
estimation, advisory torque limits, and recovery diagnostics. Accepted
battery-side electrical energy remains distinct from mechanical and electrical
recovery estimates.

```
                  +--------------------------------+
                  |      Battery State (Ψ, Φ)      |
                  |  - Voltage, Current, SOH, SOC   |
                  |  - Entropy & Metric trace       |
                  +--------------------------------+
                    /                            \
                   /                              \
                  v                                v
+----------------------------+          +----------------------------+
|    DS Torque Manager      |          |    DS Regen Manager       |
|  - Progressive derating    |          |  - High-power recovery     |
|  - Thermal protections     |          |  - Adaptive blend ratio    |
|  - Limp mode guidance      |          |  - Advisory braking limits |
+----------------------------+          +----------------------------+
                  \                                /
                   \                              /
                    v                            v
                  +--------------------------------+
                  |       Dynamic Vehicle          |
                  |       Energy Recovery          |
                  |  - Observed / estimated current |
                  |  - Closed-loop integration     |
                  +--------------------------------+
```

### 1. Battery State to Torque Delivery
* Callers supply sensor readings and elapsed time. The `DSBMSMiddleware` evaluates the coupled DS model and commits accepted cycles transactionally.
* The `DSTorqueManager` derives advisory torque limits from the model; external vehicle systems retain command ownership.

### 2. Kinetics to Regenerative Braking
* During deceleration, the driver's braking request is routed through the `DSRegenBrakingManager`.
* Based on current SOC headroom, temperatures, and cell balances, the regen manager computes safe charge acceptance bounds.
* These are software bounds and estimates, not evidence that a physical recovery event occurred.

### 3. Energy Recovery to Battery State
* The recovery coordinator selects measured or estimated current according to its existing mode, limits, and smoothing before a middleware cycle.
* Only a successful middleware cycle records accepted raw-current battery-side energy. Supplied SOC and abstract DS state do not independently measure stored physical energy.
* Accepted negative current identifies charging; independent source evidence is required to attribute it to regeneration. Software tests do not establish physical efficiency, battery protection, or reduced grid consumption.

---

## 📂 Subsystem & Module Boundaries

### 1. High-Performance Core (`/ds_core`)
Written in pure, standard C++17 with zero operating system dependencies:
* **`ds_battery_core.hpp`**: Implements the dual-state $(\Psi, \Phi)$ system model. Physical attributes (voltage, current, temperature, state of charge) are coupled to informational attributes (entropy, history, degradation) using the effective metric formulation:
  $$g^{\text{eff}}_{\mu\nu} = g_{\mu\nu} + \lambda \partial_{\mu}\Phi \partial_{\nu}\Phi$$
* **`torque_enhancement.hpp`**: Translates battery health, entropy, and cell balances into intelligent real-time motor torque limits.
* **`ds_regen_braking_manager_v1.hpp`**: Implements health-aware regenerative braking blending, allowing healthy cells to maximize energy recovery while protecting weak cells.

### 2. Python pybind11 Module Wrapper (`/ds_core/src/ds_pybind11_wrapper.cpp`)
* Implements high-performance C++ bindings that compile into `ds_enhancer_pybind.so` using `pybind11`.
* Binds core classes (`DSBMSMiddleware`, `DSTorqueManager`), enums (`DriveMode`, `RegenMode`), and configurations (`TorqueConfig`, `MiddlewareConfig`), making them accessible to standard Python control scripts with zero parsing overhead.

### 3. Verification & Diagnostics Scripts (`/scripts`)
Modular, robust diagnostics tools used during installation, updates, or maintenance:
* **`diagnostics.py`**: The primary diagnostic script checking OS compliance, Python versions, interface existence, and physical battery boundaries (SOH >= 80%, SOC 20-90%, Temperature 0-45°C).
* **`update.py`**: Secure update manager that fetches updates, validates safety within staging, takes system backups, and deploys updates cleanly.
* **`rollback.sh`**: Handles backup generation and state restores during setup and software updates.

### 4. Configuration Layer (`/config`)
* **`vehicle_profiles.json`**: Supported EV models along with nominal voltages and firmware globs.
* **`safety_thresholds.json`**: Standard threshold definitions.
* **`install_policy.json`**: System-level installation rules and dependencies.
* **`vehicle_status.json`**: Mock state representing current simulated telemetries.
* **`update_source.json`**: Update source fallback URL and policy definitions.

## Recovery accounting implementation contract

The cycle diagram above describes architectural intent. Current software
acceptance is documented in
[RECOVERY_LEARNING_AUDIT_REMEDIATION.md](RECOVERY_LEARNING_AUDIT_REMEDIATION.md)
and the [accepted raw electrical-energy contract](NUMERICAL_MIDDLEWARE_FALLBACK_REMEDIATION.md).
The recovery coordinator stages its own history until a BMS cycle succeeds.
Accepted battery-side charging is calculated from the actual raw voltage,
chosen current after coordinator limits/smoothing, and interval in seconds.
Mechanical and electrical recovery estimates are separate diagnostics and are
never added as extra energy to the BMS ledger. Measured negative current
identifies charging; it does not identify its source as regeneration. Estimated
mode is a simulation estimate, without physical regeneration attribution.

## Optional physical Energy Continuity

The v9.0.0 opt-in observer reconciles explicitly identified physical transfers
and supplied storage changes without modifying middleware, recovery, trust, or
control outcomes. Its boundary identity is

$$E_{\mathrm{initial}}+E_{\mathrm{in}}=
E_{\mathrm{final}}+E_{\mathrm{out}}+E_{\mathrm{dissipated}}.$$

The layer uses joules and explicit checked kWh conversion. It reports incomplete
evidence separately from balance/imbalance; a missing transfer is not inferred
from a residual. DS `energy_psi`, `energy_phi`, and `energy_metric` remain abstract
model variables outside this physical accounting. Telemetry and AILEE governance
are data consumers, not physical nodes, and cannot create accepted measurements.

See [RELEASE_9.0.0.md](RELEASE_9.0.0.md) for the data-flow diagram and evidence
contract, and [SIMULATION_VALIDATION_9.0.0.md](SIMULATION_VALIDATION_9.0.0.md) for
executed software-validation results.
