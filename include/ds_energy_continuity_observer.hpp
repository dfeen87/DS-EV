#ifndef DS_ENERGY_CONTINUITY_OBSERVER_HPP
#define DS_ENERGY_CONTINUITY_OBSERVER_HPP

#include "ds_energy_continuity.hpp"
#include "ds_energy_recovery_coordinator_v1.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace ds {
namespace continuity {

// Capture after BMS initialization and before the caller's existing cycle.
// A checkpoint belongs to this instance/lifetime and this BMS initialization.
// Public diagnostics have no initialization generation: the caller must start
// a newer continuity session and capture anew after every BMS reinitialization.
// Concurrent component access is not supported by these passive adapters.
struct BMSCheckpoint {
    const ds_plugin::DSBMSMiddleware* source = nullptr;
    int update_count = 0;
    double elapsed_s = 0.0;
    double throughput_kwh = 0.0;
    double charging_kwh = 0.0;
};

struct BMSObservation {
    std::uint64_t session_id = 0;
    // Canonical event identity is the BMS accepted counter, never an alias's ID.
    std::uint64_t event_id = 0;
    TransferRecord battery_transfer;
    BMSCheckpoint next_checkpoint;
};

struct RecoveryObservation {
    // Same canonical battery event/transfer as observe_bms_cycle, not another
    // charging contribution. Use either representation, never both as energy.
    BMSObservation battery;
    TransferRecord mechanical_available;
    TransferRecord electrical_available;
};

namespace observer_detail {
inline double finite(double value, const char* message) {
    if (!std::isfinite(value)) throw std::overflow_error(message);
    return value;
}

inline bool agrees(double a, double b) noexcept {
    // Only floating-point evaluation/accumulation roundoff is allowed here;
    // this is not a configurable physical conservation tolerance.
    const double scale = std::max(std::abs(a), std::abs(b));
    return std::abs(a - b) <=
        16.0 * std::numeric_limits<double>::epsilon() * scale;
}

inline void require_agreement(double a, double b, const char* message) {
    finite(a, message);
    finite(b, message);
    if (!agrees(a, b)) throw std::invalid_argument(message);
}

inline void validate_checkpoint(const BMSCheckpoint& checkpoint) {
    if (!checkpoint.source || checkpoint.update_count < 0 ||
        !std::isfinite(checkpoint.elapsed_s) || checkpoint.elapsed_s < 0.0 ||
        !std::isfinite(checkpoint.throughput_kwh) || checkpoint.throughput_kwh < 0.0 ||
        !std::isfinite(checkpoint.charging_kwh) || checkpoint.charging_kwh < 0.0 ||
        checkpoint.charging_kwh > checkpoint.throughput_kwh)
        throw std::invalid_argument("Invalid BMS acceptance checkpoint");
}

inline void validate_evidence(const Evidence& evidence) {
    // Energy from power and duration is calculated, even if sensors supplied
    // V/I. No metrology or sensor authentication is inferred from BMS success.
    if (evidence.provenance_id == 0 ||
        (evidence.kind != EvidenceKind::Calculated &&
         evidence.kind != EvidenceKind::Simulated &&
         evidence.kind != EvidenceKind::Estimated))
        throw std::invalid_argument("Calculated battery energy needs explicit provenance");
}

inline void validate_counterparty(PhysicalNode counterparty) {
    if (counterparty == PhysicalNode::Battery ||
        node_index(counterparty) >= kNodeCount)
        throw std::invalid_argument("Invalid battery energy counterparty");
}

inline double power_energy_joules(double power_kw, double dt_s) {
    return finite(finite(power_kw * 1000.0, "Observer power conversion overflow") * dt_s,
                  "Observer energy multiplication overflow");
}

inline double power_energy_kwh(double power_kw, double dt_s) {
    // Match accepted software-ledger units without converting cumulative
    // aliases into transfers. This per-step expression is checked separately.
    return finite(finite(power_kw * dt_s, "Observer kWh multiplication overflow") / 3600.0,
                  "Observer kWh conversion overflow");
}
} // namespace observer_detail

inline BMSCheckpoint capture_bms_checkpoint(const ds_plugin::DSBMSMiddleware& bms) {
    const auto& diagnostic = bms.diagnostics();
    const auto telemetry = bms.snapshot();
    BMSCheckpoint result{&bms, diagnostic.update_count, diagnostic.time_since_init_s,
                         diagnostic.energy_throughput_kwh, telemetry.recovered_energy_kwh};
    observer_detail::validate_checkpoint(result);
    observer_detail::require_agreement(diagnostic.instantaneous_power_kw,
        telemetry.pack_power_kw, "Battery telemetry power disagrees with BMS");
    const double expected_charging_kw = diagnostic.instantaneous_power_kw < 0.0
        ? -diagnostic.instantaneous_power_kw : 0.0;
    observer_detail::require_agreement(expected_charging_kw, telemetry.regen_power_kw,
                                      "Legacy charging-power alias disagrees with BMS");
    return result;
}

// This factory neither executes a cycle nor commits any checkpoint or ledger.
// Only a single newly accepted cycle is eligible. A rejected cycle leaves the
// counter unchanged and fails here. The caller retains its old checkpoint on
// adapter/reconciliation rejection and commits next_checkpoint deliberately.
// The physical counterparty is a caller assertion: charging alone never proves
// regeneration. ExternalSource is suitable for unattributed incoming charging.
inline BMSObservation observe_bms_cycle(
    const BMSCheckpoint& before, const ds_plugin::DSBMSMiddleware& bms,
    std::uint64_t session_id, PhysicalNode counterparty, Evidence evidence) {
    using namespace observer_detail;
    validate_checkpoint(before);
    validate_counterparty(counterparty);
    validate_evidence(evidence);
    if (session_id == 0 || before.source != &bms)
        throw std::invalid_argument("BMS checkpoint/session source mismatch");
    const BMSCheckpoint after = capture_bms_checkpoint(bms);
    if (before.update_count == std::numeric_limits<int>::max() ||
        after.update_count != before.update_count + 1)
        throw std::invalid_argument("Observer requires exactly one newly accepted BMS cycle");
    const auto& diagnostic = bms.diagnostics();
    const double dt_s = finite(diagnostic.last_update_time_ms / 1000.0,
                               "Nonfinite accepted BMS duration");
    if (dt_s <= 0.0 || after.elapsed_s <= before.elapsed_s)
        throw std::invalid_argument("Accepted BMS interval is not positive/representable");
    require_agreement(finite(before.elapsed_s + dt_s, "Observer elapsed-time overflow"),
                      after.elapsed_s, "Accepted BMS elapsed time is inconsistent");
    const double signed_joules = power_energy_joules(diagnostic.instantaneous_power_kw, dt_s);
    const double signed_kwh = power_energy_kwh(diagnostic.instantaneous_power_kw, dt_s);
    const double charging_increment = signed_kwh < 0.0 ? -signed_kwh : 0.0;
    require_agreement(finite(before.charging_kwh + charging_increment,
                             "Observer accepted charging overflow"),
                      after.charging_kwh, "Accepted BMS charging ledger is inconsistent");
    require_agreement(finite(before.throughput_kwh + std::abs(signed_kwh),
                             "Observer accepted throughput overflow"),
                      after.throughput_kwh, "Accepted BMS throughput ledger is inconsistent");

    BMSObservation result;
    result.session_id = session_id;
    result.event_id = static_cast<std::uint64_t>(after.update_count);
    result.next_checkpoint = after;
    auto& transfer = result.battery_transfer;
    transfer.transfer_id = 1;
    const bool charging = diagnostic.instantaneous_power_kw < 0.0;
    transfer.source = charging ? counterparty : PhysicalNode::Battery;
    transfer.destination = charging ? PhysicalNode::Battery : counterparty;
    transfer.joules = std::abs(signed_joules);
    transfer.start_s = before.elapsed_s;
    transfer.end_s = after.elapsed_s;
    evidence.origin = EvidenceOrigin::Telemetry;
    transfer.evidence = evidence;
    return result;
}

// The successful coordinator result supplies the VI provenance and available
// estimates. It must describe the same immediate BMS acceptance as the before/
// after checkpoints. No cumulative recovery/telemetry alias becomes new energy.
// The available edges describe the coordinator's illustrative modeled path,
// not measurements of actual shaft delivery, kinetic loss, or thermal loss.
inline RecoveryObservation observe_recovery_cycle(
    const BMSCheckpoint& before, const ds_plugin::DSBMSMiddleware& bms,
    const ds::drive::EnergyRecoveryResult& accepted_result,
    std::uint64_t session_id, PhysicalNode battery_counterparty, Evidence evidence) {
    using namespace observer_detail;
    const auto& recovery = accepted_result.diag;
    if (recovery.mode == "MEASURED_CURRENT") {
        if (evidence.kind != EvidenceKind::Calculated && evidence.kind != EvidenceKind::Simulated)
            throw std::invalid_argument("Measured-current mode needs calculated/simulated provenance");
    } else if (recovery.mode == "ESTIMATED_CURRENT") {
        if (evidence.kind != EvidenceKind::Estimated && evidence.kind != EvidenceKind::Simulated)
            throw std::invalid_argument("Estimated-current mode must remain estimated/simulated");
        if (!std::isfinite(accepted_result.pack_current_a_used) ||
            accepted_result.pack_current_a_used > 0.0)
            throw std::invalid_argument("Estimated recovery current must be nonpositive");
        evidence.independently_supported = false;
    } else {
        throw std::invalid_argument("Unknown recovery evidence mode");
    }

    RecoveryObservation result;
    result.battery = observe_bms_cycle(before, bms, session_id, battery_counterparty, evidence);
    const auto& current = bms.diagnostics();
    if (accepted_result.bms_diag.update_count != current.update_count)
        throw std::invalid_argument("Recovery result is not the current accepted BMS cycle");
    require_agreement(accepted_result.bms_diag.time_since_init_s, current.time_since_init_s,
                      "Recovery accepted timestamp disagrees with BMS");
    require_agreement(accepted_result.bms_diag.instantaneous_power_kw, current.instantaneous_power_kw,
                      "Recovery accepted power disagrees with BMS");
    require_agreement(accepted_result.bms_diag.energy_throughput_kwh, current.energy_throughput_kwh,
                      "Recovery accepted throughput disagrees with BMS");
    const double dt_s = current.last_update_time_ms / 1000.0;
    require_agreement(accepted_result.dt_s, dt_s, "Recovery interval disagrees with BMS");
    require_agreement(recovery.last_accepted_voltage_v, accepted_result.pack_voltage_v,
                      "Recovery accepted voltage alias disagrees");
    require_agreement(recovery.last_accepted_current_a, accepted_result.pack_current_a_used,
                      "Recovery accepted current alias disagrees");
    const auto signed_energy = battery_increment_joules(accepted_result.pack_voltage_v,
                                                       accepted_result.pack_current_a_used, dt_s);
    if (!signed_energy) throw std::invalid_argument("Invalid recovery VI energy evidence");
    const double expected_signed = current.instantaneous_power_kw < 0.0
        ? -result.battery.battery_transfer.joules : result.battery.battery_transfer.joules;
    require_agreement(signed_energy.value, expected_signed,
                      "Recovery VI energy disagrees with accepted BMS power");
    result.battery.battery_transfer.evidence.origin = EvidenceOrigin::BatteryVI;

    require_agreement(recovery.accepted_charging_energy_kwh_total,
                      recovery.recovered_energy_kwh_total, "Recovery charging total aliases disagree");
    require_agreement(recovery.accepted_charging_energy_kwh_session,
                      recovery.recovered_energy_kwh_session, "Recovery charging session aliases disagree");
    require_agreement(recovery.last_regen_power_kw_est, accepted_result.regen_power_kw_est,
                      "Recovery available electrical power aliases disagree");
    require_agreement(recovery.last_regen_torque_nm, accepted_result.regen_torque_nm,
                      "Recovery available torque aliases disagree");
    const double mechanical = power_energy_joules(recovery.last_mechanical_power_kw_est, dt_s);
    const double electrical = power_energy_joules(recovery.last_regen_power_kw_est, dt_s);
    if (mechanical < 0.0 || electrical < 0.0)
        throw std::invalid_argument("Available recovery energy must be nonnegative");
    if (electrical > mechanical && !agrees(electrical, mechanical))
        throw std::invalid_argument("Available electrical recovery exceeds mechanical estimate");

    // Availability is not physical delivery. Both records remain estimates
    // even when the battery current came from caller-asserted measurements.
    Evidence estimate{EvidenceKind::Estimated, EvidenceOrigin::RecoveryMechanical,
                      evidence.provenance_id, false};
    result.mechanical_available = {2, PhysicalNode::KineticStore, PhysicalNode::Motor,
        mechanical, result.battery.battery_transfer.start_s,
        result.battery.battery_transfer.end_s, estimate, QuantityPurpose::AvailableEstimate, true};
    estimate.origin = EvidenceOrigin::RecoveryElectrical;
    result.electrical_available = {3, PhysicalNode::Motor, PhysicalNode::PowerElectronics,
        electrical, result.battery.battery_transfer.start_s,
        result.battery.battery_transfer.end_s, estimate, QuantityPurpose::AvailableEstimate, true};
    return result;
}

} // namespace continuity
} // namespace ds

#endif // DS_ENERGY_CONTINUITY_OBSERVER_HPP
