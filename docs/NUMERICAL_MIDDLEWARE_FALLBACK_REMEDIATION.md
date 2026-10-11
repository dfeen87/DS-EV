# PB-13, PB-14 and PB-15 — numerical integrity evidence

Baseline: latest merged `main` at
`1be4cbd8bbf6bc09b87d61619234ed85dd016fbe`. This phase contributes to the
consolidated **v8.0.2 development target**. PB-09 through PB-12 remain open;
no release, tag, deployment or hardware operation is part of this work.
Historical `RELEASE_8.0.0.md` and `RELEASE_8.0.1.md` are preserved.

## Regression-first reproductions

The targeted tests were written and run before their implementation fixes.
Additional adversarial findings received failing regressions before correction.
Fresh middleware and fallback instances begin with zero accepted time, counters
and ledgers; fallback entropy, degradation, Ah throughput and cycles are zero.

### PB-13: a stable model with an infinite ledger

Initialize `75 Ah / 400 V`, then call
`enhance_cycle(360, DBL_MAX, 25, 0.8, 1)`. On the baseline:

| Measurement | Positive raw current | Negative raw current |
| --- | --- | --- |
| Raw current (A) | `1.7976931348623157e308` | `-1.7976931348623157e308` |
| Normalized model current (A) | `500` | `-500` |
| Model numerical stability / clamped input | `true / true` | `true / true` |
| Model time (s) / entropy | `1 / 1` | `1 / 1` |
| Model charge throughput (Ah) | `0.1388888888888889` | `0.1388888888888889` |
| Diagnostic instantaneous power (kW) | `180` | `-180` |
| Cumulative energy throughput (kWh) | `inf` | `inf` |
| Legacy recovered-energy telemetry (kWh) | `0` | `inf` |
| Safety fault / accepted update count | `true / 1` | `true / 1` |

The raw `360 * current` multiplication overflows, while normalized DS dynamics
remain finite. The original regression failed with
`PB-13: DBL_MAX raw energy must reject instead of reporting a stable cycle`.
After correction both signs throw `std::overflow_error` (`Observed power
overflow`); model time, ledger, numerical diagnostics and update count remain
zero, and the raw current fault remains visible. No result claims success.

A separate zero-current cycle with `dt=DBL_MAX/500 = 3.5953862697246311e305 s`
formerly returned stability true and ledger zero, but both last/average interval
diagnostics were `inf` milliseconds. Its regression failed with
`PB-13: finite ledger does not excuse overflowing millisecond diagnostics`.
It now rejects `Millisecond interval overflow` without advancing accepted time.

Adversarial review found another positive-stability path: with
`ds_config.max_temperature=DBL_MAX/2`, after a normal one-second cycle,
`enhance_cycle(360,0,DBL_MAX/2,0.8,100000)` returned entropy `1`, degradation `1`,
energy `1.2711610061536463e304 J`, model time `100001 s`, accepted count `2`,
raw throughput `0`, and stability true. Unsaturated entropy/thermal-degradation
arithmetic had overflowed before clamping. The added regression failed
`PB-13: saturation must not conceal overflowed core entropy/degradation
intermediates`. Core guards now reject before either saturation can hide it;
an entropy-weight-zero case independently exercises thermal-degradation overflow.
The guards operate on the existing DS candidate and preserve finite formulas.
With entropy weight zero and `dt=100000`, thermal accumulation is instead
`1.284066524901654e308` (finite); baseline and corrected code both legitimately
accept saturation to degradation `1`, entropy `0`, time `100001`, count `2` and
total energy `8.988465674311579e303 J`. Finite saturation was not disabled.

Further baseline probes isolated instantaneous-power and cumulative accounting:
`init(1e-310,400)` with `(DBL_MAX/2,4,25,0.8,1)` returned stability true and finite
stored model energy `25.886781142017266 J`, but instantaneous diagnostic power
and throughput were infinite. `(360,1e303,25,0.8,1e8)` overflowed kW-seconds with
finite millisecond timing. Both now reject before mutation. For each current
sign, 4000 accepted `(1,sign*DBL_MAX/2,25,0.8,1800)` cycles accumulated
`1.7976931348621678e308 kWh`; baseline cycle 4001 committed infinity, while the
corrected API rejects and retains count 4000 and that finite total. Alternating
signs accumulated `1.7976931348622175e308 kWh` at count 4000 and isolate rejection
of combined-throughput overflow while each separate ledger remains finite.

### PB-14: conflicting nominal values and failed reinitialization

Set outer nominal capacity/voltage to `150 Ah / 800 V`, retain embedded defaults
`75 Ah / 400 V`, then call `enhance_cycle(360,10,25,0.5,1)`:

| Measurement | Baseline conflicting values | Matching 150 Ah / 800 V |
| --- | --- | --- |
| Physical model energy (J) | `48600000` | `97200000` |
| Charging voltage (V) | `400` | `800` |
| Estimated charging time (minutes) | `22.500000041666667` | `45.000000041666667` |
| Numerical stability | `true` | `true` |

The conflict regressions failed `invalid or conflicting configuration must be
rejected`. They now throw `std::invalid_argument` before replacing any state.
Matching custom calibration retains exact direct-core behavior.

On the baseline, reinitializing an established instance with `lambda=NaN`
threw `Invalid DSConfig parameters`, retained diagnostic count `1`, but the next
cycle threw `DSEnhancement not initialized. Call init() first.` The new test
demonstrated that partial replacement. Failed reinitialization now preserves
the old configuration, core, optional pack/filter, diagnostics, timing, ledgers
and safety evidence, including the next accepted cycle's equality to a control.

Invalid safety configuration was also reproduced: a NaN discharge-current limit
accepted raw `2000 A`, normalized it to `500 A`, and returned stability true with
`safety_fault=false`. All 13 safety thresholds now require finite values, ordered
voltage/temperature/SOC bounds, nonnegative voltage/current/imbalance limits, and
health in `[0,100]`; invalid initialization leaves the previous instance intact.

A derived-configuration regression first demonstrated matching capacity
`DBL_MAX` accepting `(1e-308,10,25,1,1)`: charge throughput
`0.0027777777777777779 Ah`, physical model energy `6471.6952855043355 J`, stability true, but
cycle count `0` instead of the representable `7.725950897595657e-312`.
`2 * capacity` had become infinity. Matching nominal voltage `DBL_MAX` likewise
initialized despite an infinite `1.05 * voltage` charging margin.
`DSConfig::validate()` now rejects those unusable derived constants, shared by
direct-core and middleware initialization. The new regression failed before
this guard and passes afterward.

### PB-15: invalid evidence changes approximate fallback state

Each reproduction starts on a separate default instance:

| Persistent field | `(360,10,25,0.8,-1)` afterward | `(360,10,25,NaN,1)` afterward |
| --- | --- | --- |
| Time (s) | `-1` | `1` |
| Entropy | `-0.05` | `0.05` |
| Charge throughput (Ah) | `-0.002777777777777778` | `0.002777777777777778` |
| Equivalent cycles | `-1.8518518518518518e-05` | `1.8518518518518518e-05` |
| Degradation | `1.9998148149148176e-05` | `2.00018518518519e-05` |

The exact returned numeric fields were:

| Returned field | Negative timestep | NaN SOC |
| --- | --- | --- |
| Degradation | `1.9998148149148176e-05` | `2.00018518518519e-05` |
| Remaining capacity (%) | `99.99800018518509` | `99.99799981481482` |
| Cycles to 80% | `999.9000092092579` | `999.8999907407385` |
| Confidence | `0.9999800017518502` | `0.9999799981481438` |
| Recommended current (A) | `99.99800017518503` | `99.99799981481438` |
| Recommended voltage (V) | `439.9999999959999` | `439.9999999999999` |
| Recommended temperature (C) | `29` | `30` |
| Estimated charging time (minutes) | `9.000179987832787` | `0` |
| Warning / clamped / numerical stability | `false / false / true` | `false / false / true` |

NaN SOC was silently clamped to `1`; voltage was not validated. Both initial
rejection assertions failed `AssertionError: ValueError not raised`. The initial
16 unittest methods produced 58 failing subtests and 17 errors. The final
21 methods pass, including five additional adversarial/compatibility cases.
Both reproductions now raise `ValueError`, return no result, and preserve the
complete instance dictionary, with all five original accumulators still zero.

## Accepted raw electrical-energy contract

The middleware ledger represents **raw observed electrical energy for accepted
cycles**, not energy calculated from normalized DS current. This preserves the
existing ledger's purpose while making acceptance transactional.

- Positive current means discharge; negative current means charging. Positive
  voltage is volts, signed current amperes, and positive timestep seconds.
  `power_W = V * A`, `power_kW = power_W / 1000`, and
  `energy_kWh = power_kW * seconds / 3600`.
- Accepted positive energy increases `energy_out_kwh_`; accepted negative energy
  increases `energy_in_kwh_` by its absolute value. Zero current credits neither.
  Throughput is the sum. Ordinary `360 V / 10 A / 1 s` discharge is `0.001 kWh`;
  charging at `-20 A` for one second adds `0.002 kWh`, totaling `0.003 kWh`.
- Every consumed intermediate (including W and kW-seconds), each cumulative
  ledger and their sum must be finite before commit. Arithmetic that would
  overflow an intermediate is rejected even if a different algebraic ordering
  could produce a finite final value; normalization cannot conceal that evidence.
- Acceptance requires valid core state and predictions, optional filter
  state/covariance/uncertainty, **every floating diagnostic**, elapsed seconds,
  timestep-to-ms conversion, cumulative milliseconds and a checked integer
  update counter. No increment occurs at `INT_MAX`. Timing fields represent
  simulation intervals, not measured execution latency.
- Current/temperature/SOC model clamps retain their established behavior for
  finite inputs. Raw sensor values feed safety monitoring before normalization.
  Safety faults remain reported under the existing policy; they are not
  actuator authorization and do not independently veto numerically valid
  observational cycles. For example raw `2000 A` can be accepted with a fault if
  all accounting/model arithmetic is finite; its power is now coherently raw
  `720 kW`, rather than the previous normalized `180 kW` diagnostic.
- A rejected attempt throws and credits no energy, time, count or model/filter
  history. Last accepted numerical diagnostics remain available. Safety flags,
  faults and advisories may reflect the latest rejected raw observation: this
  is an intentional evidence channel, not a partial numerical commit. A NaN
  sensor explicitly becomes a safety fault when monitoring is enabled. Callers
  must not interpret a retained snapshot as proof that the latest attempt passed.
- `snapshot().recovered_energy_kwh` retains its legacy name and represents
  accepted **all-source charging** energy. It cannot distinguish charger input
  from regenerative recovery, nor establish physical recovered energy.

PB-09 must stage its smoothing/history around successful BMS completion and
use battery-side accepted raw voltage/current after coordinator limits and
smoothing. Keep available mechanical-energy estimates separate; do not credit
them as recovered electrical energy or double-count this cumulative ledger.
Source tagging and efficiency/loss policy remain PB-09 work. The fallback owns
absolute Ah throughput, with no Wh/kWh recovery ledger.

## Nominal configuration and initialization contract

History shows that commit `efda4c0` added nominal synchronization to simple
`init(capacity,voltage)`; advanced initialization continued forwarding the
embedded DS configuration. The outer fields have no independent mathematics
consumer, and neither layer records whether default-looking values were
deliberately calibrated. There is no safe implicit precedence rule.

Advanced initialization therefore requires **exact agreement** between outer
and embedded nominal capacity/voltage, finite positive nominal values, usable
derived constants, valid DS calibration and safety configuration. It never
rewrites an explicitly calibrated DS parameter. Matching defaults, custom
nominals and other DS calibrations retain direct-core parity. Previously
ambiguous outer-only or embedded-only custom nominals now reject; callers must
set both copies. Unknown modes/chemistry, invalid pack cell counts, and invalid
logging intervals are also rejected before publication.

All replacements are constructed before a no-throw commit. Failed first
initialization leaves the instance unusable; failed reinitialization preserves
the prior valid instance. Successful reinitialization starts a fresh model,
ledger, timing and safety session and removes disabled optional components.
Simple reinitialization retains established non-nominal DS calibration, supplies
both nominal values from its arguments and selects SIMPLE with optional
pack/filter features disabled. Advanced reinitialization uses the supplied
complete configuration. Public signatures and version numbers are unchanged.

## Fallback validation and commit contract

The fallback remains an independent approximate model, without a C++ numerical
equivalence claim. Finite real inputs are required; booleans, strings, objects,
unrepresentable integers, NaN and infinity are rejected. Voltage and timestep
must be positive; temperature cannot be below absolute zero (`-273.15 C`).
Legitimate finite SOC `[0,1]`, current magnitude and temperature clamps remain.
The legacy `input_clamped` flag still covers current/temperature only.

Constructor capacity/voltage and derived capacity/voltage constants are checked.
Because configuration is public, actual parameters and existing state are also
validated per cycle. Time, unsaturated entropy/degradation, Ah/cycles, gradients,
metric, health, charging and returned results are calculated and checked in
local candidates. An unstable finite metric also rejects. Commit updates all
five owned accumulators only after the entire result has passed; rejection
raises `ValueError` and preserves every persistent attribute. Voltage remains a
validated compatibility input rather than a mathematical driver of this model.

## Verification and compatibility evidence

Toolchain: GCC 14.2.0, CMake 3.31.10, Python 3.12.14, pinned pybind11 v2.12.0,
clang-tidy 22.1.8, authenticated Debian cppcheck 2.17.1. GCC Debug enables both
AddressSanitizer and UndefinedBehaviorSanitizer. Pinned FEEN v3.8.1 received
targeted copy/history verification; GPU remains optional and unvalidated.
External CMake output redirection preserves the tracked historical
CLI binary and keeps artifacts in ignored build directories; assertions remain
enabled in Release tests.

| Check | Outcome |
| --- | --- |
| Release build and complete registered CTest suite | Passed, 16/16 |
| Debug build, ASan/UBSan/leak detection, complete CTest suite | Passed, 16/16; no sanitizer diagnostics |
| Full Python discovery with optional extension on PYTHONPATH | Passed, 188/188; zero skips/errors/failures |
| New fallback regression methods | Passed, 21/21 (included in full discovery) |
| New configuration cases | Passed, 10/10 (included in CTest) |
| Accounting cases, including natural 4001st-cycle overflow and injected timing/counter/filter boundaries | Passed in Release, Debug and targeted ASan/UBSan runs |
| Existing adjacent state-integrity checks | Passed, 5681 checks |
| Valid DS probe compared with exact baseline | All 883 hexadecimal-output lines byte-for-byte identical |
| Fallback valid-cycle differential probes | 2000 implementation probe cycles and independent 1000 review cycles matched all outputs/state exactly |
| Pinned pybind11 extension build, existing smoke and limp/derating check | Passed; expanded overflow/configuration/history binding probes also passed |
| Pinned FEEN v3.8.1 accounting/integration targets | Build and CTest passed, 2/2; actual scorer-copy/rejected-history path tested with and without Kalman |
| Basic integration and RAPS simulation examples | Passed in Release and ASan/UBSan Debug |
| cppcheck warning/performance/portability command | Exit 1 on four identical baseline performance findings; no new findings |
| clang-tidy analyzer/bugprone CLI and middleware passes | Exit 0 with identical baseline findings; no new findings |
| Protected historical release records and tracked CLI | Unchanged; `git diff --check` passed |

The independent adversarial review found the saturation/derived-constant paths
above; failing tests preceded both follow-up corrections. Recompiled exploit
probes now reject, and the review found no further high/medium blocker in the
owned numerical transaction. Tests and CI assertions were not weakened or
disabled. The updater's existing temporary-filesystem staged build/CTest
integration also ran as part of full Python discovery; no live installation
or physical system was changed.

Reproduce from the repository root, with CMake/CTest on PATH. Create the output
hook outside the checkout so the historical tracked CLI remains unchanged:

```sh
ds_hook_dir=$(mktemp -d)
cat > "$ds_hook_dir/output-paths.cmake" <<'CMAKE'
function(ds_cloud_output_paths)
  if(TARGET ds_enhancer)
    set_target_properties(ds_enhancer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  endif()
  foreach(target ds_enhancer_shared ds_enhancer_pybind)
    if(TARGET ${target})
      set_target_properties(${target} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    endif()
  endforeach()
endfunction()
cmake_language(DEFER CALL ds_cloud_output_paths)
CMAKE
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDS_ENABLE_PYTHON_GOVERNOR=OFF -DCMAKE_PROJECT_INCLUDE="$ds_hook_dir/output-paths.cmake"
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p 'test_*.py' -v
./build/basic_integration
cmake -S . -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug -DDS_ENABLE_PYTHON_GOVERNOR=OFF -DCMAKE_PROJECT_INCLUDE="$ds_hook_dir/output-paths.cmake"
cmake --build cmake-build-debug --parallel 4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir cmake-build-debug --output-on-failure
cmake -S . -B cmake-build-pybind -DCMAKE_BUILD_TYPE=Release -DDS_ENABLE_PYTHON_GOVERNOR=ON -DCMAKE_PROJECT_INCLUDE="$ds_hook_dir/output-paths.cmake"
cmake --build cmake-build-pybind --target ds_enhancer_pybind --parallel 2
PYTHONPATH="$PWD/cmake-build-pybind/lib" python3 tests/smoke_pybind11.py
PYTHONPATH="$PWD/cmake-build-pybind/lib" python3 -m unittest discover -s tests -p 'test_*.py' -v
cmake -S . -B cmake-build-feen -DCMAKE_BUILD_TYPE=Release -DDS_ENABLE_FEEN=ON -DDS_ENABLE_PYTHON_GOVERNOR=OFF -DCMAKE_PROJECT_INCLUDE="$ds_hook_dir/output-paths.cmake"
cmake --build cmake-build-feen --target test_middleware_accounting test_feen_integration --parallel 2
ctest --test-dir cmake-build-feen -R '^(test_middleware_accounting|test_feen_integration)$' --output-on-failure
```

Without the optional module on PYTHONPATH, standard Python discovery retains
the existing single optional limp/derating skip; the recorded 188/188 run used
the module and executed that method. Actual validation used the equivalent
retained hook at `/workspace/shared/ds-ev/output-paths.cmake`, with separate
Release/Debug parallelism of two while the full builds overlapped. Raw logs were kept in
`/workspace/shared/ds-ev` rather than committed as generated output.

Static commands were run identically on an immutable baseline archive and the
final checkout, with no new suppressions:

```sh
cppcheck --enable=warning,performance,portability --inline-suppr --error-exitcode=1 --std=c++17 -I include include/ ds_core/src/
clang-tidy --checks='clang-analyzer-*,bugprone-*' --header-filter='.*include/(ds_battery_core|ds_bms_middleware_v2)\.hpp' ds_core/src/ds_enhancer.cpp -- -std=c++17 -Iinclude -Ids_core/src
clang-tidy --checks='clang-analyzer-*,bugprone-*' --header-filter='.*include/(ds_battery_core|ds_bms_middleware_v2)\.hpp' tests/test_middleware.cpp -- -std=c++17 -Iinclude
```

cppcheck was installed locally from authenticated Debian metadata, retaining
artifact verification; clang-tidy/CMake used TLS-verified packages. The direct
commands preserve failure status rather than the Makefile's ignored clang
status and logging side effects. No CI job was removed or disabled.

## Residual limitations and confirmed remaining risks

- Transactions cover ordinary sequential validation/arithmetic/exception paths
  for owned state. They add no concurrent-reader synchronization, process-crash,
  memory-exhaustion recovery, external-service or physical-system guarantee.
  Core and filter candidates are copied, with allocation overhead; no real-time
  performance guarantee is made. The pinned FEEN scorer-copy/history path was
  compiled and exercised; it adds no physical-system transaction guarantee.
- The optional Kalman model retains its existing use of raw current as its
  capacity-fade measurement proxy. This phase protects its state integrity,
  without validating that modeling choice. Multi-cell CUSTOM chemistry still
  has no registered profile; initialization throws and now preserves the prior
  valid instance. Per-cell chemistry calibration remains a separate contract.
- Python `middleware.snapshot()` has a preexisting unregistered
  `DSEnergyTelemetry` return type and raises `TypeError`; C++ telemetry validation
  passes independently. This PR does not claim Python telemetry binding support.
- Static analysis retains four baseline cppcheck `returnByReference` performance
  findings and the AILEE adapter's `initialize() noexcept` calling potentially
  throwing initialization. Exception termination on an actual initialization
  failure remains a risk in that adjacent adapter; it is not fixed here.
- PB-09 recovery and PB-10 through PB-12 learning/audit integrity remain open.
  No physical battery-model validity, vehicle behavior, worst-case latency,
  release readiness or certification is established by these software checks.
