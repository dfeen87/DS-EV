#include "ds_energy_recovery_coordinator_v1.hpp"

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace ds_plugin {
// Synthetic pack setup through the existing regression-only access seam. This
// exercises a nonempty diagnostic vector; no public pack mutation API is added.
struct MiddlewareAccountingTestAccess {
    static void seed_weak_cell(DSBMSMiddleware& bms) {
        if (!bms.pack_) return;
        const auto cells = bms.pack_->get_cells().size();
        std::vector<double> voltages(cells, 3.7);
        std::vector<double> temperatures(cells, 25.0);
        temperatures[0] = 60.0;
        bms.pack_->update_all_cells(voltages, temperatures, 10.0, 1.0);
    }
};
}

namespace {
// Fault injection checks the cross-object acceptance boundary, including builds
// that disable NRVO. Allocation after the BMS accepted counter advances fails.
const ds_plugin::DSBMSMiddleware* allocation_guard_bms = nullptr;
int allocation_guard_previous_count = 0;
}

// Keep replacement operators as call boundaries: their malloc/free pairing is
// intentional and must remain observable to allocation fault injection.
#if defined(__GNUC__) || defined(__clang__)
#define DS_RECOVERY_ALLOCATION_BOUNDARY __attribute__((noinline))
#else
#define DS_RECOVERY_ALLOCATION_BOUNDARY
#endif

DS_RECOVERY_ALLOCATION_BOUNDARY void* operator new(std::size_t size) {
    if (allocation_guard_bms && allocation_guard_bms->diagnostics().update_count > allocation_guard_previous_count)
        throw std::bad_alloc();
    if (void* value = std::malloc(size ? size : 1u)) return value;
    throw std::bad_alloc();
}
DS_RECOVERY_ALLOCATION_BOUNDARY void* operator new[](std::size_t size) { return ::operator new(size); }
DS_RECOVERY_ALLOCATION_BOUNDARY void operator delete(void* value) noexcept { std::free(value); }
DS_RECOVERY_ALLOCATION_BOUNDARY void operator delete[](void* value) noexcept { std::free(value); }
DS_RECOVERY_ALLOCATION_BOUNDARY void operator delete(void* value, std::size_t) noexcept { std::free(value); }
DS_RECOVERY_ALLOCATION_BOUNDARY void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

#undef DS_RECOVERY_ALLOCATION_BOUNDARY

namespace {
int checks = 0;
int failures = 0;

void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool close(double a, double b) {
    return std::abs(a - b) <= 1e-12 * std::max(1.0, std::max(std::abs(a), std::abs(b)));
}

template<class Function>
void rejects(Function function, const std::string& message) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}

ds::drive::RegenResult regen(double torque) {
    ds::drive::RegenResult value;
    value.max_regen_torque_nm = torque;
    value.regen_fraction = torque > 0.0 ? 1.0 : 0.0;
    value.limiting_factor = "TEST";
    return value;
}

ds::drive::EnergyRecoveryConfig estimated_config() {
    ds::drive::EnergyRecoveryConfig value;
    value.prefer_measured_pack_current = false;
    return value;
}

void equal_diagnostics(const ds::drive::EnergyRecoveryDiagnostics& a,
                       const ds::drive::EnergyRecoveryDiagnostics& b,
                       const std::string& context) {
    check(a.last_regen_torque_nm == b.last_regen_torque_nm &&
          a.last_regen_power_kw_est == b.last_regen_power_kw_est &&
          a.last_regen_current_a_est == b.last_regen_current_a_est &&
          a.recovered_energy_kwh_total == b.recovered_energy_kwh_total &&
          a.recovered_energy_kwh_session == b.recovered_energy_kwh_session &&
          a.mode == b.mode && a.limiting_factor == b.limiting_factor &&
          a.accepted_charging_energy_kwh_total == b.accepted_charging_energy_kwh_total &&
          a.accepted_charging_energy_kwh_session == b.accepted_charging_energy_kwh_session &&
          a.last_accepted_voltage_v == b.last_accepted_voltage_v &&
          a.last_accepted_current_a == b.last_accepted_current_a &&
          a.last_mechanical_power_kw_est == b.last_mechanical_power_kw_est &&
          a.estimated_available_mechanical_energy_kwh_total == b.estimated_available_mechanical_energy_kwh_total &&
          a.estimated_available_mechanical_energy_kwh_session == b.estimated_available_mechanical_energy_kwh_session &&
          a.estimated_available_electrical_energy_kwh_total == b.estimated_available_electrical_energy_kwh_total &&
          a.estimated_available_electrical_energy_kwh_session == b.estimated_available_electrical_energy_kwh_session &&
          a.charging_energy_source == b.charging_energy_source,
          context + ": rejected attempt changed coordinator diagnostics");
}

void test_original_rejections() {
    for (bool warmed : {false, true}) {
        for (int reason = 0; reason < 3; ++reason) {
            ds::drive::DSEnergyRecoveryCoordinator subject(estimated_config()), reference(estimated_config());
            ds_plugin::DSBMSMiddleware bms, expected_bms, uninitialized;
            bms.init(75.0, 400.0); expected_bms.init(75.0, 400.0);
            if (warmed) {
                subject.step(bms, regen(100.0), 400.0, 25.0, 0.0, 0.5, 100.0, 0.1);
                reference.step(expected_bms, regen(100.0), 400.0, 25.0, 0.0, 0.5, 100.0, 0.1);
            }
            const auto before = subject.diagnostics();
            const auto bms_before = bms.diagnostics();
            rejects([&] {
                subject.step(reason == 2 ? uninitialized : bms, regen(200.0), 400.0,
                    reason == 0 ? std::numeric_limits<double>::quiet_NaN() : 25.0,
                    0.0, 0.5, 100.0, reason == 1 ? 0.001 : 0.1);
            }, "PB-09: malformed, subminimum or unavailable BMS cycle must reject");
            equal_diagnostics(subject.diagnostics(), before, "PB-09 original rejected cycle");
            check(bms.diagnostics().update_count == bms_before.update_count &&
                  bms.diagnostics().energy_throughput_kwh == bms_before.energy_throughput_kwh &&
                  bms.diagnostics().time_since_init_s == bms_before.time_since_init_s,
                  "PB-09: rejected cycle changed accepted BMS history");
            const auto recovered = subject.step(bms, regen(0.0), 400.0, 25.0, 0.0, 0.5, 100.0, 0.1);
            const auto expected = reference.step(expected_bms, regen(0.0), 400.0, 25.0, 0.0, 0.5, 100.0, 0.1);
            check(recovered.pack_current_a_used == expected.pack_current_a_used,
                  "PB-09: rejected cycle contaminated smoothing history");
            equal_diagnostics(recovered.diag, expected.diag, "PB-09 recovery matches unchanged reference");
        }
    }
}

void test_accepted_energy() {
    for (double cap : {0.0, 1.0, 10.0, 250.0}) {
        auto config = estimated_config();
        config.max_estimated_regen_current_a = cap;
        ds::drive::DSEnergyRecoveryCoordinator coordinator(config);
        ds_plugin::DSBMSMiddleware bms; bms.init(75.0, 400.0);
        for (int cycle = 0; cycle < 4; ++cycle) {
            const auto result = coordinator.step(bms, regen(250.0), 400.0, 25.0, 0.0, 0.5, 10000.0, 1.0);
            check(result.pack_current_a_used <= 0.0 && result.pack_current_a_used >= -cap,
                  "PB-09: estimated current cap and charging sign");
            check(close(result.diag.recovered_energy_kwh_total, bms.snapshot().recovered_energy_kwh),
                  "PB-09: recovery accounting must match accepted limited/smoothed battery energy");
            check(result.bms_diag.update_count == cycle + 1 &&
                  result.bms_diag.energy_throughput_kwh == bms.diagnostics().energy_throughput_kwh,
                  "PB-09: exactly one BMS update and coherent returned diagnostics");
        }
    }
    ds::drive::DSEnergyRecoveryCoordinator measured;
    ds_plugin::DSBMSMiddleware bms; bms.init(75.0, 400.0);
    bms.enhance_cycle(360.0, -20.0, 25.0, 0.5, 1.0);
    auto charge = measured.step(bms, regen(0.0), 360.0, 25.0, -10.0, 0.5, 0.0, 1.0);
    check(charge.diag.recovered_energy_kwh_total == 0.001,
          "PB-09: volts*amps/1000*seconds/3600 has correct kWh units");
    check(bms.snapshot().recovered_energy_kwh == 0.003,
          "PB-09: coordinator observation must not credit prior energy or add an extra BMS ledger increment");
    auto discharge = measured.step(bms, regen(100.0), 360.0, 25.0, 10.0, 0.5, 100.0, 1.0);
    check(discharge.diag.recovered_energy_kwh_total == 0.001,
          "PB-09: allowed regen torque does not establish battery charging");
    auto raw = measured.step(bms, regen(0.0), 360.0, 25.0, -2000.0, 0.5, 0.0, 1.0);
    check(raw.pack_current_a_used == -2000.0 && raw.enhanced.state.current == -500.0 &&
          close(raw.diag.recovered_energy_kwh_total, 0.201),
          "PB-09: accepted raw charging energy must not use normalized model current");
    measured.reset_session_energy();
    check(measured.diagnostics().recovered_energy_kwh_session == 0.0 &&
          close(measured.diagnostics().recovered_energy_kwh_total, 0.201),
          "PB-09: session reset preserves lifetime charging ledger");
}

void test_attribution_and_available_estimates() {
    for (bool estimated : {false, true}) {
        auto config = estimated_config(); config.prefer_measured_pack_current = !estimated;
        ds::drive::DSEnergyRecoveryCoordinator coordinator(config);
        ds_plugin::DSBMSMiddleware bms; bms.init(75.0, 400.0);
        const auto zero_torque = coordinator.step(bms, regen(0.0), 400.0, 25.0, -10.0, 0.5, 100.0, 1.0);
        check(zero_torque.diag.accepted_charging_energy_kwh_total == zero_torque.diag.recovered_energy_kwh_total &&
              zero_torque.diag.accepted_charging_energy_kwh_session == zero_torque.diag.recovered_energy_kwh_session,
              "PB-09: legacy recovered fields remain aliases of explicit accepted charging fields");
        check(zero_torque.diag.last_mechanical_power_kw_est == 0.0 &&
              zero_torque.diag.estimated_available_electrical_energy_kwh_total == 0.0,
              "PB-09: negative measured current cannot establish available regen power");
        check(zero_torque.diag.charging_energy_source == (estimated ? "SIMULATED_REGEN_ESTIMATE" : "UNATTRIBUTED_MEASURED"),
              "PB-09: charging provenance distinguishes source-unknown measurement from simulation");
        const auto result = coordinator.step(bms, regen(250.0), 400.0, 25.0, -10.0, 0.5, 10000.0, 1.0);
        check(result.regen_power_kw_est == 2125.0 && result.diag.last_mechanical_power_kw_est == 2500.0,
              "PB-09: mechanical shaft rad/s power remains separate from electrical estimate");
        check(result.diag.estimated_available_mechanical_energy_kwh_total == 2500.0 / 3600.0 &&
              result.diag.estimated_available_electrical_energy_kwh_total == 2125.0 / 3600.0,
              "PB-09: available estimate diagnostics preserve units without crediting delivered energy");
        check(result.diag.accepted_charging_energy_kwh_total <
              result.diag.estimated_available_electrical_energy_kwh_total,
              "PB-09: unconstrained electrical estimate is not battery-side charging");
        const auto total = coordinator.diagnostics();
        coordinator.reset_session_energy();
        check(coordinator.diagnostics().accepted_charging_energy_kwh_session == 0.0 &&
              coordinator.diagnostics().estimated_available_mechanical_energy_kwh_session == 0.0 &&
              coordinator.diagnostics().estimated_available_electrical_energy_kwh_session == 0.0 &&
              coordinator.diagnostics().accepted_charging_energy_kwh_total == total.accepted_charging_energy_kwh_total &&
              coordinator.diagnostics().estimated_available_mechanical_energy_kwh_total == total.estimated_available_mechanical_energy_kwh_total &&
              coordinator.diagnostics().estimated_available_electrical_energy_kwh_total == total.estimated_available_electrical_energy_kwh_total,
              "PB-09: session reset clears session diagnostics and preserves lifetime totals");
    }
}

void test_no_allocation_after_acceptance() {
    for (auto mode : {ds_plugin::MiddlewareConfig::Mode::SIMPLE,
                      ds_plugin::MiddlewareConfig::Mode::MULTI_CELL_PACK,
                      ds_plugin::MiddlewareConfig::Mode::ADVANCED_ML}) {
        ds_plugin::MiddlewareConfig config; config.mode = mode;
        config.enable_kalman_filter = mode == ds_plugin::MiddlewareConfig::Mode::ADVANCED_ML;
        ds_plugin::DSBMSMiddleware bms; bms.init_advanced(config);
        ds_plugin::MiddlewareAccountingTestAccess::seed_weak_cell(bms);
        ds::drive::DSEnergyRecoveryCoordinator coordinator(estimated_config());
        auto request = regen(100.0); request.limiting_factor = std::string(8192, 'x');
        bool threw = false;
        allocation_guard_previous_count = bms.diagnostics().update_count;
        allocation_guard_bms = &bms;
        try {
            auto result = coordinator.step(bms, request, 400.0, 25.0, 0.0, 0.5, 100.0, 0.1);
            allocation_guard_bms = nullptr;
            check(result.diag.limiting_factor == request.limiting_factor &&
                  result.bms_diag.update_count == 1 &&
                  result.bms_diag.weak_cell_ids.capacity() >= bms.diagnostic_cell_capacity(),
                  "PB-09: complete accepted result and diagnostic cell bound in every middleware mode");
            if (mode != ds_plugin::MiddlewareConfig::Mode::SIMPLE)
                check(!result.bms_diag.weak_cell_ids.empty() && result.bms_diag.weak_cell_ids.front() == 0,
                      "PB-09: allocation guard covers copying actual weak-cell IDs after acceptance");
        } catch (...) { allocation_guard_bms = nullptr; threw = true; }
        check(!threw, "PB-09: no allocation may fail after BMS acceptance");
    }
}

void test_domain_and_energy_overflow() {
    for (bool estimated : {false, true}) {
        for (int field = 0; field < 7; ++field) {
            auto config = estimated_config(); config.prefer_measured_pack_current = !estimated;
            ds::drive::DSEnergyRecoveryCoordinator coordinator(config);
            ds_plugin::DSBMSMiddleware bms; bms.init(75.0, 400.0);
            auto request = regen(100.0);
            double voltage = 400.0, soc = 0.5, omega = 100.0, dt = 0.1;
            switch (field) {
            case 0: voltage = 0.0; break;
            case 1: soc = -0.01; break;
            case 2: soc = 1.01; break;
            case 3: omega = -1.0; break;
            case 4: dt = 0.0; break;
            case 5: request.max_regen_torque_nm = -1.0; break;
            case 6: request.regen_fraction = 1.01; break;
            }
            const auto before = coordinator.diagnostics();
            rejects([&] { coordinator.step(bms, request, voltage, 25.0, 0.0, soc, omega, dt); },
                    "PB-09: invalid finite input domain must reject");
            equal_diagnostics(coordinator.diagnostics(), before, "PB-09 finite domain rejection");
            check(bms.diagnostics().update_count == 0,
                  "PB-09: invalid finite domain must not update BMS");
        }
    }
    for (double current : {std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(), 1e303, -1e303}) {
        ds::drive::DSEnergyRecoveryCoordinator coordinator;
        ds_plugin::DSBMSMiddleware bms; bms.init(75.0, 400.0);
        const auto before = coordinator.diagnostics();
        rejects([&] { coordinator.step(bms, regen(0.0), 360.0, 25.0, current, 0.5, 0.0,
                                      std::abs(current) == 1e303 ? 1e8 : 1.0); },
                "PB-09: both signs of finite W or kW-seconds overflow must reject");
        equal_diagnostics(coordinator.diagnostics(), before, "PB-09 raw battery energy overflow");
        check(bms.diagnostics().update_count == 0,
              "PB-09: raw battery energy overflow must not update BMS");
    }
    auto config = estimated_config(); config.drivetrain_efficiency_regen = 0.0;
    ds::drive::DSEnergyRecoveryCoordinator coordinator(config);
    ds_plugin::DSBMSMiddleware bms; bms.init(75.0, 400.0);
    for (int count = 0; count < 4000; ++count)
        coordinator.step(bms, regen(std::numeric_limits<double>::max() / 2.0), 400.0, 25.0, 0.0, 0.5, 1.0, 1800.0);
    const auto before = coordinator.diagnostics();
    const int accepted = bms.diagnostics().update_count;
    rejects([&] { coordinator.step(bms, regen(std::numeric_limits<double>::max() / 2.0),
                                   400.0, 25.0, 0.0, 0.5, 1.0, 1800.0); },
            "PB-09: cumulative available-estimate overflow must reject despite finite zero BMS energy");
    equal_diagnostics(coordinator.diagnostics(), before, "PB-09 cumulative available estimate overflow");
    check(bms.diagnostics().update_count == accepted && bms.snapshot().recovered_energy_kwh == 0.0,
          "PB-09: rejected cumulative estimate must not commit BMS or charge ledger");
    const auto recovered = coordinator.step(bms, regen(0.0), 400.0, 25.0, 0.0, 0.5, 0.0, 1.0);
    check(recovered.pack_current_a_used == 0.0 && recovered.bms_diag.update_count == accepted + 1,
          "PB-09: ordinary operation recovers after cumulative estimate rejection");
}

void test_valid_smoothing_and_voltage_floor() {
    auto config = estimated_config();
    config.current_estimate_tau_s = 0.5;
    config.max_estimated_regen_current_a = 20.0;
    ds::drive::DSEnergyRecoveryCoordinator coordinator(config);
    ds_plugin::DSBMSMiddleware bms; bms.init(75.0, 400.0);
    double current = 0.0;
    for (double torque : {100.0, 0.0, 50.0}) {
        const double target = -std::min(torque * 100.0 * 0.85 / 400.0, 20.0);
        current += (1.0 - std::exp(-0.1 / 0.5)) * (target - current);
        const auto result = coordinator.step(bms, regen(torque), 400.0, 25.0, 0.0, 0.5, 100.0, 0.1);
        check(result.pack_current_a_used == current,
              "PB-09: valid configurable smoothing retains exact original equation");
    }
    config.current_estimate_tau_s = 0.0;
    config.max_estimated_regen_current_a = 250.0;
    ds::drive::DSEnergyRecoveryCoordinator floor(config);
    ds_plugin::DSBMSMiddleware small_voltage; small_voltage.init(75.0, 400.0);
    const auto result = floor.step(small_voltage, regen(100.0), 25.0, 25.0, 0.0, 0.5, 100.0, 1.0);
    check(result.pack_current_a_used == -170.0,
          "PB-09: configured minimum conversion voltage remains effective");
    check(close(result.diag.recovered_energy_kwh_total, 25.0 * 170.0 / 1000.0 / 3600.0),
          "PB-09: charging ledger uses actual accepted voltage rather than estimation voltage floor");
}

void test_invalid_configuration() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (int field = 0; field < 4; ++field) {
        for (double invalid : {nan, inf, -inf, -1.0}) {
            auto config = estimated_config();
            double* fields[] = {&config.drivetrain_efficiency_regen, &config.max_estimated_regen_current_a,
                                &config.min_pack_voltage_v, &config.current_estimate_tau_s};
            *fields[field] = invalid;
            rejects([&] { ds::drive::DSEnergyRecoveryCoordinator coordinator(config); },
                    "PB-09: malformed configuration must reject before any cycle");
        }
    }
    auto config = estimated_config(); config.drivetrain_efficiency_regen = 1.01;
    rejects([&] { ds::drive::DSEnergyRecoveryCoordinator coordinator(config); },
            "PB-09: estimated efficiency outside [0,1] must reject");
    config = estimated_config(); config.min_pack_voltage_v = 0.0;
    rejects([&] { ds::drive::DSEnergyRecoveryCoordinator coordinator(config); },
            "PB-09: zero conversion-voltage denominator must reject");
}

void test_malformed_and_overflow_inputs() {
    for (bool estimated : {false, true}) {
        for (int field = 0; field < 9; ++field) {
            for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity(),
                                   -std::numeric_limits<double>::infinity()}) {
                auto config = estimated_config(); config.prefer_measured_pack_current = !estimated;
                ds::drive::DSEnergyRecoveryCoordinator subject(config), reference(config);
                ds_plugin::DSBMSMiddleware bms, expected_bms; bms.init(75.0, 400.0); expected_bms.init(75.0, 400.0);
                subject.step(bms, regen(100.0), 400.0, 25.0, -10.0, 0.5, 100.0, 0.1);
                reference.step(expected_bms, regen(100.0), 400.0, 25.0, -10.0, 0.5, 100.0, 0.1);
                auto request = regen(200.0);
                double voltage = 400.0, temperature = 25.0, current = -10.0, soc = 0.5, omega = 100.0, dt = 0.1;
                double* fields[] = {&voltage, &temperature, &current, &soc, &omega, &dt,
                                    &request.max_regen_torque_nm, &request.regen_fraction, &current};
                *fields[field] = invalid;
                const auto before = subject.diagnostics();
                const int updates = bms.diagnostics().update_count;
                rejects([&] { subject.step(bms, request, voltage, temperature, current, soc, omega, dt); },
                        "PB-09: nonfinite numeric inputs must reject in either mode");
                equal_diagnostics(subject.diagnostics(), before, "PB-09 nonfinite input");
                check(bms.diagnostics().update_count == updates,
                      "PB-09: nonfinite input must not advance BMS model");
                const auto recovered = subject.step(bms, regen(50.0), 400.0, 25.0, -10.0, 0.5, 100.0, 0.1);
                const auto expected = reference.step(expected_bms, regen(50.0), 400.0, 25.0, -10.0, 0.5, 100.0, 0.1);
                check(recovered.pack_current_a_used == expected.pack_current_a_used,
                      "PB-09: nonfinite rejection must preserve smoothing");
            }
        }
        auto config = estimated_config(); config.prefer_measured_pack_current = !estimated;
        ds::drive::DSEnergyRecoveryCoordinator subject(config);
        ds_plugin::DSBMSMiddleware bms; bms.init(75.0, 400.0);
        const auto before = subject.diagnostics();
        auto request = regen(std::numeric_limits<double>::max());
        rejects([&] { subject.step(bms, request, 400.0, 25.0, -10.0, 0.5, 2.0, 0.1); },
                "PB-09: finite mechanical-power overflow must reject even when current cap is finite");
        equal_diagnostics(subject.diagnostics(), before, "PB-09 finite overflow");
        check(bms.diagnostics().update_count == 0,
              "PB-09: finite estimate overflow must not commit BMS cycle");
    }
}
}

int main() {
    test_original_rejections();
    test_accepted_energy();
    test_attribution_and_available_estimates();
    test_no_allocation_after_acceptance();
    test_domain_and_energy_overflow();
    test_valid_smoothing_and_voltage_floor();
    test_invalid_configuration();
    test_malformed_and_overflow_inputs();
    if (failures) {
        std::cerr << failures << " of " << checks << " PB-09 recovery checks failed\n";
        return 1;
    }
    std::cout << checks << " PB-09 recovery integrity checks passed\n";
    return 0;
}
