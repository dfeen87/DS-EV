#ifndef DS_ENERGY_CONTINUITY_HPP
#define DS_ENERGY_CONTINUITY_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ds {
namespace continuity {

// Passive physical accounting only. These nodes are physical boundaries, never
// DS mathematical states, governance decisions, or software data-flow endpoints.
enum class PhysicalNode : std::uint8_t {
    Battery, PowerElectronics, Motor, KineticStore, ThermalSink,
    Auxiliary, ExternalSource, Count
};
constexpr std::size_t kNodeCount = static_cast<std::size_t>(PhysicalNode::Count);
constexpr std::size_t kMaxTransfers = 32;
constexpr double kJoulesPerKwh = 3600000.0;
constexpr std::size_t node_index(PhysicalNode node) noexcept {
    return static_cast<std::size_t>(node);
}
using Boundary = std::array<bool, kNodeCount>;

enum class EvidenceKind : std::uint8_t { Measured, Calculated, Simulated, Estimated };
enum class EvidenceOrigin : std::uint8_t {
    Supplied, BatteryVI, Telemetry, RecoveryMechanical, RecoveryElectrical
};
enum class QuantityPurpose : std::uint8_t { PhysicalTransfer, AvailableEstimate };
enum class ReconciliationStatus : std::uint8_t {
    Rejected, Balanced, Imbalanced, InsufficientEvidence
};
enum class Error : std::uint8_t {
    None, InvalidSession, InvalidEvent, InvalidInterval, OverlappingInterval,
    CapacityExceeded, InvalidNode, InvalidEvidence, RejectedEvidence,
    InvalidQuantity, InvalidTolerance, DuplicateTransfer, DuplicateStorage,
    SelfTransfer, UnrelatedTransfer, BoundaryMismatch, InvalidBoundary,
    MissingStorage, StorageOutsideBoundary, ArithmeticOverflow, CounterOverflow
};

struct ScalarResult {
    double value = 0.0;
    Error error = Error::None;
    explicit operator bool() const noexcept { return error == Error::None; }
};

// Signed conversion helpers do not assign an origin to charging. A negative
// battery VI increment means charging, including external/unattributed sources.
inline ScalarResult joules_to_kwh(double joules) noexcept {
    if (!std::isfinite(joules)) return {0.0, Error::InvalidQuantity};
    const double result = joules / kJoulesPerKwh;
    if (!std::isfinite(result)) return {0.0, Error::ArithmeticOverflow};
    return {result, Error::None};
}
inline ScalarResult kwh_to_joules(double kwh) noexcept {
    if (!std::isfinite(kwh)) return {0.0, Error::InvalidQuantity};
    const double result = kwh * kJoulesPerKwh;
    if (!std::isfinite(result)) return {0.0, Error::ArithmeticOverflow};
    return {result, Error::None};
}
inline ScalarResult battery_increment_joules(double voltage_v, double signed_current_a,
                                            double elapsed_s) noexcept {
    if (!std::isfinite(voltage_v) || !std::isfinite(signed_current_a))
        return {0.0, Error::InvalidQuantity};
    if (voltage_v <= 0.0 || !std::isfinite(elapsed_s) || elapsed_s <= 0.0)
        return {0.0, Error::InvalidInterval};
    const double power_w = voltage_v * signed_current_a;
    if (!std::isfinite(power_w)) return {0.0, Error::ArithmeticOverflow};
    const double joules = power_w * elapsed_s;
    if (!std::isfinite(joules)) return {0.0, Error::ArithmeticOverflow};
    return {joules, Error::None};
}
inline ScalarResult battery_increment_kwh(double voltage_v, double signed_current_a,
                                         double elapsed_s) noexcept {
    const auto joules = battery_increment_joules(voltage_v, signed_current_a, elapsed_s);
    if (!joules) return joules;
    return joules_to_kwh(joules.value);
}

struct Evidence {
    EvidenceKind kind = EvidenceKind::Estimated;
    EvidenceOrigin origin = EvidenceOrigin::Supplied;
    // Caller-owned provenance identity; this is not sensor authentication.
    std::uint64_t provenance_id = 0;
    // Caller declaration, not inferred from a numerical zero residual. Set false
    // for storage/transfers derived circularly from the same balance equation.
    bool independently_supported = false;
};

struct TransferRecord {
    // Identity of one actual physical transfer in this event, shared by aliases.
    // Distinct IDs cannot make duplicate diagnostics independent evidence.
    std::uint64_t transfer_id = 0;
    PhysicalNode source = PhysicalNode::Battery;
    PhysicalNode destination = PhysicalNode::PowerElectronics;
    // Direction is source -> destination; a quantity must never be negative.
    double joules = 0.0;
    double start_s = 0.0;
    double end_s = 0.0;
    Evidence evidence;
    QuantityPurpose purpose = QuantityPurpose::PhysicalTransfer;
    // Acceptance here describes eligibility of supplied evidence; it does not
    // cause or replace BMS acceptance, trust authorization, or actuator action.
    bool accepted = true;
};

struct StorageObservation {
    PhysicalNode node = PhysicalNode::Battery;
    double initial_joules = 0.0;
    double final_joules = 0.0;
    Evidence evidence;
    bool accepted = true;
};

struct Tolerance {
    double absolute_joules = 1e-6;
    double relative = 1e-9;
};

struct Batch {
    std::uint64_t session_id = 0;
    // Strictly increasing within a session: old/replayed IDs are rejected with
    // constant memory. IDs may have gaps but may not wrap or be reused.
    std::uint64_t event_id = 0;
    double start_s = 0.0;
    double end_s = 0.0;
    Boundary boundary{};
    std::array<TransferRecord, kMaxTransfers> transfers{};
    std::size_t transfer_count = 0;
    std::array<StorageObservation, kNodeCount> storage{};
    std::size_t storage_count = 0;
    // Completeness is supplied explicitly. It cannot be deduced from a zero
    // residual. Every selected node needs storage evidence, including zero.
    bool transfers_complete = false;
    bool stores_complete = false;
    Tolerance tolerance;
};

struct ReconciliationResult {
    ReconciliationStatus status = ReconciliationStatus::Rejected;
    Error error = Error::None;
    // Numerically valid incomplete/imbalanced evidence is still committed;
    // malformed/rejected evidence never changes a snapshot.
    bool committed = false;
    bool evidence_complete = false;
    bool independently_supported = false;
    double initial_joules = 0.0;
    double inbound_joules = 0.0;
    double final_joules = 0.0;
    double outbound_joules = 0.0;
    double dissipated_joules = 0.0;
    double storage_change_joules = 0.0;
    double residual_joules = 0.0;
    double allowed_residual_joules = 0.0;
};

struct LedgerSnapshot {
    std::uint64_t session_id = 0;
    std::uint64_t last_event_id = 0;
    std::uint64_t accepted_events = 0;
    double last_end_s = 0.0;
    Boundary boundary{};
    double inbound_joules = 0.0;
    double outbound_joules = 0.0;
    double dissipated_joules = 0.0;
    double storage_change_joules = 0.0;
    double residual_joules = 0.0;
    // Physical transfer totals by classification, counted once per record,
    // including internal edges. Do not add these totals to boundary flux.
    std::array<double, 4> physical_joules_by_kind{};
    // Available recovery estimates are diagnostics, never boundary transfers.
    std::array<double, 4> available_estimate_joules_by_kind{};
};

// Single-caller, fixed-capacity observer. It owns only its opt-in evidence ledger;
// it never reads or writes existing BMS, coordinator, DS, or AILEE state. Calling
// reset with a newer nonzero session clears the entire session. It cannot undo
// source-system ledger entries. No history is retained beyond this snapshot.
// Balance is assessed per supplied interval. Gaps are permitted, and storage
// endpoints are not authenticated or linked across events: accumulated deltas
// cannot certify energy continuity during an unobserved part of a session.
class EnergyContinuityLedger {
public:
    explicit EnergyContinuityLedger(std::uint64_t session_id = 1) noexcept {
        state_.session_id = session_id;
    }
    const LedgerSnapshot& snapshot() const noexcept { return state_; }

    Error reset(std::uint64_t new_session_id) noexcept {
        if (new_session_id == 0 || new_session_id <= state_.session_id)
            return Error::InvalidSession;
        LedgerSnapshot fresh;
        fresh.session_id = new_session_id;
        state_ = fresh;
        accumulated_storage_change_ = CheckedSum{};
        accumulated_residual_ = CheckedSum{};
        return Error::None;
    }

    ReconciliationResult reconcile(const Batch& batch) noexcept {
        ReconciliationResult result;
        const auto reject = [&result](Error error) noexcept {
            result.status = ReconciliationStatus::Rejected;
            result.error = error;
            return result;
        };
        if (state_.session_id == 0 || batch.session_id != state_.session_id)
            return reject(Error::InvalidSession);
        if (batch.event_id == 0 || batch.event_id <= state_.last_event_id)
            return reject(Error::InvalidEvent);
        if (!valid_interval(batch.start_s, batch.end_s))
            return reject(Error::InvalidInterval);
        if (state_.accepted_events != 0 && batch.start_s < state_.last_end_s)
            return reject(Error::OverlappingInterval);
        if (batch.transfer_count > kMaxTransfers || batch.storage_count > kNodeCount)
            return reject(Error::CapacityExceeded);
        if (!std::isfinite(batch.tolerance.absolute_joules) ||
            !std::isfinite(batch.tolerance.relative) ||
            batch.tolerance.absolute_joules < 0.0 || batch.tolerance.relative < 0.0)
            return reject(Error::InvalidTolerance);
        if (std::none_of(batch.boundary.begin(), batch.boundary.end(),
                         [](bool selected) noexcept { return selected; }))
            return reject(Error::InvalidBoundary);
        if (state_.accepted_events != 0 && batch.boundary != state_.boundary)
            return reject(Error::BoundaryMismatch);
        if (state_.accepted_events == std::numeric_limits<std::uint64_t>::max())
            return reject(Error::CounterOverflow);

        // All validation/arithmetic stages against a local snapshot; the only
        // state assignment is the no-throw commit at the end.
        LedgerSnapshot candidate = state_;
        CheckedSum storage_change;
        CheckedSum residual;
        CheckedSum candidate_storage_change = accumulated_storage_change_;
        CheckedSum candidate_residual = accumulated_residual_;
        bool independent = true;
        std::array<bool, kNodeCount> observed{};
        for (std::size_t i = 0; i < batch.storage_count; ++i) {
            const auto& storage = batch.storage[i];
            if (!valid_node(storage.node)) return reject(Error::InvalidNode);
            const auto node = node_index(storage.node);
            if (!batch.boundary[node]) return reject(Error::StorageOutsideBoundary);
            if (observed[node]) return reject(Error::DuplicateStorage);
            observed[node] = true;
            if (!storage.accepted) return reject(Error::RejectedEvidence);
            if (!valid_evidence(storage.evidence)) return reject(Error::InvalidEvidence);
            if (!valid_quantity(storage.initial_joules) ||
                !valid_quantity(storage.final_joules)) return reject(Error::InvalidQuantity);
            independent = independent && storage.evidence.independently_supported;
            if (!add(result.initial_joules, storage.initial_joules) ||
                !add(result.final_joules, storage.final_joules))
                return reject(Error::ArithmeticOverflow);
            // Feed each pair as signed endpoints, preserving the small term
            // even when its difference cannot be represented by one double.
            // Unchanged large inventories also cancel before boundary flux.
            if (!storage_change.add(storage.final_joules) ||
                !storage_change.add(-storage.initial_joules) ||
                !residual.add(storage.initial_joules) ||
                !residual.add(-storage.final_joules))
                return reject(Error::ArithmeticOverflow);
        }
        bool storage_complete = true;
        for (std::size_t i = 0; i < kNodeCount; ++i)
            if (batch.boundary[i] && !observed[i]) storage_complete = false;
        // A missing observation is incomplete evidence, not an invented zero.
        // If the caller claims completeness, the contradiction is malformed.
        if (batch.stores_complete && !storage_complete) return reject(Error::MissingStorage);

        for (std::size_t i = 0; i < batch.transfer_count; ++i) {
            const auto& transfer = batch.transfers[i];
            if (transfer.transfer_id == 0) return reject(Error::InvalidEvent);
            for (std::size_t j = 0; j < i; ++j)
                if (batch.transfers[j].transfer_id == transfer.transfer_id)
                    return reject(Error::DuplicateTransfer);
            if (!valid_node(transfer.source) || !valid_node(transfer.destination))
                return reject(Error::InvalidNode);
            if (transfer.source == transfer.destination) return reject(Error::SelfTransfer);
            if (!transfer.accepted) return reject(Error::RejectedEvidence);
            if (!valid_evidence(transfer.evidence)) return reject(Error::InvalidEvidence);
            if (!valid_quantity(transfer.joules)) return reject(Error::InvalidQuantity);
            if (!valid_interval(transfer.start_s, transfer.end_s) ||
                transfer.start_s != batch.start_s || transfer.end_s != batch.end_s)
                return reject(Error::InvalidInterval);
            if (transfer.purpose != QuantityPurpose::PhysicalTransfer &&
                transfer.purpose != QuantityPurpose::AvailableEstimate)
                return reject(Error::InvalidEvidence);
            const auto kind = static_cast<std::size_t>(transfer.evidence.kind);
            if (transfer.purpose == QuantityPurpose::AvailableEstimate) {
                if (!add(candidate.available_estimate_joules_by_kind[kind], transfer.joules))
                    return reject(Error::ArithmeticOverflow);
                continue;
            }
            const bool source_inside = batch.boundary[node_index(transfer.source)];
            const bool destination_inside = batch.boundary[node_index(transfer.destination)];
            if (!source_inside && !destination_inside) return reject(Error::UnrelatedTransfer);
            independent = independent && transfer.evidence.independently_supported;
            if (!add(candidate.physical_joules_by_kind[kind], transfer.joules))
                return reject(Error::ArithmeticOverflow);
            if (!source_inside && destination_inside) {
                if (!add(result.inbound_joules, transfer.joules) || !residual.add(transfer.joules))
                    return reject(Error::ArithmeticOverflow);
            } else if (source_inside && !destination_inside) {
                // Only an explicitly supplied crossing to a thermal sink is
                // dissipation. An unexplained residual is never relabeled heat.
                double& destination = transfer.destination == PhysicalNode::ThermalSink
                    ? result.dissipated_joules : result.outbound_joules;
                if (!add(destination, transfer.joules) || !residual.add(-transfer.joules))
                    return reject(Error::ArithmeticOverflow);
            }
        }

        if (!storage_change.value(result.storage_change_joules) ||
            !residual.value(result.residual_joules)) return reject(Error::ArithmeticOverflow);
        // The equation's displayed sides must still be finite even though the
        // residual is computed from stable signed contributions, not left-right.
        double left = result.initial_joules;
        double right = result.final_joules;
        if (!add(left, result.inbound_joules) || !add(right, result.outbound_joules) ||
            !add(right, result.dissipated_joules)) return reject(Error::ArithmeticOverflow);
        // Relative tolerance scales with the largest explicit term; it does
        // not scale with a residual being assessed or with a signed cancellation.
        double outgoing = result.outbound_joules;
        if (!add(outgoing, result.dissipated_joules)) return reject(Error::ArithmeticOverflow);
        const double scale = std::max({result.initial_joules, result.final_joules,
                                       result.inbound_joules, outgoing,
                                       std::abs(result.storage_change_joules)});
        const double relative_allowance = batch.tolerance.relative * scale;
        result.allowed_residual_joules = batch.tolerance.absolute_joules;
        if (!std::isfinite(relative_allowance) ||
            !add(result.allowed_residual_joules, relative_allowance))
            return reject(Error::ArithmeticOverflow);
        if (!add(candidate.inbound_joules, result.inbound_joules) ||
            !add(candidate.outbound_joules, result.outbound_joules) ||
            !add(candidate.dissipated_joules, result.dissipated_joules) ||
            !candidate_storage_change.merge(storage_change) ||
            !candidate_residual.merge(residual) ||
            !candidate_storage_change.value(candidate.storage_change_joules) ||
            !candidate_residual.value(candidate.residual_joules))
            return reject(Error::ArithmeticOverflow);

        result.evidence_complete = batch.transfers_complete && batch.stores_complete && storage_complete;
        result.independently_supported = independent;
        if (!result.evidence_complete || !independent) {
            result.status = ReconciliationStatus::InsufficientEvidence;
        } else {
            result.status = std::abs(result.residual_joules) <= result.allowed_residual_joules
                ? ReconciliationStatus::Balanced : ReconciliationStatus::Imbalanced;
        }
        candidate.session_id = batch.session_id;
        candidate.last_event_id = batch.event_id;
        candidate.last_end_s = batch.end_s;
        candidate.boundary = batch.boundary;
        ++candidate.accepted_events;
        state_ = candidate;
        accumulated_storage_change_ = candidate_storage_change;
        accumulated_residual_ = candidate_residual;
        result.committed = true;
        return result;
    }

private:
    // Neumaier compensation preserves a small signed contribution through
    // cancellation of larger terms. Every intermediate and final value remains
    // checked; this does not recover precision absent from supplied doubles.
    struct CheckedSum {
        double sum = 0.0;
        double correction = 0.0;
        bool add(double term) noexcept {
            const double next = sum + term;
            if (!std::isfinite(next)) return false;
            const double adjustment = std::abs(sum) >= std::abs(term)
                ? (sum - next) + term : (term - next) + sum;
            if (!std::isfinite(adjustment)) return false;
            const double next_correction = correction + adjustment;
            if (!std::isfinite(next_correction)) return false;
            sum = next;
            correction = next_correction;
            return true;
        }
        bool merge(const CheckedSum& other) noexcept {
            return add(other.sum) && add(other.correction);
        }
        bool value(double& out) const noexcept {
            const double total = sum + correction;
            if (!std::isfinite(total)) return false;
            out = total;
            return true;
        }
    };
    LedgerSnapshot state_;
    CheckedSum accumulated_storage_change_;
    CheckedSum accumulated_residual_;
    static bool valid_node(PhysicalNode node) noexcept { return node_index(node) < kNodeCount; }
    static bool valid_quantity(double value) noexcept { return std::isfinite(value) && value >= 0.0; }
    static bool valid_interval(double start, double end) noexcept {
        return std::isfinite(start) && std::isfinite(end) && start >= 0.0 &&
               end > start && std::isfinite(end - start);
    }
    static bool valid_evidence(const Evidence& evidence) noexcept {
        return static_cast<unsigned>(evidence.kind) <= static_cast<unsigned>(EvidenceKind::Estimated) &&
               static_cast<unsigned>(evidence.origin) <= static_cast<unsigned>(EvidenceOrigin::RecoveryElectrical) &&
               evidence.provenance_id != 0;
    }
    static bool add(double& accumulator, double value) noexcept {
        const double sum = accumulator + value;
        if (!std::isfinite(sum)) return false;
        accumulator = sum;
        return true;
    }
};

} // namespace continuity
} // namespace ds

#endif // DS_ENERGY_CONTINUITY_HPP
