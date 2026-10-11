#include "ds_energy_continuity_observer.hpp"

#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace ec = ds::continuity;
using Node = ec::PhysicalNode;

namespace {
const char* name(ec::ReconciliationStatus state) {
    switch (state) {
        case ec::ReconciliationStatus::Balanced: return "BALANCED";
        case ec::ReconciliationStatus::Imbalanced: return "IMBALANCED";
        case ec::ReconciliationStatus::InsufficientEvidence: return "INSUFFICIENT_EVIDENCE";
        case ec::ReconciliationStatus::Rejected: return "REJECTED";
    }
    return "INVALID";
}
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
ec::Evidence simulated(std::uint64_t provenance) {
    return {ec::EvidenceKind::Simulated, ec::EvidenceOrigin::Supplied, provenance, true};
}
ec::Batch vehicle_batch(std::uint64_t event, double battery_initial, double battery_final,
                        double kinetic_initial, double kinetic_final) {
    ec::Batch batch;
    batch.session_id = 1;
    batch.event_id = event;
    batch.start_s = static_cast<double>(event - 1);
    batch.end_s = static_cast<double>(event);
    for (Node node : {Node::Battery, Node::PowerElectronics, Node::Motor, Node::KineticStore})
        batch.boundary[ec::node_index(node)] = true;
    // Fixed analytic fixture values are supplied separately from the transfer
    // observations. They are simulated stores, never SOC-derived measurements.
    batch.storage[0] = {Node::Battery, battery_initial, battery_final, simulated(100), true};
    batch.storage[1] = {Node::PowerElectronics, 0, 0, simulated(101), true};
    batch.storage[2] = {Node::Motor, 0, 0, simulated(102), true};
    batch.storage[3] = {Node::KineticStore, kinetic_initial, kinetic_final, simulated(103), true};
    batch.storage_count = 4;
    batch.transfers_complete = batch.stores_complete = true;
    return batch;
}
void transfer(ec::Batch& batch, Node source, Node destination, double joules,
              ec::QuantityPurpose purpose = ec::QuantityPurpose::PhysicalTransfer) {
    require(batch.transfer_count < ec::kMaxTransfers, "simulation transfer capacity");
    const auto id = batch.transfer_count + 1;
    batch.transfers[batch.transfer_count++] = {id, source, destination, joules,
        batch.start_s, batch.end_s, simulated(200 + id), purpose, true};
}
void print(const char* scenario, const ec::ReconciliationResult& result) {
    std::cout << scenario << " evidence=SIMULATED initial_J=" << result.initial_joules
              << " in_J=" << result.inbound_joules << " final_J=" << result.final_joules
              << " out_J=" << result.outbound_joules << " heat_J=" << result.dissipated_joules
              << " residual_J=" << result.residual_joules
              << " tolerance_J=" << result.allowed_residual_joules
              << " state=" << name(result.status) << " committed=" << result.committed << '\n';
}
ec::LedgerSnapshot drive_campaign(bool verbose) {
    ec::EnergyContinuityLedger ledger;
    ds_plugin::DSBMSMiddleware bms;
    bms.init(75, 400);
    ds::drive::EnergyRecoveryConfig config;
    config.drivetrain_efficiency_regen = 0.875; // explicit simulated assumption
    ds::drive::DSEnergyRecoveryCoordinator recovery(config);
    const double battery_stores[] = {100000, 90000, 80000, 86000, 92000};
    const double kinetic_stores[] = {0, 8000, 16000, 8000, 0};
    for (std::uint64_t event = 1; event <= 4; ++event) {
        auto batch = vehicle_batch(event, battery_stores[event - 1], battery_stores[event],
                                  kinetic_stores[event - 1], kinetic_stores[event]);
        const auto before = ec::capture_bms_checkpoint(bms);
        if (event <= 2) {
            bms.enhance_cycle(400, 25, 25, 0.8, 1);
            const auto observed = ec::observe_bms_cycle(before, bms, 1,
                Node::PowerElectronics, simulated(300));
            require(observed.event_id == event && observed.battery_transfer.joules == 10000,
                    "analytic discharge observation");
            batch.transfers[batch.transfer_count++] = observed.battery_transfer;
            transfer(batch, Node::PowerElectronics, Node::Motor, 8000);
            transfer(batch, Node::Motor, Node::KineticStore, 8000);
            transfer(batch, Node::PowerElectronics, Node::ThermalSink, 2000);
        } else {
            ds::drive::RegenResult regen;
            regen.max_regen_torque_nm = 80;
            const auto accepted = recovery.step(bms, regen, 400, 25, -15, 0.8, 100, 1);
            const auto observed = ec::observe_recovery_cycle(before, bms, accepted, 1,
                Node::PowerElectronics, simulated(301));
            require(observed.battery.event_id == event &&
                    observed.battery.battery_transfer.joules == 6000 &&
                    observed.battery.battery_transfer.destination == Node::Battery,
                    "analytic charging observation");
            batch.transfers[batch.transfer_count++] = observed.battery.battery_transfer;
            batch.transfers[batch.transfer_count++] = observed.mechanical_available;
            batch.transfers[batch.transfer_count++] = observed.electrical_available;
            transfer(batch, Node::KineticStore, Node::Motor, 8000);
            transfer(batch, Node::Motor, Node::PowerElectronics, 7000);
            transfer(batch, Node::Motor, Node::ThermalSink, 1000);
            transfer(batch, Node::PowerElectronics, Node::ThermalSink, 1000);
            if (verbose) std::cout << "B battery_signed_J=-6000"
                << " available_mechanical_ESTIMATE_J=8000 available_electrical_ESTIMATE_J=7000"
                << " charging_source=SIMULATION_ASSUMPTION\n";
        }
        const auto result = ledger.reconcile(batch);
        require(result.status == ec::ReconciliationStatus::Balanced && result.residual_joules == 0,
                "analytic whole-vehicle balance");
        if (verbose) print(event <= 2 ? "A/C discharge" : "B/C braking", result);
    }
    if (verbose) std::cout << "session battery_final_J=92000 kinetic_final_J=0 explicit_heat_J="
        << ledger.snapshot().dissipated_joules
        << " BMS_accepted_charge_kWh=" << bms.snapshot().recovered_energy_kwh
        << " coordinator_charge_alias_kWh=" << recovery.diagnostics().accepted_charging_energy_kwh_total
        << " aliases_are_not_added=true available_ESTIMATED_J="
        << ledger.snapshot().available_estimate_joules_by_kind[static_cast<std::size_t>(ec::EvidenceKind::Estimated)]
        << " physical_MEASURED_J="
        << ledger.snapshot().physical_joules_by_kind[static_cast<std::size_t>(ec::EvidenceKind::Measured)] << '\n';
    require(ledger.snapshot().dissipated_joules == 8000, "session explicit heat oracle");
    return ledger.snapshot();
}
ec::Batch battery_batch(std::uint64_t event, double initial, double final) {
    auto batch = vehicle_batch(event, initial, final, 0, 0);
    batch.boundary.fill(false);
    batch.boundary[ec::node_index(Node::Battery)] = true;
    batch.storage_count = 1;
    return batch;
}
void rejection_and_evidence_campaign() {
    ds_plugin::DSBMSMiddleware bms;
    bms.init(75, 400);
    bms.enhance_cycle(400, -10, 25, 0.8, 1);
    const auto checkpoint = ec::capture_bms_checkpoint(bms);
    bool cycle_rejected = false;
    try { bms.enhance_cycle(400, std::numeric_limits<double>::quiet_NaN(), 25, 0.8, 1); }
    catch (const std::exception&) { cycle_rejected = true; }
    bool observer_rejected = false;
    try { ec::observe_bms_cycle(checkpoint, bms, 1, Node::ExternalSource, simulated(400)); }
    catch (const std::exception&) { observer_rejected = true; }
    require(cycle_rejected && observer_rejected && bms.diagnostics().update_count == 1 &&
            bms.snapshot().recovered_energy_kwh == checkpoint.charging_kwh,
            "rejected BMS cycle cannot create accepted evidence");
    std::cout << "D rejected_BMS_cycle accepted_count=1 accepted_charge_kWh="
              << bms.snapshot().recovered_energy_kwh << " observer_rejected=true\n";
    auto batch = battery_batch(1, 10000, 0);
    transfer(batch, Node::Battery, Node::PowerElectronics, 10000);
    ec::EnergyContinuityLedger ledger;
    const auto baseline = ledger.snapshot();
    for (int fault = 0; fault != 3; ++fault) {
        auto invalid = batch;
        if (fault == 0) invalid.transfers[0].joules = std::numeric_limits<double>::quiet_NaN();
        if (fault == 1) invalid.transfers[0].accepted = false;
        if (fault == 2) invalid.transfers[0].end_s = 2;
        const auto result = ledger.reconcile(invalid);
        require(!result.committed && result.status == ec::ReconciliationStatus::Rejected &&
                ledger.snapshot().accepted_events == baseline.accepted_events, "rejected evidence atomicity");
        print("D invalid/rejected/timing", result);
    }
    auto incomplete = batch;
    incomplete.transfer_count = 0;
    incomplete.transfers_complete = false;
    const auto omitted = ledger.reconcile(incomplete);
    require(omitted.status == ec::ReconciliationStatus::InsufficientEvidence &&
            omitted.residual_joules == 10000 && omitted.dissipated_joules == 0,
            "missing evidence must not invent heat");
    print("E missing transfer", omitted);
    require(ledger.reset(2) == ec::Error::None, "reset");
    batch.session_id = 2;
    batch.storage[0].evidence.independently_supported = false;
    const auto dependent = ledger.reconcile(batch);
    require(dependent.status == ec::ReconciliationStatus::InsufficientEvidence &&
            dependent.residual_joules == 0, "circular zero is not independent validation");
    print("E dependent zero", dependent);
    require(ledger.reset(3) == ec::Error::None, "second reset");
    incomplete.session_id = 3;
    incomplete.transfers_complete = true; // deliberate claim of complete conflicting evidence
    const auto inconsistent = ledger.reconcile(incomplete);
    require(inconsistent.status == ec::ReconciliationStatus::Imbalanced, "explicit imbalance");
    print("C explicit imbalance without inferred heat", inconsistent);
}
void extreme_campaign() {
    ec::EnergyContinuityLedger ledger;
    auto zero = battery_batch(1, 0, 0);
    require(ledger.reconcile(zero).status == ec::ReconciliationStatus::Balanced, "zero-energy step");
    require(!ec::battery_increment_joules(400, 0, 0), "zero duration rejects");
    require(!ec::battery_increment_joules(400, std::numeric_limits<double>::max(), 1), "VI overflow rejects");
    require(ledger.reconcile(zero).error == ec::Error::InvalidEvent, "event replay rejects");
    require(ledger.reset(2) == ec::Error::None, "extreme reset");
    auto extreme = battery_batch(1, 0, 0);
    extreme.session_id = 2;
    extreme.transfers_complete = false;
    transfer(extreme, Node::Battery, Node::PowerElectronics, std::numeric_limits<double>::max() * 0.75);
    require(ledger.reconcile(extreme).committed, "representable finite extreme");
    extreme.event_id = 2; extreme.start_s = 1; extreme.end_s = 2;
    extreme.transfers[0].start_s = 1; extreme.transfers[0].end_s = 2;
    require(ledger.reconcile(extreme).error == ec::Error::ArithmeticOverflow &&
            ledger.snapshot().accepted_events == 1, "cumulative overflow atomicity");
    ec::EnergyContinuityLedger tolerance;
    auto edge = battery_batch(1, 1, 0);
    edge.tolerance = {1, 0};
    require(tolerance.reconcile(edge).status == ec::ReconciliationStatus::Balanced, "inclusive tolerance");
    require(tolerance.reset(2) == ec::Error::None, "tolerance reset");
    edge.session_id = 2; edge.storage[0].initial_joules = std::nextafter(1.0, 2.0);
    require(tolerance.reconcile(edge).status == ec::ReconciliationStatus::Imbalanced, "above tolerance");
    std::cout << "F zero/extreme/overflow/tolerance/duplication/reset state=PASS\n";
}
} // namespace

int main() {
    try {
        std::cout << std::setprecision(17) << "DS-EV " << ds::get_version_string()
                  << " ENERGY CONTINUITY: deterministic software simulation; no hardware measurements\n";
        const auto first = drive_campaign(true);
        const auto replay = drive_campaign(false);
        require(first.inbound_joules == replay.inbound_joules &&
                first.outbound_joules == replay.outbound_joules &&
                first.dissipated_joules == replay.dissipated_joules &&
                first.storage_change_joules == replay.storage_change_joules &&
                first.residual_joules == replay.residual_joules &&
                first.physical_joules_by_kind == replay.physical_joules_by_kind &&
                first.available_estimate_joules_by_kind == replay.available_estimate_joules_by_kind,
                "deterministic replay");
        rejection_and_evidence_campaign();
        extreme_campaign();
        std::cout << "deterministic_replay=PASS overall=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
