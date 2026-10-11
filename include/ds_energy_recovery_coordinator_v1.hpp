/*
 * ============================================================================
 * DS Energy Recovery Coordinator v1.0
 * ============================================================================
 *
 * PURPOSE:
 *   Close the EV energy loop by coordinating regen braking ↔ battery charging.
 *
 *   This module:
 *     1) Takes regen torque limits from the Regen Braking Manager
 *     2) Converts allowed regen torque into an estimated electrical regen power/current
 *     3) Feeds the result into DSBMSMiddleware as a charging event (negative current)
 *     4) Observes accepted battery-side charging energy and separate available-power estimates
 *
 * WHAT THIS MODULE IS:
 *   - A deterministic coordinator between braking control and battery state estimation.
 *   - A software boundary for sign conventions and accepted-cycle accounting.
 *
 * WHAT THIS MODULE IS NOT:
 *   - It is not a motor/inverter model.
 *   - It does not override OEM safety controllers.
 *   - It does not replace current sensors (it can use estimated current only if needed).
 *
 * OEM NOTE:
 *   Best practice is to use actual measured pack current (from sensors) as truth.
 *   This coordinator supports both:
 *     (A) "Measured-current mode" (preferred)
 *     (B) "Estimated-current mode" (simulation / early integration / missing sensors)
 *
 * AUTHORS: Don Michael Feeney Jr. + Lex
 * DATE: December 2025
 * LICENSE: Copyright (c) Don Michael Feeney Jr. Licensed under the MIT License.
 * VERSION: 1.0.0
 * ============================================================================
 */

#ifndef DS_ENERGY_RECOVERY_COORDINATOR_V1_HPP
#define DS_ENERGY_RECOVERY_COORDINATOR_V1_HPP

#include "ds_bms_middleware_v2.hpp"
#include "ds_regen_braking_manager_v1.hpp"
#include <string>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ds {
namespace drive {

constexpr int RECOVERY_VERSION_MAJOR = 1;
constexpr int RECOVERY_VERSION_MINOR = 0;
constexpr int RECOVERY_VERSION_PATCH = 0;

inline std::string energy_recovery_version() {
    return std::to_string(RECOVERY_VERSION_MAJOR) + "." +
           std::to_string(RECOVERY_VERSION_MINOR) + "." +
           std::to_string(RECOVERY_VERSION_PATCH);
}

struct EnergyRecoveryConfig {
    // If true, provide measured pack_current; charging energy is all-source evidence.
    // If false, coordinator estimates pack_current from regen torque (useful for simulation).
    bool prefer_measured_pack_current = true;

    // Efficiency estimates electrical power in both modes; current conversion
    // parameters are used only when prefer_measured_pack_current == false.
    // These are intentionally conservative approximations.
    double drivetrain_efficiency_regen = 0.85;     // electrical recovery efficiency
    double max_estimated_regen_current_a = 250.0;  // sanity clamp for estimated current
    double min_pack_voltage_v = 50.0;              // avoid divide-by-zero

    // Optional smoothing for estimated current
    double current_estimate_tau_s = 0.10;

    // Regen torque to mechanical power conversion:
    // P_mech = T * ω
    // ω must be mechanical shaft angular velocity in rad/s (not RPM or
    // electrical angular frequency). Convert vehicle speed upstream.
};

struct EnergyRecoveryDiagnostics {
    double last_regen_torque_nm = 0.0;
    double last_regen_power_kw_est = 0.0;  // electrical estimate after efficiency, before current limits
    double last_regen_current_a_est = 0.0;

    // Legacy aliases for accepted all-source charging, not proven regeneration.
    double recovered_energy_kwh_total = 0.0;
    double recovered_energy_kwh_session = 0.0;

    std::string mode = "MEASURED_CURRENT";
    std::string limiting_factor = "NONE";

    // Explicit battery-side aliases of the legacy recovered_energy fields.
    // These count accepted charging observed by this coordinator only. They
    // are not added to the BMS ledger, and measured charging has no source tag.
    double accepted_charging_energy_kwh_total = 0.0;
    double accepted_charging_energy_kwh_session = 0.0;
    double last_accepted_voltage_v = 0.0;
    double last_accepted_current_a = 0.0;

    // Available power/energy estimates precede current limits and smoothing;
    // they are neither delivered energy nor demonstrable regenerative recovery.
    double last_mechanical_power_kw_est = 0.0;
    double estimated_available_mechanical_energy_kwh_total = 0.0;
    double estimated_available_mechanical_energy_kwh_session = 0.0;
    double estimated_available_electrical_energy_kwh_total = 0.0;
    double estimated_available_electrical_energy_kwh_session = 0.0;
    std::string charging_energy_source = "UNATTRIBUTED_MEASURED";
};

struct EnergyRecoveryResult {
    // What we used to update the BMS (either measured or estimated)
    double pack_voltage_v = 0.0;
    double pack_current_a_used = 0.0;     // signed: negative = charging, source unproven
    double pack_temperature_c = 0.0;
    double soc_input = 0.0;
    double dt_s = 0.0;

    // Regen metrics
    double regen_torque_nm = 0.0;
    double regen_power_kw_est = 0.0; // electrical estimate, not accepted battery power

    // Updated enhanced state + diagnostics
    ds::EnhancedState enhanced;
    ds_plugin::DiagnosticReport bms_diag;

    EnergyRecoveryDiagnostics diag;
};

class DSEnergyRecoveryCoordinator {
public:
    explicit DSEnergyRecoveryCoordinator(const EnergyRecoveryConfig& cfg = EnergyRecoveryConfig())
        : cfg_(cfg), est_current_a_(0.0) { validate_config(cfg_); }

    static std::string get_version() { return energy_recovery_version(); }

    // Core step:
    //   - Accept regen decision (torque limit + regen fraction)
    //   - Update BMS using measured or estimated pack current
    //
    // Inputs:
    //   bms: DSBMSMiddleware instance
    //   regen: regen result from DSRegenBrakingManager
    //   pack_voltage_v, pack_temperature_c: measured pack values
    //   pack_current_a_measured: signed measured current, or finite placeholder in estimated mode
    //   soc_input: OEM SOC estimate or last SOC (0..1)
    //   motor_omega_rad_s: nonnegative mechanical shaft angular velocity in rad/s;
    //                      zero is the optional no-power-estimate value in either mode
    //
    EnergyRecoveryResult step(
        ds_plugin::DSBMSMiddleware& bms,
        const ds::drive::RegenResult& regen,
        double pack_voltage_v,
        double pack_temperature_c,
        double pack_current_a_measured,
        double soc_input,
        double motor_omega_rad_s,
        double dt_s
    ) {
        // Reject malformed evidence before touching either coordinator history
        // or the BMS. Finite sensor/model clamps in the BMS remain its policy.
        validate_inputs(regen, pack_voltage_v, pack_temperature_c,
                        pack_current_a_measured, soc_input, motor_omega_rad_s, dt_s);

        EnergyRecoveryResult out;
        out.pack_voltage_v = pack_voltage_v;
        out.pack_temperature_c = pack_temperature_c;
        out.soc_input = soc_input;
        out.dt_s = dt_s;
        out.regen_torque_nm = regen.max_regen_torque_nm;

        const double mechanical_w = finite(
            regen.max_regen_torque_nm * motor_omega_rad_s, "Mechanical power overflow");
        const double electrical_w = finite(
            mechanical_w * cfg_.drivetrain_efficiency_regen, "Electrical estimate overflow");
        out.regen_power_kw_est = finite(electrical_w / 1000.0, "Estimated kilowatt overflow");

        auto candidate = diag_;
        candidate.last_regen_torque_nm = out.regen_torque_nm;
        candidate.last_regen_power_kw_est = out.regen_power_kw_est;
        candidate.last_mechanical_power_kw_est = finite(mechanical_w / 1000.0,
                                                       "Mechanical kilowatt overflow");
        candidate.limiting_factor = regen.limiting_factor;

        double candidate_estimate = est_current_a_;
        double current_used = pack_current_a_measured;
        if (!cfg_.prefer_measured_pack_current) {
            candidate.mode = "ESTIMATED_CURRENT";
            candidate.charging_energy_source = "SIMULATED_REGEN_ESTIMATE";
            const double voltage_for_estimate = std::max(cfg_.min_pack_voltage_v, pack_voltage_v);
            const double target_magnitude = std::min(
                finite(electrical_w / voltage_for_estimate, "Estimated current overflow"),
                cfg_.max_estimated_regen_current_a);
            candidate_estimate = smooth_current(-target_magnitude, dt_s);
            current_used = candidate_estimate;
            candidate.last_regen_current_a_est = current_used;
        } else {
            candidate.mode = "MEASURED_CURRENT";
            candidate.charging_energy_source = "UNATTRIBUTED_MEASURED";
            candidate.last_regen_current_a_est = 0.0;
        }
        out.pack_current_a_used = current_used;
        candidate.last_accepted_voltage_v = pack_voltage_v;
        candidate.last_accepted_current_a = current_used;

        // PB-13 acceptance credits raw battery-side V/A, not normalized DS
        // current or unconstrained available power. Apply the same W/kW/kWh
        // intermediate checks before calling BMS; success accepts these exact
        // inputs. Safety flags are observational and do not establish source.
        const double accepted_power_w = finite(pack_voltage_v * current_used,
                                                "Battery-side power overflow");
        const double accepted_energy = energy_kwh(accepted_power_w / 1000.0, dt_s);
        const double charging_energy = current_used < 0.0 ? -accepted_energy : 0.0;
        candidate.accepted_charging_energy_kwh_total = finite(
            diag_.accepted_charging_energy_kwh_total + charging_energy, "Charging total overflow");
        candidate.accepted_charging_energy_kwh_session = finite(
            diag_.accepted_charging_energy_kwh_session + charging_energy, "Charging session overflow");
        candidate.recovered_energy_kwh_total = candidate.accepted_charging_energy_kwh_total;
        candidate.recovered_energy_kwh_session = candidate.accepted_charging_energy_kwh_session;

        const double mechanical_energy = energy_kwh(candidate.last_mechanical_power_kw_est, dt_s);
        const double electrical_energy = energy_kwh(out.regen_power_kw_est, dt_s);
        candidate.estimated_available_mechanical_energy_kwh_total = finite(
            diag_.estimated_available_mechanical_energy_kwh_total + mechanical_energy,
            "Mechanical estimate total overflow");
        candidate.estimated_available_mechanical_energy_kwh_session = finite(
            diag_.estimated_available_mechanical_energy_kwh_session + mechanical_energy,
            "Mechanical estimate session overflow");
        candidate.estimated_available_electrical_energy_kwh_total = finite(
            diag_.estimated_available_electrical_energy_kwh_total + electrical_energy,
            "Electrical estimate total overflow");
        candidate.estimated_available_electrical_energy_kwh_session = finite(
            diag_.estimated_available_electrical_energy_kwh_session + electrical_energy,
            "Electrical estimate session overflow");

        // Allocate the complete return value before BMS acceptance. The only
        // dynamic DiagnosticReport member is vector<int> weak_cell_ids, bounded
        // by the BMS cell count. Reserve that bound so accepted diagnostic copy
        // assignment below cannot allocate. Future allocating diagnostic fields
        // must receive the same preflight treatment before using this boundary.
        out.diag = candidate;
        out.bms_diag.weak_cell_ids.reserve(bms.diagnostic_cell_capacity());
        out.enhanced = bms.enhance_cycle(pack_voltage_v, current_used,
                                        pack_temperature_c, soc_input, dt_s);

        // No fallible arithmetic or allocating copies follow BMS acceptance.
        // This transaction assumes exclusive access to coordinator and BMS
        // throughout step; it does not synchronize concurrent readers/writers.
        out.bms_diag = bms.diagnostics();
        static_assert(std::is_nothrow_copy_assignable<ds::EnhancedState>::value,
                      "Accepted state copy must not throw");
        static_assert(std::is_nothrow_move_assignable<EnergyRecoveryDiagnostics>::value,
                      "Coordinator commit must not throw");
        static_assert(std::is_nothrow_move_constructible<EnergyRecoveryResult>::value,
                      "Returning accepted result must not throw");
        diag_ = std::move(candidate);
        est_current_a_ = candidate_estimate;
        return out;
    }

    const EnergyRecoveryDiagnostics& diagnostics() const { return diag_; }

    void reset_session_energy() {
        diag_.recovered_energy_kwh_session = 0.0;
        diag_.accepted_charging_energy_kwh_session = 0.0;
        diag_.estimated_available_mechanical_energy_kwh_session = 0.0;
        diag_.estimated_available_electrical_energy_kwh_session = 0.0;
    }

private:
    EnergyRecoveryConfig cfg_;
    double est_current_a_;
    EnergyRecoveryDiagnostics diag_;

    static double finite(double value, const char* message) {
        if (!std::isfinite(value)) throw std::overflow_error(message);
        return value;
    }

    static double energy_kwh(double power_kw, double dt_s) {
        return finite(finite(power_kw * dt_s, "Energy multiplication overflow") / 3600.0,
                      "Energy conversion overflow");
    }

    static void validate_config(const EnergyRecoveryConfig& cfg) {
        if (!std::isfinite(cfg.drivetrain_efficiency_regen) ||
            cfg.drivetrain_efficiency_regen < 0.0 || cfg.drivetrain_efficiency_regen > 1.0 ||
            !std::isfinite(cfg.max_estimated_regen_current_a) || cfg.max_estimated_regen_current_a < 0.0 ||
            !std::isfinite(cfg.min_pack_voltage_v) || cfg.min_pack_voltage_v <= 0.0 ||
            !std::isfinite(cfg.current_estimate_tau_s) || cfg.current_estimate_tau_s < 0.0) {
            throw std::invalid_argument("Invalid energy recovery configuration");
        }
    }

    static void validate_inputs(const RegenResult& regen, double voltage, double temperature,
                                double measured_current, double soc, double omega, double dt) {
        if (!std::isfinite(voltage) || voltage <= 0.0 || !std::isfinite(temperature) ||
            !std::isfinite(measured_current) || !std::isfinite(soc) || soc < 0.0 || soc > 1.0 ||
            !std::isfinite(omega) || omega < 0.0 || !std::isfinite(dt) || dt <= 0.0 ||
            !std::isfinite(regen.max_regen_torque_nm) || regen.max_regen_torque_nm < 0.0 ||
            !std::isfinite(regen.regen_fraction) || regen.regen_fraction < 0.0 || regen.regen_fraction > 1.0) {
            throw std::invalid_argument("Invalid energy recovery inputs");
        }
    }

    double smooth_current(double target, double dt) const {
        const double tau = std::max(1e-3, cfg_.current_estimate_tau_s);
        const double alpha = 1.0 - std::exp(-dt / tau);
        return finite(est_current_a_ + alpha * (target - est_current_a_),
                      "Current smoothing overflow");
    }

};

} // namespace drive
} // namespace ds

#endif // DS_ENERGY_RECOVERY_COORDINATOR_V1_HPP
