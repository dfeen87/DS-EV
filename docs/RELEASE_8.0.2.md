# DS-EV v8.0.2 — fail-closed updater hardening

This is the v8.0.2 development release target, based on main
`6ed0980e7bfcc1df4952633587e2ac8a770b0580` (merged PR #43). No release tag,
production deployment, or hardware certification is part of this change.
`RELEASE_8.0.0.md` and `RELEASE_8.0.1.md` remain unchanged historical records.
DS mathematics, BMS logic, torque/regen algorithms, AILEE governance, and public
C++ interfaces are unchanged; the C++ patch version is release metadata only.

## Acceptance invariant

No trustworthy update source, no validated payload, no passing required
diagnostics, or no viable verified backup means **no deployment and no success**.
The updater requires an existing installation; initial installation remains a
separate administrator operation.

## Exact corrections

- Replace the unsupported `urlretrieve(..., timeout=5)` call with streamed
  `urllib` HTTPS requests using supported socket timeouts, a monotonic elapsed
  budget, and HTTPS-only redirects. Default limits are 5 seconds per configured
  budget, 64 MiB downloaded, 256 MiB expanded archive data, and 10,000 members.
  Invalid limits, missing assets, timeouts, truncated downloads, and HTTP errors
  return failure. TLS verification stays enabled. These are supported I/O
  limits, not a hard operating-system whole-process deadline for DNS/header
  parsing.
- Require a separately provisioned, trusted SHA-256 pin before requesting an
  archive and compare the downloaded bytes against it. Production configuration
  and its parents must be root-owned and protected from other writers.
  The shipped `sha256: null` deliberately disables updates. No checksum is
  invented, obtained from the same unauthenticated payload, or bypassed.
- Remove local mock substitution and automatic archive fabrication completely.
  Legacy production mock/simulation settings are rejected. Tests inject network
  responses and commands through Python APIs; there is no configuration switch
  or command-line production simulation mode.
- Disable the git update path with a clear error when `prefer_git` is true.
  The updater never pulls, resets, or replaces the caller's checkout. An
  authenticated archive is the only implemented update source.
- Validate required commands, source files, diagnostic configurations, diagnostic
  scripts, compiled artifacts, and a nonempty registered CTest suite. Current
  and staged diagnostics, staged configuration/build, and staged tests must pass
  before any deployed software is replaced.
- Validate the entire gzip/tar stream before writing staged members. Reject
  absolute/traversal paths, links, devices/FIFOs, sparse/unsupported types,
  privileged permission bits, duplicate paths, file/directory collisions, excess
  members, and excessive expansion. Extraction writes regular files/directories
  into a separate empty temporary directory.
- Build from the validated source in staging and discard archived build outputs.
  Prepare a complete deployment layout before backup/deployment. Preserve the
  entire operator configuration, including vehicle status, safety policy, and
  the trusted update-source pin, rather than installing package defaults.
- Use `backup_restore.py` behind `rollback.sh` to create a unique snapshot with
  a manifest of exact paths, file SHA-256/size, permissions, and ownership.
  Reject unsafe backup locations, symlinks/special files, incomplete snapshots,
  and concurrent changes observed during backup. Independently compare the
  snapshot with the pre-update installation; a zero exit status alone cannot
  satisfy the gate. Copy the trusted rollback helper outside the replaced tree
  and restore the exact snapshot identifier on failure.
- Serialize updater instances using an external lock. After copying, verify the
  deployment against its prepared manifest and run deployed diagnostics. Any
  recoverable deployment or post-check failure returns nonzero even when
  restoration succeeds. Restoration is reported as verified only after its
  command succeeds and the restored installation matches the original manifest;
  failed restoration explicitly requires manual recovery.

## Diagnostic compatibility

Current installed diagnostics retain their existing CLI and evidence precedence,
including configured `DS_*` overrides and the existing `/tmp/vehicle_status.json`
behavior. The v8.0.1 script does not support `--config-dir`, so that new argument
is used only for staged and deployed v8.0.2 checks. Those checks use a copied
current configuration directory and cannot substitute the payload's simulated
vehicle fixture. Diagnostic failure still blocks deployment.

`diagnostics.py --config-dir` provides an exclusive configuration/status source;
the legacy default search behavior is unchanged. An explicit `--log-dir` now
routes logs exclusively to that directory so staged validation does not write
live or checkout logs. These additions change diagnostic I/O, not battery checks.

## Regression and verification matrix

| Boundary | Expected outcome | Verification |
|---|---|---|
| Missing release asset / network or stream timeout | Nonzero; no fallback/deployment | Focused unittest regressions |
| Missing/malformed/wrong digest; corrupt archive | Reject before deployment | Focused unittest regressions |
| Traversal, absolute paths, links, unsupported types, collisions, bombs | Reject before staged extraction | Focused unittest regressions |
| Missing source or unsafe git policy | Nonzero; checkout/live software unchanged | Focused unittest regressions |
| Missing/failing current or staged diagnostics / required command | Nonzero; no deployment | Focused unittest regressions |
| Missing/failing/lying backup; unsafe/corrupt snapshot | Nonzero; no deployment | Focused unittest regressions |
| Staged build/tests fail or CTest suite is empty | Nonzero; original installation intact; rollback unnecessary | Focused unittest regressions |
| Copy/post-deployment check fails | Nonzero; exact backup restored and verified, or explicit restoration failure | Focused unittest regressions |
| Valid SHA-pinned payload and all gates pass | Verified deployment, preserved configuration, zero status | Temporary-filesystem regressions and full staged integration |
| Production mock/fabricated fallback | Always rejected; never labeled genuine success | Focused unittest regressions |

Validation completed with these distinct outcomes:

| Check | Outcome |
|---|---|
| New updater/backup/diagnostics I/O regressions | 66 passed, 0 skipped |
| Existing Python automotive/governance suite | 11 passed |
| Compatible existing diagnostic boundary tests | 6 passed; logs redirected to temporary storage |
| Optional existing pybind limp/derating test | 1 skipped: optional extension absent |
| Migrated installer and backup/rollback tests | 2 passed against temporary fixtures; no sudo/live operations |
| Full Python unittest discovery | 86 run: 85 passed, 1 optional pybind skip; no failed or blocked checks |
| v8.0.2 Release CMake build and CTest | Build passed; all 14 registered tests passed |
| Full updater integration with real staged CMake build/CTest | Passed against a temporary installation; deployed CLI reports `DS-EV 8.0.2` and configuration is preserved |
| Remote release availability, privileged deployment, hardware | Unrun; no production release asset is claimed |

Tests use temporary installation roots and injected network/privileged command operations. No test
runs `sudo make install`, `make update`, live rollback, or mutates
`/opt/ds_enhancement`.

Reproduce focused checks from the repository root:

```bash
python3 -m unittest discover -s tests -p test_update.py -v
python3 -m unittest discover -s tests -p test_ailee_automotive.py -v
```

Use the repository CMake/CTest workflow for the C++ tests. During this validation,
an external `CMAKE_PROJECT_INCLUDE` deferred only the CLI output path to preserve
the tracked historical `ds_core/bin/ds_enhancer`; source files and assertions were
unchanged. Toolchain: GCC 14.2, CMake 3.31.10, Python 3.12.14, Release, optional
FEEN/GPU/embedded Python disabled.

The formerly privileged methods in `test_hardened_pipeline.py` now use a copied
temporary checkout and installation. The installer case executes the actual
Makefile recipes with injected privilege/build/backup commands, real diagnostic
checks, explicitly temporary destination/log paths, and checks that failed gates
prevent mutation and success logging. Backup/restore uses the real verified helper
against a temporary tree. The class's checked CMake build redirects distribution
outputs externally. All original diagnostic/pybind boundary assertions remain;
installer and backup assertions are strengthened. Full Python discovery can now
run safely without the former sudo/live `/opt` operations. Existing CI
configuration is unchanged. The full updater integration
uses the actual repository source archive with a trusted test pin, real diagnostic
processes, a real staged build and CTest run, and actual temporary directory copies;
only the network response and privileged shell dispatcher are injected. Snapshot
and restoration algorithms are exercised directly in temporary roots by the
regression suite. This does not establish actual release-server availability.

## Deployment prerequisites and limitations

The published v8.0.1 release has no `payload.tar.gz` asset. Its absence is a real
failure. This change does not publish a v8.0.2 release asset: an administrator must
produce/review a genuine archive with project files at the archive root, provision
its exact SHA-256 through a trusted channel, and configure its HTTPS URL. The
archive must contain the required hardened updater/diagnostic/backup helpers;
production uses the existing trusted helper for recovery, not the downloaded one.
Previously installed helpers require a separately reviewed administrative
bootstrap of the hardened tooling. Legacy manifest-less backups are not silently
accepted as verified snapshots; retain them for separately assessed manual recovery.

Run only during maintenance with services quiescent and sufficient space for the
download, expanded source, build, prepared deployment, and full backup. The lock
coordinates updater instances; it does not serialize separate installation,
standalone rollback, or arbitrary file writers. Root-owned configuration/helpers
and a trusted executable search path are prerequisites. SHA-256 pins authenticate
the administrator's chosen bytes; they do not establish publisher identity,
release quality, or protection against an administrator selecting an older release.
Standalone rollback verifies the snapshot manifest's consistency; administrators
must retain snapshots in protected, trusted storage because standalone recovery
does not have the updater's separately captured original manifest.

Deployment and restoration replace software directories by file copying. They
are **not atomic/transactional**, and power loss, process termination, storage
failure, or rollback failure can leave a partial installation requiring manual
recovery. Backups cover managed software directories, not external vehicle state,
logs, arbitrary system files, ACLs/xattrs, or running processes. This is software
validation, without hardware, firmware deployment, timing certification, or
automotive safety certification. No CI assertions or existing tests are weakened.
