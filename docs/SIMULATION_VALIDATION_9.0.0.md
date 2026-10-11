# DS-EV v9.0.0 — Simulation and validation record

This is fresh software execution evidence for the optional Energy Continuity
candidate. All energy values below are synthetic simulation inputs or software
outputs. No physical battery, vehicle, calorimeter, actuator, installation,
OTA operation, release publication or hardware measurement was used.

## Reviewed source and environment

- Current `main` inspected and fetched: `593f58bfe9adf5887af31ab61702ad66421ff286`.
- Immutable `v8.0.1`: `e4f0417d48d9ed217ed3d4b0cfe3363e85a0199c`, verified against GitHub tag resolution and local Git.
- Reviewed implementation commit: `6a7d00dc5f3e478049b227d2d41cfbf87b139760`.
  The following documentation commit adds README/release/validation evidence;
  it changes no compiled or Python source. The exact final candidate SHA is the
  draft PR head and is reported with the PR. This avoids a self-referential SHA
  inside the document that belongs to that commit.
- Execution date: October 11, 2026 (America/New_York).
- Debian GNU/Linux 13.6, Linux x86_64, GCC 14.2.0, Python 3.12.14,
  CMake 3.31.10; C++17, existing warnings-as-errors and Release assertions enabled.
- CMake and pybind11 Python packages were placed in private `work/tools` without
  privileged installation. CMake binaries reside in `work/tools/cmake/data/bin`.
- Optional dependency checkouts: pybind11 v2.12.0
  `3e9dfa2866941655c56877882565e7577de6fc7b`; FEEN v3.8.1
  `a58a5fab785fb1e784c949635d2c91e3e19b3f2d`.
- Parallelism: two jobs per build. A deferred output hook redirects CLI/shared
  library/module outputs into each build, preserving the tracked historical CLI.

The earlier [independent audit](RELEASE_READINESS_AUDIT_9.0.0.md) reviewed
`038653cd6600318e36276001099960dc8c523c3a`. It is unchanged and supplies no
passing verdict for the new feature. The new code, regressions and final diff
received separate numerical and release/interface reviews.

## Conservation contract

For each explicitly selected physical boundary and interval:

\[
R=E_{\mathrm{initial}}+E_{\mathrm{in}}-
E_{\mathrm{final}}-E_{\mathrm{out}}-E_{\mathrm{dissipated}}.
\]

The implementation calculates the equivalent signed sum of individual storage
endpoints and boundary transfers with checked compensation. Large unchanged
inventory therefore does not erase a small explicit flux. Both displayed sides,
per-class totals, cumulative sums and tolerance arithmetic must remain finite.
Ordinary double precision still limits supplied observations and reported values;
this is neither exact arithmetic nor arbitrary-precision metrology.

\[
\Delta E_{\mathrm{battery},J}=VI\Delta t,\qquad
\Delta E_{\mathrm{battery},kWh}=VI\Delta t/(3.6\times10^6).
\]

Positive current/increment denotes discharge; negative denotes charging. Transfer
records use nonnegative joules with explicit source and destination. The tolerance
is inclusive:

\[
|R|\le a+r\max(E_{initial},E_{final},E_{in},E_{out}+E_{dissipated},|\Delta E_{stored}|).
\]

Default `a=1e-6 J`, `r=1e-9`. Balance requires explicit complete-transfer and
complete-storage declarations, one storage observation per boundary node and
independent-support declarations. Dependent/circular zero remains insufficient
evidence. Availability estimates are excluded from flux; classification alone
never creates a delivered transfer. Residuals never create heat or efficiency.

The fixed-capacity ledger stages before commit, rejects malformed/nonfinite/
overflowing/rejected evidence atomically, uses increasing session/event IDs and
unique per-event transfer IDs, and retains only bounded totals. Valid incomplete
and imbalanced evidence commits as evidence, without upgrading its status.
Physical-edge diagnostic totals can include internal edges and must not be added
to boundary flux or existing BMS/coordinator ledgers.

## Simulation configuration and analytic oracles

`examples/energy_continuity_simulation.cpp` has no random source. All intervals
are one second. Boundary nodes are Battery, PowerElectronics, Motor and
KineticStore; the thermal sink is outside. Electronics/motor stored energy is
explicitly zero. Battery and kinetic endpoints are fixed analytic fixture
observations, supplied separately from transfer records. No stored-energy value
is inferred from DS `energy_psi`, `energy_phi`, `energy_metric`, SOC or residuals.

Discharge uses `400 V`, `+25 A`: `10000 J` battery delivery per step, `8000 J`
propulsion/kinetic storage and `2000 J` explicit simulated dissipation. Braking
uses `400 V`, `-15 A`: `6000 J` accepted battery charging, `8000 J` kinetic
release, `7000 J` motor-to-electronics transfer and two `1000 J` thermal paths.
The recovery coordinator's `80 Nm * 100 rad/s` estimate is `8000 J`; its chosen
`0.875` conversion factor gives `7000 J` electrical availability. This factor
is a simulation assumption, not measured thermodynamic efficiency. It does not
determine or authenticate the independently supplied charging current.

| Step | Battery initial/final J | Kinetic initial/final J | Boundary initial/final J | Explicit heat J | Actual residual J | Actual allowance J | State |
|---|---|---|---|---:|---:|---:|---|
| A1 discharge | 100000 / 90000 | 0 / 8000 | 100000 / 98000 | 2000 | 0 | 0.000101 | BALANCED |
| A2 discharge | 90000 / 80000 | 8000 / 16000 | 98000 / 96000 | 2000 | 0 | 0.000099000000000000008 | BALANCED |
| B1 braking | 80000 / 86000 | 16000 / 8000 | 96000 / 94000 | 2000 | 0 | 0.000097 | BALANCED |
| B2 braking | 86000 / 92000 | 8000 / 0 | 94000 / 92000 | 2000 | 0 | 0.000095000000000000005 | BALANCED |

All four analytic equations independently give zero. Final battery storage is
`92000 J`, kinetic storage `0 J`, and explicit session heat `8000 J`.
BMS accepted charging and the coordinator's alias each print
`0.0033333333333333335 kWh` (`12000 J`); they are never added together.
Mechanical/electrical availability totals sum to `30000 J` of separately labeled
**estimated diagnostics**, with `0 J` of measured physical evidence in this
simulation. Those edge diagnostics are not an additional battery contribution.

## Fault, incomplete-evidence and lifecycle results

| Scenario | Analytic expectation | Executed result |
|---|---|---|
| C explicit conversion loss | Supplied heat closes each vehicle step | Four zero residuals, BALANCED |
| C intentionally conflicting complete evidence | Battery 10000→0, no output/heat: +10000 J | IMBALANCED, +10000 J; heat remains 0 |
| D rejected BMS NaN-current cycle | Accepted count/charging unchanged; observer denies | Count 1, charging 0.0011111111111111111 kWh; observer rejected |
| D NaN quantity, denied transfer, mismatched interval | No committed event | Three REJECTED results, committed=0 |
| E omitted necessary transfer | +10000 J, incomplete evidence | INSUFFICIENT_EVIDENCE, +10000 J; heat remains 0 |
| E dependent storage with closing transfer | Zero cannot establish independent balance | INSUFFICIENT_EVIDENCE, 0 J |
| F zero energy | Positive interval valid; zero interval invalid | PASS |
| F finite extreme then repeated accumulation | First 0.75*DBL_MAX accepted; second rejects overflow | PASS; accepted count remains 1 |
| F absolute tolerance edge | 1 J allowed; nextafter(1,2) exceeds | BALANCED then IMBALANCED, PASS |
| F replay/reset | Duplicate event rejects; newer session clears | PASS |
| Full fixed campaign replay | Identical numeric results and ledgers | deterministic_replay=PASS, overall=PASS |

These are conditional checks of a supplied analytic simulation. They do not
independently validate vehicle physics, source attribution or measured losses.

## Fresh verification

| Check | Result |
|---|---|
| Complete Release build (examples, benchmarks, CLI/shared library) | PASS; CTest 22/22 |
| Complete Debug build, ASan + UBSan + leak detection | PASS; CTest 22/22, no sanitizer diagnostics in successful run |
| Complete pinned Python-governor/pybind build | PASS; CTest 22/22 |
| Complete pinned FEEN build and repository CTest | PASS; repository CTest 22/22 |
| Upstream pinned FEEN mock/simulator CTest | PASS; 4/4 upstream mock/simulator tests |
| Full Python discovery, default configuration | 203 run, 202 passed, 1 optional pybind skip; zero failures/errors |
| Full Python discovery with optional module available | 203/203 passed; zero skips/failures/errors |
| New continuity analytic/adversarial checks | 334/334 passed, independently recompiled and reviewed |
| Python raw-trust regression and automotive tests | 18/18 passed; seven new raw methods |
| Optional pybind smoke and version import | PASS; module `__version__` prints 9.0.0 |
| CLI/export/CPack version review | PASS; active overall version 9.0.0 |
| Mathematical differential probe versus main and v8.0.1 | All 883 hexadecimal lines byte-identical |
| Continuity simulation output, second invocation | Byte-identical; internal replay PASS |
| Basic integration, RAPS and continuity simulations | PASS in Release, sanitized Debug and Python-governor builds; FEEN examples also PASS |
| Historical audit, remediation records, migration pins and tracked CLI | Unchanged |
| Complete new-feature numerical and interface/security reviews | No remaining confirmed blocker under documented caller contracts |
| Final source/diff whitespace checks | PASS |

The successful sanitizer run retained `detect_leaks=1`, ASan/UBSan and
halt-on-error. An initial traced sandbox run failed all 22 processes at runtime
with `LeakSanitizer does not work under ptrace`. The same binaries and tests
passed outside that tracing restriction; no test or sanitizer setting was weakened.

An early Python discovery attempt before CMake setup had one `setUpClass` error
because `cmake` was absent from PATH. Final full discovery with the configured
CMake path supersedes that intermediate attempt. Default discovery now runs all
203 methods, with only the documented optional module skip; no ownership-related
fixture failure remains in this supported environment. Temporary installation,
backup and migration fixtures are used; no privileged/live deployment executes.

Regression-first numerical review also exposed two new residual-cancellation
cases: unchanged `1e20 J` storage plus `1 J` inbound, and initial `1e20 J` /
final `1 J` with `1e20 J` outbound. Earlier implementations incorrectly reported
zero balance. The frozen candidate reports **+1 J** and **−1 J** imbalance at
zero tolerance, respectively. Compensated individual endpoints/transfers and
cumulative signed sums are staged with the rest of the ledger. The regression
suite additionally verifies rejection preserves hidden compensation by observing
subsequent cancellation/recovery, rather than only inspecting public totals.

The public Python `AileeTrustPipeline.process_raw()` baseline accepted NaN/Inf
confidence and clamped nonfinite values into accepted history. The focused
baseline replay failed 21 subtests and raised eight errors across seven methods.
The candidate rejects malformed/nonfinite/out-of-domain confidence before clamps
or comparisons, returns finite rejection output and preserves history. Existing
finite clamps/thresholds remain tested. Current C++ embedding calls automotive
`process()`, not `process_raw()`; no demonstrated actuator-path defect or broader
upstream AILEE equivalence is claimed. No upstream AILEE code was imported.

The mathematical probe SHA-256 is
`41f070fadb3556df94cf5310b46be6245df55c6fb5f7cdf710e66a98eb2181cd`
for all three comparisons. The continuity terminal-output SHA-256 is
`fa7ee757d1a006fb9baa9baee9c95eda913260e2bd3068cdd2a86ba2df2d058a`.
These are exact software artifacts, not signatures of physical measurements.

## Reproduction commands

Run from the candidate checkout. Use CMake 3.31.10 or an equivalent supported
version on PATH. The output hook used during this campaign was:

```cmake
function(ds_candidate_output_paths)
  if(TARGET ds_enhancer)
    set_target_properties(ds_enhancer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  endif()
  foreach(target ds_enhancer_shared ds_enhancer_pybind)
    if(TARGET ${target})
      set_target_properties(${target} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    endif()
  endforeach()
endfunction()
cmake_language(DEFER CALL ds_candidate_output_paths)
```

Save it as `work/output-paths.cmake`, then:

```sh
cmake -S . -B work/build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_PROJECT_INCLUDE="$PWD/work/output-paths.cmake"
cmake --build work/build-release --parallel 2
ctest --test-dir work/build-release --output-on-failure
./work/build-release/basic_integration
./work/build-release/raps_ev_stability_demo
./work/build-release/energy_continuity_simulation
python3 -m unittest discover -s tests -p 'test_*.py' -v

cmake -S . -B work/build-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PROJECT_INCLUDE="$PWD/work/output-paths.cmake"
cmake --build work/build-debug --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir work/build-debug --output-on-failure
```

Optional dependency checkouts were supplied to FetchContent so the campaign
used the verified pins without re-downloading while building:

```sh
cmake -S . -B work/build-python -DCMAKE_BUILD_TYPE=Release -DDS_ENABLE_PYTHON_GOVERNOR=ON -DFETCHCONTENT_SOURCE_DIR_PYBIND11="$PWD/work/pybind11" -DPYTHON_LIBRARY=/opt/codex/runtimes/codex-primary-runtime/dependencies/python/lib/libpython3.12.so -DCMAKE_PROJECT_INCLUDE="$PWD/work/output-paths.cmake"
cmake --build work/build-python --parallel 2
ctest --test-dir work/build-python --output-on-failure
PYTHONPATH="$PWD/work/build-python/lib" python3 tests/smoke_pybind11.py
PYTHONPATH="$PWD/work/build-python/lib" python3 -m unittest discover -s tests -p 'test_*.py' -v

cmake -S . -B work/build-feen -DCMAKE_BUILD_TYPE=Release -DDS_ENABLE_FEEN=ON -DFETCHCONTENT_SOURCE_DIR_FEEN="$PWD/work/feen" -DCMAKE_PROJECT_INCLUDE="$PWD/work/output-paths.cmake"
cmake --build work/build-feen --parallel 2
ctest --test-dir work/build-feen --output-on-failure
ctest --test-dir work/build-feen/_deps/feen-build/tests --output-on-failure
```

The explicit `PYTHON_LIBRARY` selects the runtime's real libpython because its
sysconfig library prefix points at unavailable `/install/lib`. Other hosts should
use their own correctly discovered Python library. CUDA remains off. All three
examples were also run in each optional build; Debug examples used the sanitizer
environment shown above.

The existing 20 CTests are preserved, plus `test_energy_continuity` and
`energy_continuity_simulation`. CI adds the new simulation smoke command; its
normal CTest run also executes both new tests. The optional Python-extension
job is retained. Hosted CI passed both jobs for implementation commit `6a7d00d`: [run 38147521080](https://github.com/dfeen87/DS-EV/actions/runs/38147521080).
The exact final draft-PR head is also checked and reported with the PR; older
audit CI is not substituted for new-feature CI.

## Git statistics

<!-- BEGIN FINAL GIT STATISTICS -->
| Comparison | Commits | Files | Insertions | Deletions | Net lines |
|---|---:|---:|---:|---:|---:|
| v8.0.1 → final candidate | 37 | 62 | 12,795 | 1,341 | +11,454 |
| Preparation PR against inspected main | 2 | 22 | 2,938 | 38 | +2,900 |
<!-- END FINAL GIT STATISTICS -->

Totals use a single comparison against the immutable v8.0.1 SHA, with no summed
per-PR deltas. Reproduce using `git rev-list --count v8.0.1..HEAD`,
`git diff --shortstat v8.0.1 HEAD` and `git diff --numstat v8.0.1 HEAD` from the
final candidate. The document-only follow-up commit completes the evidence and
README/release note without changing the frozen implementation.


## Independent Cross-Boundary & Multi-Interval Simulation Validation

**Exact reviewed source commit:** `a855c22fba1b178b752e096d34799aa386806460` (Base repository head used for independent verification campaign).

**Simulation configurations:**
Dedicated verification of independent components (Battery, PowerElectronics, Motor, KineticStore) through simulated explicitly defined energy records, tolerances set tightly (`absolute_joules = 1e-6`, `relative = 1e-9`), and exact numeric probes.

**Expected analytic energy balances vs Actual software results:**

| Physical Boundary | Configuration | Expected Balance | Actual Status / Residual | Outcome |
|---|---|---|---|---|
| **Battery** | Normal discharge with exact evidence | Balanced | Balanced / 0.0 J | PASS |
| | Missing evidence/duplicate ID | Rejected/Imbalanced | Rejected/Imbalanced | PASS |
| | Unrelated evidence (not independent) | InsufficientEvidence | InsufficientEvidence | PASS |
| **Inverter** | Explicit in, out, and dissipated heat | Balanced | Balanced / 0.0 J | PASS |
| | Missing heat transfer (incomplete energy) | Imbalanced | Imbalanced / 5.0 J | PASS |
| | Incomplete evidence declaration | InsufficientEvidence | InsufficientEvidence | PASS |
| **Motor** | In, mechanical out, and dissipated heat | Balanced | Balanced / 0.0 J | PASS |
| | Injected mismatch (+10J out vs in) | Imbalanced | Imbalanced / -20.0 J | PASS |
| **Kinetic-energy** | Regen mode back to motor | Balanced | Balanced / 0.0 J | PASS |
| | Available estimate distinct from physical | Balanced | Balanced / 0.0 J | PASS |
| **Cross-Interval** | Deliberate gap in time/event | Balanced (isolation) | Balanced / 0.0 J | PASS |
| | Unmatched cross-event storage | Balanced (isolation) | Balanced / 0.0 J | PASS |
| | Old/Replayed events, invalid session | Rejected | Rejected | PASS |
| **Numerical** | Limits: `min()`, `denorm_min()` | Balanced | Balanced / 0.0 J | PASS |
| | `2^53` exact limits (`9007199254740992.0`) | Balanced | Balanced / 0.0 J | PASS |
| | `1e20` store, `1.0` transfer (`absolute=0.0`) | Imbalanced | Imbalanced / -1.0 J | PASS |
| | `nextafter` exact bounds vs outside bounds | Balanced vs Imbalanced | Balanced vs Imbalanced | PASS |

**Evidence completeness and independence:**
Tested rigorously. A missing observation explicitly causes `InsufficientEvidence` unless incorrectly declared complete by caller, at which point numeric residual correctly marks `Imbalanced`. `independently_supported = false` appropriately voids verification.

**Confirmed defects:**
None. The architecture successfully reconciles correctly structured energy observations while aggressively rejecting malformed structures, unsupported assumptions (like unrelated internal edges), and numerical overflows/precision loss (Kahan-Babuska/Neumaier summation works successfully for 1e20 vs 1.0J scenarios).

**Known limitations versus newly identified risks:**
- **Known Limitation Verified:** The observer API deliberately evaluates single intervals in isolation. It natively permits gaps between events and does not enforce continuity of final storage (interval N) matching initial storage (interval N+1). A deliberate mismatch across sequential valid intervals assesses to `Balanced` individually, demonstrating that multi-event state integrity relies on the external caller to bridge disjoint batches.

**Reproduction commands:**
```sh
cd build && cmake .. && make test_cross_boundary_simulation
ctest -R test_cross_boundary_simulation --output-on-failure
```

## Final diff and unsupported verification

- Existing BMS, coordinator, telemetry, energy-stack and C++ AILEE files have zero
  diff from inspected main. Core changes are release metadata only. Existing
  public struct layouts, C entry points and component signatures are unchanged
  by this preparation PR; new opt-in types and Python version attributes are
  additive. Historical v8.0.1-to-main ABI/behavior changes still require rebuild
  and migration as described in the release note.
- Observer factories take const component references. They never run a model
  cycle, modify source ledgers, authorize trust or issue a control command.
  Canonical BMS accepted counts and transfer ID 1 prevent aliases from becoming
  additional battery credits. Recovery availability IDs 2/3 remain diagnostics.
- Updater diff is its User-Agent only. The offline migration's authenticated
  v8.0.2 source/checker/helper pins and `config/update_source.json` are unchanged.
  The historical URL retains a null digest and fails closed. No v9.0.0 asset,
  checksum, migration trust generalization, tag or release was invented.
- Completeness, independent support, provenance and physical counterparty are
  caller assertions. Measurements are not authenticated. Checkpoints require
  exclusive immediate observation and a newer continuity session/fresh capture
  after each BMS initialization; existing diagnostics expose no generation.
- Balance is per declared interval/boundary. Gaps and cross-event storage
  endpoints are not independently validated or automatically reconciled. The
  snapshot cannot certify a continuous hardware session. No DS abstract-energy
  dimensional mapping, calibrated loss or source-attribution model is claimed.
- CUDA hardware, Valgrind, clang-tidy/cppcheck, worst-case timing, HIL/SIL plant
  validation, real charging/regeneration, OEM deployment and automotive safety
  certification were not executed. Known unregistered historical
  `multi_cell_pack.cpp` / `closed_loop_regen_charging.cpp` API mismatches and the
  unbound Python telemetry return type remain outside this bounded feature.

**Engineering verdict:** **GO WITH ACCEPTED LIMITATIONS**. The candidate remains a draft PR;
publication, tagging, merging and deployment are separate maintainer actions.
