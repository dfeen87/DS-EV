# DS-EV v8.0.2: trusted legacy maintenance migration

This explicitly initiated administrator procedure replaces four maintenance
helpers in `/opt/ds_enhancement`, after a separately verified recovery snapshot,
so that a safe legacy installation can meet PR #44's hardened updater contracts.
It does not install a release, configure provenance, or relax an updater gate.

**No trustworthy migration evidence means no migration success, no deployment,
and no security bypass.** Read the recovery procedure before starting. Development
tests never execute these commands against a live installation.

## The boundary and chosen design

Legacy diagnostics could succeed without identifying their safety evidence.
The hardened updater requires `--updater-evidence`, a protected configuration
directory, a fresh nonce, and exactly one `DS_UPDATER_EVIDENCE_V1:` receipt with
the matching contract, directory, nonce, and four document hashes. A legacy zero
exit cannot substitute for that receipt; temporary status files, environment
overrides, presets, and downloaded defaults cannot replace installed evidence.

Legacy rollback cannot establish the required recovery contract: a verified
manifest of exact software paths, bytes, sizes, ownership, and modes, in protected
root-owned storage. This procedure never executes the old rollback helper.

The smallest migration is a manual, ordered runbook plus
`scripts/legacy_migration.py`, a check-only offline utility. The administrator
installs precisely these authenticated source files as new root-owned,
root-group, `0755` replacements in `bin/`:

| Source | Installed path | Purpose |
|---|---|---|
| `scripts/update.py` | `bin/update.py` | Hardened updater and its prerequisite gates |
| `scripts/diagnostics.py` | `bin/diagnostics.py` | Protected diagnostic evidence contract |
| `scripts/backup_restore.py` | `bin/backup_restore.py` | Verified snapshots and restoration checks |
| `scripts/rollback.sh` | `bin/rollback.sh` | Hardened backup helper entry point |

The external checker authenticates all five source files before loading helper
code, checks installation/storage, runs the real diagnostic gate, and compares
live software with the selected baseline. It writes only private temporary
diagnostic scratch logs. It neither locks, backs up, installs, repairs, downloads,
nor restores; the administrator performs the explicit maintenance steps.

Automatic bootstrap was rejected because legacy diagnostics/recovery would be
trusted to replace themselves. Unverified downloads, live Git pulls, legacy
success substitution, and selecting a latest old backup cross that boundary.
Full installation/rollback could overwrite configuration; a migration framework
or new transport is unnecessary.

## Administrator prerequisites and failure classification

Use an authorized root session and trusted OS Python, Bash, `flock`, `sha256sum`,
and `install`. CMake, CTest, and a C++ compiler must be available. Diagnostic
hardware acceptance follows the authoritative installed policy; never enable
simulation/overrides to make migration pass. A policy accepting simulation does
not prove physical interface readiness.

Require canonical protected root-owned live/source paths and ancestors, without
group/other write access. All six managed directories must exist: `bin`, `lib`,
`python`, `python_fallback`, `config`, `docs`. Contents must be directories or
regular files without links, hard-linked files, special files, or privileged mode
bits. Existing `backups/` is checked too. Source must be outside the live tree and
separate from recovery storage.

| Finding | Classification and action |
|---|---|
| Safe legacy helper bytes differ from approved source | Compatibility limitation; the procedure below may replace those helpers |
| Protected legacy backup without a verified-format manifest | Unverified historical backup; report and retain it, never select it for this migration |
| Missing, corrupt, changed, or incomplete selected snapshot | Recovery prerequisite failure; stop before replacement or recover manually if replacement started |
| `.incomplete-*` entry, or an invalid verified-format backup in live storage | Interrupted/corrupt state; stop for manual investigation |
| Wrong ownership, writable metadata/ancestors, links, privileged bits, unsafe layout | Unsafe installation/source/storage; stop for a separate administrator investigation |
| Missing/malformed safety documents or failing real diagnostics | Evidence or operational failure; stop, obtain authoritative evidence through the normal operational process |
| `sha256: null`, Git preference, mock policy, or missing release identity | Update-provenance prerequisite failure; no migration success |

Never repair a failed inspection with `chmod`, `chown`, defaults, or fabricated
evidence. Explicit metadata on authenticated replacements is authorized only
after recovery verification. Prior tampering, unknown software authenticity, or
stale/bogus status requires investigation: compatibility/integrity checks cannot
remove malware or establish sensor freshness.

The shipped `sha256: null` deliberately fails inspection. Before migration,
separately authorize/provision a genuine HTTPS release URL and independently
trusted release digest using the site's configuration/recovery process, or keep
updates disabled and stop. Migration preserves all configuration, thresholds,
measurements, and operator provenance; it never invents a missing release digest.

## Independent trust anchors

Obtain an approved maintenance source revision containing PR #44 and this
procedure from a separately reviewed distribution. Record its full lowercase
40-character Git commit identifier. An authorized trusted maintainer must
provide a manifest describing these exact five files:

```json
{
  "format": "ds-ev-maintenance-v1",
  "version": "8.0.2",
  "commit": "<approved full lowercase 40-character commit identifier>",
  "files": {
    "scripts/update.py": "<SHA-256 of approved file>",
    "scripts/diagnostics.py": "<SHA-256 of approved file>",
    "scripts/backup_restore.py": "<SHA-256 of approved file>",
    "scripts/rollback.sh": "<SHA-256 of approved file>",
    "scripts/legacy_migration.py": "<SHA-256 of approved file>"
  }
}
```

These placeholders are invalid pins. Obtain the manifest SHA-256 and approved
commit through an independent authenticated channel, such as signed maintenance
authorization or a trusted offline handoff. Retain that authorization. A digest
downloaded beside an unauthenticated artifact or computed after download alone
does not establish trust. Pinning exact manifest bytes through that channel is
sufficient; the JSON need not itself contain a signature.

Authenticate the checker before executing it. It then revalidates all five
files, including itself, and the pinned identity on every invocation; it does
not authenticate Git history. This change publishes no bundle, release archive,
manifest pin, or tag. Approved distribution/provenance remain prerequisites.

## Ordered administrator procedure

After explicit authorization, use a fresh trusted root Bash shell and recorded
pins. The reviewed source/manifest must already exist at protected external
locations. Do not overwrite/repair existing source or work inside the live tree.

```bash
/bin/bash --noprofile --norc
set -euo pipefail
umask 077
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
cd /root
test "$(id -u)" -eq 0

LIVE=/opt/ds_enhancement
SRC=/root/ds-ev-approved-maintenance
MANIFEST=/root/ds-ev-approved-maintenance.json
APPROVED_COMMIT='<independently authorized full lowercase commit>'
MANIFEST_SHA256='<independently authenticated manifest SHA-256>'
LOCK=/opt/.ds_enhancement.update.lock
```

### 1. Authenticate the source before executing it

Inspect source, manifest, `/opt`, and ancestors with trusted `namei -l`/`stat`.
Require canonical protected root-owned paths and single-link regular files.
This OS-only Python check verifies those conditions and authenticates all five
files before any source code is loaded. Stop on any discrepancy.

```bash
/usr/bin/python3 -I -B - "$SRC" "$MANIFEST" "$MANIFEST_SHA256" "$APPROVED_COMMIT" <<'PY'
import hashlib, hmac, json, os, pathlib, re, stat, sys
source, manifest, pin, commit = sys.argv[1:]
names = {"scripts/" + n for n in (
    "update.py", "diagnostics.py", "backup_restore.py", "rollback.sh", "legacy_migration.py")}
def protected(value, directory=False):
    path = pathlib.Path(value).absolute()
    if path.resolve() != path:
        raise SystemExit("STOP: noncanonical or symlink path " + str(path))
    for entry in (path,) + tuple(path.parents):
        info = entry.lstat()
        is_dir = entry != path or directory
        if (not (stat.S_ISDIR(info.st_mode) if is_dir else stat.S_ISREG(info.st_mode))
                or info.st_uid != 0 or info.st_mode & 0o022
                or (not is_dir and info.st_nlink != 1)):
            raise SystemExit("STOP: unsafe ownership/type/mode " + str(entry))
    return path
if not re.fullmatch(r"[0-9a-f]{40}", commit):
    raise SystemExit("STOP: invalid approved commit")
if not re.fullmatch(r"[0-9a-fA-F]{64}", pin) or pin == "0" * 64:
    raise SystemExit("STOP: missing independent manifest pin")
source = protected(source, True)
data = protected(manifest).read_bytes()
if not hmac.compare_digest(hashlib.sha256(data).hexdigest(), pin.lower()):
    raise SystemExit("STOP: source manifest hash mismatch")
record = json.loads(data)
if (not isinstance(record, dict) or record.get("format") != "ds-ev-maintenance-v1"
        or record.get("version") != "8.0.2" or record.get("commit") != commit
        or not isinstance(record.get("files"), dict) or set(record["files"]) != names):
    raise SystemExit("STOP: source manifest identity/file set mismatch")
for name in sorted(names):
    expected = record["files"][name]
    if (not isinstance(expected, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", expected)
            or expected == "0" * 64):
        raise SystemExit("STOP: invalid file pin " + name)
    actual = hashlib.sha256(protected(source / name).read_bytes()).hexdigest()
    if not hmac.compare_digest(actual, expected.lower()):
        raise SystemExit("STOP: source hash mismatch " + name)
print("Authenticated exact approved maintenance source; no source code executed")
PY
```

Keep the source and manifest protected against changes for the whole session.
Do not execute a checker that failed this authentication.

### 2. Quiesce writers, validate and hold the external updater lock

Use the site's documented procedure to stop services, scheduled updaters, and
all managed-directory writers, including status writers. Record the actual
installation-specific services and establish safe maintenance operation. Never
refresh/fabricate status to pass. Checks cannot stop uncooperative writers or
establish physical measurement freshness.

Validate the existing updater lock without repair; create a protected lock only
when absent. Wrong owner/type/modes or links mean stop. Never replace a lock.

```bash
/usr/bin/python3 -I -B - "$LIVE" "$LOCK" <<'PY'
import os, pathlib, stat, sys
live, lock = map(pathlib.Path, sys.argv[1:])
if live.absolute() != live or live.resolve() != live:
    raise SystemExit("STOP: noncanonical installation path")
if lock != live.parent / ("." + live.name + ".update.lock"):
    raise SystemExit("STOP: incorrect updater lock path")
for parent in (live.parent,) + tuple(live.parent.parents):
    info = parent.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
        raise SystemExit("STOP: unsafe lock parent " + str(parent))
try:
    fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_NOFOLLOW, 0o600)
except FileExistsError:
    fd = os.open(lock, os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK)
try:
    info = os.fstat(fd)
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != 0
            or info.st_mode & 0o022 or info.st_nlink != 1):
        raise SystemExit("STOP: unsafe updater lock")
finally:
    os.close(fd)
PY
exec 9<>"$LOCK"
flock --nonblock --exclusive 9
```

Hold descriptor 9 through maintenance/recovery. A busy lock means stop. Prevent
concurrent root maintenance/tampering administratively; protected ancestors
exclude other local writers. Do not run the updater while holding this lock.

### 3. Inspect without changing the installation

```bash
COMMON=(--source "$SRC" --manifest "$MANIFEST"
        --manifest-sha256 "$MANIFEST_SHA256" --commit "$APPROVED_COMMIT"
        --install-dir "$LIVE")
/usr/bin/python3 -I -B "$SRC/scripts/legacy_migration.py" inspect "${COMMON[@]}"
```

Require zero exit and `CHECKS PASSED; NO MIGRATION PERFORMED`. `matching_helpers`
lists already authenticated matches; safe legacy differences are expected.
`unverified_legacy_backups` are never selected recovery points. Inspection runs
source diagnostics against installed configuration, never legacy code. Any
`BLOCKED:` result ends the procedure before live changes.

### 4. Create and pin a new recovery baseline with authenticated external tooling

Never reuse an old backup or select `latest`. The authenticated external helper
creates a new snapshot of all six quiesced directories, including configuration
for comparison. A unique new private recovery directory avoids overwriting prior
storage. Migration does not replace configuration.

```bash
SNAPSHOT_ID="$(date -u +%Y%m%dT%H%M%SZ)-$APPROVED_COMMIT"
RECOVERY="/root/ds-ev-recovery-$SNAPSHOT_ID"
mkdir -m 0700 -- "$RECOVERY"
SNAPSHOT="$RECOVERY/$SNAPSHOT_ID"
/usr/bin/python3 -I -B "$SRC/scripts/backup_restore.py" backup \
    --install-dir "$LIVE" --backup-dir "$RECOVERY" --snapshot-id "$SNAPSHOT_ID"
SNAPSHOT_SHA256="$(sha256sum -- "$SNAPSHOT/manifest.json")"
SNAPSHOT_SHA256="${SNAPSHOT_SHA256%% *}"
printf '%s  %s\n' "$SNAPSHOT_SHA256" "$SNAPSHOT/manifest.json" \
    > "$RECOVERY/recovery-manifest.sha256"
printf '%s\n' "$APPROVED_COMMIT" > "$RECOVERY/source-commit.txt"
printf '%s\n' "$MANIFEST_SHA256" > "$RECOVERY/source-manifest.sha256"
/usr/bin/python3 -I -B "$SRC/scripts/legacy_migration.py" before "${COMMON[@]}" \
    --snapshot "$SNAPSHOT" --snapshot-sha256 "$SNAPSHOT_SHA256" \
    | tee "$RECOVERY/before.json"
```

Require successful backup and zero-exit `before` with
`CHECKS PASSED; NO MIGRATION PERFORMED`. The pinned snapshot must exactly match
the entire live baseline, including metadata. Before proceeding, retain the
**original** source authorization/commit/manifest pin, snapshot path/pin, and
before-result in an independently protected site journal; a lost shell or
altered local sidecar cannot authorize a different baseline. The snapshot pin
binds verified pre-change state; it is separate from source/release trust.

Self-consistency of an old snapshot is insufficient. Mismatch, interruption,
backup error, or lost pin means no replacement. Establish a known intact
pre-migration installation before any new baseline; never snapshot partial state.

### 5. Explicitly replace only the four authenticated helpers

Repeat `before` immediately before replacement. Executing these four `install`
commands explicitly authorizes only these replacements. Never substitute
`make install`, `make update`, `git pull`, default copies, or old rollback.

```bash
/usr/bin/python3 -I -B "$SRC/scripts/legacy_migration.py" before "${COMMON[@]}" \
    --snapshot "$SNAPSHOT" --snapshot-sha256 "$SNAPSHOT_SHA256"
install -o root -g root -m 0755 -- "$SRC/scripts/update.py" "$LIVE/bin/update.py"
install -o root -g root -m 0755 -- "$SRC/scripts/diagnostics.py" "$LIVE/bin/diagnostics.py"
install -o root -g root -m 0755 -- "$SRC/scripts/backup_restore.py" "$LIVE/bin/backup_restore.py"
install -o root -g root -m 0755 -- "$SRC/scripts/rollback.sh" "$LIVE/bin/rollback.sh"
```

Replacement is sequential, without atomicity, journaling, or power-loss safety.
Failure/interruption means stop, keep services quiesced, and recover below.

### 6. Verify installed bytes, metadata, unchanged evidence, and the real contract

```bash
/usr/bin/python3 -I -B "$SRC/scripts/legacy_migration.py" verify "${COMMON[@]}" \
    --snapshot "$SNAPSHOT" --snapshot-sha256 "$SNAPSHOT_SHA256" \
    | tee "$RECOVERY/verification.json"
```

Success requires exit zero and `"status": "MIGRATION VERIFIED"` in the result.
The checker requires exactly the original baseline with those four files set to
the authenticated bytes, root ownership, root group, and `0755` permissions.
Every other path in the six managed directories must retain its bytes and
metadata, including all configuration and safety evidence. It runs the real
installed diagnostic helper using protected installed configuration, a new
nonce, isolated Python imports, and private temporary logs. It requires the
exact diagnostic receipt and unchanged evidence, then rechecks source, recovery
storage, and live state. No remote request or deployment occurs.

Retain the zero-exit verification result, configuration hashes, source commit,
independent authorization, selected snapshot, and recorded snapshot pin. An
output file alone, a successful backup alone, or a zero exit from an earlier
step is not migration success. If the command fails, there is no migration
success even when all four copy commands returned zero.

Only after successful verification may the administrator close descriptor 9
and resume services through the site's maintenance procedure:

```bash
exec 9>&-
```

Keep the selected recovery point protected according to the site's retention
policy. A later independently authorized update must still use the normal
hardened updater. HTTPS/TLS and archive SHA-256 verification, protected policy,
archive validation, current/staged/deployed diagnostic gates, staged build and
CTest, a newly verified update snapshot, deployment comparison, and restoration
checks all remain mandatory. Offline migration verification cannot establish
remote release availability or certify a future payload.

## Interrupted invocation and manual recovery

Repeat `verify` after complete migration using the same original pins/baseline.
`before` fails on changed original bytes/metadata; `verify` fails on partial or
unexpected changes. Both may pass if all original helpers already matched and
nothing changed. `inspect` alone cannot distinguish a legacy installation from
an interrupted attempt. Never snapshot partial state as a new baseline. There is
no transaction journal; exact original-baseline comparisons establish state.

After interruption, keep services stopped, reacquire the protected lock, and
reauthenticate source/checker as in step 1. Recover original source/snapshot pins
from the independently protected journal. Lost pins/authorization require site
recovery, not reconstructed trust. Preserve any later configuration/status changes
for investigation; never overwrite them with a full restore.

Do not run the old installed rollback script. Do not run the external
`backup_restore.py restore` or `rollback` commands for helper-only recovery:
those restore all managed directories and would replace configuration. Recover
only the four helpers from the explicitly selected baseline, preserving every
other live file.

Before recovery writes, run the check-only recovery gate with the original pins.
It authenticates the source and verifies the entire pinned snapshot even if
current diagnostics/configuration cannot pass. It does not label migration success.

```bash
/usr/bin/python3 -I -B "$SRC/scripts/legacy_migration.py" recovery "${COMMON[@]}" \
    --snapshot "$SNAPSHOT" --snapshot-sha256 "$SNAPSHOT_SHA256"
```

Require zero exit and `RECOVERY SNAPSHOT VERIFIED; NO MIGRATION SUCCESS`. The
`original_helpers` mapping contains each original manifest entry, or null for
original absence. Before copying, inspect targets/ancestors again; unsafe paths,
owners, permissions, links, or special files require site recovery, not repair.

For each of the four original file entries, explicitly restore its
selected `payload/bin/<helper>` using the recorded owner (necessarily root),
group, and mode. Convert the manifest's decimal mode to octal with trusted OS
tooling and verify the values against the record. For example, the exact
administrator command form is:

```bash
install -o root -g '<recorded numeric gid>' -m '<recorded octal mode>' -- \
    "$SNAPSHOT/payload/bin/<helper>" "$LIVE/bin/<helper>"
```

Substitute only one of the four helper names and its original metadata. If its
`original_helpers` entry is null, explicitly remove only that newly introduced
protected regular file (`rm -- "$LIVE/bin/<helper>"`) after confirming the target.
Do not remove other paths. Any unsafe/missing/corrupt payload or failed operation
means stop and use site recovery with the retained snapshot/evidence.

After restoring all four, run `before` from step 4 with the original pins. Only
zero exit and an exact whole-tree baseline match verifies recovery; recovery is
not migration success. If unrelated configuration changed, this fails and those
changes remain preserved. Leave services stopped and investigate. A new attempt
starts from an established intact baseline, retaining the failed record; never
fabricate evidence, choose another backup, or report success to hide a failure.

## Regression scope and limits

Focused regression tests use temporary filesystems and fixture owners or mocked
privileged operations. They never run installation, rollback, or mutation against
`/opt/ds_enhancement`.

| Scenario | Required result |
|---|---|
| Legacy diagnostics lacking the receipt interface | Normal updater still rejects them; inspection executes only authenticated source diagnostics |
| Valid source and installed policy/evidence | Check-only inspection succeeds without changing live/source/snapshot bytes |
| Missing/wrong manifest digest, wrong commit/file set/helper bytes | Reject before trusted helper execution or live modification |
| Unsafe ownership, modes, source/live ancestors, links | Reject and leave existing metadata unchanged |
| Missing, corrupt, unverified, or mismatched selected snapshot | Reject `before`/`verify`; no migration success |
| Safe historical backups versus corrupt/interrupted backup storage | Report historical entries as unverified; fail closed on corrupt or interrupted storage |
| Failure before copying | Original live installation remains unchanged |
| Partial helper replacement/interruption | Original `before` comparison and post-migration `verify` fail; selected snapshot remains verifiable |
| Successful four-helper replacement | Exact installed hashes/root metadata plus real diagnostic receipt pass |
| Configuration, safety evidence, or unrelated software changed | Fail exact baseline comparison; preserve changes for manual investigation |
| Failed diagnostics or bad receipt after replacement | No migration success and no deployment |
| Repeat invocation | Complete verification may repeat; partial/changed state cannot be rebased into success |
| Missing release pin or legacy/mock policy | Fail normal policy validation; migration cannot provision or bypass provenance |
| Successful migration followed by normal update | Every existing PR #44 updater gate remains required |

Reproduce focused checks from a development checkout:

```bash
python3 -m unittest discover -s tests -p test_legacy_migration.py -v
python3 -m unittest discover -s tests -p test_update.py -v
python3 -m unittest discover -s tests -p test_backup_permissions.py -v
```

Run the repository's compatible Python suite and CMake/CTest workflow as reported
in [the v8.0.2 release report](RELEASE_8.0.2.md). Test success establishes software
behavior in those fixtures, not a real privileged installation, actual release
hosting, battery measurements, hardware readiness, or vehicle certification.

The installation and its privileged administrator/OS are trust prerequisites.
The checker does not detect already compromised root authority, malicious
approved source, or stale but structurally valid vehicle evidence. Quiescence
and the external lock reduce concurrent maintenance; neither supplies a
filesystem transaction. Snapshot integrity is rechecked, but disks may fail and
file copying can be interrupted. No atomicity, crash recovery, or power-loss
safety is asserted. Keep an independent site recovery plan and protect the
selected snapshot and authorization outside the live installation.
