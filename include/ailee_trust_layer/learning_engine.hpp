/*
 * ============================================================================
 * AILEE TRUST LAYER — BOUNDED CONTINUOUS LEARNING ENGINE
 * ============================================================================
 *
 * Implements continuous learning mechanisms, envelope verification, parameter
 * snapshot creation, audit logging, and snapshot rollback handles.
 *
 * All parameter adaptations are gated by the configured TrustGate policy.
 * Updates are snapshotted to allow complete auditability and instant rollback.
 *
 * Namespace: ailee::ev
 * License: MIT License. Copyright (c) Don Michael Feeney Jr.
 * ============================================================================
 */

#ifndef AILEE_TRUST_LAYER_LEARNING_ENGINE_HPP
#define AILEE_TRUST_LAYER_LEARNING_ENGINE_HPP

#include "ailee_compartment.hpp"
#include "ailee_trust_gate.hpp"
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <locale>
#include <type_traits>

namespace ailee {
namespace ev {

struct ParameterSnapshot {
    uint64_t snapshot_id = 0;
    uint64_t rollback_handle = 0; // Alias for snapshot_id
    std::string timestamp;
    std::string compartment_id;   // Subsystem identifier (e.g. BMSCompartment)
    std::string compartment_name; // Backward-compatibility alias
    std::string param_name;
    double previous_params = 0.0; // Key parameter state prior to update
    double previous_value = 0.0;  // Backward-compatibility alias
    double proposed_params = 0.0; // Proposed parameter value
    double proposed_value = 0.0;  // Backward-compatibility alias
    double applied_params = 0.0;  // Resulting applied parameter value
    double applied_value = 0.0;   // Backward-compatibility alias
    double trust_score = 0.0;
    bool approved = false;
    std::string reason;
    uint64_t audit_id = 0;        // Every attempt, including denied operations
    uint64_t target_identity = 0; // Process-local identity, not a persisted handle
    uint64_t rollback_source = 0;
    std::string operation = "proposal";

    void sync_aliases() {
        rollback_handle = snapshot_id;
        compartment_id = compartment_name;
        previous_params = previous_value;
        proposed_params = proposed_value;
        applied_params = applied_value;
    }
};

class LearningEngine {
public:
    explicit LearningEngine(const std::string& audit_log_path = "logs/ailee_learning_audit.json",
                           const std::string& automotive_log_path = "logs/ailee_automotive_audit.log")
        : audit_log_path_(audit_log_path), automotive_log_path_(automotive_log_path),
          next_audit_id_(1) {}

    /**
     * Submit a proposed parameter update for a compartment.
     * Evaluates trust gate and parameter envelope with thread-safe audit logging.
     */
    bool propose_parameter_update(const std::string& compartment_name,
                                 ParameterEnvelope& envelope,
                                 double proposed_value,
                                 double current_trust_score,
                                 const TrustGate& trust_gate,
                                 ParameterSnapshot* out_snapshot = nullptr) {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        bool approved = trust_gate.verify_parameter_update(current_trust_score, envelope, proposed_value) &&
            target_matches_binding(compartment_name, envelope);

        ParameterSnapshot snap;
        snap.snapshot_id = approved ? allocate_snapshot_id() : 0;
        approved = approved && snap.snapshot_id != 0;
        snap.audit_id = next_audit_id_;
        snap.target_identity = envelope.target_identity.value();
        snap.timestamp = get_current_iso_timestamp();
        snap.compartment_name = compartment_name;
        snap.param_name = envelope.param_name != nullptr ? envelope.param_name : "";
        snap.previous_value = envelope.current_value;
        snap.proposed_value = proposed_value;
        snap.trust_score = current_trust_score;
        snap.approved = approved;

        if (approved) {
            snap.applied_value = proposed_value;
            snap.reason = "Approved: Configured trust policy, target identity and envelope checks passed.";
        } else {
            snap.applied_value = envelope.current_value;
            snap.reason = "Rejected: Invalid trust policy/evidence, target identity or parameter envelope.";
        }

        snap.sync_aliases();
        ParameterSnapshot output;
        if (out_snapshot != nullptr) output = snap; // All allocation precedes commit.
        record_attempt(snap, envelope);

        if (out_snapshot != nullptr) {
            static_assert(std::is_nothrow_move_assignable<ParameterSnapshot>::value,
                          "snapshot delivery after commit must not throw");
            *out_snapshot = std::move(output);
        }

        return approved;
    }

    /**
     * Restore a previously authorized baseline for the exact envelope identity.
     * The legacy interface has no live trust/compartment argument: this consumes
     * the historical grant, not a fresh learning authorization. Restoration must
     * fit the current envelope, but is exempt from the learning step-rate limit.
     * Handles are local to this engine instance and envelope lifetime, not durable
     * or distributed identifiers. Copies/replacements require new proposals.
     */
    bool rollback(uint64_t rollback_handle, ParameterEnvelope& envelope) {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        const ParameterSnapshot* source = nullptr;
        for (const auto& candidate : snapshots_) {
            if (candidate.snapshot_id == rollback_handle) { source = &candidate; break; }
        }
        const auto grant = authorizations_.find(rollback_handle);
        bool approved = source != nullptr && grant != authorizations_.end() &&
            grant->second.target == &envelope &&
            grant->second.identity == envelope.target_identity.value() &&
            target_matches_binding(source->compartment_name, envelope) &&
            TrustGate::is_valid_parameter_state(envelope) &&
            std::isfinite(source->previous_value) &&
            source->previous_value >= envelope.min_value &&
            source->previous_value <= envelope.max_value;

        ParameterSnapshot snap;
        snap.snapshot_id = approved ? allocate_snapshot_id() : 0;
        approved = approved && snap.snapshot_id != 0;
        snap.audit_id = next_audit_id_;
        snap.target_identity = envelope.target_identity.value();
        snap.rollback_source = rollback_handle;
        snap.operation = "rollback";
        snap.timestamp = get_current_iso_timestamp();
        snap.compartment_name = source != nullptr ? source->compartment_name : "";
        snap.param_name = envelope.param_name != nullptr ? envelope.param_name : "";
        snap.previous_value = envelope.current_value;
        snap.proposed_value = source != nullptr ? source->previous_value : envelope.current_value;
        snap.applied_value = approved ? snap.proposed_value : envelope.current_value;
        snap.trust_score = source != nullptr ? source->trust_score : 0.0;
        snap.approved = approved;
        snap.reason = approved ? "ROLLBACK EXECUTION: Historical target grant and current envelope validated." :
                                 "Rejected rollback: Unknown handle, different/replaced target or invalid current envelope.";
        snap.sync_aliases();
        record_attempt(snap, envelope);
        return approved;
    }

    /**
     * Get committed transition history. Denials are available separately below.
     */
    std::vector<ParameterSnapshot> get_snapshots() const {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        return snapshots_;
    }

    std::vector<ParameterSnapshot> get_audit_records() const {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        return audit_records_;
    }

private:
    static uint64_t allocate_snapshot_id() noexcept {
        uint64_t candidate = next_snapshot_id_.load(std::memory_order_relaxed);
        while (candidate != std::numeric_limits<uint64_t>::max()) {
            if (next_snapshot_id_.compare_exchange_weak(candidate, candidate + 1,
                                                       std::memory_order_relaxed)) return candidate;
        }
        return 0;
    }

    struct TargetAuthorization {
        const ParameterEnvelope* target;
        uint64_t identity;
        std::string compartment;
        std::string parameter;
    };

    bool target_matches_binding(const std::string& compartment,
                                const ParameterEnvelope& envelope) const {
        if (envelope.target_identity.value() == 0 || compartment.empty() ||
            envelope.param_name == nullptr || envelope.param_name[0] == '\0') return false;
        for (const auto& entry : authorizations_) {
            const auto& grant = entry.second;
            if (grant.target == &envelope && grant.identity == envelope.target_identity.value() &&
                (grant.compartment != compartment || grant.parameter != envelope.param_name)) return false;
        }
        return true;
    }

    void record_attempt(const ParameterSnapshot& snap, ParameterEnvelope& envelope) {
        // Serialization allocates: prepare it before accepted state/history changes.
        const std::string json = serialize_audit(snap);
        const std::string automotive = "[AILEE_LEARNING] " + json;
        // Prepare every allocating history change before the parameter commit.
        if (snap.approved) {
            auto committed = snapshots_;
            auto authorizations = authorizations_;
            auto evidence = audit_records_;
            committed.push_back(snap);
            authorizations.emplace(snap.snapshot_id,
                TargetAuthorization{&envelope, envelope.target_identity.value(),
                                    snap.compartment_name, snap.param_name});
            evidence.push_back(snap);
            snapshots_.swap(committed);
            authorizations_.swap(authorizations);
            audit_records_.swap(evidence);
            envelope.current_value = snap.applied_value;
        } else {
            audit_records_.push_back(snap);
        }
        ++next_audit_id_;
        write_audit_log(json, automotive);
    }

    std::string get_current_iso_timestamp() const {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        char buf[64] = {};
        std::tm utc{};
#ifdef _WIN32
        if (gmtime_s(&utc, &time_t_now) != 0) return "unavailable";
#else
        if (gmtime_r(&time_t_now, &utc) == nullptr) return "unavailable";
#endif
        if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) return "unavailable";
        return std::string(buf);
    }

    static std::size_t utf8_sequence_length(const std::string& input, std::size_t index) noexcept {
        const auto byte = static_cast<unsigned char>(input[index]);
        if (byte < 0x80) return 1;
        const std::size_t length = byte >= 0xc2 && byte <= 0xdf ? 2 :
            byte >= 0xe0 && byte <= 0xef ? 3 : byte >= 0xf0 && byte <= 0xf4 ? 4 : 0;
        if (length == 0 || input.size() - index < length) return 0;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(input[index + offset]);
            if (next < 0x80 || next > 0xbf) return 0;
        }
        const auto second = static_cast<unsigned char>(input[index + 1]);
        if ((byte == 0xe0 && second < 0xa0) || (byte == 0xed && second >= 0xa0) ||
            (byte == 0xf0 && second < 0x90) || (byte == 0xf4 && second >= 0x90)) return 0;
        return length;
    }

    static bool contains_invalid_utf8(const std::string& input) noexcept {
        for (std::size_t index = 0; index < input.size();) {
            const std::size_t length = utf8_sequence_length(input, index);
            if (length == 0) return true;
            index += length;
        }
        return false;
    }

    static std::string byte_hex(const std::string& input) {
        constexpr char hex[] = "0123456789abcdef";
        std::string output;
        for (const unsigned char byte : input) {
            output.push_back(hex[byte >> 4]);
            output.push_back(hex[byte & 0x0f]);
        }
        return output;
    }

    static std::string json_string(const std::string& input) {
        std::string output;
        output.reserve(input.size() + 2);
        output.push_back('"');
        constexpr char hex[] = "0123456789abcdef";
        for (std::size_t index = 0; index < input.size();) {
            const auto byte = static_cast<unsigned char>(input[index]);
            const std::size_t length = utf8_sequence_length(input, index);
            if (byte >= 0x80 && length != 0) {
                output.append(input, index, length);
                index += length;
                continue;
            }
            if (byte == '"' || byte == '\\') {
                output.push_back('\\');
                output.push_back(static_cast<char>(byte));
            } else if (byte < 0x20 || length == 0) {
                output += "\\u00";
                output.push_back(hex[byte >> 4]);
                output.push_back(hex[byte & 0x0f]);
            } else {
                output.push_back(static_cast<char>(byte));
            }
            ++index;
        }
        output.push_back('"');
        return output;
    }

    static const char* nonfinite_evidence(double value) noexcept {
        if (std::isnan(value)) return "NaN";
        return value > 0.0 ? "+Infinity" : "-Infinity";
    }

    static std::string serialize_audit(const ParameterSnapshot& snap) {
        std::ostringstream json;
        json.exceptions(std::ios::badbit | std::ios::failbit);
        json.imbue(std::locale::classic());
        json << std::setprecision(std::numeric_limits<double>::max_digits10);
        json << "{\"snapshot_id\":" << snap.snapshot_id
             << ",\"audit_id\":" << snap.audit_id
             << ",\"operation\":" << json_string(snap.operation)
             << ",\"target_identity\":" << snap.target_identity
             << ",\"rollback_source\":" << snap.rollback_source
             << ",\"timestamp\":" << json_string(snap.timestamp)
             << ",\"compartment\":" << json_string(snap.compartment_name)
             << ",\"param\":" << json_string(snap.param_name);
        const std::pair<const char*, double> numbers[] = {
            {"previous", snap.previous_value}, {"proposed", snap.proposed_value},
            {"applied", snap.applied_value}, {"trust_score", snap.trust_score}};
        for (const auto& field : numbers) {
            json << "," << json_string(field.first) << ':';
            if (std::isfinite(field.second)) json << field.second;
            else json << "null";
        }
        json << ",\"approved\":" << (snap.approved ? "true" : "false")
             << ",\"reason\":" << json_string(snap.reason)
             << ",\"nonfinite_evidence\":{";
        bool first = true;
        for (const auto& field : numbers) {
            if (!std::isfinite(field.second)) {
                if (!first) json << ',';
                first = false;
                json << json_string(field.first) << ':' << json_string(nonfinite_evidence(field.second));
            }
        }
        json << "},\"invalid_utf8_evidence\":{";
        const std::pair<const char*, const std::string*> strings[] = {
            {"compartment", &snap.compartment_name}, {"param", &snap.param_name},
            {"reason", &snap.reason}, {"timestamp", &snap.timestamp}, {"operation", &snap.operation}};
        first = true;
        for (const auto& field : strings) {
            if (contains_invalid_utf8(*field.second)) {
                if (!first) json << ',';
                first = false;
                json << json_string(field.first) << ':' << json_string(byte_hex(*field.second));
            }
        }
        json << "}}\n";
        return json.str();
    }

    void write_audit_log(const std::string& json, const std::string& automotive) noexcept {
        // Existing best-effort file semantics: no durability/authentication claim.
        FILE* f_json = std::fopen(audit_log_path_.c_str(), "a");
        if (f_json != nullptr) {
            std::fwrite(json.data(), 1, json.size(), f_json);
            std::fclose(f_json);
        }

        // Write to automotive audit log
        FILE* f_auto = std::fopen(automotive_log_path_.c_str(), "a");
        if (f_auto != nullptr) {
            std::fwrite(automotive.data(), 1, automotive.size(), f_auto);
            std::fclose(f_auto);
        }
    }

    std::string audit_log_path_;
    std::string automotive_log_path_;
    inline static std::atomic<uint64_t> next_snapshot_id_{1};
    uint64_t next_audit_id_;
    std::vector<ParameterSnapshot> snapshots_;
    std::vector<ParameterSnapshot> audit_records_;
    std::map<uint64_t, TargetAuthorization> authorizations_;
    mutable std::mutex engine_mutex_;
};

} // namespace ev
} // namespace ailee

#endif // AILEE_TRUST_LAYER_LEARNING_ENGINE_HPP
