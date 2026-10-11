/*
 * ============================================================================
 * AILEE TRUST LAYER — DETERMINISTIC TRUST GATE
 * ============================================================================
 *
 * Provides deterministic evaluation of system-wide trust scores, governance
 * level selection, and safety gating for parameter updates and control loops.
 *
 * Namespace: ailee::ev
 * License: MIT License. Copyright (c) Don Michael Feeney Jr.
 * ============================================================================
 */

#ifndef AILEE_TRUST_LAYER_TRUST_GATE_HPP
#define AILEE_TRUST_LAYER_TRUST_GATE_HPP

#include "ailee_compartment.hpp"
#include <cstdio>
#include <algorithm>
#include <cmath>

namespace ailee {
namespace ev {

struct TrustGateConfig {
    double accept_trust_threshold = 0.85;  // Threshold for normal learning updates & Level 0
    double soft_ceiling_threshold = 0.70;  // Threshold for Level 1 Soft Ceiling
    double hard_ceiling_threshold = 0.50;  // Threshold for Level 2 Hard Ceiling
    // Below 0.50 triggers Level 3 Protective Mode
    uint32_t protective_recovery_cycles = 50; // Hysteresis requirement (50 cycles) to exit Level 3
};

struct GovernanceDecision {
    GovernanceLevel level = GovernanceLevel::LEVEL_0_NORMAL;
    double overall_trust_score = 1.0;
    double derate_factor = 1.0;
    bool learning_allowed = true;
    bool fallback_active = false;
    bool evidence_valid = false;
    std::size_t evidence_count = 0;
    FixedMessage reason;
};

class TrustGate {
public:
    explicit TrustGate(const TrustGateConfig& config = TrustGateConfig()) noexcept
        : config_(config), valid_config_(valid_score(config.accept_trust_threshold) &&
              valid_score(config.soft_ceiling_threshold) &&
              valid_score(config.hard_ceiling_threshold) &&
              config.hard_ceiling_threshold <= config.soft_ceiling_threshold &&
              config.soft_ceiling_threshold <= config.accept_trust_threshold) {}

    /**
     * Evaluate system governance decision given a set of compartment telemetry snapshots.
     */
    GovernanceDecision evaluate_telemetry(const CompartmentTelemetry* telemetries, std::size_t count) const noexcept {
        GovernanceDecision decision;
        if (!valid_config_ || count == 0 || telemetries == nullptr) {
            decision.level = GovernanceLevel::LEVEL_3_PROTECTIVE;
            decision.overall_trust_score = 0.0;
            decision.derate_factor = 0.25;
            decision.learning_allowed = false;
            decision.fallback_active = true;
            snprintf(decision.reason.data, sizeof(decision.reason.data), "%s",
                !valid_config_ ? "Invalid TrustGate configuration; fallback activated." :
                                 "No telemetry inputs available; fallback activated.");
            return decision;
        }

        double min_trust = 1.0;
        bool any_anomaly = false;
        int highest_req_level = static_cast<int>(GovernanceLevel::LEVEL_0_NORMAL);

        for (std::size_t i = 0; i < count; ++i) {
            const auto& t = telemetries[i];
            const int level = static_cast<int>(t.recommended_level);
            if (!std::isfinite(t.timestamp) || !std::isfinite(t.health_score) ||
                !valid_score(t.trust_score) || !std::isfinite(t.stress_metric) ||
                t.health_score < 0.0 || t.health_score > 1.0 ||
                level < static_cast<int>(GovernanceLevel::LEVEL_0_NORMAL) ||
                level > static_cast<int>(GovernanceLevel::LEVEL_3_PROTECTIVE)) {
                decision.level = GovernanceLevel::LEVEL_3_PROTECTIVE;
                decision.overall_trust_score = 0.0;
                decision.derate_factor = 0.25;
                decision.learning_allowed = false;
                decision.fallback_active = true;
                decision.evidence_count = i + 1;
                snprintf(decision.reason.data, sizeof(decision.reason.data),
                    "Invalid trust evidence at index %zu; fallback activated.", i);
                return decision;
            }
            if (t.trust_score < min_trust) {
                min_trust = t.trust_score;
            }
            if (t.anomaly_detected) {
                any_anomaly = true;
            }
            int req_lvl = static_cast<int>(t.recommended_level);
            if (req_lvl > highest_req_level) {
                highest_req_level = req_lvl;
            }
        }

        decision.overall_trust_score = min_trust;
        decision.evidence_valid = true;
        decision.evidence_count = count;

        // Determine governance level
        if (any_anomaly || min_trust < config_.hard_ceiling_threshold || highest_req_level == static_cast<int>(GovernanceLevel::LEVEL_3_PROTECTIVE)) {
            decision.level = GovernanceLevel::LEVEL_3_PROTECTIVE;
            decision.derate_factor = 0.25;
            decision.learning_allowed = false;
            decision.fallback_active = true;
            snprintf(decision.reason.data, sizeof(decision.reason.data), "PROTECTIVE MODE: Anomaly or low trust score (%.2f)", min_trust);
        } else if (min_trust < config_.soft_ceiling_threshold || highest_req_level == static_cast<int>(GovernanceLevel::LEVEL_2_HARD_CEILING)) {
            decision.level = GovernanceLevel::LEVEL_2_HARD_CEILING;
            decision.derate_factor = 0.65;
            decision.learning_allowed = false;
            decision.fallback_active = false;
            snprintf(decision.reason.data, sizeof(decision.reason.data), "Hard ceiling applied (65%% envelope) due to trust score (%.2f)", min_trust);
        } else if (min_trust < config_.accept_trust_threshold || highest_req_level == static_cast<int>(GovernanceLevel::LEVEL_1_SOFT_CEILING)) {
            decision.level = GovernanceLevel::LEVEL_1_SOFT_CEILING;
            decision.derate_factor = 0.90;
            decision.learning_allowed = false;
            decision.fallback_active = false;
            snprintf(decision.reason.data, sizeof(decision.reason.data), "Soft ceiling applied (90%% envelope) due to trust score (%.2f)", min_trust);
        } else {
            decision.level = GovernanceLevel::LEVEL_0_NORMAL;
            decision.derate_factor = 1.00;
            decision.learning_allowed = true;
            decision.fallback_active = false;
            snprintf(decision.reason.data, sizeof(decision.reason.data), "Normal operation; full performance envelope authorized.");
        }

        return decision;
    }

    /**
     * Checks whether system state satisfies deterministic entry/exit criteria for exiting Level 3 Protective Mode.
     * Requires sustained trust_score >= the configured accept threshold, zero
     * active anomalies, and consecutive healthy cycles >= the configured count.
     */
    bool verify_protective_mode_exit(double current_trust_score,
                                     bool active_anomaly,
                                     uint32_t consecutive_healthy_cycles) const noexcept {
        if (!valid_config_ || !valid_score(current_trust_score) || active_anomaly) return false;
        if (current_trust_score < config_.accept_trust_threshold) return false;
        if (consecutive_healthy_cycles < config_.protective_recovery_cycles) return false;
        return true;
    }

    /**
     * Checks if a proposed parameter update passes trust gating and envelope limits.
     * Enforces the configured accept threshold (default 0.85), without clamping
     * invalid scores into an authorized domain.
     */
    bool verify_parameter_update(double current_trust_score,
                                 const ParameterEnvelope& envelope,
                                 double proposed_value) const noexcept {
        if (!valid_config_ || !valid_score(current_trust_score) ||
            !std::isfinite(proposed_value) || !is_valid_parameter_state(envelope) ||
            current_trust_score < config_.accept_trust_threshold) {
            return false;
        }
        if (proposed_value < envelope.min_value || proposed_value > envelope.max_value) {
            return false;
        }
        double step = std::abs(proposed_value - envelope.current_value);
        if (!std::isfinite(step) || step > envelope.max_step_delta + 1e-9) {
            return false;
        }
        return true;
    }

    /** Validate the current numeric envelope independently of learning policy. */
    static bool is_valid_parameter_state(const ParameterEnvelope& envelope) noexcept {
        return std::isfinite(envelope.min_value) && std::isfinite(envelope.max_value) &&
            std::isfinite(envelope.current_value) && std::isfinite(envelope.max_step_delta) &&
            std::isfinite(envelope.default_value) &&
            envelope.min_value <= envelope.max_value && envelope.max_step_delta >= 0.0 &&
            envelope.current_value >= envelope.min_value &&
            envelope.current_value <= envelope.max_value;
    }

private:
    static bool valid_score(double score) noexcept {
        return std::isfinite(score) && score >= 0.0 && score <= 1.0;
    }

    TrustGateConfig config_;
    bool valid_config_;
};

} // namespace ev
} // namespace ailee

#endif // AILEE_TRUST_LAYER_TRUST_GATE_HPP
