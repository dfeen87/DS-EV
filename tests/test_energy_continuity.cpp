#include "ds_energy_continuity.hpp"
#include "ds_energy_continuity_observer.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace ec = ds::continuity;
using Node = ec::PhysicalNode;
using Status = ec::ReconciliationStatus;
using Error = ec::Error;

int checks = 0;
int failures = 0;
const double nan = std::numeric_limits<double>::quiet_NaN();
const double inf = std::numeric_limits<double>::infinity();
const double maximum = std::numeric_limits<double>::max();

void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

bool close(double actual, double expected, double absolute = 1e-10) {
    return std::isfinite(actual) && std::abs(actual - expected) <= absolute;
}

template <typename Function>
void rejects(Function function, const std::string& label) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    check(rejected, label);
}

bool same_ledger(const ec::LedgerSnapshot& a, const ec::LedgerSnapshot& b) {
    return a.session_id == b.session_id && a.last_event_id == b.last_event_id &&
        a.accepted_events == b.accepted_events && a.last_end_s == b.last_end_s &&
        a.boundary == b.boundary && a.inbound_joules == b.inbound_joules &&
        a.outbound_joules == b.outbound_joules && a.dissipated_joules == b.dissipated_joules &&
        a.storage_change_joules == b.storage_change_joules && a.residual_joules == b.residual_joules &&
        a.physical_joules_by_kind == b.physical_joules_by_kind &&
        a.available_estimate_joules_by_kind == b.available_estimate_joules_by_kind;
}

bool same_result(const ec::ReconciliationResult& a, const ec::ReconciliationResult& b) {
    return a.status == b.status && a.error == b.error && a.committed == b.committed &&
        a.evidence_complete == b.evidence_complete &&
        a.independently_supported == b.independently_supported &&
        a.initial_joules == b.initial_joules && a.inbound_joules == b.inbound_joules &&
        a.final_joules == b.final_joules && a.outbound_joules == b.outbound_joules &&
        a.dissipated_joules == b.dissipated_joules &&
        a.storage_change_joules == b.storage_change_joules &&
        a.residual_joules == b.residual_joules &&
        a.allowed_residual_joules == b.allowed_residual_joules;
}

ec::Evidence evidence(ec::EvidenceKind kind = ec::EvidenceKind::Simulated,
                      bool independent = true) {
    return {kind, ec::EvidenceOrigin::Supplied, 42, independent};
}

ec::TransferRecord transfer(std::uint64_t id, Node source, Node destination,
                            double joules, double start = 0.0, double end = 1.0,
                            ec::EvidenceKind kind = ec::EvidenceKind::Simulated) {
    ec::TransferRecord result;
    result.transfer_id = id;
    result.source = source;
    result.destination = destination;
    result.joules = joules;
    result.start_s = start;
    result.end_s = end;
    result.evidence = evidence(kind);
    return result;
}

ec::Batch battery_batch(std::uint64_t event = 1, double start = 0.0, double end = 1.0) {
    ec::Batch batch;
    batch.session_id = 1;
    batch.event_id = event;
    batch.start_s = start;
    batch.end_s = end;
    batch.boundary[ec::node_index(Node::Battery)] = true;
    batch.storage_count = 1;
    batch.storage[0] = {Node::Battery, 1000.0, 600.0, evidence(), true};
    batch.transfer_count = 1;
    batch.transfers[0] = transfer(1, Node::Battery, Node::Motor, 400.0, start, end);
    batch.transfers_complete = true;
    batch.stores_complete = true;
    batch.tolerance = {0.0, 0.0};
    return batch;
}

void reject_batch(ec::EnergyContinuityLedger& ledger, const ec::Batch& batch,
                  Error expected, const std::string& label) {
    const auto before = ledger.snapshot();
    const auto result = ledger.reconcile(batch);
    check(result.status == Status::Rejected && !result.committed && result.error == expected,
          label + " rejects with specific error");
    check(same_ledger(ledger.snapshot(), before), label + " preserves entire committed ledger");
}

void unit_and_sign_tests() {
    const auto discharge = ec::battery_increment_joules(400.0, 10.0, 2.0);
    const auto charge = ec::battery_increment_joules(400.0, -10.0, 2.0);
    check(discharge && discharge.value == 8000.0, "400 V * 10 A * 2 s = +8000 J discharge");
    check(charge && charge.value == -8000.0, "negative current = -8000 J charging");
    check(ec::battery_increment_kwh(360.0, 10.0, 1.0).value == 0.001,
          "3600 J = 0.001 kWh independent analytic conversion");
    check(ec::battery_increment_kwh(360.0, -10.0, 1.0).value == -0.001,
          "kWh conversion retains charging sign");
    check(ec::joules_to_kwh(3600000.0).value == 1.0 &&
          ec::joules_to_kwh(-3600000.0).value == -1.0 &&
          ec::kwh_to_joules(1.5).value == 5400000.0 &&
          ec::kwh_to_joules(-1.5).value == -5400000.0,
          "signed J/kWh conversion uses explicit 3.6e6 unit factor");
    check(ec::battery_increment_joules(400.0, 0.0, 1.0).value == 0.0,
          "zero current with positive time is legitimate zero energy");
    check(ec::joules_to_kwh(maximum) &&
          std::isfinite(ec::joules_to_kwh(maximum).value), "DBL_MAX joule conversion remains finite");
    check(ec::kwh_to_joules(maximum).error == Error::ArithmeticOverflow,
          "finite kWh multiplication overflow is rejected");
    check(ec::battery_increment_joules(maximum, 2.0, 1.0).error == Error::ArithmeticOverflow,
          "finite V*I power overflow is rejected");
    check(ec::battery_increment_joules(maximum / 2.0, 1.0, 4.0).error == Error::ArithmeticOverflow,
          "finite power*time energy overflow is rejected");
    check(ec::battery_increment_joules(maximum / 4.0, 1.0, 2.0).value == maximum / 2.0,
          "large finite representable energy is retained");
    for (double invalid : {nan, inf, -inf}) {
        check(!ec::joules_to_kwh(invalid) && !ec::kwh_to_joules(invalid),
              "nonfinite unit conversion rejects");
        check(!ec::battery_increment_joules(invalid, 1.0, 1.0) &&
              !ec::battery_increment_joules(400.0, invalid, 1.0) &&
              !ec::battery_increment_joules(400.0, 1.0, invalid),
              "each nonfinite battery observation rejects");
    }
    for (double elapsed : {0.0, -1.0, -0.0})
        check(!ec::battery_increment_joules(400.0, 0.0, elapsed),
              "invalid elapsed time cannot invent accepted zero energy");
    check(!ec::battery_increment_joules(0.0, 1.0, 1.0) &&
          !ec::battery_increment_joules(-400.0, 1.0, 1.0), "nonpositive pack voltage rejects");
}

void analytic_conservation_tests() {
    ec::EnergyContinuityLedger ledger;
    auto batch = battery_batch();
    auto result = ledger.reconcile(batch);
    check(result.status == Status::Balanced && result.committed && result.evidence_complete &&
          result.independently_supported, "independent complete analytic discharge balances");
    check(result.initial_joules == 1000.0 && result.final_joules == 600.0 &&
          result.outbound_joules == 400.0 && result.inbound_joules == 0.0 &&
          result.storage_change_joules == -400.0 && result.residual_joules == 0.0,
          "battery 1000 J -> 600 J plus 400 J output obeys conservation");

    batch = battery_batch(2, 1.0, 2.0);
    batch.storage[0] = {Node::Battery, 600.0, 720.0, evidence(), true};
    batch.transfers[0] = transfer(1, Node::PowerElectronics, Node::Battery, 120.0, 1.0, 2.0);
    result = ledger.reconcile(batch);
    check(result.status == Status::Balanced && result.inbound_joules == 120.0 &&
          result.storage_change_joules == 120.0 && result.residual_joules == 0.0,
          "charging increases storage by independent 120 J inbound observation");
    check(ledger.snapshot().accepted_events == 2 && ledger.snapshot().inbound_joules == 120.0 &&
          ledger.snapshot().outbound_joules == 400.0 &&
          ledger.snapshot().storage_change_joules == -280.0,
          "accepted steps accumulate signed storage and separate inbound/outbound totals");

    ec::EnergyContinuityLedger system;
    batch = battery_batch();
    batch.boundary[ec::node_index(Node::PowerElectronics)] = true;
    batch.boundary[ec::node_index(Node::Motor)] = true;
    batch.boundary[ec::node_index(Node::KineticStore)] = true;
    batch.storage_count = 4;
    batch.storage[0] = {Node::Battery, 10000.0, 6000.0, evidence(), true};
    batch.storage[1] = {Node::PowerElectronics, 0.0, 0.0, evidence(), true};
    batch.storage[2] = {Node::Motor, 0.0, 0.0, evidence(), true};
    batch.storage[3] = {Node::KineticStore, 1000.0, 4200.0, evidence(), true};
    batch.transfer_count = 5;
    batch.transfers[0] = transfer(1, Node::Battery, Node::PowerElectronics, 4000.0);
    batch.transfers[1] = transfer(2, Node::PowerElectronics, Node::Motor, 3600.0);
    batch.transfers[2] = transfer(3, Node::PowerElectronics, Node::ThermalSink, 400.0);
    batch.transfers[3] = transfer(4, Node::Motor, Node::KineticStore, 3200.0);
    batch.transfers[4] = transfer(5, Node::Motor, Node::ThermalSink, 400.0);
    result = system.reconcile(batch);
    check(result.status == Status::Balanced && result.initial_joules == 11000.0 &&
          result.final_joules == 10200.0 && result.dissipated_joules == 800.0 &&
          result.inbound_joules == 0.0 && result.outbound_joules == 0.0 &&
          result.residual_joules == 0.0, "explicit simulated inverter/motor losses close whole boundary");
    check(system.snapshot().physical_joules_by_kind[2] == 11600.0,
          "internal path diagnostics count each physical edge once without inflating boundary flux");

    ec::EnergyContinuityLedger imbalance;
    batch = battery_batch();
    batch.transfers[0].joules = 350.0;
    result = imbalance.reconcile(batch);
    check(result.status == Status::Imbalanced && result.committed && result.residual_joules == 50.0 &&
          result.dissipated_joules == 0.0, "unexplained positive 50 J residual is never invented heat");
    ec::EnergyContinuityLedger wrong_direction;
    batch = battery_batch();
    batch.transfers[0].source = Node::Motor;
    batch.transfers[0].destination = Node::Battery;
    result = wrong_direction.reconcile(batch);
    check(result.status == Status::Imbalanced && result.residual_joules == 800.0 &&
          result.inbound_joules == 400.0 && result.outbound_joules == 0.0,
          "source/destination reversal changes signed balance; direction is not inferred from storage");
}

void evidence_and_classification_tests() {
    for (int omission = 0; omission < 4; ++omission) {
        ec::EnergyContinuityLedger ledger;
        auto batch = battery_batch();
        if (omission == 0) batch.transfers_complete = false;
        if (omission == 1) batch.stores_complete = false;
        if (omission == 2) { batch.storage_count = 0; batch.stores_complete = false; }
        if (omission == 3) { batch.transfer_count = 0; batch.transfers_complete = false; }
        const auto result = ledger.reconcile(batch);
        check(result.status == Status::InsufficientEvidence && result.committed &&
              !result.evidence_complete, "missing observation/completeness declaration cannot verify balance");
    }
    for (int dependent = 0; dependent < 2; ++dependent) {
        ec::EnergyContinuityLedger ledger;
        auto batch = battery_batch();
        if (dependent == 0) batch.storage[0].evidence.independently_supported = false;
        else batch.transfers[0].evidence.independently_supported = false;
        const auto result = ledger.reconcile(batch);
        check(result.residual_joules == 0.0 && result.evidence_complete &&
              !result.independently_supported && result.status == Status::InsufficientEvidence,
              "circular/dependent zero residual is insufficient independent evidence");
    }
    ec::EnergyContinuityLedger ledger;
    auto batch = battery_batch();
    batch.transfers[0].evidence.kind = ec::EvidenceKind::Measured;
    batch.transfer_count = 4;
    batch.transfers[1] = transfer(2, Node::KineticStore, Node::Motor, 1000.0, 0.0, 1.0,
                                  ec::EvidenceKind::Estimated);
    batch.transfers[1].purpose = ec::QuantityPurpose::AvailableEstimate;
    batch.transfers[1].evidence.independently_supported = false;
    batch.transfers[2] = transfer(3, Node::Motor, Node::Battery, 850.0, 0.0, 1.0,
                                  ec::EvidenceKind::Estimated);
    batch.transfers[2].purpose = ec::QuantityPurpose::AvailableEstimate;
    batch.transfers[3] = transfer(4, Node::Battery, Node::Auxiliary, 0.0, 0.0, 1.0,
                                  ec::EvidenceKind::Calculated);
    const auto result = ledger.reconcile(batch);
    check(result.status == Status::Balanced && result.outbound_joules == 400.0 &&
          result.inbound_joules == 0.0 && result.residual_joules == 0.0,
          "available mechanical/electrical estimates do not credit physical boundary energy");
    check(ledger.snapshot().physical_joules_by_kind == std::array<double, 4>{400.0, 0.0, 0.0, 0.0} &&
          ledger.snapshot().available_estimate_joules_by_kind == std::array<double, 4>{0.0, 0.0, 0.0, 1850.0},
          "measured physical quantities and available estimates have separate classification totals");
    ec::EnergyContinuityLedger estimated_physical;
    batch = battery_batch();
    batch.transfers[0].evidence.kind = ec::EvidenceKind::Estimated;
    batch.transfers[0].evidence.independently_supported = false;
    const auto estimated = estimated_physical.reconcile(batch);
    check(estimated.status == Status::InsufficientEvidence &&
          estimated_physical.snapshot().physical_joules_by_kind[3] == 400.0 &&
          estimated_physical.snapshot().physical_joules_by_kind[0] == 0.0,
          "estimated physical evidence is labeled separately and cannot become measured evidence");
}

void malformed_and_atomicity_tests() {
    ec::EnergyContinuityLedger ledger;
    check(ledger.reconcile(battery_batch()).status == Status::Balanced, "atomicity fixture first accepts");
    const auto valid = battery_batch(2, 1.0, 2.0);
    for (int field = 0; field < 6; ++field) {
        for (double invalid : {nan, inf, -inf, -1.0}) {
            auto batch = valid;
            switch (field) {
            case 0: batch.transfers[0].joules = invalid; break;
            case 1: batch.storage[0].initial_joules = invalid; break;
            case 2: batch.storage[0].final_joules = invalid; break;
            case 3: batch.start_s = invalid; break;
            case 4: batch.tolerance.absolute_joules = invalid; break;
            case 5: batch.tolerance.relative = invalid; break;
            }
            reject_batch(ledger, batch,
                field < 3 ? Error::InvalidQuantity :
                field == 3 ? Error::InvalidInterval : Error::InvalidTolerance,
                "nonfinite/negative quantities, interval, tolerance");
        }
    }
    for (int malformed = 0; malformed < 19; ++malformed) {
        auto batch = valid;
        Error expected = Error::None;
        switch (malformed) {
        case 0: batch.transfers[0].source = static_cast<Node>(255); expected = Error::InvalidNode; break;
        case 1: batch.transfers[0].destination = Node::Count; expected = Error::InvalidNode; break;
        case 2: batch.storage[0].node = Node::Count; expected = Error::InvalidNode; break;
        case 3: batch.transfers[0].destination = Node::Battery; expected = Error::SelfTransfer; break;
        case 4: batch.transfers[0].source = Node::Motor; batch.transfers[0].destination = Node::Auxiliary;
                expected = Error::UnrelatedTransfer; break;
        case 5: batch.transfers[0].evidence.kind = static_cast<ec::EvidenceKind>(255);
                expected = Error::InvalidEvidence; break;
        case 6: batch.transfers[0].evidence.origin = static_cast<ec::EvidenceOrigin>(255);
                expected = Error::InvalidEvidence; break;
        case 7: batch.transfers[0].evidence.provenance_id = 0; expected = Error::InvalidEvidence; break;
        case 8: batch.transfers[0].purpose = static_cast<ec::QuantityPurpose>(255);
                expected = Error::InvalidEvidence; break;
        case 9: batch.storage[0].evidence.kind = static_cast<ec::EvidenceKind>(255);
                expected = Error::InvalidEvidence; break;
        case 10: batch.transfers[0].accepted = false; expected = Error::RejectedEvidence; break;
        case 11: batch.storage[0].accepted = false; expected = Error::RejectedEvidence; break;
        case 12: batch.transfers[0].transfer_id = 0; expected = Error::InvalidEvent; break;
        case 13: batch.transfers[1] = batch.transfers[0]; batch.transfer_count = 2;
                 expected = Error::DuplicateTransfer; break;
        case 14: batch.storage[1] = batch.storage[0]; batch.storage_count = 2;
                 expected = Error::DuplicateStorage; break;
        case 15: batch.storage[0].node = Node::Motor; expected = Error::StorageOutsideBoundary; break;
        case 16: batch.storage_count = 0; expected = Error::MissingStorage; break;
        case 17: batch.transfer_count = ec::kMaxTransfers + 1; expected = Error::CapacityExceeded; break;
        case 18: batch.storage_count = ec::kNodeCount + 1; expected = Error::CapacityExceeded; break;
        }
        reject_batch(ledger, batch, expected, "malformed edge/evidence/capacity");
    }
    for (int timing = 0; timing < 6; ++timing) {
        auto batch = valid;
        if (timing == 0) batch.end_s = batch.start_s;
        if (timing == 1) batch.end_s = 0.0;
        if (timing == 2) batch.end_s = inf;
        if (timing == 3) batch.transfers[0].start_s = 1.5;
        if (timing == 4) batch.transfers[0].end_s = 3.0;
        if (timing == 5) batch.transfers[0].end_s = nan;
        reject_batch(ledger, batch, Error::InvalidInterval, "invalid or inconsistent time interval");
    }
    auto batch = valid;
    batch.start_s = 0.5;
    reject_batch(ledger, batch, Error::OverlappingInterval, "overlapping accepted events");
    batch = valid;
    batch.boundary = {};
    reject_batch(ledger, batch, Error::InvalidBoundary, "empty physical boundary");
    batch = valid;
    batch.boundary[ec::node_index(Node::Motor)] = true;
    reject_batch(ledger, batch, Error::BoundaryMismatch, "session boundary change needs explicit reset");
    batch = valid;
    batch.transfers[1] = transfer(2, Node::Battery, Node::Motor, nan, 1.0, 2.0);
    batch.transfer_count = 2;
    reject_batch(ledger, batch, Error::InvalidQuantity, "later invalid record rolls back earlier staged transfer");
    check(ledger.reconcile(valid).status == Status::Balanced && ledger.snapshot().accepted_events == 2,
          "rejected attempts do not consume event identity or prevent valid resume");
}

void tolerance_tests() {
    auto absolute_batch = [] (double loss) {
        auto batch = battery_batch();
        batch.storage[0] = {Node::Battery, 0.0, loss, evidence(), true};
        batch.transfer_count = 0;
        batch.tolerance = {1.0, 0.0};
        return batch;
    };
    for (double loss : {1.0, std::nextafter(1.0, 0.0), std::nextafter(1.0, inf)}) {
        ec::EnergyContinuityLedger ledger;
        const auto result = ledger.reconcile(absolute_batch(loss));
        check(result.residual_joules == -loss && result.allowed_residual_joules == 1.0 &&
              result.status == (loss <= 1.0 ? Status::Balanced : Status::Imbalanced),
              "absolute tolerance is inclusive with nextafter neighbors and signed residual");
    }
    for (double output : {0.5, std::nextafter(0.5, 0.0), std::nextafter(0.5, 1.0)}) {
        ec::EnergyContinuityLedger ledger;
        auto batch = battery_batch();
        batch.storage[0] = {Node::Battery, 1.0, 1.0, evidence(), true};
        batch.transfers[0].joules = output;
        batch.tolerance = {0.0, 0.5};
        const auto result = ledger.reconcile(batch);
        check(result.allowed_residual_joules == 0.5 && result.residual_joules == -output &&
              result.status == (output > 0.5 ? Status::Imbalanced : Status::Balanced),
              "relative term scales explicit initial 1 J with inclusive representable boundary");
    }
    ec::EnergyContinuityLedger combined;
    auto batch = battery_batch();
    batch.storage[0] = {Node::Battery, 1000.0, 979.0, evidence(), true};
    batch.transfer_count = 0;
    batch.tolerance = {1.0, 0.02};
    const auto result = combined.reconcile(batch);
    check(result.residual_joules == 21.0 && result.allowed_residual_joules == 21.0 &&
          result.status == Status::Balanced, "absolute plus relative allowance uses largest explicit term");
    ec::EnergyContinuityLedger zero;
    batch = battery_batch();
    batch.storage[0] = {Node::Battery, 0.0, 0.0, evidence(), true};
    batch.transfers[0].joules = 0.0;
    const auto zero_result = zero.reconcile(batch);
    check(zero_result.status == Status::Balanced && zero_result.residual_joules == 0.0 &&
          zero_result.allowed_residual_joules == 0.0, "explicit complete zero energy with zero tolerance balances");
}

void arithmetic_extremes_tests() {
    for (int field = 0; field < 7; ++field) {
        ec::EnergyContinuityLedger ledger;
        auto batch = battery_batch();
        batch.storage[0] = {Node::Battery, 0.0, 0.0, evidence(), true};
        batch.transfer_count = 0;
        if (field == 0 || field == 1) {
            batch.boundary[ec::node_index(Node::Motor)] = true;
            batch.storage_count = 2;
            batch.storage[1] = {Node::Motor, 0.0, 0.0, evidence(), true};
            if (field == 0) batch.storage[0].initial_joules = batch.storage[1].initial_joules = maximum;
            else batch.storage[0].final_joules = batch.storage[1].final_joules = maximum;
        }
        if (field == 2 || field == 3 || field == 4) {
            batch.transfer_count = 1;
            batch.transfers[0] = transfer(1, Node::ExternalSource, Node::Battery, maximum);
            if (field == 2) batch.storage[0].initial_joules = maximum;
            if (field == 3) {
                batch.storage[0].final_joules = maximum;
                batch.transfers[0] = transfer(1, Node::Battery, Node::Motor, maximum);
            }
            if (field == 4) {
                batch.storage[0].final_joules = maximum;
                batch.transfers[0] = transfer(1, Node::Battery, Node::ThermalSink, maximum);
            }
        }
        if (field == 5) { batch.storage[0].initial_joules = maximum; batch.tolerance.relative = 2.0; }
        if (field == 6) {
            batch.storage[0].initial_joules = maximum;
            batch.tolerance = {maximum, 1.0};
        }
        reject_batch(ledger, batch, Error::ArithmeticOverflow, "finite per-event sum/tolerance overflow");
    }
    // Independent fields exercise accumulation rather than per-event arithmetic.
    for (int field = 0; field < 7; ++field) {
        ec::EnergyContinuityLedger ledger;
        auto batch = battery_batch();
        batch.storage[0] = {Node::Battery, 0.0, 0.0, evidence(), true};
        batch.transfers[0] = transfer(1, Node::Battery, Node::Motor, maximum * 0.75);
        if (field == 0) batch.transfers[0] = transfer(1, Node::ExternalSource, Node::Battery, maximum * 0.75);
        if (field == 2) batch.transfers[0].destination = Node::ThermalSink;
        if (field == 3) {
            batch.transfers[0].purpose = ec::QuantityPurpose::AvailableEstimate;
            batch.transfers[0].evidence.kind = ec::EvidenceKind::Estimated;
        }
        if (field == 4 || field == 5 || field == 6) {
            batch.transfer_count = 0;
            if (field == 4) batch.storage[0].final_joules = maximum * 0.75;
            else batch.storage[0].initial_joules = maximum * 0.75;
            if (field == 6) { batch.storage[0].final_joules = maximum * 0.25; }
        }
        const auto first = ledger.reconcile(batch);
        check(first.committed && std::isfinite(first.residual_joules),
              "first extreme finite increment commits without saturation");
        batch.event_id = 2; batch.start_s = 1.0; batch.end_s = 2.0;
        batch.transfers[0].start_s = 1.0; batch.transfers[0].end_s = 2.0;
        if (field == 6) {
            // The first residual is +0.5*DBL_MAX. The second is +0.75*DBL_MAX;
            // individual initial/final terms remain finite, but signed sum cannot.
            batch.storage[0].initial_joules = maximum * 0.75;
            batch.storage[0].final_joules = 0.0;
        }
        reject_batch(ledger, batch, Error::ArithmeticOverflow,
                     "second extreme increment cannot overflow any committed ledger total");
    }
}

void cancellation_regressions() {
    ec::EnergyContinuityLedger large_inventory;
    auto batch = battery_batch();
    batch.storage[0] = {Node::Battery, 1e20, 1e20, evidence(), true};
    batch.transfers[0] = transfer(1, Node::ExternalSource, Node::Battery, 1.0);
    const auto small_flux = large_inventory.reconcile(batch);
    check(small_flux.status == Status::Imbalanced && small_flux.residual_joules == 1.0 &&
          small_flux.storage_change_joules == 0.0,
          "large equal inventory cannot round away an explicit 1 J inbound imbalance");

    ec::EnergyContinuityLedger small_final_store;
    batch = battery_batch();
    batch.storage[0] = {Node::Battery, 1e20, 1.0, evidence(), true};
    batch.transfers[0].joules = 1e20;
    const auto retained_final = small_final_store.reconcile(batch);
    check(retained_final.status == Status::Imbalanced && retained_final.residual_joules == -1.0,
          "raw final 1 J remains after initial inventory and outbound 1e20 J cancel");

    ec::EnergyContinuityLedger small_initial_store;
    batch = battery_batch();
    batch.storage[0] = {Node::Battery, 1.0, 1e20, evidence(), true};
    batch.transfers[0] = transfer(1, Node::ExternalSource, Node::Battery, 1e20);
    const auto retained_initial = small_initial_store.reconcile(batch);
    check(retained_initial.status == Status::Imbalanced && retained_initial.residual_joules == 1.0,
          "raw initial 1 J remains after inbound energy and final 1e20 J inventory cancel");

    ec::EnergyContinuityLedger mixed_crossings;
    batch = battery_batch();
    batch.storage[0] = {Node::Battery, 0.0, 0.0, evidence(), true};
    batch.transfer_count = 3;
    batch.transfers[0] = transfer(1, Node::ExternalSource, Node::Battery, 1e20);
    batch.transfers[1] = transfer(2, Node::ExternalSource, Node::Battery, 1.0);
    batch.transfers[2] = transfer(3, Node::Battery, Node::Motor, 1e20);
    const auto crossing = mixed_crossings.reconcile(batch);
    check(crossing.status == Status::Imbalanced && crossing.residual_joules == 1.0,
          "compensated signed crossings retain 1 J between opposite 1e20 J transfers");

    ec::EnergyContinuityLedger separate_stores;
    batch = battery_batch();
    batch.boundary[ec::node_index(Node::Motor)] = true;
    batch.storage_count = 2;
    batch.storage[0] = {Node::Battery, 1e20, 1e20, evidence(), true};
    batch.storage[1] = {Node::Motor, 1.0, 0.0, evidence(), true};
    batch.transfer_count = 0;
    const auto store = separate_stores.reconcile(batch);
    check(store.status == Status::Imbalanced && store.residual_joules == 1.0 &&
          store.storage_change_joules == -1.0,
          "per-store delta retains 1 J change concealed by aggregated large inventory");

    ec::EnergyContinuityLedger opposite_stores;
    batch = battery_batch();
    batch.boundary[ec::node_index(Node::Motor)] = true;
    batch.boundary[ec::node_index(Node::PowerElectronics)] = true;
    batch.storage_count = 3;
    batch.storage[0] = {Node::Battery, 1e20, 0.0, evidence(), true};
    batch.storage[1] = {Node::PowerElectronics, 0.0, 1.0, evidence(), true};
    batch.storage[2] = {Node::Motor, 0.0, 1e20, evidence(), true};
    batch.transfer_count = 0;
    const auto opposite = opposite_stores.reconcile(batch);
    check(opposite.status == Status::Imbalanced && opposite.residual_joules == -1.0 &&
          opposite.storage_change_joules == 1.0,
          "compensated separate signed storage deltas retain small intermediate change");

    ec::EnergyContinuityLedger cumulative_crossings;
    for (std::uint64_t event = 1; event <= 3; ++event) {
        batch = battery_batch(event, static_cast<double>(event - 1), static_cast<double>(event));
        batch.storage[0] = {Node::Battery, 0.0, 0.0, evidence(), true};
        batch.transfers[0] = event == 3
            ? transfer(1, Node::Battery, Node::Motor, 1e20, 2.0, 3.0)
            : transfer(1, Node::ExternalSource, Node::Battery, event == 1 ? 1e20 : 1.0,
                       static_cast<double>(event - 1), static_cast<double>(event));
        check(cumulative_crossings.reconcile(batch).committed,
              "finite signed residual contribution commits across successive events");
    }
    check(cumulative_crossings.snapshot().accepted_events == 3 &&
          cumulative_crossings.snapshot().residual_joules == 1.0,
          "session compensation retains residual +1e20,+1,-1e20 = +1 J");

    ec::EnergyContinuityLedger staged_compensation;
    batch = battery_batch();
    batch.transfer_count = 0;
    batch.storage[0] = {Node::Battery, 0.0, maximum * 0.75, evidence(), true};
    check(staged_compensation.reconcile(batch).committed, "large storage-change compensation fixture commits");
    batch.event_id = 2; batch.start_s = 1.0; batch.end_s = 2.0;
    batch.storage[0].final_joules = 1.0;
    check(staged_compensation.reconcile(batch).committed, "small hidden signed correction commits");
    batch.event_id = 3; batch.start_s = 2.0; batch.end_s = 3.0;
    batch.storage[0].final_joules = maximum * 0.75;
    reject_batch(staged_compensation, batch, Error::ArithmeticOverflow,
                 "overflow rejects staged private compensated accumulator");
    batch.storage[0] = {Node::Battery, maximum * 0.75, 0.0, evidence(), true};
    check(staged_compensation.reconcile(batch).committed &&
          staged_compensation.snapshot().storage_change_joules == 1.0 &&
          staged_compensation.snapshot().residual_joules == -1.0,
          "valid cancellation after rejection preserves hidden +1 J correction");
    check(staged_compensation.reset(2) == Error::None, "reset clears private signed compensation");
    batch = battery_batch(); batch.session_id = 2;
    check(staged_compensation.reconcile(batch).status == Status::Balanced &&
          staged_compensation.snapshot().storage_change_joules == -400.0 &&
          staged_compensation.snapshot().residual_joules == 0.0,
          "post-reset accounting receives no correction from prior session");
}

void identity_lifecycle_and_replay_tests() {
    ec::EnergyContinuityLedger ledger;
    check(ledger.reconcile(battery_batch()).status == Status::Balanced, "lifecycle fixture accepts event1");
    auto batch = battery_batch(2, 1.0, 2.0);
    auto malformed = batch;
    malformed.session_id = 2;
    reject_batch(ledger, malformed, Error::InvalidSession, "foreign session");
    for (std::uint64_t id : {std::uint64_t{0}, std::uint64_t{1}}) {
        malformed = batch; malformed.event_id = id;
        reject_batch(ledger, malformed, Error::InvalidEvent, "zero/replayed event identity");
    }
    batch.event_id = std::numeric_limits<std::uint64_t>::max();
    check(ledger.reconcile(batch).committed, "event identity gaps and maximum nonzero event are supported");
    malformed = batch; malformed.start_s = 2.0; malformed.end_s = 3.0;
    reject_batch(ledger, malformed, Error::InvalidEvent, "event cannot wrap or reuse maximum identity");
    const auto before_reset = ledger.snapshot();
    for (std::uint64_t id : {std::uint64_t{0}, std::uint64_t{1}})
        check(ledger.reset(id) == Error::InvalidSession && same_ledger(ledger.snapshot(), before_reset),
              "zero/same/older reset is transactional");
    check(ledger.reset(2) == Error::None && ledger.snapshot().session_id == 2 &&
          ledger.snapshot().accepted_events == 0 && ledger.snapshot().last_event_id == 0 &&
          ledger.snapshot().inbound_joules == 0.0 && ledger.snapshot().outbound_joules == 0.0 &&
          ledger.snapshot().physical_joules_by_kind == std::array<double, 4>{},
          "new session reset clears accounting and permits new sequence");
    batch = battery_batch(); batch.session_id = 2;
    check(ledger.reconcile(batch).status == Status::Balanced, "new session may reuse event1");
    ec::EnergyContinuityLedger invalid(0);
    reject_batch(invalid, battery_batch(), Error::InvalidSession, "zero constructor session fails closed");
    check(invalid.reset(1) == Error::None && invalid.reconcile(battery_batch()).committed,
          "invalid initial session can recover through explicit reset");
    ec::EnergyContinuityLedger terminal(std::numeric_limits<std::uint64_t>::max());
    const auto terminal_before = terminal.snapshot();
    check(terminal.reset(1) == Error::InvalidSession && same_ledger(terminal.snapshot(), terminal_before),
          "session identity cannot wrap");

    ec::EnergyContinuityLedger first, second;
    for (std::uint64_t event = 1; event <= 8; ++event) {
        batch = battery_batch(event, static_cast<double>(event - 1), static_cast<double>(event));
        if (event % 3 == 0) batch.transfers_complete = false;
        if (event % 4 == 0) batch.transfers[0].joules = 399.0;
        const auto a = first.reconcile(batch), b = second.reconcile(batch);
        check(same_result(a, b) && same_ledger(first.snapshot(), second.snapshot()),
              "fixed sequence yields deterministic result and complete ledger replay");
    }
    check(first.snapshot().accepted_events == 8 && first.snapshot().outbound_joules == 3198.0 &&
          first.snapshot().storage_change_joules == -3200.0 && first.snapshot().residual_joules == 2.0,
          "deterministic replay accumulation independently equals analytic totals");
}

std::vector<double> model_fields(const ds::EnhancedState& value) {
    const auto& state = value.state;
    std::vector<double> result{state.voltage, state.current, state.temperature, state.state_of_charge,
        state.entropy, state.cycle_count, state.degradation, state.phi_magnitude, state.lambda,
        state.energy_psi, state.energy_phi, state.energy_metric, state.energy_total,
        state.charge_throughput_ah, state.capacity_fade, state.time, state.last_update,
        value.feen_trust_metric, value.ds_confidence, value.energy_conservation_error,
        value.health.remaining_capacity_percent, value.health.cycles_to_80_percent,
        value.health.estimated_eol_cycles, value.health.confidence, value.health.degradation_rate,
        value.health.time_to_80_percent_years, value.charging.recommended_current_limit,
        value.charging.recommended_voltage_limit, value.charging.recommended_temperature,
        value.charging.estimated_charge_time, value.charging.degradation_impact,
        value.charging.max_safe_current, value.charging.max_safe_voltage};
    result.insert(result.end(), state.grad_phi.begin(), state.grad_phi.end());
    for (const auto& row : state.g_eff.data) result.insert(result.end(), row.begin(), row.end());
    return result;
}

bool same_telemetry(const ds::DSEnergyTelemetry& a, const ds::DSEnergyTelemetry& b) {
    return a.soc_percent == b.soc_percent && a.soh_percent == b.soh_percent &&
        a.pack_power_kw == b.pack_power_kw && a.regen_power_kw == b.regen_power_kw &&
        a.recovered_energy_kwh == b.recovered_energy_kwh && a.ds_metric_trace == b.ds_metric_trace &&
        a.ds_entropy == b.ds_entropy && a.ds_confidence == b.ds_confidence &&
        a.limiting_factor == b.limiting_factor;
}

void bms_observer_noninterference_tests() {
    ds_plugin::MiddlewareConfig config;
    config.enable_kalman_filter = true;
    ds_plugin::DSBMSMiddleware observed, control;
    observed.init_advanced(config); control.init_advanced(config);
    auto checkpoint = ec::capture_bms_checkpoint(observed);
    ec::EnergyContinuityLedger ledger;
    const std::array<double, 5> currents{10.0, -20.0, 0.0, 2000.0, -2000.0};
    const std::array<double, 5> expected_signed_joules{3600.0, -7200.0, 0.0, 720000.0, -720000.0};
    double stored = 2000000.0;
    double expected_throughput_kwh = 0.0;
    double expected_charging_kwh = 0.0;
    for (std::size_t cycle = 0; cycle < currents.size(); ++cycle) {
        const auto result = observed.enhance_cycle(360.0, currents[cycle], 25.0, 0.8, 1.0);
        const auto reference = control.enhance_cycle(360.0, currents[cycle], 25.0, 0.8, 1.0);
        const auto telemetry_before = observed.snapshot();
        const auto diagnostic_before = observed.diagnostics();
        const auto observation = ec::observe_bms_cycle(checkpoint, observed, 1,
            Node::PowerElectronics, evidence(ec::EvidenceKind::Simulated));
        check(observation.event_id == cycle + 1 && observation.session_id == 1 &&
              observation.battery_transfer.transfer_id == 1 &&
              observation.battery_transfer.joules == std::abs(expected_signed_joules[cycle]) &&
              observation.battery_transfer.start_s == static_cast<double>(cycle) &&
              observation.battery_transfer.end_s == static_cast<double>(cycle + 1),
              "observer derives exactly one accepted raw-current cycle with canonical identity");
        check(observation.battery_transfer.source == (currents[cycle] < 0.0 ? Node::PowerElectronics : Node::Battery) &&
              observation.battery_transfer.destination == (currents[cycle] < 0.0 ? Node::Battery : Node::PowerElectronics) &&
              observation.battery_transfer.evidence.kind == ec::EvidenceKind::Simulated &&
              observation.battery_transfer.evidence.origin == ec::EvidenceOrigin::Telemetry,
              "observer preserves charging/discharging direction and explicit calculated provenance");
        auto batch = battery_batch(cycle + 1, static_cast<double>(cycle), static_cast<double>(cycle + 1));
        batch.transfers[0] = observation.battery_transfer;
        batch.storage[0] = {Node::Battery, stored, stored - expected_signed_joules[cycle], evidence(), true};
        const auto balanced = ledger.reconcile(batch);
        check(balanced.status == Status::Balanced && balanced.residual_joules == 0.0,
              "observer transfer matches independent fixed analytic storage change");
        stored -= expected_signed_joules[cycle];
        expected_throughput_kwh += std::abs(expected_signed_joules[cycle]) / 3600000.0;
        if (currents[cycle] < 0.0) expected_charging_kwh -= expected_signed_joules[cycle] / 3600000.0;
        check(close(observed.diagnostics().energy_throughput_kwh, expected_throughput_kwh) &&
              close(observed.snapshot().recovered_energy_kwh, expected_charging_kwh),
              "opt-in continuity never adds its accounting into existing BMS ledgers");
        check(same_telemetry(observed.snapshot(), telemetry_before) &&
              observed.diagnostics().update_count == diagnostic_before.update_count &&
              observed.diagnostics().energy_throughput_kwh == diagnostic_before.energy_throughput_kwh,
              "passive factory and reconciliation preserve source snapshot and accepted ledger");
        check(model_fields(result) == model_fields(reference) &&
              result.numerical_stability == reference.numerical_stability &&
              result.input_clamped == reference.input_clamped &&
              result.health.warning_triggered == reference.health.warning_triggered &&
              same_telemetry(observed.snapshot(), control.snapshot()),
              "observed and unobserved twins retain mathematical, filtering, health and telemetry behavior");
        reject_batch(ledger, batch, Error::InvalidEvent, "same accepted observation cannot credit twice");
        checkpoint = observation.next_checkpoint;
    }
    check(ledger.snapshot().outbound_joules == 723600.0 &&
          ledger.snapshot().inbound_joules == 727200.0 &&
          ledger.snapshot().storage_change_joules == 3600.0,
          "multi-cycle raw-current boundary totals analytically preserve accepted charge/discharge");
    const auto ledger_before = ledger.snapshot();
    const auto before_rejection = observed.snapshot();
    rejects([&] { observed.enhance_cycle(360.0, maximum, 25.0, 0.8, 1.0); },
            "underlying BMS rejects overflowing raw current");
    rejects([&] { control.enhance_cycle(360.0, maximum, 25.0, 0.8, 1.0); },
            "unobserved twin rejects identical invalid cycle");
    rejects([&] { ec::observe_bms_cycle(checkpoint, observed, 1, Node::PowerElectronics, evidence()); },
            "unchanged accepted counter cannot fabricate rejected-cycle transfer");
    check(observed.snapshot().recovered_energy_kwh == before_rejection.recovered_energy_kwh &&
          same_ledger(ledger.snapshot(), ledger_before), "rejected source cycle preserves both accepted ledgers");
    const auto resumed = observed.enhance_cycle(400.0, 5.0, 30.0, 0.7, 2.0);
    const auto reference = control.enhance_cycle(400.0, 5.0, 30.0, 0.7, 2.0);
    const auto resume_observation = ec::observe_bms_cycle(checkpoint, observed, 1, Node::Motor, evidence());
    check(resume_observation.event_id == 6 && resume_observation.battery_transfer.joules == 4000.0 &&
          model_fields(resumed) == model_fields(reference) &&
          same_telemetry(observed.snapshot(), control.snapshot()), "valid resume retains twin rollback parity");
    checkpoint = resume_observation.next_checkpoint;
    rejects([&] { ec::observe_bms_cycle(checkpoint, observed, 1, Node::Motor, evidence()); },
            "advanced checkpoint prevents same cycle from being observed again");
    observed.init(75.0, 400.0);
    observed.enhance_cycle(400.0, -10.0, 25.0, 0.8, 1.0);
    rejects([&] { ec::observe_bms_cycle(checkpoint, observed, 1, Node::ExternalSource, evidence()); },
            "old multi-cycle checkpoint cannot cross BMS reinitialization");
    observed.init(75.0, 400.0);
    checkpoint = ec::capture_bms_checkpoint(observed);
    check(ledger.reset(2) == Error::None, "BMS reinit explicitly starts newer continuity session");
    observed.enhance_cycle(400.0, -10.0, 25.0, 0.8, 1.0);
    const auto fresh = ec::observe_bms_cycle(checkpoint, observed, 2, Node::ExternalSource, evidence());
    check(fresh.event_id == 1 && fresh.session_id == 2 && fresh.battery_transfer.joules == 4000.0 &&
          fresh.battery_transfer.source == Node::ExternalSource &&
          fresh.battery_transfer.destination == Node::Battery,
          "reinitialized session permits event1 and keeps charging source an explicit assertion");
}

void bms_observer_rejection_tests() {
    ds_plugin::DSBMSMiddleware bms, other;
    bms.init(75.0, 400.0); other.init(75.0, 400.0);
    const auto before = ec::capture_bms_checkpoint(bms);
    bms.enhance_cycle(400.0, 10.0, 25.0, 0.8, 1.0);
    for (int malformed = 0; malformed < 10; ++malformed) {
        auto checkpoint = before;
        switch (malformed) {
        case 0: checkpoint.source = nullptr; break;
        case 1: checkpoint.source = &other; break;
        case 2: checkpoint.update_count = -1; break;
        case 3: checkpoint.update_count = std::numeric_limits<int>::max(); break;
        case 4: checkpoint.elapsed_s = nan; break;
        case 5: checkpoint.elapsed_s = 0.5; break;
        case 6: checkpoint.throughput_kwh = inf; break;
        case 7: checkpoint.throughput_kwh = 1.0; break;
        case 8: checkpoint.charging_kwh = -1.0; break;
        case 9: checkpoint.charging_kwh = 1.0; break;
        }
        rejects([&] { ec::observe_bms_cycle(checkpoint, bms, 1, Node::Motor, evidence()); },
                "malformed or inconsistent observer checkpoint rejects");
    }
    for (Node counterparty : {Node::Battery, Node::Count, static_cast<Node>(255)})
        rejects([&] { ec::observe_bms_cycle(before, bms, 1, counterparty, evidence()); },
                "invalid/self observer counterparty rejects");
    rejects([&] { ec::observe_bms_cycle(before, bms, 0, Node::Motor, evidence()); },
            "zero observer session rejects");
    auto invalid_evidence = evidence(ec::EvidenceKind::Measured);
    rejects([&] { ec::observe_bms_cycle(before, bms, 1, Node::Motor, invalid_evidence); },
            "computed power*time energy cannot masquerade as direct measured energy");
    invalid_evidence = evidence(); invalid_evidence.provenance_id = 0;
    rejects([&] { ec::observe_bms_cycle(before, bms, 1, Node::Motor, invalid_evidence); },
            "observer requires explicit provenance identity");
    bms.enhance_cycle(400.0, 10.0, 25.0, 0.8, 1.0);
    rejects([&] { ec::observe_bms_cycle(before, bms, 1, Node::Motor, evidence()); },
            "skipped source cycle cannot be silently condensed into a transfer");
    ds_plugin::DSBMSMiddleware extreme;
    extreme.init(75.0, 400.0);
    const auto extreme_before = ec::capture_bms_checkpoint(extreme);
    extreme.enhance_cycle(1.0, maximum / 2.0, 25.0, 0.8, 1800.0);
    const auto accepted_snapshot = extreme.snapshot();
    const auto accepted_count = extreme.diagnostics().update_count;
    rejects([&] { ec::observe_bms_cycle(extreme_before, extreme, 1, Node::Motor, evidence()); },
            "joule observer overflow rejects despite representable source kWh ledger");
    check(same_telemetry(extreme.snapshot(), accepted_snapshot) &&
          extreme.diagnostics().update_count == accepted_count,
          "optional observer overflow cannot roll back or change previously accepted BMS cycle");
}

ds::drive::RegenResult regen(double torque) {
    ds::drive::RegenResult result;
    result.max_regen_torque_nm = torque;
    result.regen_fraction = 0.5;
    return result;
}

void recovery_observer_tests() {
    for (bool measured : {true, false}) {
        ds::drive::EnergyRecoveryConfig config;
        config.prefer_measured_pack_current = measured;
        config.current_estimate_tau_s = 0.0;
        config.max_estimated_regen_current_a = 20.0;
        ds::drive::DSEnergyRecoveryCoordinator coordinator(config);
        ds_plugin::DSBMSMiddleware bms;
        bms.init(75.0, 400.0);
        const auto before = ec::capture_bms_checkpoint(bms);
        const auto cycle = coordinator.step(bms, regen(100.0), 400.0, 25.0, -10.0, 0.5, 100.0, 1.0);
        const auto observed = ec::observe_recovery_cycle(before, bms, cycle, 1, Node::PowerElectronics,
            evidence(measured ? ec::EvidenceKind::Calculated : ec::EvidenceKind::Estimated));
        const auto ordinary = ec::observe_bms_cycle(before, bms, 1, Node::PowerElectronics,
            evidence(measured ? ec::EvidenceKind::Calculated : ec::EvidenceKind::Estimated));
        const double expected_charge = measured ? 4000.0 : 8000.0;
        check(observed.battery.battery_transfer.joules == expected_charge &&
              observed.battery.battery_transfer.source == Node::PowerElectronics &&
              observed.battery.battery_transfer.destination == Node::Battery &&
              observed.battery.event_id == ordinary.event_id &&
              observed.battery.battery_transfer.transfer_id == ordinary.battery_transfer.transfer_id,
              "recovery and middleware aliases share canonical accepted battery transfer identity");
        check(observed.mechanical_available.joules == 10000.0 &&
              observed.electrical_available.joules == 8500.0 &&
              observed.mechanical_available.purpose == ec::QuantityPurpose::AvailableEstimate &&
              observed.electrical_available.purpose == ec::QuantityPurpose::AvailableEstimate &&
              observed.mechanical_available.evidence.kind == ec::EvidenceKind::Estimated &&
              !observed.electrical_available.evidence.independently_supported,
              "T*omega =10000 J and estimated electrical 8500 J remain available estimates");
        check(observed.battery.battery_transfer.evidence.kind ==
                  (measured ? ec::EvidenceKind::Calculated : ec::EvidenceKind::Estimated) &&
              observed.battery.battery_transfer.evidence.independently_supported == measured,
              "estimated-current mode cannot claim independent charging measurement");
        auto batch = battery_batch();
        batch.storage[0] = {Node::Battery, 100000.0, 100000.0 + expected_charge, evidence(), true};
        batch.transfers[0] = observed.battery.battery_transfer;
        batch.transfers[1] = observed.mechanical_available;
        batch.transfers[2] = observed.electrical_available;
        batch.transfer_count = 3;
        ec::EnergyContinuityLedger ledger;
        const auto result = ledger.reconcile(batch);
        check(result.inbound_joules == expected_charge && result.outbound_joules == 0.0 &&
              result.residual_joules == 0.0 && result.status ==
                  (measured ? Status::Balanced : Status::InsufficientEvidence),
              "battery accepted charge is reconciled once; availability never adds extra 18500 J");
        check(close(bms.snapshot().recovered_energy_kwh, expected_charge / 3600000.0) &&
              bms.diagnostics().update_count == 1 &&
              coordinator.diagnostics().accepted_charging_energy_kwh_total ==
                  cycle.diag.accepted_charging_energy_kwh_total,
              "passive recovery observation preserves BMS and coordinator accepted ledgers");
        ec::EnergyContinuityLedger aliases;
        batch.transfers[3] = ordinary.battery_transfer; batch.transfer_count = 4;
        reject_batch(aliases, batch, Error::DuplicateTransfer,
                     "BMS plus coordinator representations cannot duplicate accepted energy");
        for (int mismatch = 0; mismatch < 8; ++mismatch) {
            auto malformed = cycle;
            if (mismatch == 0) malformed.dt_s = 2.0;
            if (mismatch == 1) malformed.bms_diag.update_count = 0;
            if (mismatch == 2) malformed.pack_current_a_used = nan;
            if (mismatch == 3) malformed.diag.recovered_energy_kwh_total = 1.0;
            if (mismatch == 4) malformed.diag.last_mechanical_power_kw_est = inf;
            if (mismatch == 5) malformed.diag.mode = "UNKNOWN";
            if (mismatch == 6) {
                malformed.diag.last_regen_power_kw_est = 11.0;
                malformed.regen_power_kw_est = 11.0;
            }
            if (mismatch == 7) malformed.regen_torque_nm = 101.0;
            rejects([&] { ec::observe_recovery_cycle(before, bms, malformed, 1,
                Node::PowerElectronics,
                evidence(measured ? ec::EvidenceKind::Calculated : ec::EvidenceKind::Estimated)); },
                "malformed/inconsistent recovery result cannot supply accepted evidence");
        }
    }
    ds::drive::DSEnergyRecoveryCoordinator coordinator;
    ds_plugin::DSBMSMiddleware bms;
    bms.init(75.0, 400.0);
    auto before = ec::capture_bms_checkpoint(bms);
    const auto discharge = coordinator.step(bms, regen(100.0), 400.0, 25.0, 10.0, 0.5, 100.0, 1.0);
    const auto observation = ec::observe_recovery_cycle(before, bms, discharge, 1, Node::PowerElectronics,
                                                       evidence(ec::EvidenceKind::Calculated));
    check(observation.battery.battery_transfer.source == Node::Battery &&
          observation.battery.battery_transfer.joules == 4000.0 &&
          discharge.diag.accepted_charging_energy_kwh_total == 0.0 &&
          observation.electrical_available.joules == 8500.0,
          "positive measured pack current remains discharge despite available regen estimate");
    before = observation.battery.next_checkpoint;
    const auto charge = coordinator.step(bms, regen(0.0), 400.0, 25.0, -10.0, 0.5, 0.0, 1.0);
    const auto unattributed = ec::observe_recovery_cycle(before, bms, charge, 1, Node::ExternalSource,
                                                       evidence(ec::EvidenceKind::Calculated));
    check(unattributed.battery.battery_transfer.source == Node::ExternalSource &&
          unattributed.battery.battery_transfer.joules == 4000.0 &&
          unattributed.mechanical_available.joules == 0.0 &&
          unattributed.electrical_available.joules == 0.0 &&
          charge.diag.charging_energy_source == "UNATTRIBUTED_MEASURED",
          "accepted charging without regen torque does not fabricate mechanical recovery or source attribution");
}
} // namespace

int main() {
    unit_and_sign_tests();
    analytic_conservation_tests();
    evidence_and_classification_tests();
    malformed_and_atomicity_tests();
    tolerance_tests();
    arithmetic_extremes_tests();
    cancellation_regressions();
    identity_lifecycle_and_replay_tests();
    bms_observer_noninterference_tests();
    bms_observer_rejection_tests();
    recovery_observer_tests();
    if (failures) {
        std::cerr << failures << " of " << checks << " energy continuity checks failed\n";
        return 1;
    }
    std::cout << checks << " energy continuity checks passed\n";
    return 0;
}
