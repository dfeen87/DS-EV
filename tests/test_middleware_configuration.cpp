// PB-14: nominal agreement, calibrated-model compatibility and initialization
// transaction boundaries. Each failure is reported so baseline evidence includes
// independent configuration and failed-reinitialization defects.
#include "ds_bms_middleware_v2.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using ds_plugin::DSBMSMiddleware;
using ds_plugin::MiddlewareConfig;

void require(bool condition, const char* invariant) {
    if (!condition) throw std::runtime_error(invariant);
}

template <class F>
void require_invalid_configuration(F&& action) {
    bool rejected = false;
    try { action(); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "invalid or conflicting configuration must be rejected");
}

template <class F>
void require_uninitialized(F&& action) {
    bool unusable = false;
    try { action(); }
    catch (const std::runtime_error&) { unusable = true; }
    require(unusable, "rejected initial configuration must leave instance unusable");
}

void require_state_equal(const ds::EnhancedState& actual,
                         const ds::EnhancedState& expected) {
    require(actual.state.time == expected.state.time, "model time must be preserved");
    require(actual.state.entropy == expected.state.entropy, "model entropy must be preserved");
    require(actual.state.charge_throughput_ah == expected.state.charge_throughput_ah,
            "model charge throughput must be preserved");
    require(actual.state.cycle_count == expected.state.cycle_count,
            "model cycle count must be preserved");
    require(actual.state.degradation == expected.state.degradation,
            "model degradation must be preserved");
    require(actual.state.energy_psi == expected.state.energy_psi,
            "model stored energy must follow calibrated capacity");
    require(actual.state.energy_phi == expected.state.energy_phi,
            "model information energy must follow calibrated parameters");
    require(actual.state.energy_metric == expected.state.energy_metric,
            "model metric energy must follow calibrated parameters");
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            require(actual.state.g_eff(row, column) == expected.state.g_eff(row, column),
                    "model metric must follow calibrated parameters");
        }
    }
    require(actual.charging.recommended_voltage_limit == expected.charging.recommended_voltage_limit,
            "charging voltage must follow the same nominal configuration");
    require(actual.charging.estimated_charge_time == expected.charging.estimated_charge_time,
            "charging duration must follow the same nominal capacity");
    require(actual.charging.recommended_current_limit == expected.charging.recommended_current_limit,
            "charging current must preserve calibrated-model behavior");
    require(actual.ds_confidence == expected.ds_confidence,
            "model confidence must preserve calibrated-model behavior");
    require(actual.numerical_stability == expected.numerical_stability,
            "model stability must preserve calibrated-model behavior");
}

void require_diagnostics_equal(const ds_plugin::DiagnosticReport& actual,
                               const ds_plugin::DiagnosticReport& expected) {
    require(actual.update_count == expected.update_count, "diagnostic count must be preserved");
    require(actual.time_since_init_s == expected.time_since_init_s,
            "diagnostic session time must be preserved");
    require(actual.energy_throughput_kwh == expected.energy_throughput_kwh,
            "energy ledger must be preserved");
    require(actual.last_update_time_ms == expected.last_update_time_ms &&
            actual.average_update_time_ms == expected.average_update_time_ms,
            "diagnostic timing must be preserved");
    require(actual.instantaneous_power_kw == expected.instantaneous_power_kw,
            "diagnostic power must be preserved");
    require(actual.pack_soc_percent == expected.pack_soc_percent &&
            actual.pack_soh_percent == expected.pack_soh_percent,
            "diagnostic battery state must be preserved");
    require(actual.ds_entropy == expected.ds_entropy &&
            actual.ds_metric_trace == expected.ds_metric_trace &&
            actual.ds_phi_magnitude == expected.ds_phi_magnitude,
            "diagnostic model state must be preserved");
    require(actual.total_cells == expected.total_cells &&
            actual.weak_cell_ids == expected.weak_cell_ids,
            "diagnostic pack state must be preserved");
    require(actual.safety_fault == expected.safety_fault &&
            actual.thermal_warning == expected.thermal_warning &&
            actual.voltage_warning == expected.voltage_warning &&
            actual.soc_warning == expected.soc_warning &&
            actual.low_temp_warning == expected.low_temp_warning,
            "diagnostic safety state must be preserved");
}

MiddlewareConfig custom_configuration() {
    MiddlewareConfig config;
    config.nominal_capacity_ah = config.ds_config.nominal_capacity_ah = 150.0;
    config.nominal_voltage = config.ds_config.nominal_voltage = 800.0;
    config.ds_config.lambda = 2e-6;
    config.ds_config.phi_decay_rate = 0.003;
    config.ds_config.entropy_weight = 0.7;
    config.ds_config.thermodynamic_beta = 1.3;
    config.ds_config.max_current = 80.0;
    return config;
}

void matching_configurations() {
    for (const auto& config : {MiddlewareConfig{}, custom_configuration()}) {
        DSBMSMiddleware middleware;
        ds::DSEnhancement direct;
        middleware.init_advanced(config);
        direct.init(config.ds_config);
        for (double current : {10.0, -20.0, 150.0}) {
            const auto result = middleware.enhance_cycle(360.0, current, 25.0, 0.5, 1.0);
            const auto expected = direct.enhance(360.0, current, 25.0, 0.5, 1.0);
            require_state_equal(result, expected);
            require(result.state.energy_psi ==
                    360.0 * 0.5 * config.nominal_capacity_ah * 3600.0,
                    "stored energy must follow agreed capacity in joules");
            require(result.charging.recommended_voltage_limit == config.nominal_voltage,
                    "charging voltage must follow agreed nominal voltage");
            require(std::isfinite(result.state.energy_total) &&
                    std::isfinite(result.charging.estimated_charge_time),
                    "valid calibrated outputs must remain finite");
        }
    }
    DSBMSMiddleware simple;
    simple.init(150.0, 800.0);
    const auto result = simple.enhance_cycle(360.0, 10.0, 25.0, 0.5, 1.0);
    require(result.state.energy_psi == 97200000.0,
            "simple initialization must retain nominal forwarding compatibility");
    require(result.charging.recommended_voltage_limit == 800.0,
            "simple initialization must retain charging-voltage compatibility");
}

void conflicting_capacity() {
    MiddlewareConfig config;
    config.nominal_capacity_ah = 150.0;
    DSBMSMiddleware middleware;
    require_invalid_configuration([&] { middleware.init_advanced(config); });
    require_uninitialized([&] { middleware.enhance_cycle(360, 10, 25, 0.5, 1); });
    config = MiddlewareConfig{};
    config.ds_config.nominal_capacity_ah = 150.0;
    require_invalid_configuration([&] { middleware.init_advanced(config); });
}

void conflicting_voltage() {
    MiddlewareConfig config;
    config.nominal_voltage = 800.0;
    DSBMSMiddleware middleware;
    require_invalid_configuration([&] { middleware.init_advanced(config); });
    require_uninitialized([&] { middleware.enhance_cycle(360, 10, 25, 0.5, 1); });
    config = MiddlewareConfig{};
    config.ds_config.nominal_voltage = 800.0;
    require_invalid_configuration([&] { middleware.init_advanced(config); });
}

void simultaneous_conflicts() {
    MiddlewareConfig config;
    config.nominal_capacity_ah = 150.0;
    config.nominal_voltage = 800.0;
    DSBMSMiddleware middleware;
    require_invalid_configuration([&] { middleware.init_advanced(config); });
    require_uninitialized([&] { middleware.get_health_forecast(); });
    config = custom_configuration();
    config.ds_config.nominal_capacity_ah = 120.0;
    config.ds_config.nominal_voltage = 600.0;
    require_invalid_configuration([&] { middleware.init_advanced(config); });
}

void invalid_nominal_configuration() {
    const double invalid[] = {0.0, -1.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    for (double value : invalid) {
        for (bool embedded : {false, true}) {
            for (bool capacity : {false, true}) {
                MiddlewareConfig config;
                if (capacity) {
                    if (embedded) config.ds_config.nominal_capacity_ah = value;
                    else config.nominal_capacity_ah = value;
                } else {
                    if (embedded) config.ds_config.nominal_voltage = value;
                    else config.nominal_voltage = value;
                }
                DSBMSMiddleware middleware;
                require_invalid_configuration([&] { middleware.init_advanced(config); });
                require_uninitialized([&] { middleware.enhance_cycle(360,10,25,.8,1); });
            }
        }
        DSBMSMiddleware middleware;
        require_invalid_configuration([&] { middleware.init(value, 400.0); });
        require_invalid_configuration([&] { middleware.init(75.0, value); });
        require_uninitialized([&] { middleware.enhance_cycle(360,10,25,.8,1); });
    }
}

void invalid_advanced_configuration() {
    std::vector<MiddlewareConfig> invalid(8);
    invalid[0].ds_config.lambda = 0.0;
    invalid[1].ds_config.entropy_weight = std::numeric_limits<double>::quiet_NaN();
    invalid[2].mode = static_cast<MiddlewareConfig::Mode>(99);
    invalid[3].mode = MiddlewareConfig::Mode::MULTI_CELL_PACK;
    invalid[3].series_cells = 0;
    invalid[4].mode = MiddlewareConfig::Mode::MULTI_CELL_PACK;
    invalid[4].series_cells = -1;
    invalid[5].chemistry = static_cast<ds::advanced::ChemistryType>(99);
    invalid[6].logging_interval_s = 0.0;
    invalid[7].logging_interval_s = std::numeric_limits<double>::infinity();
    for (const auto& config : invalid) {
        DSBMSMiddleware middleware;
        require_invalid_configuration([&] { middleware.init_advanced(config); });
        require_uninitialized([&] { middleware.enhance_cycle(360,10,25,.8,1); });
    }
}

void invalid_safety_configuration() {
    // Nonfinite thresholds must not silently turn comparison-based safety
    // monitoring into an apparently healthy configuration.
    const std::vector<double ds_plugin::SafetyLimits::*> thresholds = {
        &ds_plugin::SafetyLimits::min_cell_voltage,
        &ds_plugin::SafetyLimits::max_cell_voltage,
        &ds_plugin::SafetyLimits::min_pack_voltage,
        &ds_plugin::SafetyLimits::max_pack_voltage,
        &ds_plugin::SafetyLimits::max_discharge_current,
        &ds_plugin::SafetyLimits::max_charge_current,
        &ds_plugin::SafetyLimits::min_operating_temp,
        &ds_plugin::SafetyLimits::max_operating_temp,
        &ds_plugin::SafetyLimits::max_cell_temp,
        &ds_plugin::SafetyLimits::min_soc,
        &ds_plugin::SafetyLimits::max_soc,
        &ds_plugin::SafetyLimits::max_voltage_imbalance_mv,
        &ds_plugin::SafetyLimits::min_health_percent
    };
    const double nonfinite[] = {std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    std::vector<MiddlewareConfig> invalid;
    for (auto threshold : thresholds) {
        for (double value : nonfinite) {
            MiddlewareConfig config;
            config.safety_limits.*threshold = value;
            invalid.push_back(config);
        }
    }
    std::vector<MiddlewareConfig> domain_errors(9);
    domain_errors[0].safety_limits.min_cell_voltage = 4.4;
    domain_errors[1].safety_limits.min_pack_voltage = 451.0;
    domain_errors[2].safety_limits.min_cell_voltage = -1.0;
    domain_errors[3].safety_limits.min_pack_voltage = -1.0;
    domain_errors[4].safety_limits.max_discharge_current = -1.0;
    domain_errors[5].safety_limits.max_charge_current = -1.0;
    domain_errors[6].safety_limits.max_voltage_imbalance_mv = -1.0;
    domain_errors[7].safety_limits.min_health_percent = -1.0;
    domain_errors[8].safety_limits.min_health_percent = 101.0;
    invalid.insert(invalid.end(), domain_errors.begin(), domain_errors.end());
    for (const auto& config : invalid) {
        DSBMSMiddleware fresh;
        require_invalid_configuration([&] { fresh.init_advanced(config); });
        require_uninitialized([&] { fresh.enhance_cycle(360,10,25,.8,1); });
        DSBMSMiddleware populated;
        DSBMSMiddleware control;
        populated.init(75,400);
        control.init(75,400);
        populated.enhance_cycle(360,10,25,.8,1);
        control.enhance_cycle(360,10,25,.8,1);
        const auto before = populated.diagnostics();
        require_invalid_configuration([&] { populated.init_advanced(config); });
        require_diagnostics_equal(populated.diagnostics(), before);
        require_state_equal(populated.enhance_cycle(360,10,25,.8,1),
                            control.enhance_cycle(360,10,25,.8,1));
    }
}

void invalid_derived_configuration() {
    // Finite nominal inputs can still make configured divisors or charging
    // margins nonfinite. Reject these constants at the initialization boundary.
    const double maximum = std::numeric_limits<double>::max();
    std::vector<MiddlewareConfig> invalid(2);
    invalid[0].nominal_capacity_ah = invalid[0].ds_config.nominal_capacity_ah = maximum;
    invalid[1].nominal_voltage = invalid[1].ds_config.nominal_voltage = maximum;
    for (const auto& config : invalid) {
        require(!config.ds_config.validate(),
                "unsafe derived constants must be invalid for direct DS configurations");
        ds::DSEnhancement direct;
        require_invalid_configuration([&] { direct.init(config.ds_config); });
        DSBMSMiddleware fresh;
        require_invalid_configuration([&] { fresh.init_advanced(config); });
        require_uninitialized([&] { fresh.enhance_cycle(360,10,25,.8,1); });
        DSBMSMiddleware established;
        DSBMSMiddleware control;
        established.init(75,400);
        control.init(75,400);
        established.enhance_cycle(360,-10,25,.8,1);
        control.enhance_cycle(360,-10,25,.8,1);
        const auto before = established.diagnostics();
        const auto telemetry = established.snapshot();
        require_invalid_configuration([&] { established.init_advanced(config); });
        require_diagnostics_equal(established.diagnostics(), before);
        require(established.snapshot().recovered_energy_kwh == telemetry.recovered_energy_kwh,
                "derived-configuration rejection must preserve accepted charge ledger");
        require_state_equal(established.enhance_cycle(360,10,25,.8,1),
                            control.enhance_cycle(360,10,25,.8,1));
    }
}

void failed_reinitialization_preserves_instance() {
    for (bool kalman : {false, true}) {
        MiddlewareConfig original = custom_configuration();
        original.mode = MiddlewareConfig::Mode::MULTI_CELL_PACK;
        original.series_cells = 4;
        original.enable_kalman_filter = kalman;
        std::vector<MiddlewareConfig> invalid(5, original);
        invalid[0].ds_config.lambda = std::numeric_limits<double>::quiet_NaN();
        invalid[1].nominal_capacity_ah = 75.0;
        invalid[2].chemistry = static_cast<ds::advanced::ChemistryType>(99);
        invalid[3].series_cells = -1;
        invalid[4].mode = static_cast<MiddlewareConfig::Mode>(99);
        for (const auto& config : invalid) {
            DSBMSMiddleware middleware;
            DSBMSMiddleware control;
            middleware.init_advanced(original);
            control.init_advanced(original);
            middleware.enhance_cycle(360.0, -20.0, 25.0, 0.04, 1.0);
            control.enhance_cycle(360.0, -20.0, 25.0, 0.04, 1.0);
            const auto before = middleware.diagnostics();
            const auto telemetry = middleware.snapshot();
            const auto advisories = middleware.advisories();
            const auto faults = middleware.faults();
            require_invalid_configuration([&] { middleware.init_advanced(config); });
            require_diagnostics_equal(middleware.diagnostics(), before);
            require(middleware.snapshot().recovered_energy_kwh == telemetry.recovered_energy_kwh,
                    "failed reinitialization must preserve charge ledger telemetry");
            require(middleware.advisories() == advisories && middleware.faults() == faults,
                    "failed reinitialization must preserve raw safety evidence");
            require_invalid_configuration([&] { middleware.init(0.0, 400.0); });
            require_diagnostics_equal(middleware.diagnostics(), before);
            const auto actual = middleware.enhance_cycle(360.0, 10.0, 25.0, 0.6, 2.0);
            const auto expected = control.enhance_cycle(360.0, 10.0, 25.0, 0.6, 2.0);
            require_state_equal(actual, expected);
            require(actual.state.state_of_charge == expected.state.state_of_charge,
                    "failed reinitialization must preserve Kalman filter history");
            require_diagnostics_equal(middleware.diagnostics(), control.diagnostics());
        }
    }
}

void successful_reinitialization_starts_coherent_session() {
    MiddlewareConfig original = custom_configuration();
    original.mode = MiddlewareConfig::Mode::MULTI_CELL_PACK;
    original.series_cells = 4;
    original.enable_kalman_filter = true;
    for (bool simple_api : {false, true}) {
        DSBMSMiddleware middleware;
        middleware.init_advanced(original);
        middleware.enhance_cycle(360.0, -20.0, 25.0, 0.04, 2.0);
        require(middleware.diagnostics().total_cells == 4,
                "test setup must have optional pack state");
        if (simple_api) middleware.init(75.0, 400.0);
        else middleware.init_advanced(MiddlewareConfig{});
        const auto& diagnostics = middleware.diagnostics();
        require(diagnostics.update_count == 0 && diagnostics.time_since_init_s == 0.0,
                "successful reinitialization must reset session counters");
        require(diagnostics.energy_throughput_kwh == 0.0 &&
                middleware.snapshot().recovered_energy_kwh == 0.0,
                "successful reinitialization must reset energy ledgers");
        require(diagnostics.last_update_time_ms == 0.0 &&
                diagnostics.average_update_time_ms == 0.0,
                "successful reinitialization must reset diagnostic timing");
        require(diagnostics.total_cells == 1 && middleware.advisories().empty() &&
                middleware.faults().empty(),
                "successful reinitialization must reset optional components and safety snapshot");
        ds::DSEnhancement direct;
        ds::DSConfig expected_configuration;
        if (simple_api) {
            // The simple API historically preserves established DS calibration
            // while forwarding its two explicit nominal arguments.
            expected_configuration = original.ds_config;
            expected_configuration.nominal_capacity_ah = 75.0;
            expected_configuration.nominal_voltage = 400.0;
        }
        direct.init(expected_configuration);
        const auto actual = middleware.enhance_cycle(360.0, 10.0, 25.0, 0.7, 1.0);
        const auto expected = direct.enhance(360.0, 10.0, 25.0, 0.7, 1.0);
        require_state_equal(actual, expected);
        require(actual.state.state_of_charge == expected.state.state_of_charge,
                "disabled Kalman filter must not survive successful reinitialization");
        require(middleware.diagnostics().total_cells == 1,
                "disabled pack must not survive successful reinitialization");
        require(middleware.diagnostics().update_count == 1,
                "new session must begin at the first accepted update");
    }
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> cases = {
        {"matching default and calibrated custom configurations", matching_configurations},
        {"capacity conflicts", conflicting_capacity},
        {"voltage conflicts", conflicting_voltage},
        {"simultaneous conflicts", simultaneous_conflicts},
        {"invalid nominal configuration", invalid_nominal_configuration},
        {"invalid advanced configuration", invalid_advanced_configuration},
        {"invalid safety configuration", invalid_safety_configuration},
        {"invalid derived configuration constants", invalid_derived_configuration},
        {"failed reinitialization preserves complete valid instance", failed_reinitialization_preserves_instance},
        {"successful reinitialization starts coherent session", successful_reinitialization_starts_coherent_session}
    };
    int failures = 0;
    for (const auto& test : cases) {
        try {
            test.second();
            std::cout << "PASS: " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << test.first << ": " << error.what() << '\n';
        }
    }
    return failures ? 1 : 0;
}
