# Power Capacity

Power Capacity is the electrical-capacity authority for facility power domains.

It answers one question, precisely and with explicit authority:

> How much electrical capacity is allocatable now, through which power domains and
> redundancy assumptions, after protected load, reserve, derating, and current
> evidence are applied?

It owns the electrical-capacity *model*, not electrical actuation. It consumes
typed evidence about feeds, PDUs, UPS domains, generators, circuits, containment,
load, derating, maintenance and degraded state, redundancy, and policy; applies
checked integer accounting in exact fixed units; and refuses to produce a number
that current evidence and authority cannot justify.

The product is a reusable C++20 library with a small administration and
inspection command line tool, three runnable examples, and a completed-operation
benchmark. There is no graphical interface and no telemetry: the library performs
no network I/O of any kind.

## Contents

- [The question, answered exactly](#the-question-answered-exactly)
- [Systems boundary](#systems-boundary)
- [Units and exact accounting](#units-and-exact-accounting)
- [The capacity model](#the-capacity-model)
- [Redundancy](#redundancy)
- [Rollups, shared domains, and bottlenecks](#rollups-shared-domains-and-bottlenecks)
- [Candidate load evaluation](#candidate-load-evaluation)
- [Evidence, generations, and authority](#evidence-generations-and-authority)
- [Persistence](#persistence)
- [Recovery](#recovery)
- [Concurrency and lock order](#concurrency-and-lock-order)
- [Process authority](#process-authority)
- [Error model](#error-model)
- [Command line tool](#command-line-tool)
- [Library use](#library-use)
- [Examples](#examples)
- [Build, test, and install](#build-test-and-install)
- [Validation](#validation)
- [Benchmarks](#benchmarks)
- [Honest limitations](#honest-limitations)
- [License](#license)

## The question, answered exactly

For one power domain, in order:

| Stage | Definition |
| --- | --- |
| `nominal` | Nameplate capacity declared by evidence. |
| `usable` | Capacity after fixed conversion losses. Must not exceed `nominal`. |
| `derated` | `floor(usable × derate_ratio)`, exact, rounded down. |
| `operational` | `floor(derated × degradation_ratio)`, or `0` when the domain is unavailable. |
| `reserve` | Per the declared reserve policy: none, a fixed quantity, a fraction of `operational`, or the greater of the two. |
| `safe_capacity` | `operational − reserve`, clamped at zero. |
| `protected_load` | The rolled-up load classified as protected. |
| `carryable_capacity` | `safe_capacity − protected_load`, clamped at zero. |
| `committed_load` | The rolled-up load committed under an external authority. |
| `allocatable_headroom` | `carryable_capacity − committed_load`, clamped at zero. |

Every subtraction reports whether it clamped, so a clamped zero is never presented
as an ordinary zero. Every failure to justify a value is reported as UNKNOWN with
a stable reason code and is never reported as zero. Rounding is always toward zero,
which for non-negative capacity is the conservative direction: derating can never
invent capacity.

The answer also names the **bottleneck**: the most restrictive domain on the path
from the queried domain up to the root, and the redundancy group limit that applies
at each step.

## Systems boundary

### Owned

- Typed power-domain identities, classification metadata, and at most one immediate
  upstream domain per domain.
- Capacity generations, source generations, control-plane epoch and incarnation
  binding, attempt identity, and revision identity.
- Nameplate, usable, derated, operational, reserved, protected, committed, safe,
  carryable, and allocatable quantities in exact fixed units.
- Derating, degradation, reserve policy, and redundancy obligation as explicit
  declarations.
- Evidence provenance and freshness for every fact the model depends on.
- Deterministic rollups, shared-domain accounting, bottleneck analysis, candidate
  load evaluation, explanation, and revalidation.
- Versioned, integrity-checked persistence with an explicit commit point,
  anti-rollback generation floors, identity and path binding, idempotent retries,
  and cross-process writer authority.

### Not owned

Power Capacity deliberately does not implement, and does not contain:

- **Power Control Plane** — it does not command electrical equipment.
- **Power Topology** — it consumes a containment projection as evidence; it does
  not publish topology authority.
- **Feed Authority**, **PDU Control**, **UPS Control**, **Generator Control** — it
  does not own, switch, or operate any of them.
- **Load Shedding** — it classifies protected load for accounting; it never sheds.
- **Energy Ledger** — it performs no energy or time integration. There is no kWh
  accounting anywhere in this repository.
- **Facility Capacity** (runtime 14) — it does not compose space, rack, power, and
  cooling into one facility-wide quantity.
- **Facility Capacity Reservation** (runtime 15) — it does not grant, hold, or
  release reservations.

The last point is enforced by the type system, not by convention. A committed load
is a `LoadRecord` that **must** cite the `AuthorityRef` of the authority outside
Power Capacity that granted it; a record without one is refused with
`invalid_argument`. Candidate evaluation returns an advisory verdict and never
mutates anything: `evaluate` and `evaluate_batch` are `const`, and a candidate
that is reported `admissible` leaves the committed model and the generation
untouched.

### Adjacent-runtime separation

| Layer | Owns | Power Capacity's relationship |
| --- | --- | --- |
| ASI (Accelerated Systems Infrastructure) | Accelerator execution, memory, serving, scheduling, reusable state, accelerator-resource semantics | None. No accelerator concept appears in this model. |
| DFI (Distributed Fabric Infrastructure) | Network topology, paths, transport, network authority, congestion, fabric federation | None. No network concept appears in this model. |

Dual-corded delivery, where a load can be served from two independent sources, is
modelled with a **redundancy group**, not with a second upstream domain. A domain
has at most one immediate upstream domain, so the containment structure is a
forest. Multi-parent containment is explicitly **unsupported**.

## Units and exact accounting

`Power` stores signed 64-bit **milliwatts**. No floating point is used for any
authoritative quantity; the only floating point in the repository is inside the
benchmark's reporting. `Power::to_watts_string()` renders exact decimal text for
presentation.

`Ratio` stores **basis points** (1/10000) in a signed 32-bit integer. There is no
default constructor: a ratio must be stated explicitly, so a derating factor can
never be silently zero because a field was left unset.

Every arithmetic operation is checked:

- `add_power` and `subtract_power` fail with `limit_exceeded` on 64-bit overflow.
- `subtract_power_clamped` clamps at zero and reports that it did.
- `mul_div_floor` computes `floor(value × multiplier ÷ divisor)` with a 128-bit
  intermediate product. It fails rather than truncating when the quotient cannot
  be represented, and it never loses the low bits of the product.
- `Power::from_watts`, `from_kilowatts`, and `from_megawatts` fail with
  `limit_exceeded` rather than overflowing.

Configured bounds, checked before allocation or arithmetic:

| Bound | Default |
| --- | --- |
| Store artifact size, and payload size | 256 MiB |
| Domains | 200000 |
| Loads | 500000 |
| Redundancy groups | 100000 |
| Members per redundancy group | 256 |
| Redundancy tolerance | 64 |
| Sources | 4096 |
| Containment depth | 64 |
| Identifier length | 128 bytes |
| Label length | 256 bytes |
| Store path length | 4096 bytes |
| Applied idempotency keys | 256 |
| Idempotency key length | 64 bytes |
| Single declared capacity, load, or reserve | `1e14` mW (100 GW) |
| Any rollup or sum | `1e15` mW (1 PW) |

Declared counts in a persisted artifact are checked against these bounds *before*
any memory is reserved for them, and a declared section length that exceeds the
bytes actually present is `corruption` rather than a truncated model.

Identifiers are restricted to `A-Z a-z 0-9 . _ - : @`, at most 128 bytes. Empty,
over-long, NUL-bearing, whitespace-bearing, path-separator-bearing, and non-ASCII
identifiers are refused, so an identity can never be a path fragment or a Unicode
confusable. Labels are free UTF-8 without NUL or control bytes.

## The capacity model

### Domains

A `PowerDomain` declares:

- an identity, classification metadata (`kind`, `label`), and at most one parent;
- `nominal_capacity`, `usable_capacity`, `derate_ratio`, `degradation_ratio`;
- a reserve policy;
- an operational state with an explicit cause;
- an evidence reference and the source generations it was derived from.

Identity is separate from mutable metadata: `kind` and `label` are descriptive and
are never used to infer redundancy, capacity, or authority. Two domains named and
classified as though they were an N+1 pair receive no redundancy behaviour at all
unless a group states the obligation.

State invariants, all enforced at commit and at read:

- `usable_capacity <= nominal_capacity`, both non-negative and within the
  component bound.
- `available` requires cause `none` and a degradation ratio of exactly 100%.
- `degraded` requires a maintenance, fault, or evidence-unknown cause **and** a
  degradation ratio strictly below 100%, so the state cannot be meaningless.
- `unavailable` requires a cause, and its degradation ratio is ignored and must be
  100%.
- A reserve policy states exactly one intent: `none` carries no value, `absolute`
  carries no ratio, `ratio` carries a non-zero ratio and no absolute value, and
  `greater_of_absolute_and_ratio` carries both.
- An evidence window must be non-empty: `valid_until > observed_at`.

### Loads

A `LoadRecord` attaches one load to exactly one domain, as `committed` or
`protected`, and must cite the granting `AuthorityRef`. Zero and negative loads are
refused. Because a load has exactly one attachment point and a domain has exactly
one parent, each load is counted exactly once in each of its ancestors — the
property that makes the rollups free of double counting.

### Reason precedence

When several conditions hold, exactly one primary `ReasonCode` is reported, by this
documented order:

1. `domain_unavailable_*` — unavailability dominates everything.
2. `committed_load_exceeds_capacity` — the remainder clamped, so load must be removed.
3. `protected_load_exceeds_capacity`
4. `reserve_exceeds_capacity`
5. `domain_degraded_*` — degraded but still serving.
6. `derate_removes_capacity` — a non-zero usable capacity reduced to zero.
7. `zero_nameplate_capacity`
8. `ok`

Flags accompany the reason: `reserve_clamped`, `protected_clamped`,
`committed_clamped`, `degraded`, and `load_on_unavailable` (a load is attributed to
a domain that is currently unavailable, so that load is stranded).

## Redundancy

A `RedundancyGroup` declares:

- a set of members, canonically sorted and unique;
- `required_simultaneous_failures`, the explicit obligation, stated by a
  `policy_authority`;
- a descriptive `declared_class`, which is a label and never an obligation.

Three invariants make the arithmetic sound:

- **Members are pairwise independent.** No member may be an ancestor or a
  descendant of another. A violating group is refused with `invariant_violation`.
- **A domain belongs to at most one group.** A second claim is refused with
  `conflict`.
- **At least two members, and a tolerance strictly smaller than the member count.**

### The arithmetic

Let `carryable(d) = safe_capacity(d) − protected_load(d)` and let `u` be the number
of members currently unavailable.

- If `u > tolerance` the obligation is already violated: allocatable headroom for
  new commitments is `0`, with `redundancy_tolerance_exceeded`.
- If `tolerance > 0` and `u == tolerance` the obligation is met exactly at its
  limit, so no failure margin remains: allocatable headroom is `0`, with
  `redundancy_tolerance_at_limit`, and `physical_headroom` still reports what the
  survivors could physically carry.
- Otherwise the obligation is met with margin:

```
effective_tolerance  = tolerance − u
tolerated_loss       = sum of the effective_tolerance largest carryable values
                       among the members that are still available
survivable_capacity  = member_carryable_total − tolerated_loss
group_load           = sum of committed load over all members
raw_headroom         = max(0, survivable_capacity − group_load)
```

This closed form is exactly `min over all tolerated failure sets of the sum of the
survivors' carrying ability`, and it reduces to both well-known cases: for a
tolerance of one it is `total − largest`, which is the N+1 answer and, for two
members, the 2N answer. The randomized suite compares it against an exhaustive
enumeration of every tolerated failure set for member counts up to six.

### Shared upstream

If every member of a group shares a common ancestor, the most restrictive of those
ancestors bounds the group as a whole. The group's effective headroom is
`min(raw_headroom, shared_upstream_headroom)` with reason `shared_upstream_limits`,
and the limiting ancestor is named. This accounts for shared delivery capacity once
rather than once per member.

## Rollups, shared domains, and bottlenecks

A committed or protected load consumes capacity at its attachment point **and at
every ancestor**. `CapacityState` precomputes that rollup once per published state,
so a query is a walk up the path rather than a traversal of the model.

- `rolled_up_committed_of(d)` and `rolled_up_protected_of(d)` include `d`'s directly
  attached loads and the totals of every descendant.
- The sum of every load in the model equals the root's rollup. The test suite
  asserts this equality explicitly, which is a direct check that nothing is counted
  twice.
- Ancestry is a chain, and an ancestor's headroom already accounts for its
  descendants' load, so accounting a load at a domain necessarily accounts it at
  each ancestor exactly once.

**Bottleneck selection** is deterministic. For a query at `D`, every node on the
path `D → root` contributes its own allocatable headroom capped by its redundancy
group's effective headroom; the binding constraint is the minimum. Ties resolve
toward the root and then by identifier, so the same question always names the same
bottleneck.

`DomainAssessment.effective_allocatable_headroom` is that minimum: the quantity a
new commitment at `D` must be compared against. `DomainAssessment.derivation` is
`D`'s own derivation, which is a different and also useful number.

## Candidate load evaluation

`evaluate` and `evaluate_batch` are advisory and non-mutating. Per-candidate
precedence:

| # | Condition | Outcome |
| --- | --- | --- |
| C1 | Duplicate candidate identity in the batch | whole call: `duplicate_identity` |
| C2 | Empty identity or domain, or a zero or negative load | whole call: `invalid_argument`; a load above the component bound is `limit_exceeded` |
| C3 | Engine closed, not revalidated, or expected generation stale | whole call: typed error |
| C4 | Target domain not in the model | `refused`, `domain_not_found` |
| C5 | Identity already committed | `refused`, `load_already_committed` |
| C6 | Evidence missing, not yet valid, or stale on any node of the path | `indeterminate` |
| C7 | Any node of the path unavailable | `refused`, `unavailable` |
| C8 | A group on the path is at or beyond its tolerance | `refused`, `redundancy_violated` |
| C9 | Insufficient headroom at the bottleneck | `refused`, `capacity_exceeded`, reason `insufficient_headroom` |
| C10 | Otherwise | `admissible` |

`indeterminate` is deliberately distinct from `refused`: the first says the answer
cannot be determined and the caller must refresh evidence or authority; the second
says the load does not fit.

A **batch is cumulative**. Each accepted candidate is applied to an in-memory
overlay, so the batch answers "does the whole set fit", not "does each fit alone".
The overlay never touches the committed state, never advances the generation, and
saturates at the aggregate bound instead of wrapping, so a candidate can never
become admissible again after a refusal caused by exhaustion.

## Evidence, generations, and authority

Nothing in this model is self-observed. Every capacity, load, derating, state, and
redundancy fact arrives as an `EvidenceRef`: an identity, a source, an observation
instant, a validity window, a source revision, and a source generation.

### Evidence freshness

Freshness is evaluated against a **caller-supplied logical instant** — never a wall
clock — so evaluation is deterministic and replayable:

- `observed_at > as_of` → `not_yet_valid` (evidence from the future is not authority)
- `observed_at <= as_of < valid_until` → `current`
- `as_of >= valid_until` → `stale`
- no record → `missing`

A domain whose evidence is not current produces an assessment with
`known == false` and a reason, and no derivation at all. It is never reported as
zero. Evidence is evaluated from the queried domain outward, so the reason reported
is the one closest to the question.

### Capacity generation and source generations

- The capacity **generation** starts at 1 for a freshly created store and advances
  by exactly one per committed mutation. A commit that skips a generation is
  refused with `stale_generation`.
- **Source generations** (topology, electrical evidence, policy, …) are durable
  per-source counters. A mutation may only proceed if every source it depends on
  still holds the generation the caller read; otherwise `stale_source_generation`.
  A source may be advanced only if it was declared as read, and only upward.

Every mutation carries an explicit `expected_generation`. There is no implicit
"update whatever is there".

### Control-plane epoch and incarnation

The `(epoch, incarnation)` pair is consumed from the Control Plane Epoch runtime.
It is durable in the store, monotonic, and never regresses: a revalidation that
cites an older epoch, or the same epoch with an older incarnation, is refused with
`stale_authority`. Every mutation and query may cite the pair it believes is
current; a mismatch is refused.

### Idempotency

A mutation may carry a bounded idempotency key. The store keeps the most recent 256
applied keys with the generation each produced; older keys are evicted, so the
history is bounded rather than growing without limit. Replaying a recorded key
returns the previously recorded generation with `already_applied = true` and
touches nothing.

The replay is answered **before** the generation precondition is compared. A caller
retrying an operation whose response was lost still cites the generation it read
before the first attempt, which is by then stale; refusing that retry would make
the key useless for exactly the case it exists for. The authority binding and the
presence of an explicit precondition are still required.

## Persistence

### Artifact format

```
header  72 bytes  magic, format version, header size, endian marker, flags,
                  payload length, payload CRC-32C, header CRC-32C, and a
                  redundant copy of the generation, epoch, and incarnation
payload  n bytes  six length-prefixed sections in fixed order:
                  meta, domains, loads, groups, sources, applied operations
footer  16 bytes  end magic, repeated payload CRC-32C, footer CRC-32C
```

All multi-byte integers are little-endian and written explicitly, so the format
does not depend on host byte order. The full field-level specification is in
[`docs/artifact-format.md`](docs/artifact-format.md).

The reader rejects, with a typed status rather than a partial model:

| Defect | Status |
| --- | --- |
| Shorter than the envelope, or a truncated section | `corruption` |
| Larger than the configured store bound | `limit_exceeded` |
| Unknown magic | `corruption` |
| Unsupported format version, or unknown flags | `incompatible_version` |
| Byte-swapped endian marker | `endian_mismatch` |
| Any other endian marker, reserved field, length, or checksum mismatch | `corruption` |
| Declared count or section length above a configured bound | `limit_exceeded` |
| Section order wrong, or trailing bytes | `corruption` |
| A section with trailing bytes | `corruption` |
| A count that exceeds the bytes that follow | `corruption` |
| Store identity that disagrees with the caller's expectation | `stale_authority` |
| Generation below the caller's floor | `stale_generation` |
| Creation path that disagrees when path binding is enforced | `conflict` |
| Path that is a symbolic link, a directory, or not a regular file | `invalid_argument` |
| Missing parent directory | `not_found` |

### Integrity versus semantics

A checksum proves only that the bytes were not corrupted in transit. It says
nothing about whether the payload is a possible model. The reader therefore does
both: the envelope and CRC-32C checks detect accidental corruption, and the full
semantic validation in `CapacityState::build` rejects a structurally perfect
artifact whose contents are impossible. The adversarial suite proves this by
re-sealing edited artifacts so that every checksum is valid, and requiring the
reader to refuse them on `usable > nominal`, a degraded domain without a
degradation ratio, a group with overlapping members, a duplicate identity, and an
impossible declared count.

CRC-32C here is an accidental-corruption check. It is not a cryptographic
authentication code, uses no secret, and does not resist an adversary who can
rewrite the artifact and recompute it. What rejects a *deliberately* crafted
artifact is the structural and semantic validation, not the checksum. This is
stated plainly rather than implied.

### Commit protocol

```
plan → validate → reserve generation and attempt → write staging → flush →
verify → atomic publish → cleanup
```

1. The new state is fully validated before anything is written.
2. The generation must be exactly one greater than the committed generation, and
   the store identity must match.
3. A staging file is written beside the target, named
   `<store>.stg-<pid>-<session>-<attempt>`, so two processes can never collide.
4. The stream is flushed and the file's data is committed to the device
   (`_commit` on Windows, `fsync` on POSIX).
5. The staged bytes are re-read and fully re-decoded through the same strict reader
   as the open path, and the resulting state is rebuilt and revalidated. An artifact
   that could not be opened can therefore never be published.
6. The publish is a single atomic replace. Nothing is visible at the store path
   before it, and the in-memory state is published only after it succeeds.

If any step before the publish fails, the staging file is removed, the previous
committed state is untouched, and the engine's in-memory state is not advanced.

On POSIX the containing directory is flushed after the rename. On Windows that
would require a directory handle opened with backup semantics, so the call reports
`unsupported` and is deliberately ignored rather than reported as success. The
consequence is stated in [Honest limitations](#honest-limitations).

### Anti-rollback generation floors

A store file can be replaced by an *older but perfectly valid copy of itself*, and
no checksum can detect it. `StoreReadOptions::minimum_generation` is a durable
floor recorded elsewhere by the operator: a store committed below the floor is
refused with `stale_generation`. The adversarial suite performs exactly this
rollback and shows that the artifact is accepted without a floor and refused with
one.

### Identity and path binding

Every store carries a 128-bit identity created with it. Passing
`expected_store_identity` makes an open or a read fail with `stale_authority` if the
file is a different store. The store also records the creation path;
`enforce_path_binding` turns a move or a swap into `conflict`. Path comparison is
normalized and case-folded on Windows.

Staging-file cleanup only ever removes regular files whose names begin with the
store's own file name followed by the staging marker, in the store's own directory,
and at most 64 per sweep, so a sweep cannot wander outside the store or delete an
unrelated file.

## Recovery

State recovered from a store starts in `recovered_pending_revalidation`. In that
state every query and every mutation is refused with `not_revalidated`. The engine
answers capacity questions only after `revalidate_recovered_state` binds it to an
evidence instant and an authority binding, and reports exactly how many domains are
current, stale, and missing evidence.

Persisted dynamic evidence never becomes fresh by being recovered: freshness is
never stored, only the instant at which it was last accepted, and every recovered
engine re-evaluates it against the instant the caller supplies. A window that
closed while the process was down stays closed.

Re-qualifying under the binding the store already records does not commit and does
not advance the generation. Moving the binding does commit, because the persisted
binding is itself an authoritative fact.

A recovered state is one whole committed generation. Recovery never merges, never
interpolates, and never adopts residue: a staging file left by a process that died
between the staging write and the publish is swept and ignored, even when its
contents are a perfectly valid artifact of a different store.

`revalidate` answers whether an earlier assessment still holds, in this order:

| Condition | Verdict |
| --- | --- |
| Different store identity | `superseded`, `store_identity_mismatch` |
| Different engine session | `superseded`, `store_reopened` |
| Different epoch or incarnation | `superseded`, `authority_epoch_changed` |
| Different generation | `superseded`, `generation_advanced` |
| Domain no longer present | `superseded`, `domain_not_found` |
| Recompute is unknown | `indeterminate`, with the assessment's reason |
| Digest differs | `superseded`, `evidence_revision_changed` |
| Otherwise | `still_valid` |

The digest is a deterministic change-detection digest over every authoritative
field of the answer, including the evidence revisions on the path. It detects
change; it is not a cryptographic commitment and does not resist deliberate
collision construction. On the five hard fences no fresh assessment is attached:
the token is refused and the caller asks again.

## Concurrency and lock order

Ownership is an immutable published snapshot plus a narrow mutation serialization.

```
L1  store writer lock        operating-system level, held for the store's lifetime
L2  engine mutation mutex    serializes mutations and revalidation
L3  binding mutex            guards the accepted (epoch, incarnation) pair
L4  atomic snapshot publish   std::atomic<std::shared_ptr<const CapacityState>>
```

The full order is **L1 → L2 → L3 → L4**, with these properties:

- Queries take **no engine lock at all**. `assess`, `evaluate`, `evaluate_batch`, and
  `revalidate` load the published snapshot atomically and read it. They never block
  a writer and a writer never blocks them.
- A published `CapacityState` is immutable and fully validated. A reader can hold
  one for as long as it likes; it will never observe a partially applied mutation,
  a half-built index, or a value that contradicts another value in the same state.
- L1 is acquired before any engine exists and is never re-acquired, so no path
  acquires L1 while holding L2 or vice versa.
- L2 is always acquired before L3, and L3 is never held while acquiring anything
  else. There is no reversed nesting anywhere.
- Emission never happens under a lock. There are no callbacks, no observers, and no
  events in this library, so a lock can never be re-entered through one.
- There is no shutdown join: there are no worker threads. `close` releases the store
  and marks the engine closed without waiting on anything.
- Stale authority is refused rather than merged, so a lost race is a typed
  `stale_generation` error, not a corrupted state. Exactly one mutation per
  generation can succeed.

`CapacityEngine` is not safe to destroy while another thread is inside a call on
it, and it is not copyable: one owner, as documented.

## Process authority

Writer authority is an operating-system guarantee, not a convention:

- On Windows the lock file is opened with share mode zero (`_wsopen_s` with
  `_SH_DENYRW`).
- On POSIX it is held with `flock(LOCK_EX | LOCK_NB)`.

Either way the kernel releases the lock when the holding process exits, **including
abnormal exit**. The proof is a set of real multiprocess tests, not threads:

| Test | Evidence |
| --- | --- |
| `recovery.a_live_process_holds_writer_authority_against_this_process` | A probe process holds the lock; this process is refused with `lock_conflict` while a read-only open still succeeds; after `taskkill /F` the lock is free |
| `recovery.process_death_without_cleanup_relinquishes_writer_authority` | A probe acquires the lock and terminates with `std::_Exit`, no unwinding and no cleanup; the store reopens immediately |
| `recovery.a_commit_survives_the_death_of_the_committing_process` | A second process commits a domain and dies; the committed domain, its derivation, and its generation are present afterwards |
| `recovery.an_independent_process_reads_the_committed_state` | A fresh process reads the store and reports the same identity, counts, and generation |
| `recovery.a_force_kill_during_a_commit_leaves_a_valid_store` | A probe commits in a loop and is force-killed mid-commit; the store still opens, is structurally whole, and its model matches its generation exactly |
| `recovery.staging_residue_from_a_dead_process_is_swept_and_never_adopted` | A dead process leaves a *valid artifact of a different store* at the staging path; it is swept, never adopted, and nothing else in the directory is touched |
| `recovery.a_residue_of_garbage_never_becomes_the_committed_state` | Staging residue of arbitrary bytes is swept and ignored |
| `recovery.two_engines_cannot_both_hold_writer_authority` | A second engine over the same store is refused while the probe holds it |

The probe program writes its results to files rather than to standard output, so
none of these proofs depends on capturing a child process's stdio.

## Error model

`StatusCode` is a stable enumeration with stable lower_snake_case tokens:

`ok`, `invalid_argument`, `not_found`, `already_exists`, `duplicate_identity`,
`conflict`, `precondition_failed`, `stale_generation`, `stale_authority`,
`stale_source_generation`, `incompatible_version`, `corruption`, `limit_exceeded`,
`unsupported`, `unavailable`, `unknown`, `indeterminate`, `permission_denied`,
`io_failure`, `lock_conflict`, `invariant_violation`, `capacity_exceeded`,
`redundancy_violated`, `evidence_missing`, `evidence_stale`, `not_revalidated`,
`closed`, `endian_mismatch`, `read_only`.

`ReasonCode` is a second stable enumeration that explains a *value* rather than an
outcome: `ok`, the evidence reasons, the unavailability and degradation reasons,
the clamping reasons, the redundancy reasons, `shared_upstream_limits`,
`bottleneck_upstream`, `insufficient_headroom`, `load_already_committed`,
`path_contains_degraded_domain`, and the revalidation reasons. A reason and a
status are distinct types and are always reported under distinct keys (`reason`
and `error`), so they can never be confused. The token tables are asserted unique
and stable by the test suite.

`Result<T>` carries a value or a `Status`, and the value is never observable on an
error path. `Status::to_string()` renders `token` or `token: message`, and
diagnostic messages carry the expected and current generation, epoch, incarnation,
identity, or the exact violated constraint.

## Command line tool

```
power-capacity <command> [options]
```

| Command | Purpose |
| --- | --- |
| `version` | Print the library version. |
| `init` | Create a store and print its identity, generation, and binding. |
| `verify` | Verify a store artifact end to end. |
| `inspect` | Print committed store metadata, read-only. |
| `status` | Open an engine and print its lifecycle, binding, and model size. |
| `put-domain`, `erase-domain` | Commit or remove a power domain. |
| `put-load`, `erase-load` | Commit or remove a load. |
| `put-group`, `erase-group` | Commit or remove a redundancy group. |
| `advance-source` | Record that a source generation advanced. |
| `revalidate` | Qualify recovered state, or move the authority binding. |
| `assess` | Answer the capacity question for one domain. |
| `evaluate` | Evaluate a proposed load against current authority. |

Every output field is deterministic and can be rendered as `key=value` lines or as
one JSON object with `--json`. Only `version` does not need `--store`.

Mutating commands take `--as-of-tick`, `--expected-generation`, and optionally
`--epoch`, `--incarnation`, `--expect-source <id>=<gen>`,
`--advance-source <id>=<gen>`, and `--idempotency-key`. The authority binding
defaults to the binding the store already accepts. Supplying a *different* binding
to a mutating command is refused with `stale_authority` and a message pointing at
the `revalidate` command, because moving the binding is itself an authoritative act
that advances the generation and must be stated explicitly.

`assess` and `evaluate` accept `--read-only` and take no writer lock, so a store can
be inspected while another process owns it.

Exit codes: `0` success, `1` typed error, `2` usage error.

```
$ power-capacity init --store facility.pcstore --tick 0
$ power-capacity put-domain --store facility.pcstore --id utility.feed.a \
    --kind utility_feed --nominal-w 12000000 --usable-w 11400000 \
    --derate-bp 9000 --reserve-mode ratio --reserve-bp 1500 \
    --evidence-id ev.1 --evidence-source source.electrical \
    --observed-tick 0 --valid-until-tick 100000 --as-of-tick 10 \
    --expected-generation 1
$ power-capacity assess --store facility.pcstore --domain utility.feed.a \
    --as-of-tick 100 --read-only
allocatable_w=8712000.000
```

## Library use

```cpp
#include "power_capacity/engine.hpp"

using namespace power_capacity;

EngineOptions options;
options.store_path = "facility.pcstore";
auto engine = CapacityEngine::open(options);
if (!engine.ok()) { /* engine.status() */ }

// A newly created store is current; recovered state must be qualified first.
if (engine.value()->lifecycle() == EngineLifecycle::RecoveredPendingRevalidation) {
  RevalidationRequest request;
  request.as_of = Tick(100);
  if (!engine.value()->revalidate_recovered_state(request).ok()) { /* ... */ }
}

CapacityQuery query;
query.domain = DomainId::parse("pdu.a", 128).value();
query.as_of = Tick(100);
auto assessment = engine.value()->assess(query);
if (assessment.ok() && assessment.value().known) {
  const Power headroom = *assessment.value().effective_allocatable_headroom;
  const DomainId bottleneck = *assessment.value().bottleneck;
}
```

Ready-to-link consumers use the exported package:

```cmake
find_package(PowerCapacity 1.0 REQUIRED)
target_link_libraries(your_target PRIVATE PowerCapacity::power_capacity)
```

## Examples

| Example | Shows |
| --- | --- |
| `examples/basic_capacity.cpp` | The full derivation for one domain, and that a closed evidence window yields UNKNOWN rather than zero. |
| `examples/redundancy_bottleneck.cpp` | An N+1 group with a degraded member, the shared-upstream bound, the bottleneck on a path, and cumulative candidate evaluation. |
| `examples/persistence_recovery.cpp` | Two lifecycles over one store: commit, close, reopen, refuse queries until revalidated, supersede a token from the previous session, and verify the artifact. |

## Build, test, and install

Requirements: CMake 3.21 or newer and a C++20 compiler. No third-party
dependencies: the library uses the C++ standard library only.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --prefix /some/prefix
```

Options: `POWER_CAPACITY_BUILD_TESTS`, `POWER_CAPACITY_BUILD_EXAMPLES`,
`POWER_CAPACITY_BUILD_BENCHMARKS`, `POWER_CAPACITY_BUILD_TOOLS`,
`POWER_CAPACITY_WARNINGS_AS_ERRORS`, and
`POWER_CAPACITY_DOWNSTREAM_PREFIX` (see below).

First-party warnings are errors: MSVC builds with `/W4 /WX /permissive- /utf-8`,
other compilers with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Wsign-conversion -Wnon-virtual-dtor -Wcast-qual -Wdouble-promotion -Werror`. No
warning is suppressed anywhere in the repository.

### Installed package and downstream consumer

The install exports headers, the library, the tool, `LICENSE`, and `NOTICE`, and a
versioned CMake package (`PowerCapacityConfig.cmake`,
`PowerCapacityConfigVersion.cmake`, `PowerCapacityTargets.cmake`) with the
namespaced target `PowerCapacity::power_capacity`.

`downstream/consumer/` is an independent out-of-tree project that is **not** part
of the Power Capacity build tree. It uses `find_package(PowerCapacity 1.0 REQUIRED)`
against an install prefix and runs a real lifecycle: create a durable store, commit
a model, close, reopen, confirm that a query is refused before revalidation,
revalidate, assess, evaluate a candidate, and check the exact allocatable value.

```sh
cmake -S downstream/consumer -B /tmp/consumer -DCMAKE_PREFIX_PATH=/some/prefix
cmake --build /tmp/consumer
cmake --build /tmp/consumer --target run_consumer
```

Set `POWER_CAPACITY_DOWNSTREAM_PREFIX` to an install prefix to run the same check
as a CTest test (`pc_test_downstream_install`). The check locates the MSVC developer
environment itself through `vswhere` and fails with an explicit message if no C++
compiler is reachable, rather than reporting a pass it did not earn.

## Validation

Everything below was executed on the repository as released, on Windows with
MSVC. Nothing in this section is extrapolated from a different configuration.

### Platform and toolchain exercised

| Item | Value |
| --- | --- |
| Operating system | Windows, x64 |
| Compiler | Microsoft C/C++ 19.44.35222 (Visual Studio 2022 Build Tools 17.14) |
| CMake | 4.3.2 |
| Generator | Ninja 1.13.2 |
| Configurations | `Release` and `Debug` |

### Test suites

15 CTest tests: 11 suites containing 160 individual test cases, 3 examples, and the
installed-package check. All 15 pass in both `Release` and `Debug`.

| Suite | Cases | Covers |
| --- | --- | --- |
| `pc_test_units` | 15 | Exact unit construction, checked arithmetic, 128-bit `mul_div_floor`, ratio rounding, identifier and fingerprint validation, status and reason token stability |
| `pc_test_model` | 16 | Declaration invariants, the documented derivation order, the documented reason precedence, clamping flags |
| `pc_test_redundancy` | 14 | The closed form against exhaustive enumeration, N+1 and 2N, tolerance at and beyond its limit, shared upstream, group invariants |
| `pc_test_rollup` | 21 | Containment, rollups counted once, load attribution, bottleneck determinism, candidate evaluation and cumulative batches, erase dependencies |
| `pc_test_evidence` | 21 | Freshness windows, unknown versus zero, generation and authority fencing, source generations, idempotency and its bound, revalidation verdicts |
| `pc_test_persistence` | 21 | Close/reopen, every header field, payload corruption, trailing bytes, size and count bounds, identity and path binding, staging sweep, strictness parity, Unicode paths |
| `pc_test_recovery` | 8 | Eight real multiprocess and crash-recovery proofs (see [Process authority](#process-authority)) |
| `pc_test_concurrency` | 5 | Snapshot immutability, four readers against a live writer, racing mutations, closed-engine refusal |
| `pc_test_property` | 3 | 24 seeded random facilities compared against an independent reference model, candidate evaluation against reference headroom, generation discipline |
| `pc_test_adversarial` | 28 | Cycles, deep containment, re-sealed impossible artifacts, declared-count and section-length attacks, oversized declarations, aggregate overflow, invalid identifiers, path manipulation, symbolic links, rollback, read-only files, batch overlay saturation |
| `pc_test_cli` | 8 | The tool as a real process: lifecycle, JSON output, typed errors, usage errors, lock conflict, and that a read-only command cannot mutate |
| `pc_test_downstream_install` | 1 | Out-of-tree `find_package` consumer against an installed prefix |

The randomized suites use fixed seeds, so a failure reproduces exactly. No test uses
a timeout of any kind, and no bounded wait can turn a missing fact into a pass. Three
bounded waits exist — process readiness, reacquisition of writer authority after a
force kill, and confirmation that a reader thread inspected a state published after
a given commit — and each of them can only exhaust into a failure. In particular the
concurrency suite will not pass without having actually interleaved readers and a
writer, which is why it fails rather than passes if no reader is ever scheduled.

### Sanitizer

AddressSanitizer was run as `RelWithDebInfo` with MSVC `/fsanitize=address`. All 14
suites built in that configuration pass with no ASan report. ASan found one real
defect during hardening: the test harness compared a `Status` through a reference
that dangled once the temporary `Result<T>` in the same full expression was
destroyed. It was a use-after-scope in the test support code, not in the library,
and it was fixed by returning the status by value.

The downstream check was not part of the sanitizer configuration.

### Adversarial results

Defects found and fixed during the hardening phase:

1. **The strict read path followed a symbolic link.** `CapacityStore::open` refused
   a symlinked store path, but `read_file` and `verify_file` accepted it, so audit
   tooling was *less* strict than the engine. The read path now applies the same
   path validation. Proven by a test that creates a real symbolic link and requires
   `invalid_argument` from both paths while the real store behind the link stays
   readable.
2. **No protection against a rollback to an older valid artifact.** Nothing about a
   restored earlier copy of a store reveals itself. A durable `minimum_generation`
   floor was added to the read and open paths, with a test that performs the
   rollback and shows the artifact is accepted without a floor and refused with
   `stale_generation` with one.
3. **The test harness had a use-after-scope**, found by AddressSanitizer (above).
4. **A commit that could not replace the store file was not covered.** The
   adversarial suite now makes the store file read-only, attempts a commit, requires
   the failure to be reported, and requires the committed state and the directory to
   be exactly as they were.
5. **Idempotent retries were refused as stale.** The replay check originally ran
   after the generation precondition, which defeats the purpose of the key. The
   order was corrected and is documented.
6. **`_spawnv` does not quote arguments on Windows**, so an executable path
   containing a space is split by the child's command-line parser. The multiprocess
   support now quotes every argument. This was a defect in the test infrastructure
   that would have silently produced a false negative.
7. **`std::filesystem::path::string()` throws on a non-ASCII path** on Windows
   because it converts through the ANSI code page, so a legitimate Unicode store
   path became an exception. Every diagnostic and derived file name now goes through
   a UTF-8 conversion, and a Unicode store path is exercised end to end.
8. **CLI authority handling was too eager.** Implicit qualification moved the
   authority binding of the store, which advanced the generation before the caller's
   mutation and turned an intended `stale_authority` into a confusing
   `stale_generation`. Implicit qualification now never moves the binding, and doing
   so requires the explicit `revalidate` command.
9. **The publish counter counted attempts**, not publishes. Corrected.
10. **The concurrency interleaving proof could pass vacuously.** On a fast machine the
    writer could complete every commit before any reader thread was scheduled, so the
    suite asserted invariants over a model that never moved. It now requires each
    commit to be observed by a reader, and fails — rather than passes — if no reader
    is scheduled within the bounded wait. Verified by 80 consecutive runs.
11. **A missing install prefix produced an opaque downstream failure.** The check now
    names the prefix and the exact `cmake --install` command to run.
12. **The strict read path re-read the store after the store had already read it**, so
    a read-only engine could report a store generation that disagreed with its own
    published state. The store now exposes the snapshot it read, and the engine uses
    it. The recorded mutation instant was also made monotonic so an older caller
    instant cannot move the audit trail backwards.

### What is proven, and at what level

| Claim | Level of evidence |
| --- | --- |
| Capacity arithmetic, rollups, redundancy, bottleneck, candidate evaluation | Unit tests plus a seeded randomized comparison against an independent reference model |
| Persistence and reopen | Real close/reopen in-process, plus reads from separate processes |
| Crash recovery | Real `std::_Exit` process death and real operating-system force kills, with the store verified afterwards |
| Cross-process writer authority | Real OS-level file locking across independent processes, including release on process death |
| Concurrency | Immutable snapshots and lock-free reads exercised by four concurrent readers against a live writer |
| Memory safety | AddressSanitizer on the whole suite |
| Installed package | Out-of-tree `find_package` consumer against a real prefix, through a full lifecycle |
| Performance | Completed-operation benchmark, synthetic workload, labelled as such |

Not claimed, and not proven here: POSIX behaviour, GCC or Clang behaviour, any
electrical or hardware behaviour, and any behaviour under a power loss that is not
also a process death.

## Benchmarks

`bench/bench_capacity.cpp` measures **completed operations**, not submissions. Each
reported number is the cost of the call that returns the result: an assessment is
timed around the call that returns the assessment, and a durable commit is timed
around the call that returns only after the staging write, the flush, the
verification, and the atomic publish have all finished.

### Methodology

- The workload is **SYNTHETIC**: a generated three-level facility with 521 power
  domains, 480 loads, and one redundancy group. It is not measured facility
  telemetry, and no hardware behaviour is claimed.
- Each row runs `kRepeats` (7) alternating rounds of a fixed operation count, with
  one warmup round discarded, and reports the **median** round. A median of several
  rounds is published rather than a best-of, and no before/after pair is published,
  because the methodology could not isolate such a pair.
- The durable-commit row reports the per-phase breakdown, so the flush and publish
  cost is visible rather than hidden.
- The state the benchmark produced is verified afterwards, and the scratch directory
  is removed. The benchmark fails if the model it produced does not match its own
  generation.

### Results

Measured on the toolchain in [Validation](#validation), on an ordinary
general-purpose machine. Range across repeated runs, because the machine is shared:

| Operation | Time per completed operation | Throughput |
| --- | --- | --- |
| `assess` at a leaf (path walk to the root) | 2.4 – 3.2 µs | 313k – 418k op/s |
| `evaluate` a candidate at a leaf | 2.1 – 3.6 µs | 274k – 479k op/s |
| `assess` at the root (rollup read over the whole model) | 1.0 – 1.1 µs | 929k – 987k op/s |
| Durable commit — encode, flush, verify, publish | 14.1 – 18.6 ms | 54 – 71 op/s |
| — of which the staging write and device flush | 1.4 – 1.7 ms | 605 – 702 op/s |
| — of which the staged-artifact verification | 8.6 – 10.8 ms | 93 – 117 op/s |

Artifact size per commit at this model size: 191,696 bytes (about 187 KiB).

### Interpretation

In-memory queries are microsecond-scale and allocation-free on the query path: the
engine precomputes every rollup and derivation once per published state, so an
assessment is a walk up a path of depth at most 64.

A durable commit is **O(model size)** by design. Each committed mutation revalidates
the whole model, encodes the whole model, writes it, re-reads and re-decodes it
through the same strict reader as the open path, rebuilds and revalidates the
decoded state, and then publishes. The verification phase is the largest single
component, and it is deliberately not skipped: it is what makes it impossible to
publish an artifact that the open path could not read. This makes Power Capacity
appropriate for authoritative facility-capacity state that changes on the order of
seconds to minutes, and inappropriate as a high-frequency mutation store. That is a
property of the design, not an accident, and it is measured rather than asserted.

The measured durable-commit time also includes the operating system's file creation,
flush, and rename costs on the machine measured, including any security filtering
applied to newly written files. The in-memory rows are the ones that isolate the
algorithm.

## Honest limitations

- **POSIX is compiled but not exercised.** The POSIX branches for `flock`, `fsync`,
  `rename`, and `posix_spawn` exist and are written to be correct, but they were
  never built or run on this machine. No POSIX behaviour, including POSIX crash
  recovery and POSIX writer authority, is claimed. The same applies to GCC and
  Clang: the warning flags for those compilers are configured but were not run.
- **Directory-entry durability on Windows is not established.** The rename that
  publishes a store is atomic with respect to process death, which is proven. Making
  the *directory entry* durable across an operating-system crash or a power loss
  would require a directory handle opened with backup semantics, which this
  implementation does not open. Durability across process crash is proven;
  durability across power loss is not claimed.
- **CRC-32C detects accidental corruption only.** It is not a cryptographic
  authentication code and does not resist an adversary who can rewrite the artifact.
  Semantic validation is what rejects a deliberately crafted artifact.
- **A store swap is only detected when the caller states what it expects.** Without
  `expected_store_identity`, `minimum_generation`, or `enforce_path_binding`, any
  valid store reads successfully, by design. The defenses are opt-in preconditions
  that the operator records elsewhere.
- **Symbolic-link refusal is exercised on hosts that permit creating symlinks.** The
  test attempts to create one and reports explicitly when the host refuses; on this
  host it succeeded, so the refusal is proven here. Reparse points other than
  symlinks are rejected only in that a directory is refused as a directory.
- **Multi-parent containment is unsupported.** A domain has at most one immediate
  upstream domain. Dual-corded delivery is expressed with a redundancy group.
- **Redundancy capacity is symmetric.** The group arithmetic assumes every member
  can carry any part of the group's load. Asymmetric members are modelled with
  different carrying abilities, which the arithmetic handles, but not with
  per-member load-transfer constraints.
- **`redundancy_tolerance_at_limit` is a policy position, not physics.** A group that
  has lost exactly its stated tolerance is reported with zero allocatable headroom
  even though its survivors could physically carry more. `physical_headroom` reports
  that difference so the distinction is visible.
- **Mutation cost is O(model size)**, as measured and explained above.
- **No energy accounting and no actuation.** There is no kWh integration, no control
  output, and no interface to any electrical device.
- **The workload in the benchmark is synthetic**, and the machine it ran on is a
  general-purpose shared machine, so the timings characterize the algorithm and not
  any particular deployment.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
