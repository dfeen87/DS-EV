#ifndef DS_ENERGY_TELEMETRY_HPP
#define DS_ENERGY_TELEMETRY_HPP

#include <string>

namespace ds {

/*
 * ============================================================================
 * DS Energy Telemetry (Optional)
 * ============================================================================
 *
 * PURPOSE:
 *   Provide a minimal, OEM-friendly snapshot of DS energy state for
 *   dashboards, logging, CAN mapping, or cloud pipelines.
 *
 * DESIGN PRINCIPLES:
 *   - No UI assumptions
 *   - No timing assumptions
 *   - No ownership of visualization
 *   - Read-only, snapshot-based
 *
 * This struct is intentionally small and stable.
 * ============================================================================
 */

struct DSEnergyTelemetry {
    // Battery state
    double soc_percent = 0.0;
    double soh_percent = 0.0;

    // Power flow
    double pack_power_kw = 0.0;        // signed: +discharge / -charge
    double regen_power_kw = 0.0;       // legacy name: all-source accepted charging power
    double recovered_energy_kwh = 0.0; // all-source accepted charging, regeneration unproven

    // DS metrics
    double ds_metric_trace = 0.0;
    double ds_entropy = 0.0;
    double ds_confidence = 0.0;

    // Control context
    std::string limiting_factor = "NONE"; // e.g. SOC, THERMAL, CELL, ABS
};

} // namespace ds

#endif // DS_ENERGY_TELEMETRY_HPP
