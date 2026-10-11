/*
 * ============================================================================
 * AILEE TRUST LAYER — EV COMPARTMENT INTERFACE
 * ============================================================================
 *
 * Defines the abstract interface and core data types for modular EV subsystems
 * operating under the AILEE Trust Layer.
 *
 * All hot-path methods are designated noexcept and avoid dynamic heap allocation
 * to guarantee real-time execution in automotive embedded environments.
 *
 * Namespace: ailee::ev
 * License: MIT License. Copyright (c) Don Michael Feeney Jr.
 * ============================================================================
 */

#ifndef AILEE_TRUST_LAYER_COMPARTMENT_HPP
#define AILEE_TRUST_LAYER_COMPARTMENT_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <limits>

namespace ailee {
namespace ev {

/**
 * Discrete 4-tier AILEE Governance Levels for EV control.
 */
enum class GovernanceLevel : int32_t {
    LEVEL_0_NORMAL = 0,       // Full performance envelope (100% cap)
    LEVEL_1_SOFT_CEILING = 1, // Soft ceiling applied (90% cap)
    LEVEL_2_HARD_CEILING = 2, // Hard ceiling applied (65% cap)
    LEVEL_3_PROTECTIVE = 3    // Protective mode / conservative fallback (25% cap)
};

/**
 * Fixed-size status message string buffer (zero dynamic allocation).
 */
struct FixedMessage {
    char data[128] = {0};
};

/**
 * Standard telemetry snapshot returned by compartment evaluation.
 */
struct CompartmentTelemetry {
    double timestamp = 0.0;
    double health_score = 1.0;     // 0.0 .. 1.0
    double trust_score = 1.0;      // 0.0 .. 1.0
    double stress_metric = 0.0;    // Accumulated stress indicator
    bool anomaly_detected = false;
    GovernanceLevel recommended_level = GovernanceLevel::LEVEL_0_NORMAL;
    FixedMessage status_message;
};

/**
 * Process-local authorization identity for one envelope target. Construction and
 * replacement assignment obtain fresh identities, including copy/move operations.
 * Moving also retires the source identity. A destroyed, moved or replaced
 * envelope therefore cannot inherit old rollback grants.
 * No allocation occurs. Exhaustion returns zero and learning fails closed.
 */
class ParameterTargetIdentity {
public:
    ParameterTargetIdentity() noexcept : value_(allocate()) {}
    ParameterTargetIdentity(const ParameterTargetIdentity&) noexcept : value_(allocate()) {}
    ParameterTargetIdentity(ParameterTargetIdentity&& source) noexcept : value_(allocate()) {
        source.value_ = allocate();
    }
    ParameterTargetIdentity& operator=(const ParameterTargetIdentity&) noexcept {
        value_ = allocate();
        return *this;
    }
    ParameterTargetIdentity& operator=(ParameterTargetIdentity&& source) noexcept {
        value_ = allocate();
        if (this != &source) source.value_ = allocate();
        return *this;
    }
    uint64_t value() const noexcept { return value_; }

private:
    static uint64_t allocate() noexcept {
        uint64_t candidate = next_.load(std::memory_order_relaxed);
        while (candidate != std::numeric_limits<uint64_t>::max()) {
            if (next_.compare_exchange_weak(candidate, candidate + 1,
                                           std::memory_order_relaxed)) return candidate;
        }
        return 0;
    }
    inline static std::atomic<uint64_t> next_{1};
    uint64_t value_;
};

/**
 * Parameter Envelope definition for bounding continuous learning updates.
 * Keep one live envelope per logical target and serialize caller access. Its
 * param_name must remain a valid C string during calls. Whole-envelope copy/move
 * construction or assignment invalidates prior rollback identity; changing
 * ordinary scalar limits does not. The additive identity changes struct layout,
 * but preserves default and six-field aggregate initialization in C++17.
 */
struct ParameterEnvelope {
    const char* param_name = "";
    double min_value = 0.0;
    double max_value = 1.0;
    double max_step_delta = 0.05; // Maximum allowed change per update cycle
    double current_value = 0.5;
    double default_value = 0.5;
    ParameterTargetIdentity target_identity{};
};

/**
 * Abstract interface for all AILEE EV compartments.
 * Subsystems (BMS, Thermal, Charging, Anomaly Detection) implement this interface.
 */
class ICompartment {
public:
    virtual ~ICompartment() = default;

    /**
     * Unique human-readable compartment identifier.
     */
    virtual const char* get_name() const noexcept = 0;

    /**
     * Initialize compartment resources.
     */
    virtual bool initialize() noexcept = 0;

    /**
     * Evaluates compartment state for time step dt (seconds).
     * Must be real-time safe (zero heap allocation).
     */
    virtual CompartmentTelemetry evaluate(double dt) noexcept = 0;

    /**
     * Get the current recommended governance level based on internal safety checks.
     */
    virtual GovernanceLevel get_recommended_governance_level() const noexcept = 0;

    /**
     * Reset compartment to baseline safe state.
     */
    virtual void reset_to_baseline() noexcept = 0;
};

} // namespace ev
} // namespace ailee

#endif // AILEE_TRUST_LAYER_COMPARTMENT_HPP
