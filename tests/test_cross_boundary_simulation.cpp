#include "ds_energy_continuity.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace ec = ds::continuity;
using Node = ec::PhysicalNode;
using Status = ec::ReconciliationStatus;
using Error = ec::Error;

namespace {

int checks = 0;
int failures = 0;

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

ec::Evidence sim(std::uint64_t id, bool independent = true) {
    return {ec::EvidenceKind::Simulated, ec::EvidenceOrigin::Supplied, id, independent};
}

ec::Batch make_batch(std::uint64_t session, std::uint64_t event, double start, double end) {
    ec::Batch b;
    b.session_id = session;
    b.event_id = event;
    b.start_s = start;
    b.end_s = end;
    b.transfers_complete = true;
    b.stores_complete = true;
    b.tolerance.absolute_joules = 1e-6;
    b.tolerance.relative = 1e-9;
    return b;
}

void transfer(ec::Batch& b, Node src, Node dest, double joules, ec::Evidence ev) {
    if (b.transfer_count >= ec::kMaxTransfers) throw std::runtime_error("transfer capacity");
    b.transfers[b.transfer_count++] = {b.transfer_count + 1, src, dest, joules, b.start_s, b.end_s, ev, ec::QuantityPurpose::PhysicalTransfer, true};
}

void store(ec::Batch& b, Node n, double initial, double final, ec::Evidence ev) {
    if (b.storage_count >= ec::kNodeCount) throw std::runtime_error("storage capacity");
    b.boundary[ec::node_index(n)] = true;
    b.storage[b.storage_count++] = {n, initial, final, ev, true};
}

void test_battery_boundary() {
    ec::EnergyContinuityLedger ledger;

    // 1. Normal discharge
    ledger.reset(1);
    ec::Batch b1 = make_batch(1, 1, 0.0, 1.0);
    store(b1, Node::Battery, 1000.0, 900.0, sim(1));
    transfer(b1, Node::Battery, Node::PowerElectronics, 100.0, sim(2));
    auto r1 = ledger.reconcile(b1);
    check(r1.status == Status::Balanced, "Battery normal discharge should be balanced");
    check(close(r1.residual_joules, 0.0), "Battery discharge residual should be zero");

    // 2. Normal charge (regen)
    ledger.reset(1);
    ec::Batch b2 = make_batch(1, 2, 1.0, 2.0);
    store(b2, Node::Battery, 900.0, 950.0, sim(3));
    transfer(b2, Node::PowerElectronics, Node::Battery, 50.0, sim(4));
    auto r2 = ledger.reconcile(b2);
    check(r2.status == Status::Balanced, "Battery normal charge should be balanced");
    check(close(r2.residual_joules, 0.0), "Battery charge residual should be zero");

    // 3. Rejected BMS cycle - evidence not accepted
    ledger.reset(1);
    ec::Batch b3 = make_batch(1, 3, 2.0, 3.0);
    store(b3, Node::Battery, 950.0, 900.0, sim(5));
    b3.storage[0].accepted = false;
    transfer(b3, Node::Battery, Node::PowerElectronics, 50.0, sim(6));
    auto r3 = ledger.reconcile(b3);
    check(r3.status == Status::Rejected, "Rejected BMS cycle should be rejected");
    check(r3.error == Error::RejectedEvidence, "Error should be RejectedEvidence");

    // 4. Distinction between measured, calculated, estimated (independent flag)
    ledger.reset(1);
    ec::Batch b4 = make_batch(1, 4, 3.0, 4.0);
    store(b4, Node::Battery, 950.0, 900.0, sim(7, false)); // not independent
    transfer(b4, Node::Battery, Node::PowerElectronics, 50.0, sim(8, true));
    auto r4 = ledger.reconcile(b4);
    check(r4.status == Status::InsufficientEvidence, "Not independently supported should be insufficient");
    check(!r4.independently_supported, "Flag independently_supported should be false");

    // 5. Duplicate charging credits (imbalanced)
    ledger.reset(1);
    ec::Batch b5 = make_batch(1, 5, 4.0, 5.0);
    store(b5, Node::Battery, 900.0, 950.0, sim(9));
    transfer(b5, Node::PowerElectronics, Node::Battery, 50.0, sim(10));
    transfer(b5, Node::PowerElectronics, Node::Battery, 50.0, sim(11)); // Oops, double charging
    auto r5 = ledger.reconcile(b5);
    check(r5.status == Status::Imbalanced, "Duplicate charging should be imbalanced");
}

void test_inverter_boundary() {
    ec::EnergyContinuityLedger ledger;

    // 1. Normal transfer with losses (dissipated heat)
    ledger.reset(2);
    ec::Batch b1 = make_batch(2, 1, 0.0, 1.0);
    store(b1, Node::PowerElectronics, 0.0, 0.0, sim(20)); // Inverter holds no energy
    transfer(b1, Node::Battery, Node::PowerElectronics, 100.0, sim(21)); // In
    transfer(b1, Node::PowerElectronics, Node::Motor, 95.0, sim(22)); // Out
    transfer(b1, Node::PowerElectronics, Node::ThermalSink, 5.0, sim(23)); // Heat
    auto r1 = ledger.reconcile(b1);
    check(r1.status == Status::Balanced, "Inverter normal operation should be balanced");

    // 2. Detection of missing transfer (imbalanced if complete)
    ledger.reset(2);
    ec::Batch b2 = make_batch(2, 2, 1.0, 2.0);
    store(b2, Node::PowerElectronics, 0.0, 0.0, sim(24));
    transfer(b2, Node::Battery, Node::PowerElectronics, 100.0, sim(25)); // In
    transfer(b2, Node::PowerElectronics, Node::Motor, 95.0, sim(26)); // Out (missing heat)
    auto r2 = ledger.reconcile(b2);
    check(r2.status == Status::Imbalanced, "Inverter missing heat should be imbalanced");
    check(close(r2.residual_joules, 5.0), "Inverter missing heat residual should be 5.0");

    // 3. Incomplete transfers (status should be InsufficientEvidence)
    ledger.reset(2);
    ec::Batch b3 = make_batch(2, 3, 2.0, 3.0);
    store(b3, Node::PowerElectronics, 0.0, 0.0, sim(27));
    transfer(b3, Node::Battery, Node::PowerElectronics, 100.0, sim(28));
    b3.transfers_complete = false;
    auto r3 = ledger.reconcile(b3);
    check(r3.status == Status::InsufficientEvidence, "Incomplete transfers should be insufficient evidence");
}

void test_motor_boundary() {
    ec::EnergyContinuityLedger ledger;

    // 1. Normal electrical in, mechanical out + losses
    ledger.reset(3);
    ec::Batch b1 = make_batch(3, 1, 0.0, 1.0);
    store(b1, Node::Motor, 0.0, 0.0, sim(40));
    transfer(b1, Node::PowerElectronics, Node::Motor, 100.0, sim(41)); // In
    transfer(b1, Node::Motor, Node::KineticStore, 90.0, sim(42)); // Out
    transfer(b1, Node::Motor, Node::ThermalSink, 10.0, sim(43)); // Heat
    auto r1 = ledger.reconcile(b1);
    check(r1.status == Status::Balanced, "Motor normal operation should be balanced");

    // 2. Independently injected imbalance
    ledger.reset(3);
    ec::Batch b2 = make_batch(3, 2, 1.0, 2.0);
    store(b2, Node::Motor, 0.0, 0.0, sim(44));
    transfer(b2, Node::PowerElectronics, Node::Motor, 100.0, sim(45)); // In
    transfer(b2, Node::Motor, Node::KineticStore, 110.0, sim(46)); // Out (more than in)
    transfer(b2, Node::Motor, Node::ThermalSink, 10.0, sim(47));
    auto r2 = ledger.reconcile(b2);
    check(r2.status == Status::Imbalanced, "Motor injected imbalance should be imbalanced");
    check(close(r2.residual_joules, -20.0), "Motor injected imbalance residual should be -20");
}

void test_kinetic_boundary() {
    ec::EnergyContinuityLedger ledger;

    // 1. Modeled kinetic energy accumulation
    ledger.reset(4);
    ec::Batch b1 = make_batch(4, 1, 0.0, 1.0);
    store(b1, Node::KineticStore, 1000.0, 1100.0, sim(60));
    transfer(b1, Node::Motor, Node::KineticStore, 100.0, sim(61)); // from Motor
    auto r1 = ledger.reconcile(b1);
    check(r1.status == Status::Balanced, "Kinetic energy accumulation should be balanced");

    // 2. Kinetic energy reduction during regen braking
    ledger.reset(4);
    ec::Batch b2 = make_batch(4, 2, 1.0, 2.0);
    store(b2, Node::KineticStore, 1100.0, 1000.0, sim(62));
    transfer(b2, Node::KineticStore, Node::Motor, 100.0, sim(63)); // back to Motor
    auto r2 = ledger.reconcile(b2);
    check(r2.status == Status::Balanced, "Kinetic regen should be balanced");

    // 3. Separation of available mechanical recovery vs accepted battery charging
    // We send some mechanical energy to the motor, and add an available estimate
    ledger.reset(4);
    ec::Batch b3 = make_batch(4, 3, 2.0, 3.0);
    store(b3, Node::KineticStore, 1000.0, 900.0, sim(64));
    transfer(b3, Node::KineticStore, Node::Motor, 100.0, sim(65));
    // Provide a diagnostic transfer (estimate) which shouldn't affect physical balance
    b3.transfers[b3.transfer_count++] = {b3.transfer_count + 1, Node::KineticStore, Node::Motor, 100.0, b3.start_s, b3.end_s,
        {ec::EvidenceKind::Estimated, ec::EvidenceOrigin::RecoveryMechanical, 66, true},
        ec::QuantityPurpose::AvailableEstimate, true};
    auto r3 = ledger.reconcile(b3);
    check(r3.status == Status::Balanced, "Kinetic regen with diagnostic should be balanced");
}


void test_numerical_adversarial() {
    // 1. Extreme numeric limits (min, denorm_min)
    {
        ec::EnergyContinuityLedger ledger;
        ledger.reset(10);
        ec::Batch b1 = make_batch(10, 1, 0.0, 1.0);
        double min_val = std::numeric_limits<double>::min();
        store(b1, Node::Battery, min_val, min_val, sim(200));
        auto r1 = ledger.reconcile(b1);
        check(r1.status == Status::Balanced, "Min val storage should balance");

        ledger.reset(11);
        ec::Batch b2 = make_batch(11, 1, 0.0, 1.0);
        double denorm = std::numeric_limits<double>::denorm_min();
        store(b2, Node::Battery, denorm, 0.0, sim(201));
        transfer(b2, Node::Battery, Node::PowerElectronics, denorm, sim(202));
        auto r2 = ledger.reconcile(b2);
        check(r2.status == Status::Balanced, "Denorm val transfer should balance");
    }

    // 2. Magnitudes near 2^53 (limit of exact integer representation in double)
    {
        ec::EnergyContinuityLedger ledger;
        ledger.reset(12);
        ec::Batch b1 = make_batch(12, 1, 0.0, 1.0);
        double large = 9007199254740992.0; // 2^53
        store(b1, Node::Battery, large, large - 1.0, sim(203));
        transfer(b1, Node::Battery, Node::PowerElectronics, 1.0, sim(204));
        auto r1 = ledger.reconcile(b1);
        check(r1.status == Status::Balanced, "2^53 - 1 should balance exactly");
    }

    // 3. Very large finite energy combined with small positive/negative transfers
    {
        ec::EnergyContinuityLedger ledger;
        ledger.reset(13);
        ec::Batch b1 = make_batch(13, 1, 0.0, 1.0);
        double very_large = 1e20;
        store(b1, Node::Battery, very_large, very_large, sim(205));
        transfer(b1, Node::Battery, Node::PowerElectronics, 1.0, sim(206)); // outgoing
        b1.tolerance.absolute_joules = 0.0;
        b1.tolerance.relative = 0.0;
        auto r1 = ledger.reconcile(b1);
        check(r1.status == Status::Imbalanced, "Very large energy with 1.0 transfer should not cancel out internally");
        check(close(r1.residual_joules, -1.0), "Residual should be -1.0 due to compensated summation");
    }

    // 4. Positive and negative zero where sign behavior matters
    {
        ec::EnergyContinuityLedger ledger;
        ledger.reset(14);
        ec::Batch b1 = make_batch(14, 1, 0.0, 1.0);
        store(b1, Node::Battery, 0.0, -0.0, sim(207));
        transfer(b1, Node::PowerElectronics, Node::Battery, +0.0, sim(208));
        auto r1 = ledger.reconcile(b1);
        check(r1.status == Status::Balanced, "Signed zeros should evaluate correctly to balanced");
    }

    // 5. Tiny durations and large finite power values that stress multiplication
    {
        ec::EnergyContinuityLedger ledger;
        ledger.reset(15);
        ec::Batch b1 = make_batch(15, 1, 0.0, 1e-9);
        double power_w = 1e9;
        double elapsed_s = 1e-9;
        double joules = power_w * elapsed_s; // 1.0 J
        store(b1, Node::Battery, 1000.0, 999.0, sim(209));
        transfer(b1, Node::Battery, Node::PowerElectronics, joules, sim(210));
        auto r1 = ledger.reconcile(b1);
        check(r1.status == Status::Balanced, "Tiny duration / large power should multiply correctly and balance");
    }

    // 6. nextafter tolerance boundaries
    {
        ec::EnergyContinuityLedger ledger;
        ledger.reset(16);
        ec::Batch b1 = make_batch(16, 1, 0.0, 1.0);
        b1.tolerance.absolute_joules = 1.0;
        b1.tolerance.relative = 0.0;

        store(b1, Node::Battery, 100.0, 90.0, sim(211));

        // Exact boundary (residual +1.0)
        double transfer_exact = 9.0;
        transfer(b1, Node::Battery, Node::PowerElectronics, transfer_exact, sim(212));
        auto r1 = ledger.reconcile(b1);
        check(r1.status == Status::Balanced, "Transfer at exact tolerance boundary should balance");

        // Just outside boundary
        ledger.reset(16);
        ec::Batch b2 = make_batch(16, 2, 1.0, 2.0);
        b2.tolerance = b1.tolerance;
        store(b2, Node::Battery, 100.0, 90.0, sim(213));
        double transfer_out = std::nextafter(9.0, 0.0); // > 1.0 residual
        transfer(b2, Node::Battery, Node::PowerElectronics, transfer_out, sim(214));
        auto r2 = ledger.reconcile(b2);
        check(r2.status == Status::Imbalanced, "Transfer just outside absolute tolerance boundary should be imbalanced");
    }
}

void test_cross_interval_challenges() {
    // 1. Consecutive intervals with consistent storage endpoints
    {
        ec::EnergyContinuityLedger ledger;
        ledger.reset(1);
        ec::Batch b1 = make_batch(1, 1, 0.0, 1.0);
        store(b1, Node::Battery, 1000.0, 900.0, sim(100));
        transfer(b1, Node::Battery, Node::PowerElectronics, 100.0, sim(101));
        auto r1 = ledger.reconcile(b1);
        check(r1.status == Status::Balanced, "b1 should be balanced");

        ec::Batch b2 = make_batch(1, 2, 1.0, 2.0);
        // Consistent endpoints: b2 initial is b1 final
        store(b2, Node::Battery, 900.0, 850.0, sim(102));
        transfer(b2, Node::Battery, Node::PowerElectronics, 50.0, sim(103));
        b2.boundary = b1.boundary; // ensure boundary matches to pass BoundaryMismatch check
        auto r2 = ledger.reconcile(b2);
        check(r2.status == Status::Balanced, "b2 should be balanced");
    }

    // 2. Deliberate gap between intervals
    {
        ec::EnergyContinuityLedger ledger;
        ledger.reset(2);
        ec::Batch b1 = make_batch(2, 1, 0.0, 1.0);
        store(b1, Node::Battery, 1000.0, 900.0, sim(104));
        transfer(b1, Node::Battery, Node::PowerElectronics, 100.0, sim(105));
        ledger.reconcile(b1);

        ec::Batch b2 = make_batch(2, 2, 5.0, 6.0); // gap in time from 1.0 to 5.0
        store(b2, Node::Battery, 900.0, 850.0, sim(106));
        transfer(b2, Node::Battery, Node::PowerElectronics, 50.0, sim(107));
        b2.boundary = b1.boundary;
        auto r2 = ledger.reconcile(b2);
        check(r2.status == Status::Balanced, "Gap between intervals is permitted and balanced");
    }

    // 3. Deliberate mismatch between preceding final storage and next initial storage
    {
        ec::EnergyContinuityLedger ledger;
        ledger.reset(3);
        ec::Batch b1 = make_batch(3, 1, 0.0, 1.0);
        store(b1, Node::Battery, 1000.0, 900.0, sim(108));
        transfer(b1, Node::Battery, Node::PowerElectronics, 100.0, sim(109));
        ledger.reconcile(b1);

        ec::Batch b2 = make_batch(3, 2, 1.0, 2.0);
        // Mismatch: initial is 800 but b1 ended at 900
        store(b2, Node::Battery, 800.0, 750.0, sim(110));
        transfer(b2, Node::Battery, Node::PowerElectronics, 50.0, sim(111));
        b2.boundary = b1.boundary;
        auto r2 = ledger.reconcile(b2);
        // API does NOT verify storage continuity across events, so it evaluates b2 in isolation.
        check(r2.status == Status::Balanced, "Storage mismatch across events is a documented limitation and should be balanced per-interval");
    }

    // 4. Replayed event identifiers
    {
        ec::EnergyContinuityLedger ledger;
        ledger.reset(4);
        ec::Batch b1 = make_batch(4, 5, 0.0, 1.0); // event 5
        store(b1, Node::Battery, 1000.0, 900.0, sim(112));
        transfer(b1, Node::Battery, Node::PowerElectronics, 100.0, sim(113));
        ledger.reconcile(b1);

        ec::Batch b2 = make_batch(4, 5, 1.0, 2.0); // replay event 5
        store(b2, Node::Battery, 900.0, 850.0, sim(114));
        transfer(b2, Node::Battery, Node::PowerElectronics, 50.0, sim(115));
        b2.boundary = b1.boundary;
        auto r2 = ledger.reconcile(b2);
        check(r2.status == Status::Rejected, "Replayed event ID should be rejected");
        check(r2.error == Error::InvalidEvent, "Error should be InvalidEvent");

        ec::Batch b3 = make_batch(4, 4, 1.0, 2.0); // older event 4
        store(b3, Node::Battery, 900.0, 850.0, sim(116));
        transfer(b3, Node::Battery, Node::PowerElectronics, 50.0, sim(117));
        b3.boundary = b1.boundary;
        auto r3 = ledger.reconcile(b3);
        check(r3.status == Status::Rejected, "Older event ID should be rejected");
    }

    // 5. Invalid session identifiers
    {
        ec::EnergyContinuityLedger ledger;
        ledger.reset(5);
        ec::Batch b1 = make_batch(99, 1, 0.0, 1.0); // different session id
        store(b1, Node::Battery, 1000.0, 900.0, sim(118));
        transfer(b1, Node::Battery, Node::PowerElectronics, 100.0, sim(119));
        auto r1 = ledger.reconcile(b1);
        check(r1.status == Status::Rejected, "Invalid session ID should be rejected");
        check(r1.error == Error::InvalidSession, "Error should be InvalidSession");
    }

    // 6. Session reset and BMS reinitialization
    {
        ec::EnergyContinuityLedger ledger;
        ledger.reset(6);
        ec::Batch b1 = make_batch(6, 1, 0.0, 1.0);
        store(b1, Node::Battery, 1000.0, 900.0, sim(120));
        transfer(b1, Node::Battery, Node::PowerElectronics, 100.0, sim(121));
        ledger.reconcile(b1);

        // BMS reinitialization -> new session
        ledger.reset(7);
        ec::Batch b2 = make_batch(7, 1, 0.0, 1.0); // event ID can reset now
        store(b2, Node::Battery, 500.0, 450.0, sim(122));
        transfer(b2, Node::Battery, Node::PowerElectronics, 50.0, sim(123));
        auto r2 = ledger.reconcile(b2);
        check(r2.status == Status::Balanced, "New session should reset state and accept event 1");
    }

    // 7. Repeated charging/discharging transitions
    {
        ec::EnergyContinuityLedger ledger;
        ledger.reset(8);
        ec::Batch b1 = make_batch(8, 1, 0.0, 1.0);
        store(b1, Node::Battery, 1000.0, 900.0, sim(124)); // discharge
        transfer(b1, Node::Battery, Node::PowerElectronics, 100.0, sim(125));
        ledger.reconcile(b1);

        ec::Batch b2 = make_batch(8, 2, 1.0, 2.0);
        b2.boundary = b1.boundary;
        store(b2, Node::Battery, 900.0, 1050.0, sim(126)); // charge
        transfer(b2, Node::PowerElectronics, Node::Battery, 150.0, sim(127));
        ledger.reconcile(b2);

        ec::Batch b3 = make_batch(8, 3, 2.0, 3.0);
        b3.boundary = b1.boundary;
        store(b3, Node::Battery, 1050.0, 1000.0, sim(128)); // discharge
        transfer(b3, Node::Battery, Node::PowerElectronics, 50.0, sim(129));
        auto r3 = ledger.reconcile(b3);
        check(r3.status == Status::Balanced, "Repeated transitions should remain balanced");
    }
}

} // namespace

int main() {
    std::cout << "Starting Cross-Boundary Simulation Validations...\n";
    try {
        test_battery_boundary();
        test_inverter_boundary();
        test_motor_boundary();
        test_kinetic_boundary();
        test_cross_interval_challenges();
        test_numerical_adversarial();

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return 1;
    }
    std::cout << checks << " checks, " << failures << " failures.\n";
    return failures > 0 ? 1 : 0;
}
