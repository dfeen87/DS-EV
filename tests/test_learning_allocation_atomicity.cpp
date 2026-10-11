#include "ailee_trust_layer/learning_engine.hpp"
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
bool monitoring = false;
bool triggered = false;
std::size_t failure_index = 0;
std::size_t allocations = 0;
int checks = 0;
int failures = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void* allocate(std::size_t size) {
    if (monitoring && ++allocations == failure_index) {
        monitoring = false;
        triggered = true;
        throw std::bad_alloc();
    }
    if (void* result = std::malloc(size == 0 ? 1 : size)) return result;
    throw std::bad_alloc();
}
} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

int main() {
    using namespace ailee::ev;
    for (const bool rollback : {false, true}) {
        bool reached_success = false;
        for (std::size_t nth = 1; nth <= 250; ++nth) {
            LearningEngine engine("", "");
            ParameterEnvelope target{"bounded_parameter_with_long_name", 0, 1, .1, .5, .5};
            ParameterSnapshot first;
            if (rollback)
                check(engine.propose_parameter_update("Compartment_with_long_name", target, .54,
                          .95, TrustGate(), &first), "prepare valid rollback grant");
            const double before = target.current_value;
            const auto prior_history = engine.get_snapshots();
            const auto prior_evidence = engine.get_audit_records();
            ParameterSnapshot output;
            output.reason = "unchanged_output_snapshot";
            failure_index = nth;
            allocations = 0;
            triggered = false;
            monitoring = true;
            bool threw = false;
            bool accepted = false;
            try {
                accepted = rollback ? engine.rollback(first.rollback_handle, target) :
                    engine.propose_parameter_update("Compartment_with_long_name", target, .54,
                                                   .95, TrustGate(), &output);
            } catch (const std::exception&) {
                threw = true;
            }
            monitoring = false;
            if (!triggered) {
                check(accepted && !threw, "operation without injected failure must succeed");
                reached_success = true;
                break;
            }
            check(threw && !accepted, "injected allocation failure must not be swallowed into acceptance");
            check(target.current_value == before, "allocation failure must preserve target");
            check(engine.get_snapshots().size() == prior_history.size(),
                  "allocation failure must preserve committed history");
            check(engine.get_audit_records().size() == prior_evidence.size(),
                  "allocation failure must preserve in-memory audit history");
            if (!rollback)
                check(output.reason == "unchanged_output_snapshot" && output.snapshot_id == 0,
                      "allocation failure must preserve caller output snapshot");
        }
        check(reached_success, "probe must inject every allocation then reach ordinary success");
    }
    std::cout << checks << " allocation checks; " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
