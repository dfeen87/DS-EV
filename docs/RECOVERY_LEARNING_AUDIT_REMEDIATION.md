# Recovery, learning, rollback and audit integrity evidence

Baseline: latest merged main
`a1bc500de5c3ebd32fb72983bede50dc42ee04d6` (merged PR #50).
This phase implements PB-11, then PB-10, then PB-12, with independent PB-09 work
aligned to the accepted raw-energy contract from merged PR #50. It contributes
to the v8.0.2 development target; it does not create a release, deployment,
production parameter distribution or hardware operation. Historical reports,
including v8.0.0/v8.0.1 and the preceding numerical phase, remain intact.

## Regression-first evidence

Every targeted original failure was demonstrated before its correction.

| Finding | Before correction | Corrected contract |
| --- | --- | --- |
| PB-11 | The initial trust regression produced 64 failures in 103 checks. NaN thresholds authorized Level 0/learning, and trust above one authorized updates and protective exit. | All three thresholds must be finite, in `[0,1]`, with `hard <= soft <= accept`. Invalid policy denies telemetry, learning and exit. Every score entry point uses the same finite `[0,1]` domain. |
| PB-10 | After PB-11, rollback regressions produced 41 failures in 146 checks. Foreign/same-name targets, object replacement, tightened envelopes and rejected history were unsafe. The original regen handle could restore `100` into a charging envelope. | A grant binds the originating engine, exact envelope address, lifetime token, compartment and parameter name. Current state and restoration must fit the current envelope. Invalid prior state never becomes a restoration grant. |
| PB-12 | After PB-10, the older-handle test failed one of 20 checks: `0.5 -> 0.54 -> 0.58`, then restoring the first handle, reported previous `0.54`. Twelve hostile audit attempts emitted 84 physical lines, none parseable. | Rollback records actual current `0.58` before restoring `0.5`. Audit lines escape strings and represent nonfinite numbers without invalid JSON tokens. Rejected evidence is distinct from committed transitions. |
| PB-09 | Original failed-temperature reproduction left next current `-4.9415633561151306 A`, versus unchanged reference `0 A`. A `250 Nm`, `10000 rad/s`, `400 V`, `1 s` request credited `0.59027777777777779 kWh`, versus accepted battery energy `0.027776516668617707 kWh` at `-249.98865001755937 A`. Initial new recovery tests failed 164 of 328 checks. | Input/configuration checks and all fallible coordinator work precede BMS acceptance. Accepted energy follows actual raw battery-side voltage/current after limits and smoothing. Separate estimates never credit additional BMS energy. |

Focused regressions are `test_trust_learning_integrity`,
`test_recovery_integrity`, `test_remediation_reconciliation`, and the strict
Python audit parser regression. Full verification results are recorded below.

## Trust policy and rollback identity

TrustGate construction and authorization entry points retain `noexcept`.
Invalid configuration selects protective fallback with invalid evidence and no
learning permission; it cannot permit protective exit or parameter updates.
Valid nondefault policies remain authoritative. Equality is inclusive, equal
thresholds and zero recovery cycles remain valid configurations, and the existing
`1e-9` update-step tolerance remains. Finite subtraction overflow denies an update.
Current/minimum/maximum/step/default values must be finite; current and proposed
values must be within bounds. An unused finite default outside the envelope is
still permitted for source compatibility; it is never used as a rollback baseline.

Parameter names are labels, not identities. `ParameterEnvelope` now appends a
`ParameterTargetIdentity` member whose internal token cannot be set by callers.
Construction, copy/move construction and whole-envelope replacement assignment
obtain new process-local tokens. Move operations also retire the source identity;
copying does not retire the still-live source. Placement-new at a recycled address therefore
cannot inherit an earlier grant. Scalar limit changes retain identity and force
rollback to satisfy those current limits. One identity can be bound to only one
compartment/name within an engine. Same-name targets in different compartments
must use different envelope instances. Accepted snapshot handles are non-reused
within the process so foreign or recreated engines cannot alias old handles.
Exhausted identity/handle space denies authorization.

Callers must keep the envelope alive and its `param_name` a valid C string during
calls, dedicate that identity to one logical parameter, and serialize external
access. Replacing an unrelated parameter by directly writing the same public
scalar fields is indistinguishable from tuning that parameter; callers must use
whole-envelope replacement to establish a new identity. The engine does not
own the envelope and does not synchronize other writers. Tokens/handles are
local reference-API grants, not persisted, cryptographic or distributed identities.
Independently loaded modules/processes require an explicit registration/identity
API before sharing handles.

Public method signatures and existing default/six-field aggregate initialization
are preserved. The additive nontrivial identity member changes layout, trivial
copy properties and binary ABI: rebuild consumers, and do not serialize or copy
envelopes as raw bytes. Code assuming exactly six aggregate members must adapt.
An unchanged POD layout cannot distinguish address reuse safely. This is an
explicit representation incompatibility, not an inferred name-based identity.
A future durable registry with owner, generation, revocation and lifetime management
requires a separately reviewed API redesign.

The legacy rollback signature has neither live trust nor compartment arguments.
Rollback therefore consumes a previously accepted target grant and validates the
current envelope; it cannot perform a new live-trust authorization or prove owner
freshness. Restoration may exceed the current learning step delta, preserving
the existing distinction between restoration and a new learning adjustment.
It never exceeds current minimum/maximum bounds. It records the originating
grant's trust as historical evidence, rather than claiming a new score of one.
Production control-loop integration, live revocation and automatic baseline
restoration remain outside this reference API.

## Audit and committed history

`get_snapshots()` contains only accepted transitions. Rejected proposals and
rollbacks retain audit evidence separately through `get_audit_records()` and the
audit sink. Rejections have no restoration grant and use snapshot/rollback ID
zero. A denial does not change the envelope or any committed snapshot. Snapshot
aliases remain synchronized. A successful older-handle rollback captures the
actual pre-restoration value and creates an accurate new transition.

The audit is newline-delimited JSON, not one JSON array. Existing numeric/key
names remain. Additive fields are `audit_id`, `operation`, `target_identity`,
`rollback_source`, `nonfinite_evidence` and `invalid_utf8_evidence`. Nonfinite numeric values become JSON
`null`; `nonfinite_evidence` maps the affected field to `NaN`, `+Infinity` or
`-Infinity`, preserving rejection evidence. Finite doubles use enough digits to
round-trip and a locale-independent decimal point. String quoting includes
quotes, backslashes, embedded NUL and every ASCII control character. Valid UTF-8
text is preserved. Invalid UTF-8 bytes are escaped as `\u00XX` in the displayed
field, while `invalid_utf8_evidence` maps each affected string field to its entire
original byte sequence as lowercase hexadecimal, allowing lossless recovery.
The human
automotive sink uses the same escaped single-line JSON with an
`[AILEE_LEARNING]` prefix; consumers relying on its former text layout must adapt.
The serializer raises on stream `badbit`/`failbit` rather than silently accepting
truncated JSON after an allocation failure. All allocating serialization,
output-snapshot preparation and history preparation
precede accepted state mutation. File writes retain existing best-effort semantics:
disk errors may lose/truncate an external record and do not roll back a committed
parameter. In-memory evidence remains available; no authenticity, durability or
multi-process logging guarantee is established.

## Accepted energy and transaction boundary

Positive current means discharge; negative means charging. Positive voltage is
volts, current amperes and timestep seconds. `V * A` gives watts;
`W / 1000 * seconds / 3600` gives kWh. BMS acceptance retains the PB-13 raw-input
contract: coordinator-chosen raw current, not the normalized DS model current,
defines accepted battery energy. BMS safety flags remain observational evidence
and do not grant actuator authority or automatically veto a numerically valid
observation.

Coordinator `accepted_charging_energy_kwh_*` fields explicitly name this
all-source charging observation. Existing `recovered_energy_kwh_*` fields are
aliases of those totals for compatibility. They cover only accepted calls through
that coordinator, not earlier/direct BMS calls; resets affect session totals
without changing lifetime totals or BMS energy. Never sum these observations
with the BMS ledger: they describe the same accepted energy.

`UNATTRIBUTED_MEASURED` identifies measured-current mode. Negative measured
current, even with a nonzero torque limit, cannot establish a regenerative
source. `SIMULATED_REGEN_ESTIMATE` identifies estimated mode; this is a simulation
assumption, not demonstrated physical regeneration. Mechanical power uses
torque in Nm times mechanical shaft angular speed in rad/s. Separate available
mechanical/electrical energy estimates precede current caps and smoothing and
do not represent delivered energy. The electrical estimate uses the configured
efficiency; no second efficiency factor is applied to accepted battery energy.

Malformed domains/configuration, nonfinite values, overflowing intermediate or
cumulative accounting and rejected BMS cycles leave coordinator smoothing and
diagnostics unchanged. Supported cycles retain the existing exponential filter
and its minimum effective time constant. Former silent clamps for invalid SOC,
torque, angular speed and timestep now reject explicitly. Result allocations,
diagnostic vector capacity and calculations are staged before the BMS call.
The additive read-only `diagnostic_cell_capacity() noexcept` helper provides the
owned-cell bound for reserving `weak_cell_ids`; no allocating copy remains after
BMS acceptance. This proof depends on DiagnosticReport's single dynamic member
and exclusive coordinator/BMS access, including no concurrent reinitialization.
Future allocating diagnostic fields require equivalent preflight treatment.

## PB-01 through PB-15 reconciliation and verification

Historical originals were independently replayed from `e4f0417` for PB-01..03,
`b0558bc` for PB-04/05/07, `370ce23^` (`c55e665`) for PB-06/08 and `1be4cbd`
for PB-13..15, then against current source. PB-09..12 were reproduced against
this phase's merged baseline and dependency-stage implementations before repair.
Closure is scoped to each finding and its software regressions.

| PB | Status | Current passing regression and remaining scope |
| --- | --- | --- |
| PB-01 | Resolved | `test_state_integrity pack`: atomic cells/caches, malformed positions, overflow/recovery; O(N) storage and successful element-reference invalidation remain. |
| PB-02 | Resolved | `test_state_integrity coupling`: complete direct state preservation on invalid input/overflow; valid formulas/order retained. |
| PB-03 | Resolved, behavior change | `test_state_integrity short`: adjacent/equal/custom minimum, repeated rejection and coherent epochs; short intervals throw and are not buffered. |
| PB-04 | Resolved | `test_direct_numeric_boundaries`, `test_regen_boundaries`, `test_ailee_governor`: direct nonfinite/overflow denials and history recovery; configured software derating is not a physical guarantee. |
| PB-05 | Resolved | `test_regen_boundaries`: mechanical power ceiling, diagnostic-independent voltage policy, near-zero conversion and composed protections; positive-speed enforcement requires actual RPM. |
| PB-06 | Resolved, reconciled | Existing `test_middleware` plus independent default/custom directional/equality matrix; at-rest warnings are advisory, not actuator shutdown. |
| PB-07 | Resolved at adapter boundary | `test_bms_adapter_evidence`: missing/rejected latest cycle denies, accepted recovery succeeds; no external sensor-freshness guarantee. |
| PB-08 | Resolved, reconciled | Existing `test_hardware_adapter` plus all-four-getter synthetic clock/age matrix; positive-age clock rollback remains an external monotonic-clock obligation. |
| PB-09 | Resolved under energy contract | `test_recovery_integrity`: measured/estimated caps, smoothing, units, rejection/overflow/allocator boundaries and no double credit; physical regeneration attribution remains unavailable. |
| PB-10 | Resolved under local target contract | `test_trust_learning_integrity`: foreign/same-name/compartment/replaced/copied/moved/stale-engine targets, invalid baseline/tightened envelopes and unchanged committed rejection history; ABI rebuild and durable/live-revocation API redesign limits above remain. |
| PB-11 | Resolved | `test_trust_learning_integrity`: every policy/score entry point, configuration domains/order, nondefault/equal policies and compile-time `noexcept` checks. |
| PB-12 | Resolved at serialization/history boundary | C++ actual-prior/alias/history checks and `test_learning_audit.py` strict parser; external write durability/authenticity remain outside scope. |
| PB-13 | Resolved | `test_middleware_accounting`: raw signs/units, 4001st-cycle overflow, diagnostic/timing/counter/filter boundaries and accepted-state preservation; numeric acceptance is not physical safety authorization. |
| PB-14 | Resolved under agreement contract | `test_middleware_configuration`: ten default/custom/conflict/failed-reinit cases; exact nominal agreement required, CUSTOM pack chemistry unsupported. |
| PB-15 | Resolved for separate fallback | `test_fallback_integrity.py`: 21 methods, invalid sensors/time/configuration/overflow and complete atomicity; approximate fallback has no proved C++ or physical equivalence. |

PB-06 source guards were merged in `370ce23` and refined in `678c593` but the
backlog had not reconciled them. Low-SOC discharge and high-SOC charge fault;
corrective-direction and resting SOC excursions advise. Cold active current in
either direction faults; cold rest advises. Exact equal bounds pass. The three
original at-rest stimuli still have no hard fault but now expose explicit warning
and advisory evidence, which closes the unused-limit gap without changing policy.

PB-08 validates finite clocks/timestamps, finite nonnegative calculated age and
finite nonnegative configured age, including exact maximum-age equality. It does
not maintain a monotonic-clock trajectory: rollback from age `0.4` to `0.2 s`
still passes. The new regression explicitly records this limit. All transport
frames are synthetic receives; `send_count` remains zero.

## Complete verification

Toolchain: GCC 14.2.0, CMake 3.31.10, Python 3.12.14, pinned pybind11 v2.12.0
(`3e9dfa2866941655c56877882565e7577de6fc7b`) and FEEN v3.8.1
(`a58a5fab785fb1e784c949635d2c91e3e19b3f2d`). Existing CI assertions remain
enabled, including Release `-UNDEBUG`; warning-as-error policy is unchanged.
An external CMake output hook redirects distribution binaries into the build
directory, preserving the historical tracked CLI. No hardware or privileged
installation is involved.

The fresh complete-diff review independently confirmed the original malformed
UTF-8 ingestion failure at byte `0x80`. Its repaired probe parses 275 strict
UTF-8/JSON records, covering all 256 byte values and 19 sequence boundaries,
and recovers original invalid bytes exactly; denials preserve target/committed
history. Allocation fault injection initially failed nine checks when stream
failures could be swallowed. Stream exceptions and complete allocation staging
now pass all 385 checks. The reviewer independently verified both corrections.
Final focused suites pass 394 trust/rollback/audit checks, 408 recovery checks,
849 PB-06/PB-08 reconciliation checks, 385 learning allocation checks and eight
strict Python audit methods. Recovery allocation injection exercises actual
nonempty weak-cell IDs in advanced modes and successful return with NRVO disabled.

The independent original replay confirmed every prior resolved finding's current
behavior, rather than accepting issue labels. The state-integrity suite passes
5,681 checks; direct numerical checks pass 295; the approximate fallback passes
21 methods. Both a current-main differential probe and an independent historic
`e4f0417` probe match all 883 hexadecimal DS output lines exactly (historic
SHA-256 `41f070fadb3556df94cf5310b46be6245df55c6fb5f7cdf710e66a98eb2181cd`).
These are sampled valid-input comparisons, not universal numerical or binary
equivalence claims.

| Complete check | Final outcome |
| --- | --- |
| Unchanged merged-main Release baseline | Build and CTest 16/16 passed before task changes. |
| Final default Release, all registered targets including examples/benchmarks/CLI/shared library | Build passed, CTest 20/20 passed. |
| Final Debug, all registered targets | Build passed; ASan/UBSan with leak detection and halt-on-error, CTest 20/20 passed with no sanitizer diagnostics. |
| Full Python discovery with optional module on PYTHONPATH | 196/196 passed; zero skips/errors/failures, including all eight audit methods and optional limp/derate binding test. |
| Pinned optional Python-governor build | All targets built; CTest 20/20, binding smoke and both simulation examples passed. |
| Pinned FEEN build | All targets built; repository CTest 20/20 and separate upstream mock/simulator CTest 4/4 passed; assertions enabled, no physical interface used. |
| Basic integration and RAPS simulations | Passed in Release and leak-enabled ASan/UBSan Debug, also both optional builds. |
| GCC 14.2 `-fanalyzer` on final trust/learning regression translation unit | Exit zero, no diagnostics; targeted static check only. |
| Fresh independent complete-diff review | Reviewed all production/CMake/new tests/docs; confirmed malformed UTF-8 correction and independently passed 394/385/408/849 focused checks and eight parser methods. No remaining confirmed regression in reviewed scope. |
| Preserved records/artifacts | Historical v8.0.0/v8.0.1, state/numerical phase reports, tracked CLI and CI workflow unchanged; generated runtime logs restored, `git diff --check` passed. |

Full sanitizer execution used additional runtime permissions to avoid the shell
sandbox's ptrace tracing: initial standalone leak-enabled attempts hit the
documented LeakSanitizer-under-ptrace runtime failure. The final complete suite
and both simulations ran with `detect_leaks=1`, without disabling assertions or
sanitizers. Optional Python linking needed an explicit `PYTHON_LIBRARY` pointing
to this runtime's actual libpython, because its sysconfig prefix points at an
unavailable `/install/lib`; this is an environment selector, not a source change.
Upstream FEEN tests are registered in its child tests directory and were run
separately; the successful 20 repository tests do not subsume those four tests.

Unregistered `closed_loop_regen_charging.cpp` and `multi_cell_pack.cpp` examples
retain independently confirmed baseline API mismatches: the former calls missing
`get_enhanced_state()`, the latter calls missing `update_pack`/`get_weak_cells`
and `imbalance_warning`. CUDA was unavailable; `DS_USE_GPU=ON` merely falls back
and is not GPU execution evidence. clang-tidy/cppcheck were unavailable. These
limitations are not represented as passing builds or new findings in this scope.
No physical regeneration, calibration, worst-case timing, broader freshness or
upstream AILEE binary/API equivalence is established. No published v8.0.2 asset,
release, deployment or complete vehicle-remediation claim follows.

## Reproduction commands

Use the external output hook construction in the preceding
[numerical phase verification](NUMERICAL_MIDDLEWARE_FALLBACK_REMEDIATION.md#verification-and-compatibility-evidence)
to preserve the tracked CLI; set `ds_output_hook` to that hook's absolute path.
The actual run used `work/output-paths.cmake` and separate `work/build-*`
directories with two build workers. From repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDS_ENABLE_PYTHON_GOVERNOR=OFF -DCMAKE_PROJECT_INCLUDE="$ds_output_hook"
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
cmake -S . -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug -DDS_ENABLE_PYTHON_GOVERNOR=OFF -DCMAKE_PROJECT_INCLUDE="$ds_output_hook"
cmake --build cmake-build-debug --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir cmake-build-debug --output-on-failure
cmake -S . -B cmake-build-pybind -DCMAKE_BUILD_TYPE=Release -DDS_ENABLE_PYTHON_GOVERNOR=ON -DCMAKE_PROJECT_INCLUDE="$ds_output_hook"
cmake --build cmake-build-pybind --parallel 2
ctest --test-dir cmake-build-pybind --output-on-failure
PYTHONPATH="$PWD/cmake-build-pybind/lib" python3 tests/smoke_pybind11.py
PYTHONPATH="$PWD/cmake-build-pybind/lib" python3 -m unittest discover -s tests -p 'test_*.py' -v
cmake -S . -B cmake-build-feen -DCMAKE_BUILD_TYPE=Release -DDS_ENABLE_FEEN=ON -DDS_ENABLE_PYTHON_GOVERNOR=OFF -DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG -UNDEBUG' -DCMAKE_PROJECT_INCLUDE="$ds_output_hook"
cmake --build cmake-build-feen --parallel 2
ctest --test-dir cmake-build-feen --output-on-failure
ctest --test-dir cmake-build-feen/_deps/feen-build/tests --output-on-failure
```

Run simulation binaries from a scratch working directory with `logs/` available
to avoid changing tracked sample logs. Both `basic_integration` and
`raps_ev_stability_demo` must exit zero; use the same sanitizer environment for
Debug simulation binaries. Optional Python-governed RAPS uses
`PYTHONPATH=<repository>/ds_core/python`. New focused executables support ordinary
no-argument execution; `test_trust_learning_integrity` also supports `trust`,
`rollback`, `audit`, `emit-audit <path>` and `emit-byte-audit <path>` modes.

Original reproductions can be compiled against archived baseline headers, e.g.
`git archive <baseline> include | tar -x -C <scratch>` with C++17 and
`-I<scratch>/include`; current tests then build against current headers. The
original reproduction entries in the backlog preserve exact stimuli. Raw logs and
temporary probe binaries were kept under `work/`, rather than committed as
generated artifacts. Hosted CI status is reported on the draft PR's exact head;
local results above do not imply an unobserved remote CI outcome.
