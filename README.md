# DS-EV
**Battery Intelligence, Energy Recovery & Safety Governance**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Status: HIL Validation Required](https://img.shields.io/badge/Status-HIL%20Validation%20Required-orange.svg)](docs/SAFETY.md)
[![Stage: Software Validation](https://img.shields.io/badge/Stage-Software%20Validation-blue.svg)](docs/POST_BEDROCK_REMEDIATION_BACKLOG.md)
[![Development: 8.0.2](https://img.shields.io/badge/Development-8.0.2-blue.svg)](docs/RELEASE_8.0.2.md)
[![CI](https://github.com/dfeen87/DS-EV/actions/workflows/ci.yml/badge.svg)](https://github.com/dfeen87/DS-EV/actions/workflows/ci.yml)

## Research-Stage EV Battery Management & Control Architecture

DS-EV is an actively developed C++17/Python research framework for dual-state battery modeling, health and degradation estimates, battery-aware torque and regenerative-energy logic, BMS middleware, and automotive governance reference components.

Engineering work focuses on input validation, raw-evidence preservation, explicit protection boundaries, regression-tested state integrity, and fail-closed software update/rollback processes. These are **software engineering practices**, not proof of vehicle safety, performance, or accuracy.

> **Vehicle safety notice:** DS-EV is **not vehicle-validated, road-ready, or automotive safety-certified**. Do not connect its examples, scripts, or computed commands to a live vehicle, traction inverter, charger, brake system, or battery actuators. Use software simulation and non-actuating research environments only. Any eventual vehicle application needs independent system hazard analysis, hardware-in-the-loop (HIL) and fault-injection validation, real hardware integration testing, applicable functional-safety assessment, and qualified engineering oversight. HIL alone does not confer certification.

**Version status:** `8.0.2` is the current **development target**, not a published GitHub release. The latest published release is [v8.0.1](https://github.com/dfeen87/DS-EV/releases/tag/v8.0.1). A source version string, passing CI, or development report does not prove availability of a validated deployable release.

**Start with:** [v8.0.2 engineering report](docs/RELEASE_8.0.2.md) · [Post-BEDROCK backlog](docs/POST_BEDROCK_REMEDIATION_BACKLOG.md) · [Safety scope](docs/SAFETY.md) · [Simulation quick start](#-simulation-only-quick-start) · [Citation](CITATION.cff)

## Evidence & Release Status

| Surface | Currently supported evidence | Not established |
|---|---|---|
| DS core and middleware | C++ compilation, CTest regressions, deterministic simulation, fixes to selected safety/state boundaries | Physical battery-model validation or vehicle safety |
| Automotive AILEE/governance | C++/Python reference paths, targeted adversarial regression tests, configurable protection rules | Certification for drivetrain, brake, or charge actuation |
| Python fallback/extension | Python unit tests; separate CI build and smoke test for optional pybind11 extension | Numerical equivalence of the approximate fallback to C++ |
| Update and rollback | Authenticated-payload gates, protected evidence/backup tests, offline maintenance migration guidance | Available v8.0.2 release payload, atomic/power-loss-safe deployment |
| Performance and benefit | Runnable examples and local benchmark targets | Independent accuracy, battery-life, range, worst-case latency, or CPU/memory benefit claims |

**Remaining work:** PB-09 through PB-15 are tracked in the [remediation backlog](docs/POST_BEDROCK_REMEDIATION_BACKLOG.md); do not treat open findings as fixed. Historical engineering reports describe the specific tests and conditions actually exercised.

---

## 📋 Table of Contents

- [Research Stage and Safety](#research-stage-ev-battery-management--control-architecture)
- [Evidence & Release Status](#evidence--release-status)
- [Why This Matters](#-why-this-matters)
- [Key Features](#-key-features)
- [Performance & Validation Evidence](#performance--validation-evidence)
- [Advanced Features & Examples](#-advanced-features--examples)
- [DS Torque Enhancement Module](#ds-torque-enhancement-module-v20)
- [AILEE Automotive Governance Layer](#ailee-automotive-governance-layer)
- [BMS Middleware, Hardware Adapters & OEM Integration](#bms-middleware-hardware-adapters--oem-integration)
- [Intelligent Regenerative Braking](#-intelligent-regenerative-braking-ds-regen-module)
- [Closed-Loop Energy Recovery](#-closed-loop-energy-recovery-battery--torque--braking--battery)
- [Optional Telemetry Interface](#-optional-telemetry-interface)
- [Repository Structure](#-repository-structure)
- [Energy Recovery Discussion](#-closed-cycle-energy-recovery)
- [Simulation-Only Quick Start](#-simulation-only-quick-start)
- [Quick Start C++ Guide](#-quick-start-c-guide)
- [Integration Guide](#-integration-guide)
- [Validation & Testing](#-validation--testing)
- [Continuous Integration](#-continuous-integration)
- [Use Cases](#-use-cases)
- [Architecture](#-architecture)
- [Safety & Reliability](#-safety--reliability)
- [Roadmap](#-roadmap)
- [Citation](#citation)
- [License](#license)
- [Get Involved](#-get-involved)

---

## 🚀 Why This Matters

Battery behavior depends on instantaneous readings (voltage, current, temperature, SOC) and accumulated stress. DS-EV explores combining those signals in a dual-state model to produce **research estimates** of health, charging behavior, and battery-aware power constraints.

### The Research Question

Can explicitly modeled stress history improve state estimates and support more consistent protective decisions than instantaneous-only modeling? This is a hypothesis to test—not a demonstrated improvement over a commercial BMS.

### The DS Approach

```text
Battery readings + modeled stress history
             ↓
       DS state and estimates
             ↓
Governed power/regen recommendations, diagnostics, simulation
```

The code exercises software protection contracts; it does **not** establish a percentage improvement in degradation detection or vehicle battery longevity.

---

## ⚡ Key Features

- **Dual-state modeling:** Physical state combined with accumulated modeled stress/entropy and candidate battery-health estimates.
- **BMS and multi-cell integration:** Chemistry profiles, state aggregation, diagnostic middleware and reference hardware-adapter boundaries.
- **Torque and regen modeling:** Battery-aware software *recommendations*, not authorized real-vehicle commands.
- **Automotive governance:** Reference trust/protection-envelope evaluation with adversarial regression coverage.
- **Engineering validation:** C++/Python testing, state-integrity remediation, authenticated updater gates, and protected backup/restore checks.

This repository is **not** a drop-in, two-line vehicle integration.

---

## Performance & Validation Evidence

The repository contains local benchmark sources: [update](benchmarks/benchmark_update.cpp) and [multi-cell](benchmarks/benchmark_multicell.cpp). Measure the actual platform, compiler, workload and model configuration before quoting performance.

| Question | Evidence needed |
|---|---|
| Cycle latency, memory and CPU | Reproducible target-specific benchmarks; worst-case execution analysis for real-time use |
| Degradation-estimation accuracy | Representative calibrated battery datasets and out-of-sample baselines |
| Battery lifetime, charging and range | Independent physical/vehicle comparative testing |
| Integration effort and runtime guarantees | Target-specific engineering and integration measurements |

**Passing CI does not establish sub-millisecond deadlines, a 20–30% accuracy improvement, or universal footprint figures.** Optional/experimental GPU paths are not a verified automotive performance improvement.

---

## 🔧 Advanced Features & Examples

These components provide research paths for chemistry profiles, multi-cell models, Kalman filtering, and optional hybrid modeling. An interface or example does **not** establish hardware behavior or vehicle certification.

### ⚙️ Advanced Capabilities

1. **Chemistry profiles:** LFP, NMC, NCA, LTO and configurable stress/thermal parameters. Cell-specific correctness requires calibration and independent test evidence.
2. **Multi-cell modeling:** Reference pack aggregation, cell imbalance indicators, weak-cell flags, and health estimates. The [`multi_cell_pack.cpp` example](examples/multi_cell_pack.cpp) is **disabled in the default CMake example build due to API mismatches**; it is not a verified integration test.
3. **Kalman filtering:** Optional state-estimation filtering of model and sensor values; filter results need calibration and validation.
4. **ML hybrid (optional):** Exploratory interface concepts; no trained-model benefit is established.
5. **Fleet learning (proposed):** Potential aggregated chemistry/stress analytics, not a deployed opt-in fleet-data service.
6. **GPU (experimental):** Optional CUDA configuration and interface stubs are not a supported, benchmarked, safety-validated GPU execution pipeline.

Use [`examples/basic_integration.cpp`](examples/basic_integration.cpp) for a default-build software example.

---

## DS Torque Enhancement Module v2.0

### Overview

The **DS Torque Enhancement Module** models battery-aware torque limits in simulation. It is a research/reference implementation, **not** an independently validated vehicle command source or battery-protection guarantee.

Unlike traditional torque limiters that only consider instantaneous power limits, this module uses the dual-state DS framework (physical state + historical stress state) to make intelligent decisions about power delivery based on:
- **Long-term battery health** - Progressive derating as pack ages
- **Stress and temperature history** - Reduces power after demanding driving
- **Dynamic coupling** - Uses state and stress indicators
- **Cell-level health** - Protects weak cells in multi-cell packs
- **Thermal constraints** - Candidate derating based on model inputs; no shutdown-prevention guarantee

---

### Key Features

### 🚗 Multi-Mode Operation
- **Drive Modes**: ECO, NORMAL, SPORT, CUSTOM
- **Regen Modes**: LOW, MEDIUM, HIGH, ADAPTIVE
- Smooth mode transitions with configurable time constants
- DS-adaptive regen that optimizes for battery health

### 🔥 Advanced Thermal Management
- Real-time thermal modeling for motor, inverter, and battery
- Predictive derating based on thermal trajectory
- Component-specific temperature limits
- Cold weather performance optimization

### 🛡️ Comprehensive Safety Systems
- Multi-layer SOC protection (normal and critical limits)
- Automatic limp mode for critical battery states
- Torque rate limiting for smooth, predictable behavior
- Independent safety checks for all critical parameters

### 🔋 Cell-Level Intelligence
- Integration with multi-cell pack diagnostics
- Weak cell detection and protection
- Voltage imbalance compensation
- Dynamic derating based on pack health distribution

### ⚡ Performance Features
- **Overboost Mode**: Temporary power increase (configurable duration)
- **Launch Control**: Maximum acceleration from standstill
- **Dual-Motor Support**: Independent front/rear torque distribution
- **Traction Control Integration**: Hooks for slip control systems

### 📊 Rich Diagnostics
- Real-time performance metrics (avg/peak torque, power, efficiency)
- Thermal tracking (motor, inverter, battery temperatures)
- Derate event logging and limiting factor identification
- Microsecond-precision timing measurements

---

### Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    DS Torque Manager                           │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐    │
│  │  DS Battery │───▶│   Torque     │───▶│    Motor     │    │
│  │    State     │    │  Computation │    │   Commands   │    │
│  └──────────────┘    └──────────────┘    └──────────────┘    │
│         │                    │                    │            │
│         ▼                    ▼                    ▼            │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐    │
│  │  Health &    │    │  Thermal     │    │ Front/Rear   │    │
│  │  Degradation │    │  Model       │    │ Distribution │    │
│  └──────────────┘    └──────────────┘    └──────────────┘    │
│         │                    │                    │            │
│         └────────────────────┴────────────────────┘            │
│                              │                                 │
│                              ▼                                 │
│                    ┌──────────────────┐                        │
│                    │   Diagnostics    │                        │
│                    │   & Logging      │                        │
│                    └──────────────────┘                        │
└─────────────────────────────────────────────────────────────────┘
```

---

### Torque Computation Pipeline

The module uses a multi-factor scaling approach:

```
Final Torque = Base Motor Torque × Combined Scaling Factor

Combined Scaling = 
    Base Motor Curve (speed-dependent)
  × DS Health Factor (remaining capacity)
  × DS Stress Factor (historical stress)
  × DS Dynamic Coupling Factor
  × Thermal Factor (motor + inverter + battery)
  × SOC Factor (state of charge protection)
  × Cell Balance Factor (weak cell protection)
  × Drive Mode Factor (ECO/NORMAL/SPORT)
  × Overboost Factor (if active)
```

These are modeled factors, not a physical safety proof. Actual actuation authority must come from an independently assessed host system.

---

### Integration Examples

**Schematic simulation pseudocode:** The following snippets contain host placeholders and may not compile against the current API. They illustrate intent, not approved actuator integration. For a built example, start with [`examples/basic_integration.cpp`](examples/basic_integration.cpp).

### Basic Integration (Single Motor, Simple BMS)

```cpp
#include "torque_enhancement.hpp"
#include "ds_bms_middleware_v2.hpp"

// Initialize BMS
ds_plugin::DSBMSMiddleware bms;
bms.init(75.0, 400.0);  // 75Ah, 400V

// Configure torque manager
ds::drive::TorqueConfig config;
config.drive_mode = ds::drive::DriveMode::NORMAL;
config.drivetrain.rear_motor.peak_torque_nm = 400.0;
config.battery.max_discharge_power_kw = 250.0;

ds::drive::DSTorqueManager torque_mgr(config);

// Hypothetical simulation loop; timing is not validated on vehicle hardware:
void control_loop() {
    double dt = 0.01;  // 10ms
    
    // Update BMS with sensor data
    auto enhanced = bms.enhance_cycle(voltage, current, temperature, soc, dt);
    
    // Get torque limit
    double motor_rpm = read_motor_speed();
    auto result = torque_mgr.compute_torque_limit(enhanced, motor_rpm, dt);
    
    // Apply limit to driver request
    double driver_request = read_accelerator_pedal() * 400.0;
    double commanded_torque = std::min(driver_request, 
                                      result.max_drive_torque_nm);
    
    // Simulation only: modeled torque; do not send to a real motor controller.
    (void)commanded_torque;
    
    // Handle warnings
    if (result.thermal_derate_active) {
        show_warning("Thermal limiting active");
    }
}
```

### Advanced Integration (Multi-Cell Pack, Dual Motor)

```cpp
// Initialize multi-cell pack BMS
ds_plugin::MiddlewareConfig bms_cfg;
bms_cfg.mode = ds_plugin::MiddlewareConfig::Mode::MULTI_CELL_PACK;
bms_cfg.chemistry = ds::advanced::ChemistryType::NMC;
bms_cfg.series_cells = 96;
bms_cfg.enable_kalman_filter = true;

ds_plugin::DSBMSMiddleware bms;
bms.init_advanced(bms_cfg);

// Configure dual-motor torque manager
ds::drive::TorqueConfig config;
config.drivetrain.has_front_motor = true;
config.drivetrain.has_rear_motor = true;
config.drivetrain.front_motor.peak_torque_nm = 300.0;
config.drivetrain.rear_motor.peak_torque_nm = 400.0;
config.ds_weights.enable_cell_aware_limiting = true;

ds::drive::DSTorqueManager torque_mgr(config);

// In control loop:
void advanced_control_loop() {
    std::vector<double> cell_voltages = read_all_cell_voltages();
    std::vector<double> cell_temps = read_all_cell_temperatures();
    double pack_current = read_pack_current();
    
    // Update multi-cell pack
    bms.update_pack(cell_voltages, cell_temps, pack_current, dt);
    
    // Get pack diagnostics for cell-aware limiting
    auto pack_diag = bms.get_diagnostics();
    auto health = bms.get_health_forecast(100.0);
    
    // Compute torque with cell awareness
    auto result = torque_mgr.compute_torque_limit(enhanced, motor_rpm, 
                                                  dt, &pack_diag);
    
    // Apply front/rear split
    double front_torque = result.max_drive_torque_nm * 
                         result.front_torque_fraction;
    double rear_torque = result.max_drive_torque_nm * 
                        result.rear_torque_fraction;
    
    // Simulation only: record modeled front/rear outputs; do not actuate.
    (void)front_torque;
    (void)rear_torque;
    
    // Handle weak cells
    if (result.weak_cell_derate_active) {
        auto weak_cells = bms.get_weak_cells();
        // Any cell balancing requires a separately validated host controller.
    }
}
```

### Launch Control Example

```cpp
// Enable performance features
config.enable_launch_control = true;
config.enable_overboost = true;
config.overboost_duration_s = 15.0;
config.drive_mode = ds::drive::DriveMode::SPORT;

ds::drive::DSTorqueManager torque_mgr(config);

// Launch control logic
bool launch_armed = false;

void launch_control_handler() {
    // Arm launch control
    if (vehicle_stopped() && brake_pressed() && throttle_full()) {
        launch_armed = true;
        show_message("Launch Control Armed");
    }
    
    // Execute launch
    if (launch_armed && !brake_pressed()) {
        auto result = torque_mgr.compute_torque_limit(enhanced, 0.0, dt);
        
        if (result.overboost_active) {
            // Simulated overboost candidate, NOT a physical launch command
            commanded_torque = result.max_drive_torque_nm;
            show_message("OVERBOOST ACTIVE");
        }
        
        launch_armed = false;
    }
}
```

---

### Configuration

### Drive Modes

| Mode | Power Fraction | Behavior |
|------|---------------|----------|
| **ECO** | 60% | Gentle acceleration, maximum efficiency |
| **NORMAL** | 85% | Balanced performance and efficiency |
| **SPORT** | 100% | Maximum performance, aggressive response |
| **CUSTOM** | User-defined | Custom tuning parameters |

### Regen Modes

| Mode | Behavior |
|------|----------|
| **LOW** | Minimal regenerative braking, coast-like feel |
| **MEDIUM** | Moderate one-pedal driving |
| **HIGH** | Aggressive one-pedal, maximum energy recovery |
| **ADAPTIVE** | DS-optimized based on battery health and temperature |

### DS Tuning Weights

```cpp
struct DSTorqueWeights {
    double health_influence = 0.40;          // 40% influence from health
    double degradation_influence = 0.25;     // 25% from degradation rate
    double entropy_influence = 0.20;         // 20% from stress history
    double metric_stress_influence = 0.15;   // 15% from dynamic coupling
    
    double min_torque_fraction = 0.20;       // Always allow ≥20% torque
    double max_ds_derate = 0.70;            // Max 70% DS reduction
};
```

These are research configuration parameters; impact on physical battery performance or longevity is not validated.

---

### Diagnostics and Monitoring

### Real-Time Metrics

```cpp
auto diag = torque_mgr.get_diagnostics();

std::cout << "Average Torque: " << diag.average_torque_nm << " Nm\n";
std::cout << "Peak Torque: " << diag.peak_torque_nm << " Nm\n";
std::cout << "Average Power: " << diag.average_power_kw << " kW\n";
std::cout << "Total Energy: " << diag.total_energy_kwh << " kWh\n";
std::cout << "Regen Energy: " << diag.regen_energy_kwh << " kWh\n";
std::cout << "Average Efficiency: " << diag.average_efficiency * 100 << "%\n";
std::cout << "Derate Events: " << diag.derate_event_count << "\n";
```

### Status Summary

```cpp
std::cout << torque_mgr.get_status_summary();
```

Output:
```
=== DS Torque Manager Status ===
Drive Mode: SPORT
Regen Mode: ADAPTIVE

Diagnostics:
  Average Torque: 285.4 Nm
  Peak Torque: 400.0 Nm
  Average DS Scaling: 92.3%
  Derate Events: 12
  Motor Temp: 78.5 °C
  Inverter Temp: 65.2 °C
```

### Limiting Factor Analysis

```cpp
auto result = torque_mgr.compute_torque_limit(enhanced, motor_rpm, dt);

std::cout << "Limiting Factor: " << result.limiting_factor << "\n";
std::cout << "Active Derates:\n";
if (result.health_derate_active) std::cout << "  - Health\n";
if (result.thermal_derate_active) std::cout << "  - Thermal\n";
if (result.entropy_derate_active) std::cout << "  - Stress\n";
if (result.soc_derate_active) std::cout << "  - SOC\n";
if (result.weak_cell_derate_active) std::cout << "  - Weak Cells\n";
```

---

### Safety Features

### Multiple Protection Layers

1. **Hard Limits**: Absolute maximum values that cannot be exceeded
2. **Soft Limits**: Gradual derating as limits are approached
3. **Rate Limiting**: Prevents sudden torque changes
4. **Fault Detection**: Automatic shutdown on critical faults
5. **Limp Mode**: Minimal power delivery in emergency situations

### SOC Protection

```
100% ──────────────────────────────── No regen above 95%
 95% ────────────────────────┐        
 90% ────────────┐           │ Gradual regen reduction
     Normal      │           │
     Operation   │           │
 10% ────────────┘           │
  5% ────────────────────────┘ Limp mode below 5%
  0% ──────────────────────────────── 
```

### Thermal Protection

```
Temperature (°C)    Action
───────────────────────────────────────
   < 0          │  Cold weather derating
 0 - 25         │  Optimal performance
25 - 45         │  Normal operation
45 - 60         │  Gradual thermal derating
   > 60         │  Aggressive derating
   > 70         │  Emergency shutdown
```

---

### Performance Characteristics

Outputs depend on the configured battery/motor envelope and input sequence. A fixed example table cannot establish universally safe available torque.

**No verified worst-case execution time, emergency-stop latency, or battery-life improvement is provided by the current software tests.** Previous example figures (including sub-100-µs timing and 15–25% lifetime benefit) are not independent performance evidence.

### Benefits and Research Questions

DS-EV explores whether battery-health, weak-cell and stress-history signals improve modeled torque governance. Comparative physical battery and vehicle outcomes remain unverified.

### Requirements

- C++17 and supported CMake toolchain.
- Current DS core/middleware interfaces; optional Python embedding requires a separate build setting.
- Simulation/research use only; hardware timing, concurrency and functional safety demand application-specific evidence.

---

### Thread Safety

⚠️ **Not thread-safe by default.** The torque manager maintains internal state and should be called from a single control thread. If multi-threaded access is required, implement external synchronization.

---

## AILEE Automotive Governance Layer

### Overview

The **AILEE Automotive Governance Layer** (`ds_core/python/ailee` and C++ `ds_ailee_governor`) is a reference software trust and protection boundary for modeled signals, scores and torque limits. Its governance evaluations are **not** a certified defense against physical vehicle faults or malicious command manipulation.

### Governance Levels & Derating Matrix

AILEE evaluates vehicle state and trust scores in real time and classifies operations into four distinct governance levels (configured via `config/ailee_automotive_config.yaml`):

| Level | Name | Cap Fraction (HP / Torque / Current) | Description |
|-------|------|--------------------------------------|-------------|
| **Level 0** | **Normal Operation** | **1.00 (100%)** | Full performance envelope authorized. High signal trust and HP consistency. |
| **Level 1** | **Soft Ceiling** | **0.90 (90%)** | Soft derating applied when mild signal drift or thermal soft limits are approached. |
| **Level 2** | **Hard Ceiling** | **0.65 (65%)** | Hard derating applied when consistency or trust scores experience moderate anomalies. |
| **Level 3** | **Protective Mode** | **0.25 (25%)** | Protective fallback mode applied when trust drops below critical threshold (e.g., < 0.65). |

### Key Features & Components

- **C++ Native Governor (`AileeHorsepowerGovernor`)**: High-performance C++ execution engine that computes mechanical horsepower ($HP_{mech} = \frac{\tau \times RPM}{7121.23}$), electrical horsepower ($HP_{elec} = \frac{V \times I}{745.7}$), and signal consistency scores.
- **Optional Python trust pipeline (`AileeTrustPipeline`)**: Embedding is disabled in the default CMake configuration and requires an optional build; no zero-latency guarantee is established.
- **Software audit output**: Configured logging can record evaluated governance decisions; these logs are not independently authenticated vehicle black-box recordings.
- **Native evaluation path**: A C++ path exists without optional Python embedding; refer to the regression reports for its precise rejected-evidence behavior.

### C++ Integration Example

```cpp
#include "ailee_horsepower_governor.hpp"

// Initialize governor
AileeHorsepowerGovernor governor;

// Prepare vehicle raw signals
RawSignals signals;
signals.torque_nm = 400.0;
signals.rpm = 4000.0;
signals.v_batt = 400.0;
signals.i_batt = 400.0;
signals.ctx.soc = 85.0;
signals.ctx.soh = 95.0;
signals.ctx.temp_c = 30.0;
signals.ctx.sensor_valid = true;

// Evaluate governance policy
GovernanceDecisionCpp decision = governor.evaluate(signals);

std::cout << "Governance Level: " << decision.level << "\n";
std::cout << "Governed HP: " << decision.governed_hp << " HP\n";
std::cout << "Governed Torque: " << decision.governed_torque << " Nm\n";
std::cout << "Trust Score: " << decision.trust_score << "\n";
std::cout << "Reason: " << decision.reason << "\n";
```

### Python Integration Example

```python
from ailee.core_min import AileeTrustPipeline
from ailee.domains.automotive.ailee_automotive_domain import AileeAutomotiveDomain

# Instantiate trust pipeline with automotive domain
domain = AileeAutomotiveDomain(config_path="config/ailee_automotive_config.yaml")
pipeline = AileeTrustPipeline(domain=domain)

# Process signal evaluation
context = {
    "hp_mech": 224.68,
    "hp_elec": 214.56,
    "soc": 85.0,
    "soh": 95.0,
    "temp_c": 30.0,
    "sensor_valid": True
}

decision = pipeline.evaluate(context)
print(f"Level: {decision.level}, Governed HP: {decision.governed_hp}, Trust: {decision.trust_score}")
```

---

## BMS Middleware, Hardware Adapters & OEM Integration

DS-EV includes BMS middleware and **reference** ECU/hardware integration scaffolding, intended for software evaluation rather than direct vehicle installation or actuation.

### 🔧 DS BMS Middleware v2.0

A unified integration layer that connects:

- DS core models
- Multi-cell pack intelligence
- Safety monitoring & diagnostics
- Vehicle systems (e.g. torque management)

Key capabilities:

- Single-pack and full multi-cell operation
- Deterministic diagnostics (`DiagnosticReport`)
- Selected fail-closed sensor/evidence boundaries verified in the v8 engineering reports
- Clean API for torque, power, and UI systems
- C++17/header-based components; real-time deadlines require target-specific measurement

📁 `include/ds_bms_middleware_v2.hpp`

### 🔌 Reference Hardware Adapter

A software reference illustrating integration boundaries for:

- CAN-based pack telemetry
- Optional SPI/I2C cell monitor ICs
- Contactor and balancing commands
- Time sources and safety checks

This is **not a validated OEM integration**. Real CAN/DBC mapping, diagnostics, transport authenticity and actuator authority require independent host-specific engineering.

📁 `src/ds_bms_hardware_adapter.hpp`

### 📡 OEM CAN Mapping Reference

A clear, neutral CAN mapping document defining:

- Required pack-level signals
- Example CAN IDs and scaling
- Freshness and safety expectations
- Actuator command semantics
- Integration checklist for deployment

📁 `docs/oem_can_mapping.md`

---

## 🛑 Intelligent Regenerative Braking (DS Regen Module)

The repository includes a **DS-based regenerative braking reference manager** for simulated battery-aware torque limits.

The module computes **real-time, battery-aware regen torque limits** and a **recommended regen–friction blend**, using:
- state of charge and voltage headroom  
- battery temperature and charge acceptance  
- cell imbalance and weak-cell indicators  
- DS stress and confidence metrics
- ABS / ESC cooperation signals  

Any physical design must give independent vehicle brake, ABS and ESC systems priority. **DS-EV does not establish hardware arbitration, physical braking response times, or actuator safety.**

---

📁 **Files:**  

- `include/ds_regen_braking_manager_v1.hpp`
- `examples/regen_braking_loop.cpp`
- `docs/regen_braking_overview.md`


This completes the DS control loop:
**Battery Intelligence → Torque Intelligence → Braking Intelligence → Battery Health**

## 🔄 Closed-Loop Energy Recovery (Battery → Torque → Braking → Battery)

Version **v1.3.0** completes the DS energy control loop by treating regenerative braking as an active, battery-aware charging event.

The software models battery-side energy updates and regen recommendations subject to policy limits. This is not evidence of measured physical recovery or safe charging.

A real implementation would require independently validated OEM brake/ESC arbitration and battery charging constraints. These are software integration concepts.

**Design goal:** Coherent software accounting for energy delivery and modeled recovery; remaining accounting questions are tracked in the remediation backlog.

---

## 📡 Optional Telemetry Interface

The DS stack exposes an optional, read-only telemetry snapshot designed to make OEM integration easier without imposing any dashboard or UI assumptions.

The telemetry struct provides a stable summary of:
- battery state (SOC / SOH)
- power flow (drive vs regenerative)
- recovered energy
- key DS metrics (stress, confidence)
- active limiting factors

This interface is intended for dashboards, CAN mapping, logging, or cloud pipelines and does not participate in control decisions.

Visualization, UX, and presentation remain fully owned by the automaker.

---

## 📁 Repository Structure

```
DS-EV/
├── include/                              # Public header-only API
│   ├── ds_battery_enhancement.hpp       # Core DS dual-state engine
│   ├── ds_battery_core.hpp              # Foundational battery types and state
│   ├── ds_bms_interfaces.hpp            # Abstract BMS interface definitions
│   ├── ds_bms_middleware.hpp            # BMS middleware v1
│   ├── ds_bms_middleware_v2.hpp         # BMS middleware v2 (multi-cell, diagnostics)
│   ├── ds_advanced_features.hpp         # Chemistry profiles, Kalman, multi-cell pack
│   ├── torque_enhancement.hpp            # DS Torque Enhancement Module v2.0
│   ├── ds_regen_braking_manager_v1.hpp  # Intelligent regenerative braking manager
│   ├── ds_energy_recovery_coordinator_v1.hpp  # Closed-loop energy recovery
│   ├── ds_energy_stack.hpp              # Layered energy accounting stack
│   ├── ds_energy_telemetry.hpp          # Read-only telemetry snapshot interface
│   ├── ds_rest_api.hpp                  # Optional REST API surface
│   └── battery_feen_adapter/
│       └── battery_feen_adapter.hpp      # Third-party battery adapter shim
├── src/
│   └── ds_bms_hardware_adapter.hpp      # OEM hardware adapter (CAN/SPI/I2C)
├── examples/                             # Reference integration examples
│   ├── basic_integration.cpp             # Minimal single-pack DS usage (CI smoke test)
│   ├── simple_bms_loop.cpp               # Simple BMS control loop
│   ├── multi_cell_pack.cpp               # Multi-cell pack with advanced features
│   ├── multicell_pack_loop.cpp           # 96s pack loop with weak-cell detection
│   ├── regen_braking_loop.cpp            # Regen braking integration example
│   ├── closed_loop_regen_charging.cpp    # Closed-loop energy recovery example
│   ├── rest_api_server.cpp               # Embedded REST API server example
│   └── rest_api_client.py                # Python REST API client example
├── tests/                                # Unit test suite
│   ├── test_core.cpp
│   ├── test_advanced.cpp
│   ├── test_middleware.cpp
│   └── test_feen_integration.cpp
├── benchmarks/                           # Performance benchmarks
│   ├── benchmark_update.cpp
│   └── benchmark_multicell.cpp
├── docs/                                 # Reference documentation
│   ├── architecture_overview.md
│   ├── oem_can_mapping.md
│   ├── oem_quick_start.md
│   ├── regen_braking_overview.md
│   ├── REST_API.md
│   └── simulation_observations.md
├── cmake/                                # CMake helpers
├── CMakeLists.txt
└── LICENSE
```

---

## 🔄 Closed-Cycle Energy Recovery

Traditional EV systems treat driving, braking, and charging as loosely connected subsystems.  
With **DS v1.3.0**, these phases are unified into a **continuous energy cycle** where energy is intentionally recovered, conditioned, and reintegrated into the battery state.

**Battery → Torque → Braking → Battery**

This does not create energy.  
It **reduces dependence on external charging** by maximizing *healthy* energy recovery during normal vehicle operation.

---

## 🛑 Regen as Controlled Charging

In v1.3.0, regenerative braking is treated as a **bounded charging event**, not a passive side effect.

Charging is dynamically constrained by:
- state of charge headroom
- pack voltage limits
- temperature
- cell imbalance and weak-cell protection
- DS stress and confidence metrics

If any constraint is violated, regen is smoothly reduced or disabled.

---

## 🔁 From “Closed Loop” to “Energy Cycle”

Because braking energy is continuously reintegrated into the battery model, the system behaves as a **cycle**, not a one-way control loop.

```
Battery State
↓
Available Power & Health Limits
↓
Torque Delivery
↓
Vehicle Kinetics
↓
Regenerative Braking
↓
Controlled Charging
↓
Battery State Update
```

The battery is no longer treated as a component that only depletes and is later recharged — it becomes an **active participant in the energy flow** of the vehicle.

---

## 📉 Reduced External Charging Demand

DS does **not** eliminate the need for external charging.
It **reduces how often and how deeply external charging is required**.

In typical mixed-use driving, a significant portion of energy normally lost to braking is recovered and reintegrated *without accelerating degradation*.

### Example Recovery Scenarios (75Ah / 400V Pack)

| Scenario | Energy Recovered | SOC Gain | Primary Limit |
|--------|------------------|----------|---------------|
| Urban stop-and-go (5 min) | ~0.32 kWh | +0.43% | Voltage |
| Highway decel (90→40 km/h) | ~0.18 kWh | +0.24% | Power |
| Mountain descent (3 km) | ~0.85 kWh | +1.1% | Thermal |
| Weak cell present | ~0.54 kWh | +0.7% | Cell protection |

Over daily operation, this reduces net energy drawn from chargers and extends usable driving range between plug-in events.

---

## 🧠 What This Enables

- Fewer deep discharge cycles  
- Reduced charging frequency for daily driving  
- Improved battery longevity  
- Predictable, health-aware energy recovery  
- A complete, optimized EV energy cycle

This is not “more regen.”  
It is **better energy management**, powered by the Dual-State (DS/ds) EV Enhancement framework.

---

## ⚠️ Important Note

DS does **not** violate conservation of energy and does **not** claim perpetual motion.
External charging remains necessary.  
DS simply ensures that energy already paid for during motion is **not unnecessarily wasted**.



---

### 🏗️ Architecture Overview

A system-level view explaining how hardware, middleware, DS physics models, and vehicle control connect — including multi-layer safety enforcement and deployment modes from bench testing to production EV packs.

📁 `docs/architecture_overview.md`

### ⚡ OEM Quick Start

A concise, manufacturer-focused guide covering:

- What must be implemented
- Where to plug in hardware
- How to reach a running integration in 15–30 minutes

📁 `docs/oem_quick_start.md`

### 🧪 Canonical Examples

Reference examples demonstrating correct usage patterns:

- Simple pack loop (single-pack equivalent): `examples/simple_bms_loop.cpp`
- Multi-cell pack loop (96s EV-style pack, weak-cell detection): `examples/multicell_pack_loop.cpp`

---

## Support & Feedback

For issues, questions, or feature requests, please refer to the main DS project repository.

**Note**: This is an intelligent power management system. Always perform thorough validation and safety testing before deploying in production vehicles.


## ⚡ Simulation-Only Quick Start

Run DS-EV on a development computer with **simulated numeric inputs only**. This is not an EV-owner installation guide or a live vehicle-diagnostic application.

**Prerequisites:** C++17 compiler, CMake 3.15+, Python 3.8+ for the Python test suite, and Git. The example commands use a Unix-like shell.

### Clone and build

```bash
git clone https://github.com/dfeen87/DS-EV.git
cd DS-EV
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDS_ENABLE_PYTHON_GOVERNOR=OFF
cmake --build build --parallel 4
```

### Run simulation and tests

```bash
./build/basic_integration
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p 'test_*.py' -v
```

The optional pybind11 extension is tested in a separate [CI job](.github/workflows/ci.yml); the default Python test discovery may record an optional extension skip.

### Use the actual numeric-input CLI

CMake builds `ds_enhancer`, historically output to `ds_core/bin/`. It accepts battery *numbers*, not vehicle-profile commands such as `ds --vehicle ... --diagnostics` or `--enhance`.

```bash
./ds_core/bin/ds_enhancer --help
./ds_core/bin/ds_enhancer --version
./ds_core/bin/ds_enhancer --voltage 360 --current 10 --temp 25 --soc 0.8 --dt 1 --json
```

Outputs are **software estimates**, not measured real-vehicle battery health or safe charging instructions.

### Software maintenance is separate and privileged

Do **not** run `make install` or `make update` as part of the simulation quick start. v8.0.2 software updates require an independently pinned authenticated HTTPS payload, protected diagnostic evidence, validated backup and review. A shipped null digest deliberately blocks updates. Legacy installations require administrator-led [offline maintenance migration](docs/UPDATER_LEGACY_MIGRATION.md) first. See [updater engineering requirements](docs/RELEASE_8.0.2.md) and [installation scope](docs/INSTALL.md).

---

## 🔧 Quick Start C++ Guide

### Installation

```cpp
// Just include the header
#include "ds_battery_enhancement.hpp"
```

### Basic Usage

```cpp
#include "ds_battery_enhancement.hpp"

int main() {
    // 1. Create and configure DS enhancement
    ds::DSEnhancement ds;
    ds::DSConfig config;
    config.nominal_capacity_ah = 75.0;  // Your battery capacity
    config.nominal_voltage = 400.0;     // Your battery voltage
    ds.init(config);
    
    // 2. In your BMS update loop
    while (battery_running) {
        // Read sensors (your existing code)
        double voltage = read_voltage();
        double current = read_current();
        double temperature = read_temperature();
        double soc = calculate_soc();
        
        // Get DS enhancement (ADD THIS LINE)
        auto enhanced = ds.enhance(voltage, current, temperature, soc, 0.1);
        
        // Use enhanced predictions
        if (enhanced.degradation_warning) {
            trigger_maintenance_alert();
        }
        
        double remaining_capacity = enhanced.health.remaining_capacity_percent;
        double optimal_charge_current = enhanced.charging.recommended_current_limit;
        
        // Continue with your BMS logic...
    }
    
    return 0;
}
```

**That's it.** Two lines of code for DS enhancement.

---

## 📖 Integration Guide

### Step 1: Add to Your BMS Class

```cpp
class BatteryManagementSystem {
private:
    ds::DSEnhancement ds_;  // Add this
    // ... your existing members
    
public:
    void initialize() {
        // Configure DS for your battery
        ds::DSConfig config;
        config.nominal_capacity_ah = BATTERY_CAPACITY;
        config.nominal_voltage = BATTERY_VOLTAGE;
        config.lambda = 1e-6;  // Coupling strength
        ds_.init(config);
    }
    
    void update_cycle(double dt) {
        // Your existing sensor reads
        double v = read_voltage();
        double i = read_current();
        double t = read_temperature();
        double soc = calculate_soc();
        
        // Get DS enhancement
        auto enhanced = ds_.enhance(v, i, t, soc, dt);
        
        // Now you have access to:
        // - enhanced.health.remaining_capacity_percent
        // - enhanced.health.cycles_to_80_percent
        // - enhanced.health.estimated_eol_cycles
        // - enhanced.charging.recommended_current_limit
        // - enhanced.charging.recommended_voltage_limit
        // - enhanced.degradation_warning
        
        // Use these to improve your control logic
        update_charging_profile(enhanced.charging);
        check_health_warnings(enhanced.health);
        
        // Continue with existing logic...
    }
};
```

### Step 2: Tune Parameters (Optional)

```cpp
ds::DSConfig config;

// Core DS parameters
config.lambda = 1e-6;              // Coupling strength (1e-7 to 1e-5)
config.tau_min = 0.01;             // Minimum update interval (seconds)
config.phi_decay_rate = 0.001;     // Information decay rate
config.entropy_weight = 0.5;       // Entropy contribution weight

// Battery-specific
config.nominal_capacity_ah = 75.0;
config.nominal_voltage = 400.0;
config.max_temperature = 60.0;
```

**Tuning tips:**
- Higher `lambda` → stronger coupling → more sensitive to degradation
- Lower `tau_min` → faster updates → higher CPU usage
- Adjust `entropy_weight` based on chemistry (higher for high-temp chemistries)

---

## 🧪 Validation & Testing

### Compile and Run Example

```bash
# Compile the example
g++ -std=c++17 -O3 -DDS_EXAMPLE_MAIN ds_battery_enhancement.hpp -o ds_demo

# Run simulation
./ds_demo
```

**Output:**
```
=== DS Battery Enhancement Demo ===

Simulating 1000 charge cycles...

Cycle 0:
  Degradation: 0.00%
  Remaining Capacity: 100.00%
  Cycles to 80%: 2000.0
  DS Confidence: 100.00%

Cycle 100:
  Degradation: 1.50%
  Remaining Capacity: 98.50%
  Cycles to 80%: 1850.0
  DS Confidence: 98.50%
  
...

=== Final Health Assessment ===
Predicted remaining capacity: 90.23%
Estimated cycles to EOL: 1547
Prediction confidence: 89.45%
```

### Unit Tests

```bash
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

### Benchmarks

```bash
cmake -B build -DDS_BUILD_BENCHMARKS=ON && cmake --build build
./build/benchmark_update
./build/benchmark_multicell
```

---

## ✅ Continuous Integration

[GitHub Actions CI](.github/workflows/ci.yml) provides **software-only** checks:

- **Default build/test job:** C++17 compilation, CTest integration/regression suites, Python `unittest` discovery, and a `basic_integration` simulation smoke test.
- **Optional extension job:** separate pybind11 build and smoke check.
- **Additional evidence:** Debug/sanitizer/static-analysis and performance measurements are documented when run; they are not implied by every standard CI build.

Reproduce the standard checks:

```bash
cmake -S . -B build -DDS_ENABLE_PYTHON_GOVERNOR=OFF
cmake --build build
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p 'test_*.py' -v
./build/basic_integration
```

**CI cannot establish** real sensor authenticity, calibrated health/charging predictions, bounded hardware deadlines, CAN/ECU integration, HIL safety, power-loss-safe deployment, or automotive safety certification.

---

## 🎯 Use Cases

### Early Degradation Detection
**Problem:** By the time traditional BMS detects accelerating degradation, significant damage is done.

**DS Solution:** Monitors physical and historical stress indicators to catch accelerating degradation 20-30% earlier.

**Impact:** Extended warranty periods, reduced field failures, better residual value prediction.

---

### Optimized Fast Charging
**Problem:** Fast charging degrades batteries, but optimal charging profiles are chemistry-specific and hard to model.

**DS Solution:** Computes dynamic charging profiles that minimize degradation.

**Impact:** Faster charging with less degradation, personalized charging curves per vehicle.

---

### Predictive Maintenance
**Problem:** Current BMS can estimate remaining capacity but struggles with sudden failure modes.

**DS Solution:** Tracks stress accumulation and performance stability to detect pre-failure signatures.

**Impact:** Schedule maintenance before failures, reduce roadside breakdowns, improve fleet management.

---

### Second-Life Battery Assessment
**Problem:** Hard to accurately assess degraded batteries for second-life applications (grid storage, etc.).

**DS Solution:** Assessment powered by complete historical stress modeling. Better remaining-capacity and reliability estimates.

**Impact:** Unlock second-life battery markets, reduce waste, improve circular economy.

---

## 🏗️ Architecture

### Module Structure

```
ds_battery_enhancement.hpp
├── Constants & Configuration (DSConfig)
├── Matrix4x4 (lightweight metric calculations)
├── DSState (dual-state representation)
├── Prediction Results (output structures)
├── DSCoupling (core engine)
│   ├── compute_gradients()
│   ├── update_state()
│   ├── compute_energies()
│   ├── predict_health()
│   └── optimize_charging()
└── DSEnhancement (integration interface)
    ├── init()
    ├── enhance() ← Main API
    ├── get_health_forecast()
    └── get_optimal_charging()
```

### Data Flow

```
Sensors → DSState → Coupling Engine → DSState → Predictions
   ↓                        ↓               ↓            ↓
Voltage              update_state()   Energy Balance   Health Forecast
Current                                                Optimal Charging
Temp                                                   Warnings
SoC
```

### Performance Characteristics

- **Algorithmic complexity:** O(1) per update (no iteration, no search)
- **Memory access:** Sequential, cache-friendly
- **Parallelizable:** Each battery pack independent
- **Deterministic timing:** No dynamic allocation in update loop

---

## 🔐 Safety & Reliability

### Built-in Safeguards

- ✅ **Bounds checking** - All state variables clamped to physical ranges
- ✅ **Numerical stability** - Validated for 1M+ update cycles
- ✅ **Exception safety** - No memory leaks, strong exception guarantee

---

## 🗺️ Roadmap

### Software engineering milestones

- **v5–v7:** DS core, torque/regen reference components, middleware, AILEE governance integration, and example programs; historical version claims must be read with their actual verification records.
- **v8.0.0 BEDROCK:** Invariant review and trust-boundary integration; [engineering report](docs/RELEASE_8.0.0.md).
- **v8.0.1 (published):** Middleware safety evidence and protective torque fixes; [release report](docs/RELEASE_8.0.1.md).
- **v8.0.2 (development):** Fail-closed updater and backup controls, manual legacy migration validation, and further numerical/integrity remediation; [development report](docs/RELEASE_8.0.2.md).

### Open validation work

- Resolve [PB-09–PB-15](docs/POST_BEDROCK_REMEDIATION_BACKLOG.md), including recovery accounting, learning/audit integrity, energy ledger/configuration consistency, and fallback validation.
- Calibrate model predictions and quantify benchmarks against reproducible baselines before claiming measurable benefits.
- Pursue hardware-in-the-loop testing, fault injection, real integration assessments, and independent automotive safety review before considering vehicle operation.
- Explore fleet data analysis, embedded optimization, and ML as **research directions** rather than current validated production features.

---

## Citation

For scholarly use, see [`CITATION.cff`](CITATION.cff) or GitHub's **Cite this repository** feature. The citation identifies the code, not a vehicle-certified product.

---

## License

This project is licensed under the **MIT License** - see the [LICENSE](LICENSE) file for full legal details and terms.

---

## 🙏 Acknowledgments

We would like to acknowledge the engineers and developers who contributed to the formulation and testing of the Dual-State battery models.

I would like to acknowledge **Google Jules**, **Microsoft Copilot**, **Anthropic Claude**, and **OpenAI ChatGPT** for their meaningful assistance in refining concepts, improving clarity, and strengthening the overall quality of this work.

---

## Enterprise Consulting & Integration
If your organization requires custom scaling, proprietary integration, or dedicated technical consulting to deploy these models at an enterprise level, please reach out at: dfeen87@gmail.com

---

## 📞 Get Involved

### For Engineers
⭐ **Star this repo** if you find it interesting
🔧 **Try it out** in your BMS and let us know results
🐛 **Report issues** or suggest improvements
📖 **Contribute examples** from different battery types

### For Researchers
📝 **Cite our work** if you use DS in your research
🤝 **Collaborate** on validation studies
📊 **Share results** (anonymized) to improve the model

### For Companies
💼 **Contact us** for integration support
🤝 **Partner** on validation and deployment
🚀 **Build** the next generation of battery management together

---
