#include "ds_bms_middleware_v2.hpp"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace ds_plugin {
// Boundary injection avoids billions of cycles; it is not evidence of an
// actual multi-century run. No application-facing mutation API is introduced.
struct MiddlewareAccountingTestAccess {
    struct Snapshot {
        ds::DSState core;
        ds::EnhancedState enhanced;
        double energy_in, energy_out, time, total_ms, model_residual;
        int updates;
        std::array<double, 4> filter_state{}, filter_covariance{};
    };
    static Snapshot snapshot(const DSBMSMiddleware& middleware) {
        Snapshot result{middleware.ds_core_->get_state(), middleware.enhanced_,
            middleware.energy_in_kwh_, middleware.energy_out_kwh_, middleware.time_s_,
            middleware.total_update_time_ms_, middleware.ds_core_->check_energy_conservation(),
            middleware.updates_};
        if (middleware.kalman_) {
            result.filter_state = middleware.kalman_->get_state();
            result.filter_covariance = middleware.kalman_->get_covariance();
        }
        return result;
    }
    static void set_timing(DSBMSMiddleware& middleware, double seconds, double milliseconds) {
        middleware.time_s_ = seconds;
        middleware.total_update_time_ms_ = milliseconds;
        middleware.diag_.time_since_init_s = seconds;
    }
    static void set_updates(DSBMSMiddleware& middleware, int updates) {
        middleware.updates_ = updates;
        middleware.diag_.update_count = updates;
    }
    static void set_filter_noise(DSBMSMiddleware& middleware, double noise) {
        *middleware.kalman_ = ds::advanced::KalmanDSFilter(noise);
    }
    static void set_diagnostic_range(DSBMSMiddleware& middleware, double range) {
        middleware.diag_.estimated_range_km = range;
    }
};
} // namespace ds_plugin

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Operation>
void require_rejection(Operation operation, const char* message) {
    bool rejected = false;
    try { operation(); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}

void reproduce() {
    const double maximum = std::numeric_limits<double>::max();
    std::cout << std::setprecision(17);
    for (double current : {maximum, -maximum}) {
        ds_plugin::DSBMSMiddleware middleware;
        middleware.init(75.0, 400.0);
        try {
            const auto result = middleware.enhance_cycle(360.0, current, 25.0, 0.8, 1.0);
            const auto diag = middleware.diagnostics();
            std::cout << "raw_current=" << current
                      << " model_current=" << result.state.current
                      << " stable=" << result.numerical_stability
                      << " input_clamped=" << result.input_clamped
                      << " model_time=" << result.state.time
                      << " model_charge_ah=" << result.state.charge_throughput_ah
                      << " model_entropy=" << result.state.entropy
                      << " throughput_kwh=" << diag.energy_throughput_kwh
                      << " diagnostic_power_kw=" << diag.instantaneous_power_kw
                      << " recovered_kwh=" << middleware.snapshot().recovered_energy_kwh
                      << " safety_fault=" << diag.safety_fault
                      << " update_count=" << diag.update_count << '\n';
        } catch (const std::exception& error) {
            std::cout << "raw_current=" << current << " rejected=" << error.what()
                      << " throughput_kwh=" << middleware.diagnostics().energy_throughput_kwh
                      << " safety_fault=" << middleware.diagnostics().safety_fault
                      << " update_count=" << middleware.diagnostics().update_count << '\n';
        }
    }
    ds_plugin::DSBMSMiddleware middleware;
    middleware.init(75.0, 400.0);
    const double dt = maximum / 500.0;
    try {
        const auto result = middleware.enhance_cycle(360.0, 0.0, 25.0, 0.8, dt);
        const auto diag = middleware.diagnostics();
        std::cout << "dt=" << dt << " stable=" << result.numerical_stability
                  << " throughput_kwh=" << diag.energy_throughput_kwh
                  << " last_update_ms=" << diag.last_update_time_ms
                  << " average_update_ms=" << diag.average_update_time_ms
                  << " elapsed_s=" << diag.time_since_init_s
                  << " update_count=" << diag.update_count << '\n';
    } catch (const std::exception& error) {
        std::cout << "dt=" << dt << " rejected=" << error.what()
                  << " elapsed_s=" << middleware.diagnostics().time_since_init_s
                  << " update_count=" << middleware.diagnostics().update_count << '\n';
    }
}

void extreme_currents() {
    for (double sign : {1.0, -1.0}) {
        ds_plugin::DSBMSMiddleware middleware;
        middleware.init(75.0, 400.0);
        require_rejection([&] {
            middleware.enhance_cycle(360.0, sign * std::numeric_limits<double>::max(),
                                     25.0, 0.8, 1.0);
        }, "PB-13: DBL_MAX raw energy must reject instead of reporting a stable cycle");
        require(middleware.diagnostics().update_count == 0,
                "PB-13: rejected overflow must preserve accepted update count");
        require(middleware.diagnostics().energy_throughput_kwh == 0.0,
                "PB-13: rejected overflow must preserve accepted energy ledger");
        require(middleware.diagnostics().safety_fault,
                "PB-13: rejection must retain raw current safety evidence");
    }
}

void long_timestep() {
    ds_plugin::DSBMSMiddleware middleware;
    middleware.init(75.0, 400.0);
    require_rejection([&] {
        middleware.enhance_cycle(360.0, 0.0, 25.0, 0.8,
                                 std::numeric_limits<double>::max() / 500.0);
    }, "PB-13: finite ledger does not excuse overflowing millisecond diagnostics");
    require(middleware.diagnostics().update_count == 0,
            "PB-13: timing rejection must preserve accepted counters");
}

std::vector<double> state_values(const ds::DSState& state) {
    std::vector<double> values{state.voltage, state.current, state.temperature,
        state.state_of_charge, state.entropy, state.cycle_count, state.degradation,
        state.phi_magnitude, state.lambda, state.energy_psi, state.energy_phi,
        state.energy_metric, state.energy_total, state.charge_throughput_ah,
        state.capacity_fade, state.time, state.last_update};
    values.insert(values.end(), state.grad_phi.begin(), state.grad_phi.end());
    for (const auto& row : state.g_eff.data) values.insert(values.end(), row.begin(), row.end());
    return values;
}

std::vector<double> enhanced_values(const ds::EnhancedState& value) {
    auto values = state_values(value.state);
    const auto& h = value.health;
    const auto& c = value.charging;
    const double extra[] = {value.feen_trust_metric, value.ds_confidence,
        value.energy_conservation_error, h.remaining_capacity_percent,
        h.cycles_to_80_percent, h.estimated_eol_cycles, h.confidence,
        h.degradation_rate, h.time_to_80_percent_years, c.recommended_current_limit,
        c.recommended_voltage_limit, c.recommended_temperature, c.estimated_charge_time,
        c.degradation_impact, c.max_safe_current, c.max_safe_voltage};
    values.insert(values.end(), std::begin(extra), std::end(extra));
    return values;
}

std::vector<double> diagnostic_values(const ds_plugin::DiagnosticReport& value) {
    return {value.pack_soh_percent, value.pack_soc_percent, value.estimated_range_km,
        value.cycles_to_eol, value.pack_health_percent, value.estimated_remaining_cycles,
        value.min_cell_voltage, value.max_cell_voltage, value.voltage_imbalance_mv,
        value.average_cell_temp_c, value.max_cell_temp_c, value.min_cell_temp_c,
        value.ds_metric_trace, value.ds_entropy, value.ds_phi_magnitude, value.ds_confidence,
        value.instantaneous_power_kw, value.average_efficiency, value.energy_throughput_kwh,
        value.last_update_time_ms, value.average_update_time_ms, value.degradation_rate_per_cycle,
        value.metric_trace, value.phi_magnitude, value.entropy_level, value.time_since_init_s};
}

void require_preserved(const ds_plugin::DSBMSMiddleware& middleware,
                       const ds_plugin::MiddlewareAccountingTestAccess::Snapshot& prior,
                       const ds_plugin::DiagnosticReport& diag) {
    const auto after = ds_plugin::MiddlewareAccountingTestAccess::snapshot(middleware);
    require(state_values(after.core) == state_values(prior.core), "Rejected cycle changed core state");
    require(enhanced_values(after.enhanced) == enhanced_values(prior.enhanced),
            "Rejected cycle changed enhanced snapshot");
    require(after.enhanced.numerical_stability == prior.enhanced.numerical_stability &&
            after.enhanced.input_clamped == prior.enhanced.input_clamped &&
            after.enhanced.degradation_warning == prior.enhanced.degradation_warning &&
            after.enhanced.health.warning_triggered == prior.enhanced.health.warning_triggered,
            "Rejected cycle changed enhanced result flags");
    require(after.energy_in == prior.energy_in && after.energy_out == prior.energy_out &&
            after.time == prior.time && after.total_ms == prior.total_ms &&
            after.model_residual == prior.model_residual && after.updates == prior.updates,
            "Rejected cycle changed ledger, timing, counter, or model residual");
    require(after.filter_state == prior.filter_state && after.filter_covariance == prior.filter_covariance,
            "Rejected cycle changed filter history");
    require(diagnostic_values(middleware.diagnostics()) == diagnostic_values(diag) &&
            middleware.diagnostics().update_count == diag.update_count &&
            middleware.diagnostics().weak_cell_ids == diag.weak_cell_ids,
            "Rejected cycle changed accepted numerical diagnostics");
}

void require_finite_outputs(const ds_plugin::DSBMSMiddleware& middleware) {
    for (double value : diagnostic_values(middleware.diagnostics()))
        require(std::isfinite(value), "Middleware exposed a nonfinite diagnostic");
    const auto t = middleware.snapshot();
    for (double value : {t.soc_percent, t.soh_percent, t.pack_power_kw, t.regen_power_kw,
                         t.recovered_energy_kwh, t.ds_metric_trace, t.ds_entropy, t.ds_confidence})
        require(std::isfinite(value), "Middleware exposed nonfinite telemetry");
}

void arithmetic_overflows() {
    ds_plugin::DSBMSMiddleware middleware;
    middleware.init(75.0, 400.0);
    middleware.enhance_cycle(360.0, 10.0, 25.0, 0.8, 1.0);
    const auto prior = ds_plugin::MiddlewareAccountingTestAccess::snapshot(middleware);
    const auto diag = middleware.diagnostics();
    require_rejection([&] {
        middleware.enhance_cycle(360.0, std::numeric_limits<double>::max() / 4.0, 25.0, 0.8, 1.0);
    }, "Finite sensor multiplication overflow must reject");
    require_preserved(middleware, prior, diag);
    require_rejection([&] {
        middleware.enhance_cycle(360.0, 1e303, 25.0, 0.8, 1e8);
    }, "Finite power times timestep overflow must reject");
    require_preserved(middleware, prior, diag);
    require_finite_outputs(middleware);

    // This baseline permits finite model energy while voltage*current itself
    // overflows. It isolates instantaneous diagnostic power from DS stability.
    ds_plugin::DSBMSMiddleware power;
    power.init(1e-310, 400.0);
    const auto power_prior = ds_plugin::MiddlewareAccountingTestAccess::snapshot(power);
    const auto power_diag = power.diagnostics();
    require_rejection([&] {
        power.enhance_cycle(std::numeric_limits<double>::max() / 2.0, 4.0, 25.0, 0.8, 1.0);
    }, "Overflowing instantaneous power must reject even when model energy is finite");
    require_preserved(power, power_prior, power_diag);
    require_finite_outputs(power);
}

void normal_accounting_and_model_compatibility() {
    ds_plugin::DSBMSMiddleware middleware;
    middleware.init(75.0, 400.0);
    ds::DSEnhancement direct;
    direct.init();
    double expected = 0.0;
    for (double current : {10.0, -20.0, 0.0, 2000.0, -2000.0}) {
        const auto result = middleware.enhance_cycle(360.0, current, 25.0, 0.8, 1.0);
        const auto reference = direct.enhance(360.0, current, 25.0, 0.8, 1.0);
        require(enhanced_values(result) == enhanced_values(reference) &&
                result.numerical_stability == reference.numerical_stability,
                "Valid DS mathematical behavior changed");
        expected += std::abs(360.0 * current / 1000.0 / 3600.0);
        require(middleware.diagnostics().energy_throughput_kwh == expected,
                "Accepted raw energy unit conversion or signs incorrect");
        require(middleware.diagnostics().instantaneous_power_kw == 360.0 * current / 1000.0,
                "Diagnostic power must use the same raw contract as the ledger");
        require_finite_outputs(middleware);
        for (double value : enhanced_values(result)) require(std::isfinite(value), "Nonfinite model output");
    }
    require(middleware.diagnostics().update_count == 5 &&
            middleware.diagnostics().time_since_init_s == 5.0 &&
            middleware.diagnostics().last_update_time_ms == 1000.0 &&
            middleware.diagnostics().average_update_time_ms == 1000.0,
            "Accepted cycle timing contract changed");
    const auto t = middleware.snapshot();
    const double expected_recovery = 360.0 * 20.0 / 1000.0 / 3600.0 +
                                     360.0 * 2000.0 / 1000.0 / 3600.0;
    require(t.recovered_energy_kwh == expected_recovery && t.regen_power_kw == 720.0,
            "Charging ledger and negative raw power accounting mismatch");
    require(middleware.diagnostics().safety_fault, "Normalized current hid raw physical fault");
}

void rejected_state_and_filter_atomicity() {
    for (bool filtering : {false, true}) {
        ds_plugin::MiddlewareConfig config;
        config.enable_kalman_filter = filtering;
        config.ds_config.enable_feen_battery_integration = true;
        ds_plugin::DSBMSMiddleware middleware, control;
        middleware.init_advanced(config);
        control.init_advanced(config);
        middleware.enhance_cycle(360.0, 10.0, 25.0, 0.8, 1.0);
        control.enhance_cycle(360.0, 10.0, 25.0, 0.8, 1.0);
        const auto prior = ds_plugin::MiddlewareAccountingTestAccess::snapshot(middleware);
        const auto diag = middleware.diagnostics();
        for (double current : {std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()}) {
            require_rejection([&] { middleware.enhance_cycle(360.0, current, 100.0, 0.8, 1.0); },
                              "Extreme overflow must reject");
            require_preserved(middleware, prior, diag);
            require(middleware.diagnostics().safety_fault && middleware.diagnostics().thermal_warning &&
                    !middleware.faults().empty(), "Rejected raw safety evidence lost");
            require_finite_outputs(middleware);
        }
        for (double value : {std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
            require_rejection([&] { middleware.enhance_cycle(360.0, 10.0, 25.0, value, 1.0); },
                              "Nonfinite SOC must reject");
            require_preserved(middleware, prior, diag);
            require(middleware.diagnostics().safety_fault && !middleware.faults().empty(),
                    "Nonfinite raw evidence concealed by comparisons");
        }
        require_rejection([&] { middleware.enhance_cycle(360.0, 10.0, 25.0, 0.8, 0.001); },
                          "Short core interval must reject");
        require_preserved(middleware, prior, diag);
        const auto result = middleware.enhance_cycle(390.0, -20.0, 30.0, 0.7, 2.0);
        const auto reference = control.enhance_cycle(390.0, -20.0, 30.0, 0.7, 2.0);
        require(enhanced_values(result) == enhanced_values(reference),
                "Rejected cycles changed subsequent valid model/filter history");
        const auto after = ds_plugin::MiddlewareAccountingTestAccess::snapshot(middleware);
        const auto control_after = ds_plugin::MiddlewareAccountingTestAccess::snapshot(control);
        require(after.filter_state == control_after.filter_state &&
                after.filter_covariance == control_after.filter_covariance,
                "Filter covariance changed after rejection and resume");
        require(!middleware.diagnostics().safety_fault, "Accepted raw recovery did not clear fault");
    }
}

void cumulative_energy_overflow() {
    for (double sign : {1.0, -1.0}) {
        ds_plugin::DSBMSMiddleware middleware;
        middleware.init(75.0, 400.0);
        bool rejected = false;
        for (int attempt = 0; attempt < 5000; ++attempt) {
            const auto prior = ds_plugin::MiddlewareAccountingTestAccess::snapshot(middleware);
            const auto diag = middleware.diagnostics();
            try {
                middleware.enhance_cycle(1.0, sign * std::numeric_limits<double>::max() / 2.0,
                                         25.0, 0.8, 1800.0);
            } catch (const std::overflow_error&) {
                rejected = true;
                require_preserved(middleware, prior, diag);
                require(diag.update_count > 3000, "Cumulative overflow test rejected before accumulation");
                require_finite_outputs(middleware);
                std::cout << "Cumulative " << (sign > 0.0 ? "discharge" : "charge")
                          << " overflow rejected after " << diag.update_count
                          << " accepted cycles, throughput=" << std::setprecision(17)
                          << diag.energy_throughput_kwh << '\n';
                break;
            }
        }
        require(rejected, "Cumulative ledger must reject finite increments before overflow");
    }
    // Individual ledgers are finite, but their sum must also remain finite.
    ds_plugin::DSBMSMiddleware middleware;
    middleware.init(75.0, 400.0);
    bool rejected = false;
    for (int attempt = 0; attempt < 5000; ++attempt) {
        const auto prior = ds_plugin::MiddlewareAccountingTestAccess::snapshot(middleware);
        const auto diag = middleware.diagnostics();
        try {
            const double current = (attempt % 2 ? -1.0 : 1.0) * std::numeric_limits<double>::max() / 2.0;
            middleware.enhance_cycle(1.0, current, 25.0, 0.8, 1800.0);
        } catch (const std::overflow_error&) {
            rejected = true;
            require_preserved(middleware, prior, diag);
            require(prior.energy_in < std::numeric_limits<double>::max() &&
                    prior.energy_out < std::numeric_limits<double>::max(),
                    "Throughput overflow must be checked independently of each ledger");
            require_finite_outputs(middleware);
            break;
        }
    }
    require(rejected, "Combined finite ledgers must reject overflowing throughput");
}

void timing_and_counter_boundaries() {
    using Access = ds_plugin::MiddlewareAccountingTestAccess;
    ds_plugin::DSBMSMiddleware middleware;
    middleware.init(75.0, 400.0);
    const double maximum = std::numeric_limits<double>::max();
    Access::set_timing(middleware, 1.0, maximum);
    auto prior = Access::snapshot(middleware);
    auto diag = middleware.diagnostics();
    require_rejection([&] { middleware.enhance_cycle(360.0, 0.0, 25.0, 0.8, maximum / 2000.0); },
                      "Cumulative timing overflow must reject even with finite dt-ms and zero energy");
    require_preserved(middleware, prior, diag);
    Access::set_timing(middleware, maximum, 0.0);
    prior = Access::snapshot(middleware);
    diag = middleware.diagnostics();
    require_rejection([&] { middleware.enhance_cycle(360.0, 0.0, 25.0, 0.8, maximum / 2000.0); },
                      "Accumulated seconds overflow must reject");
    require_preserved(middleware, prior, diag);
    Access::set_timing(middleware, 0.0, 0.0);
    Access::set_updates(middleware, std::numeric_limits<int>::max() - 1);
    middleware.enhance_cycle(360.0, 0.0, 25.0, 0.8, 1.0);
    require(middleware.diagnostics().update_count == std::numeric_limits<int>::max(),
            "Last representable update count must remain compatible");
    prior = Access::snapshot(middleware);
    diag = middleware.diagnostics();
    require_rejection([&] { middleware.enhance_cycle(360.0, 0.0, 25.0, 0.8, 1.0); },
                      "INT_MAX counter must reject before signed overflow");
    require_preserved(middleware, prior, diag);
    require_finite_outputs(middleware);
}

void downstream_failure_atomicity() {
    using Access = ds_plugin::MiddlewareAccountingTestAccess;
    // A finite raw ledger must not manufacture success when the model's
    // charging recommendation or optional filter arithmetic is nonfinite.
    ds_plugin::MiddlewareConfig config;
    config.nominal_capacity_ah = config.ds_config.nominal_capacity_ah =
        std::numeric_limits<double>::max() / 4.0;
    ds_plugin::DSBMSMiddleware middleware;
    middleware.init_advanced(config);
    const auto prior = Access::snapshot(middleware);
    const auto diag = middleware.diagnostics();
    require_rejection([&] { middleware.enhance_cycle(1e-308, 10.0, 25.0, 0.0, 1.0); },
                      "Nonfinite charging output must reject despite stable DSState and raw ledger");
    require_preserved(middleware, prior, diag);
    require_finite_outputs(middleware);

    config = {};
    config.enable_kalman_filter = true;
    middleware.init_advanced(config);
    Access::set_filter_noise(middleware, std::numeric_limits<double>::max());
    const auto filter_prior = Access::snapshot(middleware);
    const auto filter_diag = middleware.diagnostics();
    require_rejection([&] { middleware.enhance_cycle(360.0, 10.0, 25.0, 0.8, 2.0); },
                      "Nonfinite candidate filter must reject");
    require_preserved(middleware, filter_prior, filter_diag);
    require_finite_outputs(middleware);

    // Exercise final diagnostic validation after model and filter candidates
    // have been prepared. The intentionally injected NaN is retained evidence,
    // never normalized into a successful numerical cycle.
    middleware.init_advanced(config);
    Access::set_diagnostic_range(middleware, std::numeric_limits<double>::quiet_NaN());
    const auto diagnostic_prior = Access::snapshot(middleware);
    require_rejection([&] { middleware.enhance_cycle(360.0, 10.0, 25.0, 0.8, 1.0); },
                      "All diagnostics must be validated before candidate state commits");
    const auto diagnostic_after = Access::snapshot(middleware);
    require(state_values(diagnostic_after.core) == state_values(diagnostic_prior.core) &&
            diagnostic_after.filter_covariance == diagnostic_prior.filter_covariance &&
            diagnostic_after.filter_state == diagnostic_prior.filter_state &&
            diagnostic_after.updates == diagnostic_prior.updates &&
            diagnostic_after.energy_in == diagnostic_prior.energy_in &&
            diagnostic_after.energy_out == diagnostic_prior.energy_out,
            "Diagnostic rejection committed candidate core/filter/ledger");
}

void core_intermediate_overflow(bool measurements = false) {
    for (double entropy_weight : {0.5, 0.0}) {
        ds_plugin::MiddlewareConfig config;
        config.ds_config.max_temperature = std::numeric_limits<double>::max() / 2.0;
        config.ds_config.entropy_weight = entropy_weight;
        const double interval = entropy_weight == 0.0 ? 200000.0 : 100000.0;
        ds_plugin::DSBMSMiddleware middleware, control;
        middleware.init_advanced(config);
        control.init_advanced(config);
        middleware.enhance_cycle(360.0, 0.0, 25.0, 0.8, 1.0);
        control.enhance_cycle(360.0, 0.0, 25.0, 0.8, 1.0);
        const auto prior = ds_plugin::MiddlewareAccountingTestAccess::snapshot(middleware);
        const auto diag = middleware.diagnostics();
        if (measurements) {
            const auto result = middleware.enhance_cycle(360.0, 0.0, config.ds_config.max_temperature, 0.8, interval);
            std::cout << std::setprecision(17)
                      << "entropy_weight=" << entropy_weight
                      << " model_temperature=" << result.state.temperature
                      << " dt=" << interval
                      << " stable=" << result.numerical_stability
                      << " entropy=" << result.state.entropy
                      << " degradation=" << result.state.degradation
                      << " model_energy_total=" << result.state.energy_total
                      << " model_time=" << result.state.time
                      << " update_count=" << middleware.diagnostics().update_count
                      << " raw_energy_kwh=" << middleware.diagnostics().energy_throughput_kwh << '\n';
            continue;
        }
        require_rejection([&] {
            middleware.enhance_cycle(360.0, 0.0, config.ds_config.max_temperature, 0.8, interval);
        }, "PB-13: saturation must not conceal overflowed core entropy/degradation intermediates");
        require_preserved(middleware, prior, diag);
        require(middleware.diagnostics().safety_fault && middleware.diagnostics().thermal_warning,
                "Rejected unsaturated core overflow lost raw thermal evidence");
        require_finite_outputs(middleware);
        const auto resumed = middleware.enhance_cycle(360.0, 10.0, 25.0, 0.8, 1.0);
        const auto reference = control.enhance_cycle(360.0, 10.0, 25.0, 0.8, 1.0);
        require(enhanced_values(resumed) == enhanced_values(reference),
                "Rejected unsaturated core overflow changed subsequent valid history");
    }
}
} // namespace

int main(int argc, char** argv) {
    const std::string selected = argc > 1 ? argv[1] : "all";
    const std::vector<std::string> selections = {"all", "--reproduce", "--core-reproduce",
        "extreme", "timing", "arithmetic", "normal", "atomicity", "cumulative",
        "boundaries", "downstream", "core-intermediate"};
    if (argc > 2 || std::find(selections.begin(), selections.end(), selected) == selections.end()) {
        std::cerr << "Unknown accounting regression selection\n";
        return 2;
    }
    try {
        if (selected == "--reproduce") { reproduce(); return 0; }
        if (selected == "--core-reproduce") { core_intermediate_overflow(true); return 0; }
        if (selected == "all" || selected == "extreme") extreme_currents();
        if (selected == "all" || selected == "timing") long_timestep();
        if (selected == "all" || selected == "arithmetic") arithmetic_overflows();
        if (selected == "all" || selected == "normal") normal_accounting_and_model_compatibility();
        if (selected == "all" || selected == "atomicity") rejected_state_and_filter_atomicity();
        if (selected == "all" || selected == "cumulative") cumulative_energy_overflow();
        if (selected == "all" || selected == "boundaries") timing_and_counter_boundaries();
        if (selected == "all" || selected == "downstream") downstream_failure_atomicity();
        if (selected == "all" || selected == "core-intermediate") core_intermediate_overflow();
        std::cout << "PB-13 accounting regressions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
