#include "ailee_trust_layer/learning_engine.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <utility>

using namespace ailee::ev;

namespace {
int checks = 0;
int failures = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void denied_config(const TrustGateConfig& config) {
    const TrustGate gate(config);
    CompartmentTelemetry telemetry;
    const auto decision = gate.evaluate_telemetry(&telemetry, 1);
    check(decision.level == GovernanceLevel::LEVEL_3_PROTECTIVE,
          "invalid configuration must select protective governance");
    check(!decision.learning_allowed && decision.fallback_active &&
          !decision.evidence_valid && decision.overall_trust_score == 0.0,
          "invalid configuration must deny telemetry authorization");
    ParameterEnvelope envelope;
    check(!gate.verify_parameter_update(1.0, envelope, 0.51),
          "invalid configuration must deny learning");
    check(!gate.verify_protective_mode_exit(1.0, false, 100),
          "invalid configuration must deny protective-mode exit");
}

void test_trust() {
    static_assert(noexcept(TrustGate()), "TrustGate construction must remain noexcept");
    static_assert(noexcept(std::declval<const TrustGate&>().evaluate_telemetry(nullptr, 0)),
                  "telemetry authorization must remain noexcept");
    static_assert(noexcept(std::declval<const TrustGate&>().verify_parameter_update(
                      1.0, std::declval<const ParameterEnvelope&>(), 0.5)),
                  "parameter authorization must remain noexcept");
    static_assert(noexcept(std::declval<const TrustGate&>().verify_protective_mode_exit(
                      1.0, false, 100)), "protective exit must remain noexcept");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const double value : {nan, inf, -inf, -0.01, 1.01}) {
        for (int field = 0; field < 3; ++field) {
            TrustGateConfig config;
            if (field == 0) config.accept_trust_threshold = value;
            if (field == 1) config.soft_ceiling_threshold = value;
            if (field == 2) config.hard_ceiling_threshold = value;
            denied_config(config);
        }
    }
    TrustGateConfig reversed;
    reversed.soft_ceiling_threshold = 0.9;
    denied_config(reversed);
    reversed = TrustGateConfig();
    reversed.hard_ceiling_threshold = 0.8;
    denied_config(reversed);

    const TrustGate gate;
    ParameterEnvelope envelope;
    for (const double score : {nan, inf, -inf, -0.01, 1.01}) {
        CompartmentTelemetry telemetry;
        telemetry.trust_score = score;
        check(!gate.evaluate_telemetry(&telemetry, 1).learning_allowed,
              "out-of-domain score must deny telemetry learning");
        check(!gate.verify_parameter_update(score, envelope, 0.51),
              "out-of-domain score must deny direct learning");
        check(!gate.verify_protective_mode_exit(score, false, 100),
              "out-of-domain score must deny protective-mode exit");
    }

    for (const double current : {nan, inf, -inf, -0.01, 1.01}) {
        envelope.current_value = current;
        check(!gate.verify_parameter_update(1.0, envelope, 0.5),
              "invalid prior parameter state must deny learning");
    }
    for (int field = 0; field < 4; ++field) {
        for (const double invalid : {nan, inf, -inf}) {
            envelope = ParameterEnvelope();
            if (field == 0) envelope.min_value = invalid;
            if (field == 1) envelope.max_value = invalid;
            if (field == 2) envelope.max_step_delta = invalid;
            if (field == 3) envelope.default_value = invalid;
            check(!gate.verify_parameter_update(1.0, envelope, 0.5),
                  "every nonfinite envelope numeric field must deny learning");
        }
    }
    envelope = ParameterEnvelope();
    envelope.min_value = 0.8;
    envelope.max_value = 0.2;
    check(!gate.verify_parameter_update(1.0, envelope, 0.5), "reversed parameter bounds must deny");
    envelope = ParameterEnvelope();
    envelope.max_step_delta = -0.1;
    check(!gate.verify_parameter_update(1.0, envelope, 0.5), "negative step allowance must deny");
    envelope = ParameterEnvelope();
    TrustGateConfig policy;
    policy.accept_trust_threshold = 0.6;
    policy.soft_ceiling_threshold = 0.4;
    policy.hard_ceiling_threshold = 0.2;
    policy.protective_recovery_cycles = 3;
    const TrustGate configured(policy);
    CompartmentTelemetry telemetry;
    for (const auto& threshold : {std::pair<double, GovernanceLevel>{0.2, GovernanceLevel::LEVEL_2_HARD_CEILING},
                                 {0.4, GovernanceLevel::LEVEL_1_SOFT_CEILING},
                                 {0.6, GovernanceLevel::LEVEL_0_NORMAL}}) {
        telemetry.trust_score = threshold.first;
        check(configured.evaluate_telemetry(&telemetry, 1).level == threshold.second,
              "configured exact threshold equality must retain existing semantics");
        telemetry.trust_score = std::nextafter(threshold.first, 0.0);
        check(static_cast<int>(configured.evaluate_telemetry(&telemetry, 1).level) ==
              static_cast<int>(threshold.second) + 1,
              "just below configured threshold must select next governance level");
    }
    check(configured.verify_parameter_update(0.6, envelope, 0.55),
          "valid configured learning threshold must be honored");
    check(!configured.verify_parameter_update(std::nextafter(0.6, 0.0), envelope, 0.55),
          "below configured learning threshold must deny");
    check(configured.verify_protective_mode_exit(0.6, false, 3),
          "configured equality and recovery cycle count must be honored");
    check(!configured.verify_protective_mode_exit(0.6, false, 2),
          "configured hysteresis must be retained");
    check(!configured.verify_protective_mode_exit(0.6, true, 3),
          "active anomaly must deny exit");
    policy.accept_trust_threshold = 0.0;
    policy.soft_ceiling_threshold = 0.0;
    policy.hard_ceiling_threshold = 0.0;
    policy.protective_recovery_cycles = 0;
    const TrustGate zero(policy);
    check(zero.verify_parameter_update(0.0, envelope, 0.55) &&
          zero.verify_protective_mode_exit(0.0, false, 0),
          "ordered equal thresholds and zero hysteresis remain valid policy");
    policy.accept_trust_threshold = policy.soft_ceiling_threshold = policy.hard_ceiling_threshold = 1.0;
    const TrustGate one(policy);
    check(one.verify_parameter_update(1.0, envelope, 0.55),
          "all-one equal policy permits exact score one");
    check(!one.verify_parameter_update(std::nextafter(1.0, 0.0), envelope, 0.55),
          "all-one policy preserves strict lower rejection");
    envelope.min_value = -std::numeric_limits<double>::max();
    envelope.max_value = std::numeric_limits<double>::max();
    envelope.current_value = -std::numeric_limits<double>::max();
    envelope.max_step_delta = std::numeric_limits<double>::max();
    check(!gate.verify_parameter_update(1.0, envelope, std::numeric_limits<double>::max()),
          "finite subtraction overflow must deny");
}

ParameterEnvelope learning_target(const char* name = "regen_torque_limit") {
    return ParameterEnvelope{name, 0.0, 1.0, 0.1, 0.5, 0.5};
}

ParameterSnapshot approve(LearningEngine& engine, ParameterEnvelope& target,
                          double value = 0.54, const char* compartment = "BMSCompartment") {
    ParameterSnapshot snapshot;
    check(engine.propose_parameter_update(compartment, target, value, 0.95, TrustGate(), &snapshot),
          "ordinary bounded learning must approve");
    check(snapshot.approved && snapshot.rollback_handle == snapshot.snapshot_id &&
          snapshot.previous_params == snapshot.previous_value &&
          snapshot.proposed_params == snapshot.proposed_value &&
          snapshot.applied_params == snapshot.applied_value &&
          snapshot.compartment_id == snapshot.compartment_name,
          "accepted snapshot aliases must agree");
    return snapshot;
}

void denied_rollback(LearningEngine& engine, uint64_t handle, ParameterEnvelope& target) {
    const double prior = target.current_value;
    const auto history = engine.get_snapshots();
    check(!engine.rollback(handle, target), "unauthorized rollback must deny");
    check(target.current_value == prior || (std::isnan(target.current_value) && std::isnan(prior)),
          "denied rollback must preserve target value");
    const auto after = engine.get_snapshots();
    check(after.size() == history.size(), "denied rollback must preserve committed history size");
    if (after.size() == history.size()) {
        for (std::size_t i = 0; i < history.size(); ++i) {
            check(after[i].snapshot_id == history[i].snapshot_id &&
                  after[i].rollback_handle == history[i].rollback_handle &&
                  after[i].previous_value == history[i].previous_value &&
                  after[i].applied_value == history[i].applied_value &&
                  after[i].reason == history[i].reason,
                  "denied rollback must preserve each committed snapshot");
        }
    }
}

void test_rollback() {
    static_assert(std::is_aggregate<ParameterEnvelope>::value, "legacy envelope aggregate construction must survive");
    static_assert(std::is_nothrow_default_constructible<ParameterEnvelope>::value &&
                  std::is_nothrow_copy_constructible<ParameterEnvelope>::value &&
                  std::is_nothrow_move_constructible<ParameterEnvelope>::value &&
                  std::is_nothrow_copy_assignable<ParameterEnvelope>::value &&
                  std::is_nothrow_move_assignable<ParameterEnvelope>::value,
                  "envelope identity operations must remain nonthrowing");
    LearningEngine engine("", "");
    auto target = learning_target();
    const auto first = approve(engine, target);
    LearningEngine foreign_engine("", "");
    const auto foreign_snapshot = approve(foreign_engine, target, 0.58);
    denied_rollback(foreign_engine, first.rollback_handle, target);
    denied_rollback(engine, foreign_snapshot.rollback_handle, target);
    auto charging = learning_target("charge_current_limit");
    denied_rollback(engine, first.rollback_handle, charging);
    auto same_name = learning_target();
    denied_rollback(engine, first.rollback_handle, same_name);
    auto copied = target;
    denied_rollback(engine, first.rollback_handle, copied);
    auto moved = std::move(copied);
    denied_rollback(engine, first.rollback_handle, moved);
    auto move_source = learning_target();
    const auto move_handle = approve(engine, move_source);
    auto move_destination = std::move(move_source);
    denied_rollback(engine, move_handle.rollback_handle, move_destination);
    denied_rollback(engine, move_handle.rollback_handle, move_source);
    auto assigned = learning_target();
    const auto assigned_handle = approve(engine, assigned);
    assigned = target;
    denied_rollback(engine, assigned_handle.rollback_handle, assigned);
    const auto renewed_handle = approve(engine, assigned, 0.58);
    assigned = std::move(target);
    denied_rollback(engine, renewed_handle.rollback_handle, assigned);

    alignas(ParameterEnvelope) unsigned char storage[sizeof(ParameterEnvelope)];
    auto* reused = new (storage) ParameterEnvelope(learning_target());
    const auto stale_handle = approve(engine, *reused);
    reused->~ParameterEnvelope();
    reused = new (storage) ParameterEnvelope(learning_target());
    denied_rollback(engine, stale_handle.rollback_handle, *reused);
    const auto replacement_handle = approve(engine, *reused);
    check(engine.rollback(replacement_handle.rollback_handle, *reused),
          "new lifetime at reused address must support its own valid rollback");
    reused->~ParameterEnvelope();

    auto bound = learning_target();
    const auto bound_handle = approve(engine, bound);
    const auto bound_history = engine.get_snapshots().size();
    ParameterSnapshot rejected;
    check(!engine.propose_parameter_update("ChargingCompartment", bound, 0.58, 0.95,
                                         TrustGate(), &rejected),
          "same lifetime cannot be reused for another compartment");
    check(bound.current_value == 0.54 && engine.get_snapshots().size() == bound_history,
          "compartment ambiguity must leave target and committed history unchanged");
    bound.param_name = "charge_current_limit";
    denied_rollback(engine, bound_handle.rollback_handle, bound);
    check(!engine.propose_parameter_update("BMSCompartment", bound, 0.58, 0.95, TrustGate()),
          "renamed live target must require a fresh envelope identity");
    bound.param_name = "regen_torque_limit";
    bound.min_value = 0.52;
    denied_rollback(engine, bound_handle.rollback_handle, bound);
    bound.min_value = 0.0;
    bound.max_value = 0.53;
    denied_rollback(engine, bound_handle.rollback_handle, bound);
    bound.max_value = 1.0;
    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity(), -0.01, 1.01}) {
        bound.current_value = invalid;
        denied_rollback(engine, bound_handle.rollback_handle, bound);
    }
    bound.current_value = 0.54;
    for (int field = 0; field < 4; ++field) {
        const double prior_min = bound.min_value;
        const double prior_max = bound.max_value;
        const double prior_step = bound.max_step_delta;
        const double prior_default = bound.default_value;
        if (field == 0) bound.min_value = std::numeric_limits<double>::quiet_NaN();
        if (field == 1) bound.max_value = std::numeric_limits<double>::infinity();
        if (field == 2) bound.max_step_delta = -1.0;
        if (field == 3) bound.default_value = std::numeric_limits<double>::quiet_NaN();
        denied_rollback(engine, bound_handle.rollback_handle, bound);
        bound.min_value = prior_min;
        bound.max_value = prior_max;
        bound.max_step_delta = prior_step;
        bound.default_value = prior_default;
    }
    bound.param_name = nullptr;
    denied_rollback(engine, bound_handle.rollback_handle, bound);
    bound.param_name = "regen_torque_limit";
    bound.max_step_delta = 0.0;
    check(engine.rollback(bound_handle.rollback_handle, bound) && bound.current_value == 0.5,
          "rollback within bounds is a restoration, exempt from learning step limit");
    denied_rollback(engine, 999999, bound);
    denied_rollback(engine, 0, bound);

    const auto count = engine.get_snapshots().size();
    check(!engine.propose_parameter_update("BMSCompartment", bound, 0.6, 0.2, TrustGate(), &rejected),
          "low-trust proposal must deny");
    check(engine.get_snapshots().size() == count,
          "rejected proposal evidence must not enter committed transition history");
    denied_rollback(engine, rejected.rollback_handle, bound);

    // Public six-value brace initialization remains source compatible.
    ParameterEnvelope legacy{"legacy", 50.0, 300.0, 20.0, 100.0, 0.5};
    const auto legacy_handle = approve(engine, legacy, 115.0);
    check(engine.rollback(legacy_handle.snapshot_id, legacy) && legacy.current_value == 100.0,
          "unused finite default outside bounds must retain legacy compatibility");

    alignas(LearningEngine) unsigned char engine_storage[sizeof(LearningEngine)];
    auto* reincarnated = new (engine_storage) LearningEngine("", "");
    auto durable_target = learning_target();
    const auto stale_engine_handle = approve(*reincarnated, durable_target);
    reincarnated->~LearningEngine();
    reincarnated = new (engine_storage) LearningEngine("", "");
    approve(*reincarnated, durable_target, 0.58);
    denied_rollback(*reincarnated, stale_engine_handle.rollback_handle, durable_target);
    reincarnated->~LearningEngine();
}

void test_audit() {
    LearningEngine engine("", "");
    auto target = learning_target();
    const auto first = approve(engine, target);
    approve(engine, target, 0.58);
    check(engine.rollback(first.rollback_handle, target), "older valid rollback handle must remain supported");
    const auto history = engine.get_snapshots();
    const auto& rollback = history.back();
    check(rollback.previous_value == 0.58 && rollback.previous_params == 0.58,
          "rollback audit previous value must be actual current target before restoration");
    check(rollback.proposed_value == 0.5 && rollback.applied_value == 0.5 &&
          rollback.proposed_params == 0.5 && rollback.applied_params == 0.5,
          "rollback restored values and aliases must agree");
    const auto committed = history.size();
    ParameterSnapshot rejected;
    check(!engine.propose_parameter_update("BMSCompartment", target,
                 std::numeric_limits<double>::quiet_NaN(), 0.95, TrustGate(), &rejected),
          "nonfinite rejected proposal must deny");
    auto other = learning_target();
    denied_rollback(engine, first.rollback_handle, other);
    const auto evidence = engine.get_audit_records();
    check(engine.get_snapshots().size() == committed && evidence.size() == committed + 2,
          "rejected evidence must remain separate from committed transition history");
    check(rejected.snapshot_id == 0 && rejected.rollback_handle == 0 && rejected.audit_id > 0,
          "denied proposal must expose audit identity without a rollback grant");
    check(!evidence.back().approved && evidence.back().snapshot_id == 0 &&
          evidence.back().rollback_source == first.rollback_handle && evidence.back().operation == "rollback",
          "denied rollback must preserve operation evidence without a transition");
    for (const auto& accepted : engine.get_snapshots())
        check(accepted.approved, "committed history must contain only accepted transitions");
}

void emit_audit(const std::string& path) {
    LearningEngine engine(path, path + ".log");
    const std::string compartment = std::string("charger\"identity\\\n\r\t\b\f") + char(1) + char(0) + "tail";
    const char* name = "regen\"limit\\\n\r\t\b\f\x01";
    auto target = learning_target(name);
    const auto first = approve(engine, target, 0.54, compartment.c_str());
    // std::string API must also preserve embedded NUL compartment evidence.
    ParameterSnapshot rejected;
    check(!engine.propose_parameter_update(compartment, target, 0.58, 0.95, TrustGate(), &rejected),
          "different embedded-NUL compartment must not alias bound identity");
    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity()}) {
        check(!engine.propose_parameter_update(compartment, target, invalid, 0.95, TrustGate()),
              "nonfinite proposed value must deny and emit evidence");
        check(!engine.propose_parameter_update(compartment, target, 0.58, invalid, TrustGate()),
              "nonfinite trust value must deny and emit evidence");
    }
    engine.propose_parameter_update(compartment.substr(0, compartment.find(char(0))), target, 0.58, 0.95, TrustGate());
    engine.rollback(first.rollback_handle, target);
    auto other = learning_target(name);
    other.current_value = std::numeric_limits<double>::quiet_NaN();
    engine.rollback(first.rollback_handle, other);
    target.current_value = std::numeric_limits<double>::infinity();
    engine.propose_parameter_update(compartment, target, 0.58, 0.95, TrustGate());
}

void emit_byte_audit(const std::string& path) {
    LearningEngine engine(path, "");
    auto target = learning_target(u8"regén_充電");
    check(engine.propose_parameter_update(u8"充電é", target, 0.54, 0.95, TrustGate()),
          "valid UTF-8 metadata must preserve ordinary operation");
    const std::string invalid_compartment = std::string("raw") + char(0xff) + char(0xc0);
    check(!engine.propose_parameter_update(invalid_compartment, target, 0.58, 0.95, TrustGate()),
          "malformed UTF-8 metadata must remain parseable rejection evidence");
    const char invalid_parameter[] = {'r', char(0xc2), '.', '\0'};
    target.param_name = invalid_parameter;
    check(!engine.propose_parameter_update(u8"充電é", target, 0.58, 0.95, TrustGate()),
          "truncated UTF-8 parameter metadata must preserve rejection evidence");
    target.param_name = u8"regén_充電";
    for (const std::string& invalid : {std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"),
                                      std::string("\xe0\x80\x80"), std::string("\xf0\x80\x80\x80")}) {
        check(!engine.propose_parameter_update(invalid, target, 0.58, 0.95, TrustGate()),
              "invalid Unicode scalar encodings must emit recoverable byte evidence");
    }
}
} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "all";
    if (mode == "trust") test_trust();
    else if (mode == "rollback") test_rollback();
    else if (mode == "audit") test_audit();
    else if (mode == "emit-audit" && argc > 2) emit_audit(argv[2]);
    else if (mode == "emit-byte-audit" && argc > 2) emit_byte_audit(argv[2]);
    else if (mode == "all") { test_trust(); test_rollback(); test_audit(); }
    else return 2;
    std::cout << checks << " checks; " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
