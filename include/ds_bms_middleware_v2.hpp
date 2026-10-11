#ifndef DS_BMS_MIDDLEWARE_V2_HPP
#define DS_BMS_MIDDLEWARE_V2_HPP

#include "ds_battery_core.hpp"
#include "ds_battery_enhancement.hpp"
#include "ds_advanced_features.hpp"
#include "ds_energy_telemetry.hpp"

#include <vector>
#include <array>
#include <string>
#include <memory>
#include <chrono>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <type_traits>
#include <utility>

namespace ds_plugin {

/* ================= VERSION ================= */

constexpr int MIDDLEWARE_VERSION_MAJOR = 2;
constexpr int MIDDLEWARE_VERSION_MINOR = 0;
constexpr int MIDDLEWARE_VERSION_PATCH = 1;

inline std::string get_middleware_version() {
    return std::to_string(MIDDLEWARE_VERSION_MAJOR) + "." +
           std::to_string(MIDDLEWARE_VERSION_MINOR) + "." +
           std::to_string(MIDDLEWARE_VERSION_PATCH);
}

/* ================= DIAGNOSTICS ================= */

struct DiagnosticReport {
    double pack_soh_percent = 100.0;
    double pack_soc_percent = 100.0;
    double estimated_range_km = 0.0;
    double cycles_to_eol = 0.0;
    double pack_health_percent = 100.0;
    double estimated_remaining_cycles = 0.0;

    int total_cells = 1;
    int weak_cell_count = 0;
    std::vector<int> weak_cell_ids;

    double min_cell_voltage = 0.0;
    double max_cell_voltage = 0.0;
    double voltage_imbalance_mv = 0.0;

    double average_cell_temp_c = 25.0;
    double max_cell_temp_c = 25.0;
    double min_cell_temp_c = 25.0;

    double ds_metric_trace = 2.0;
    double ds_entropy = 0.0;
    double ds_phi_magnitude = 0.0;
    double ds_confidence = 1.0;

    bool weak_cell_warning = false;
    bool thermal_warning = false;
    bool voltage_warning = false;
    bool degradation_warning = false;
    bool balancing_required = false;
    bool safety_fault = false;
    bool soc_warning = false;
    bool low_temp_warning = false;

    double instantaneous_power_kw = 0.0;
    double average_efficiency = 0.95;
    double energy_throughput_kwh = 0.0;
    double last_update_time_ms = 0.0;
    double average_update_time_ms = 0.0;
    double degradation_rate_per_cycle = 0.0;
    double metric_trace = 2.0;
    double phi_magnitude = 0.0;
    double entropy_level = 0.0;

    double time_since_init_s = 0.0;
    int update_count = 0;
};

/* ================= SAFETY LIMITS ================= */

struct SafetyLimits {
    double min_cell_voltage = 2.5;
    double max_cell_voltage = 4.3;
    double min_pack_voltage = 300.0;
    double max_pack_voltage = 450.0;

    double max_discharge_current = 600.0;
    double max_charge_current = 250.0;

    double min_operating_temp = -20.0;
    double max_operating_temp = 60.0;
    double max_cell_temp = 65.0;

    double min_soc = 0.05;
    double max_soc = 0.95;

    double max_voltage_imbalance_mv = 100.0;
    double min_health_percent = 70.0;

    void validate() const {
        const double thresholds[] = {min_cell_voltage, max_cell_voltage,
            min_pack_voltage, max_pack_voltage, max_discharge_current,
            max_charge_current, max_cell_temp, max_voltage_imbalance_mv,
            min_health_percent};
        for (double value : thresholds) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument("Safety thresholds must be finite");
            }
        }
        if (min_cell_voltage < 0.0 || min_cell_voltage > max_cell_voltage ||
            min_pack_voltage < 0.0 || min_pack_voltage > max_pack_voltage ||
            max_discharge_current < 0.0 || max_charge_current < 0.0 ||
            max_voltage_imbalance_mv < 0.0 || min_health_percent < 0.0 ||
            min_health_percent > 100.0) {
            throw std::invalid_argument("Invalid safety threshold domains");
        }
        if (!std::isfinite(min_soc) || !std::isfinite(max_soc)) {
            throw std::invalid_argument("SOC limits must be finite");
        }
        if (min_soc < 0.0 || max_soc > 1.0) {
            throw std::invalid_argument("SOC limits must be between 0.0 and 1.0");
        }
        if (min_soc > max_soc) {
            throw std::invalid_argument("min_soc cannot be greater than max_soc");
        }
        if (!std::isfinite(min_operating_temp) || !std::isfinite(max_operating_temp)) {
            throw std::invalid_argument("Operating temperature limits must be finite");
        }
        if (min_operating_temp > max_operating_temp) {
            throw std::invalid_argument("min_operating_temp cannot be greater than max_operating_temp");
        }
    }
};

/* ================= CONFIG ================= */

struct MiddlewareConfig {
    enum class Mode { SIMPLE, SINGLE_BATTERY = SIMPLE, MULTI_CELL_PACK, ADVANCED_ML };

    Mode mode = Mode::SIMPLE;
    ds::advanced::ChemistryType chemistry = ds::advanced::ChemistryType::NMC;

    double nominal_capacity_ah = 75.0;
    double nominal_voltage = 400.0;

    int series_cells = 96;

    ds::DSConfig ds_config;

    bool enable_kalman_filter = false;
    bool enable_ml_hybrid = false;
    bool enable_cell_balancing = false;

    SafetyLimits safety_limits;
    bool enable_safety_monitoring = true;

    bool enable_logging = true;
    double logging_interval_s = 1.0;
};

/* ================= SAFETY MONITOR ================= */

class SafetyMonitor {
    SafetyLimits limits_;
    bool fault_active_ = false;
    std::vector<std::string> faults_;
    std::vector<std::string> advisories_;
    bool soc_warning_ = false;
    bool low_temp_warning_ = false;

public:
    explicit SafetyMonitor(const SafetyLimits& limits = {}) : limits_(limits) {}

    bool check(const ds::DSState& s, const DiagnosticReport& d) {
        faults_.clear();
        advisories_.clear();
        fault_active_ = false;
        soc_warning_ = false;
        low_temp_warning_ = false;

        // NaN comparisons do not trigger ordinary range checks. Retain invalid
        // raw evidence as a fault, independently of model normalization.
        if (!std::isfinite(s.voltage) || !std::isfinite(s.current) ||
            !std::isfinite(s.temperature) || !std::isfinite(s.state_of_charge))
            faults_.push_back("Nonfinite sensor reading");

        if (s.current > limits_.max_discharge_current)
            faults_.push_back("Discharge current exceeded");

        if (s.current < -limits_.max_charge_current)
            faults_.push_back("Charge current exceeded");

        if (s.voltage < limits_.min_pack_voltage || s.voltage > limits_.max_pack_voltage)
            faults_.push_back("Pack voltage out of bounds");

        if (s.temperature > limits_.max_operating_temp)
            faults_.push_back("Temperature too high");

        if (d.max_cell_temp_c > limits_.max_cell_temp)
            faults_.push_back("Cell temperature critical");

        if (s.state_of_charge < limits_.min_soc) {
            soc_warning_ = true;
            if (s.current > 0.0) {
                faults_.push_back("SOC below minimum operating limit while discharging");
            } else if (s.current < 0.0) {
                advisories_.push_back("SOC below minimum operating limit while charging");
            } else if (s.current == 0.0) {
                advisories_.push_back("SOC below minimum operating limit while resting");
            }
        }

        if (s.state_of_charge > limits_.max_soc) {
            soc_warning_ = true;
            if (s.current < 0.0) {
                faults_.push_back("SOC above maximum operating limit while charging");
            } else if (s.current > 0.0) {
                advisories_.push_back("SOC above maximum operating limit while discharging");
            } else if (s.current == 0.0) {
                advisories_.push_back("SOC above maximum operating limit while resting");
            }
        }

        if (s.temperature < limits_.min_operating_temp) {
            low_temp_warning_ = true;
            if (s.current != 0.0) {
                faults_.push_back("Temperature below minimum operating limit while active");
            } else {
                advisories_.push_back("Temperature below minimum operating limit while resting");
            }
        }

        if (!faults_.empty()) fault_active_ = true;
        return !fault_active_;
    }

    bool has_fault() const { return fault_active_; }
    const std::vector<std::string>& faults() const { return faults_; }

    const std::vector<std::string>& advisories() const { return advisories_; }
    bool has_soc_warning() const { return soc_warning_; }
    bool has_low_temp_warning() const { return low_temp_warning_; }
};

/* ================= MIDDLEWARE ================= */

class DSBMSMiddleware {
    // Only the regression fixture uses this to exercise unreachable-scale
    // accumulated timing and integer-counter boundaries.
    friend struct MiddlewareAccountingTestAccess;

    MiddlewareConfig config_;

    std::unique_ptr<ds::DSEnhancement> ds_core_;
    std::unique_ptr<ds::advanced::MultiCellPack> pack_;
    std::unique_ptr<ds::advanced::KalmanDSFilter> kalman_;

    SafetyMonitor safety_;
    DiagnosticReport diag_ = {};
    ds::EnhancedState enhanced_ = {};

    bool initialized_ = false;
    double time_s_ = 0.0;
    int updates_ = 0;
    double total_update_time_ms_ = 0.0;

    double energy_in_kwh_ = 0.0;
    double energy_out_kwh_ = 0.0;

public:
    DSBMSMiddleware() = default;

    void init(double capacity_ah, double voltage_v) {
        // Preserve existing DS calibration while the simple API supplies both
        // nominal values. A successful initialization starts a fresh session.
        MiddlewareConfig candidate = config_;
        candidate.mode = MiddlewareConfig::Mode::SIMPLE;
        candidate.enable_kalman_filter = false;
        candidate.enable_ml_hybrid = false;
        candidate.enable_cell_balancing = false;
        candidate.nominal_capacity_ah = capacity_ah;
        candidate.nominal_voltage = voltage_v;
        candidate.ds_config.nominal_capacity_ah = capacity_ah;
        candidate.ds_config.nominal_voltage = voltage_v;
        init_advanced(candidate);
    }

    void init_advanced(const MiddlewareConfig& cfg) {
        cfg.safety_limits.validate();
        // Advanced callers must declare the same nominal battery in both
        // layers. Neither field can identify whether the other was calibrated;
        // reject ambiguity rather than silently rewriting model parameters.
        if (!std::isfinite(cfg.nominal_capacity_ah) || cfg.nominal_capacity_ah <= 0.0 ||
            !std::isfinite(cfg.nominal_voltage) || cfg.nominal_voltage <= 0.0 ||
            !cfg.ds_config.validate() ||
            cfg.nominal_capacity_ah != cfg.ds_config.nominal_capacity_ah ||
            cfg.nominal_voltage != cfg.ds_config.nominal_voltage) {
            throw std::invalid_argument("Middleware and DS nominal configuration must be valid and agree");
        }
        if (cfg.mode != MiddlewareConfig::Mode::SIMPLE &&
            cfg.mode != MiddlewareConfig::Mode::MULTI_CELL_PACK &&
            cfg.mode != MiddlewareConfig::Mode::ADVANCED_ML) {
            throw std::invalid_argument("Unknown middleware mode");
        }
        if (!std::isfinite(cfg.logging_interval_s) || cfg.logging_interval_s <= 0.0) {
            throw std::invalid_argument("Logging interval must be finite and positive");
        }
        const ds::advanced::ChemistryLibrary chemistry;
        if (cfg.chemistry != ds::advanced::ChemistryType::LFP &&
            cfg.chemistry != ds::advanced::ChemistryType::NMC &&
            cfg.chemistry != ds::advanced::ChemistryType::NCA &&
            cfg.chemistry != ds::advanced::ChemistryType::LTO &&
            cfg.chemistry != ds::advanced::ChemistryType::CUSTOM) {
            throw std::invalid_argument("Unknown battery chemistry");
        }

        // Allocate/initialize every replacement before publishing any part.
        auto core = std::make_unique<ds::DSEnhancement>();
        core->init(cfg.ds_config);
        std::unique_ptr<ds::advanced::MultiCellPack> pack;
        std::unique_ptr<ds::advanced::KalmanDSFilter> kalman;
        SafetyMonitor safety(cfg.safety_limits);
        DiagnosticReport diagnostics;

        if (cfg.mode == MiddlewareConfig::Mode::MULTI_CELL_PACK ||
            cfg.mode == MiddlewareConfig::Mode::ADVANCED_ML) {
            if (cfg.series_cells <= 0) {
                throw std::invalid_argument("Multi-cell configuration requires positive series_cells");
            }
            pack = std::make_unique<ds::advanced::MultiCellPack>(
                cfg.series_cells,
                chemistry.get_profile(cfg.chemistry));
        }

        if (cfg.enable_kalman_filter) {
            kalman = std::make_unique<ds::advanced::KalmanDSFilter>();
        }

        static_assert(std::is_nothrow_copy_assignable<MiddlewareConfig>::value, "Configuration commit must not throw");
        static_assert(std::is_nothrow_move_assignable<DiagnosticReport>::value, "Diagnostic commit must not throw");
        static_assert(std::is_nothrow_move_assignable<SafetyMonitor>::value, "Safety commit must not throw");
        static_assert(std::is_nothrow_copy_assignable<ds::EnhancedState>::value, "State commit must not throw");
        config_ = cfg;
        ds_core_.swap(core);
        pack_.swap(pack);
        kalman_.swap(kalman);
        safety_ = std::move(safety);
        diag_ = std::move(diagnostics);
        enhanced_ = {};
        time_s_ = 0.0;
        updates_ = 0;
        total_update_time_ms_ = 0.0;
        energy_in_kwh_ = 0.0;
        energy_out_kwh_ = 0.0;
        initialized_ = true;
    }

    ds::EnhancedState enhance_cycle(double v, double i, double t, double soc, double dt) {
        if (!initialized_) throw std::runtime_error("Middleware not initialized");

        // Safety observations are separate from accepted numerical history. A
        // rejected cycle may report new raw faults, but cannot change the last
        // accepted model, filter, ledger, timing, or numerical diagnostics.
        ds::DSState observed;
        observed.voltage = v;
        observed.current = i;
        observed.temperature = t;
        observed.state_of_charge = soc;
        SafetyMonitor observed_safety = safety_;
        DiagnosticReport observed_diag = diag_;
        observe_safety(observed, observed_diag, observed_safety);

        try {
            if (!std::isfinite(v) || v <= 0.0 || !std::isfinite(i) ||
                !std::isfinite(t) || !std::isfinite(soc) ||
                !std::isfinite(dt) || dt <= 0.0) {
                throw std::invalid_argument("Sensor values must be finite, voltage and dt must be positive");
            }

            // The ledger records raw observed electrical energy only for
            // accepted cycles: +current is discharge, -current is charge;
            // V*A = W, /1000 = kW, kW*s/3600 = kWh. Model normalization never
            // changes that evidence. Safety faults remain separately reported
            // under the existing policy and do not authorize control action.
            const auto accounting = candidate_accounting(observed, dt);
            auto candidate_core = std::make_unique<ds::DSEnhancement>(*ds_core_);
            auto candidate = candidate_core->enhance(v, i, t, soc, dt);
            validate_enhanced(candidate);
            require_finite(candidate_core->check_energy_conservation(), "Nonfinite cumulative model energy residual");

            std::unique_ptr<ds::advanced::KalmanDSFilter> candidate_filter;
            if (kalman_) {
                candidate_filter = std::make_unique<ds::advanced::KalmanDSFilter>(*kalman_);
                candidate_filter->predict(candidate.state, dt);
                candidate_filter->update(std::clamp(soc, 0.0, 1.0), i);
                for (double value : candidate_filter->get_state())
                    require_finite(value, "Nonfinite Kalman state");
                for (double value : candidate_filter->get_covariance())
                    require_finite(value, "Nonfinite Kalman covariance");
                require_finite(candidate_filter->get_uncertainty(), "Nonfinite Kalman uncertainty");
                candidate.state.state_of_charge = candidate_filter->get_filtered_soc();
                validate_enhanced(candidate);
            }

            SafetyMonitor candidate_safety = observed_safety;
            auto candidate_diag = update_diagnostics(candidate, dt, observed, accounting, candidate_safety);

            // All allocations and fallible arithmetic precede this boundary.
            // This is an exception guarantee for this instance, not concurrent
            // reader synchronization or a transaction over external services.
            static_assert(std::is_nothrow_move_assignable<DiagnosticReport>::value, "Diagnostic commit must not throw");
            static_assert(std::is_nothrow_move_assignable<SafetyMonitor>::value, "Safety commit must not throw");
            static_assert(std::is_nothrow_copy_assignable<ds::EnhancedState>::value, "State commit must not throw");
            ds_core_.swap(candidate_core);
            kalman_.swap(candidate_filter);
            enhanced_ = candidate;
            diag_ = std::move(candidate_diag);
            safety_ = std::move(candidate_safety);
            energy_in_kwh_ = accounting.energy_in_kwh;
            energy_out_kwh_ = accounting.energy_out_kwh;
            time_s_ = accounting.time_s;
            total_update_time_ms_ = accounting.total_update_time_ms;
            updates_ = accounting.updates;
            return enhanced_;
        } catch (...) {
            // Publish only safety evidence from the rejected observation. The
            // retained power and all numerical fields describe the last
            // accepted cycle, while flags/faults describe the attempted one.
            safety_ = std::move(observed_safety);
            diag_.safety_fault = observed_diag.safety_fault;
            diag_.soc_warning = observed_diag.soc_warning;
            diag_.low_temp_warning = observed_diag.low_temp_warning;
            diag_.thermal_warning = observed_diag.thermal_warning;
            diag_.voltage_warning = observed_diag.voltage_warning;
            throw;
        }
    }

    const DiagnosticReport& diagnostics() const { return diag_; }
    const DiagnosticReport* diagnostics_ptr() const { return &diag_; }
    DiagnosticReport get_diagnostics() const { return diag_; }
    const std::vector<std::string>& advisories() const { return safety_.advisories(); }
    const std::vector<std::string>& faults() const { return safety_.faults(); }

    ds::HealthPrediction get_health_forecast(double cycles_ahead = 100.0) {
        if (!initialized_) throw std::runtime_error("Middleware not initialized");

        if (config_.mode == MiddlewareConfig::Mode::SIMPLE ||
            config_.mode == MiddlewareConfig::Mode::SINGLE_BATTERY) {
            return ds_core_->get_health_forecast(cycles_ahead);
        }

        if (!pack_) {
            throw std::runtime_error("Multi-cell pack not initialized");
        }

        return pack_->get_pack_health(cycles_ahead);
    }

    std::string get_status_summary() const {
        std::string status = "DS BMS Status:\n";
        status += "  Mode: " + std::string(
            config_.mode == MiddlewareConfig::Mode::SIMPLE ||
            config_.mode == MiddlewareConfig::Mode::SINGLE_BATTERY
                ? "Single Battery"
                : "Multi-Cell Pack"
        ) + "\n";
        status += "  Health: " + std::to_string(diag_.pack_health_percent) + "%\n";
        status += "  Remaining Cycles: " + std::to_string(diag_.estimated_remaining_cycles) + "\n";
        status += "  Update Count: " + std::to_string(diag_.update_count) + "\n";

        if (diag_.degradation_warning || diag_.thermal_warning ||
            diag_.weak_cell_warning || diag_.voltage_warning || diag_.safety_fault) {
            status += "  ⚠️  WARNINGS ACTIVE\n";
        }

        if (!safety_.advisories().empty()) {
            status += "  ℹ️  ADVISORIES ACTIVE\n";
        }

        return status;
    }

// --------------------------------------------------------------------
    // Optional Telemetry Snapshot (Non-Control, Read-Only)
    // --------------------------------------------------------------------
    ds::DSEnergyTelemetry snapshot() const {
        ds::DSEnergyTelemetry t;

        t.soc_percent = diag_.pack_soc_percent;
        t.soh_percent = diag_.pack_soh_percent;

        t.pack_power_kw = diag_.instantaneous_power_kw;
        t.regen_power_kw = (diag_.instantaneous_power_kw < 0.0)
                             ? -diag_.instantaneous_power_kw
                             : 0.0;

        t.recovered_energy_kwh = energy_in_kwh_;
        t.ds_metric_trace = diag_.ds_metric_trace;
        t.ds_entropy = diag_.ds_entropy;
        t.ds_confidence = diag_.ds_confidence;

        t.limiting_factor = diag_.safety_fault ? "SAFETY" : "NONE";

        return t;
    }

private:
    struct CycleAccounting {
        double power_kw;
        double energy_in_kwh;
        double energy_out_kwh;
        double time_s;
        double total_update_time_ms;
        int updates;
    };

    static double require_finite(double value, const char* message) {
        if (!std::isfinite(value)) throw std::overflow_error(message);
        return value;
    }

    CycleAccounting candidate_accounting(const ds::DSState& observed, double dt) const {
        if (updates_ < 0 || updates_ == std::numeric_limits<int>::max())
            throw std::overflow_error("Middleware update counter exhausted");
        const double power_w = require_finite(observed.voltage * observed.current, "Observed power overflow");
        const double power_kw = require_finite(power_w / 1000.0, "Observed kW overflow");
        const double energy = require_finite(
            require_finite(power_kw * dt, "Observed energy multiplication overflow") / 3600.0,
            "Observed kWh overflow");
        const double elapsed_ms = require_finite(dt * 1000.0, "Millisecond interval overflow");
        CycleAccounting next{power_kw, energy_in_kwh_, energy_out_kwh_,
            require_finite(time_s_ + dt, "Cumulative elapsed time overflow"),
            require_finite(total_update_time_ms_ + elapsed_ms, "Cumulative millisecond timing overflow"),
            updates_ + 1};
        if (observed.current > 0.0) next.energy_out_kwh = require_finite(
            next.energy_out_kwh + energy, "Discharge ledger overflow");
        else next.energy_in_kwh = require_finite(
            next.energy_in_kwh + std::abs(energy), "Charging ledger overflow");
        require_finite(next.energy_in_kwh, "Nonfinite charging ledger");
        require_finite(next.energy_out_kwh, "Nonfinite discharge ledger");
        require_finite(next.energy_in_kwh + next.energy_out_kwh, "Total energy throughput overflow");
        return next;
    }

    static void validate_enhanced(const ds::EnhancedState& candidate) {
        if (!candidate.numerical_stability || !candidate.state.is_valid())
            throw std::runtime_error("Middleware cycle failed model numerical validation");
        for (const auto& row : candidate.state.g_eff.data)
            for (double value : row) require_finite(value, "Nonfinite model metric");
        const auto& h = candidate.health;
        const auto& c = candidate.charging;
        const double values[] = {candidate.feen_trust_metric, candidate.ds_confidence,
            candidate.energy_conservation_error, h.remaining_capacity_percent,
            h.cycles_to_80_percent, h.estimated_eol_cycles, h.confidence,
            h.degradation_rate, h.time_to_80_percent_years,
            c.recommended_current_limit, c.recommended_voltage_limit,
            c.recommended_temperature, c.estimated_charge_time,
            c.degradation_impact, c.max_safe_current, c.max_safe_voltage};
        for (double value : values) require_finite(value, "Nonfinite enhanced result");
    }

    void observe_safety(const ds::DSState& observed, DiagnosticReport& report,
                        SafetyMonitor& safety) const {
        report.thermal_warning = observed.temperature > config_.safety_limits.max_operating_temp ||
            report.max_cell_temp_c > config_.safety_limits.max_cell_temp;
        report.voltage_warning = observed.voltage < config_.safety_limits.min_pack_voltage ||
            observed.voltage > config_.safety_limits.max_pack_voltage ||
            report.voltage_imbalance_mv > config_.safety_limits.max_voltage_imbalance_mv;
        if (config_.enable_safety_monitoring) {
            report.safety_fault = !safety.check(observed, report);
            report.soc_warning = safety.has_soc_warning();
            report.low_temp_warning = safety.has_low_temp_warning();
        }
    }

    DiagnosticReport update_diagnostics(const ds::EnhancedState& candidate, double dt,
                                        const ds::DSState& observed,
                                        const CycleAccounting& accounting,
                                        SafetyMonitor& safety) const {
        const auto& s = candidate.state;
        DiagnosticReport next = diag_;

        next.pack_soc_percent = s.state_of_charge * 100.0;
        next.pack_soh_percent = (1.0 - s.degradation) * 100.0;
        next.pack_health_percent = next.pack_soh_percent;
        next.instantaneous_power_kw = accounting.power_kw;
        next.ds_metric_trace = s.g_eff.trace();
        next.ds_entropy = s.entropy;
        next.ds_phi_magnitude = s.phi_magnitude;
        next.ds_confidence = candidate.ds_confidence;
        next.metric_trace = next.ds_metric_trace;
        next.phi_magnitude = next.ds_phi_magnitude;
        next.entropy_level = next.ds_entropy;
        next.energy_throughput_kwh = accounting.energy_in_kwh + accounting.energy_out_kwh;
        next.last_update_time_ms = dt * 1000.0;
        next.average_update_time_ms = accounting.total_update_time_ms / accounting.updates;
        if (s.cycle_count > 0.0) {
            next.degradation_rate_per_cycle = s.degradation / s.cycle_count;
        } else {
            next.degradation_rate_per_cycle = 0.0;
        }

        next.weak_cell_warning = false;
        next.balancing_required = false;

        if (pack_) {
            if (pack_->get_cells().size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                throw std::overflow_error("Diagnostic cell count overflow");
            next.total_cells = static_cast<int>(pack_->get_cells().size());
            next.weak_cell_ids = pack_->get_weak_cell_ids();
            next.weak_cell_count = static_cast<int>(next.weak_cell_ids.size());
            next.voltage_imbalance_mv = pack_->get_voltage_imbalance() * 1000.0;
            next.average_cell_temp_c = pack_->get_average_temperature();
            next.min_cell_temp_c = pack_->get_min_cell_temperature();
            next.max_cell_temp_c = pack_->get_max_cell_temperature();
            next.weak_cell_warning = next.weak_cell_count > 0;
            next.balancing_required =
                next.voltage_imbalance_mv > config_.safety_limits.max_voltage_imbalance_mv;
        }
        next.estimated_remaining_cycles = std::max(0.0, (1.0 - s.degradation) * 2000.0);
        observe_safety(observed, next, safety);
        next.time_since_init_s = accounting.time_s;
        next.update_count = accounting.updates;

        // A finite ledger alone is insufficient. Validate every floating
        // diagnostic plus cumulative timing and the counter before publication.
        const double values[] = {next.pack_soh_percent, next.pack_soc_percent,
            next.estimated_range_km, next.cycles_to_eol, next.pack_health_percent,
            next.estimated_remaining_cycles, next.min_cell_voltage, next.max_cell_voltage,
            next.voltage_imbalance_mv, next.average_cell_temp_c, next.max_cell_temp_c,
            next.min_cell_temp_c, next.ds_metric_trace, next.ds_entropy,
            next.ds_phi_magnitude, next.ds_confidence, next.instantaneous_power_kw,
            next.average_efficiency, next.energy_throughput_kwh, next.last_update_time_ms,
            next.average_update_time_ms, next.degradation_rate_per_cycle,
            next.metric_trace, next.phi_magnitude, next.entropy_level,
            next.time_since_init_s, accounting.total_update_time_ms};
        for (double value : values) require_finite(value, "Nonfinite middleware diagnostic");
        if (next.update_count <= 0)
            throw std::overflow_error("Invalid diagnostic update counter");
        return next;
    }
};

} // namespace ds_plugin

#endif
