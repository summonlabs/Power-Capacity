# Power Capacity store artifact format

Format version 1. Little-endian throughout. The reader rejects anything that does
not match this specification exactly; there is no permissive or legacy mode.

## Envelope

```
offset  size  field
------  ----  ---------------------------------------------------------------
     0     8  magic "PWCAPST\0"
     8     4  format_version            u32, must be 1
    12     4  header_bytes              u32, must be 72
    16     4  endian_marker             u32, must be 0x01020304
    20     4  flags                     u32, must be 0
    24     8  payload_bytes             u64
    32     4  payload_crc32c            u32, CRC-32C of the payload
    36     4  header_crc32c             u32, CRC-32C of bytes [0, 36)
    40     8  generation                u64, redundant copy of the meta value
    48     8  epoch                     u64, redundant copy of the meta value
    56     8  incarnation               u64, redundant copy of the meta value
    64     8  reserved                  u64, must be 0
    72     n  payload
  72+n    16  footer
```

```
footer +  0     8  magic "PWCAPEND"
footer +  8     4  payload_crc32c      u32, must equal the header value
footer + 12     4  footer_crc32c       u32, CRC-32C of the first 12 footer bytes
```

The file size must equal `72 + payload_bytes + 16` exactly. Trailing bytes are
`corruption`. The generation, epoch, and incarnation in the header must equal the
values in the meta section; a disagreement is `corruption`.

Reading order, and therefore the status a defect produces:

1. File size below 88 bytes, or above `limits.max_store_bytes` → `corruption` or
   `limit_exceeded`.
2. Magic → `corruption`.
3. `endian_marker`: `0x04030201` → `endian_mismatch`; anything else → `corruption`.
4. `format_version` → `incompatible_version`.
5. `header_bytes` → `corruption`.
6. `flags` → `incompatible_version`.
7. `header_crc32c` → `corruption`.
8. `reserved` → `corruption`.
9. `payload_bytes` against the envelope, then against `limits.max_payload_bytes` →
   `corruption` or `limit_exceeded`.
10. `payload_crc32c` → `corruption`.
11. Footer magic, repeated payload CRC, footer CRC → `corruption`.
12. Sections, in order → `corruption`, `limit_exceeded`, or a semantic status.

## Payload sections

Each section is `u32 tag`, `u64 byte_length`, then `byte_length` bytes. Sections
appear exactly once, in this order. A section that runs past the end of the payload
is `corruption`; a declared length above `limits.max_payload_bytes` is
`limit_exceeded` before anything is allocated.

| Tag | Section |
| --- | --- |
| 1 | Meta |
| 2 | Domains |
| 3 | Loads |
| 4 | Groups |
| 5 | Sources |
| 6 | Applied operations |

Every section must be consumed exactly by its own reader; leftover bytes inside a
section are `corruption`, and leftover bytes after the last section are
`corruption`.

### Primitive encodings

| Primitive | Encoding |
| --- | --- |
| `u8`, `u32`, `u64`, `i64` | Little-endian, fixed width |
| `text` | `u32` byte length, then that many bytes, no terminator |
| `flag` | `u8`, must be 0 or 1 |
| `id` | `text`, at most `limits.max_identifier_bytes` |
| `label` | `text`, at most `limits.max_label_bytes` |
| `path` | `text`, at most `limits.max_path_bytes` |
| `fingerprint` | `text` of exactly 32 lowercase hex characters, never all zero |
| `ratio` | `u32` basis points, at most 10000 |
| `power` | `i64` milliwatts |
| `instant` | `i64` logical tick |

A `text` whose declared length exceeds its own field bound is `limit_exceeded`; a
`text` whose declared length exceeds the bytes that follow is `corruption`. Both
checks happen before any buffer is allocated.

### Section 1 — Meta

```
text   store identity     32 lowercase hex characters
text   created path       the normalized path the store was created at
u64    generation
u64    epoch
u64    incarnation
i64    created_at
i64    updated_at
i64    last_revalidated_at
```

`updated_at` must be at least `created_at`, and `last_revalidated_at` must not be
negative; anything else is `invariant_violation`.

### Section 2 — Domains

```
u32    count              at most limits.max_domains
repeat count times:
  text   id
  u8     kind             0 utility_feed, 1 transformer, 2 switchgear,
                          3 transfer_switch, 4 generator, 5 ups_system,
                          6 ups_module, 7 busway, 8 pdu, 9 rack_pdu, 10 other
  text   label
  u8     has_parent        0 or 1; if 1, a text parent id follows
  i64    nominal capacity  milliwatts
  i64    usable capacity   milliwatts
  u32    derate ratio      basis points
  u32    degradation ratio basis points
  u8     reserve mode      0 none, 1 absolute, 2 ratio,
                           3 greater_of_absolute_and_ratio
  i64    reserve absolute  milliwatts
  u32    reserve ratio     basis points
  u8     state             0 available, 1 degraded, 2 unavailable
  u8     state cause       0 none, 1 maintenance, 2 fault, 3 evidence_unknown,
                           4 decommissioned
  u8     has_evidence      0 or 1; if 1, an evidence record follows
  u32    source count      at most limits.max_sources
  repeat source count times:
    text   source id
    u64    generation
```

An evidence record:

```
text   evidence id
text   source id
i64    observed_at
i64    valid_until        strictly greater than observed_at
u64    revision
u64    source generation
```

The record identity must equal the map key the decoder builds; two records with the
same identity are `duplicate_identity`.

### Section 3 — Loads

```
u32    count              at most limits.max_loads
repeat count times:
  text   id
  text   attachment domain id
  u8     class            0 committed, 1 protected
  i64    load             milliwatts, strictly positive
  text   authority ref    the external authority that granted the commitment
  u8     has_evidence      0 or 1; if 1, an evidence record follows
  text   label
```

### Section 4 — Groups

```
u32    count              at most limits.max_groups
repeat count times:
  text   id
  u8     declared class    0 unspecified, 1 n, 2 n_plus_one, 3 n_plus_two,
                           4 two_n, 5 two_n_plus_one, 6 two_n_plus_two, 7 other
  u32    member count      at most limits.max_members_per_group
  repeat member count times:
    text   member domain id
  u32    required simultaneously failed members
  u8     has_evidence      0 or 1; if 1, an evidence record follows
  text   policy authority ref
  u32    source count      at most limits.max_sources
  repeat source count times:
    text   source id
    u64    generation
  text   label
```

Members must be sorted ascending and unique, at least two, and the tolerance must be
strictly smaller than the member count and at most
`limits.max_redundancy_tolerance`.

### Section 5 — Sources

```
u32    count              at most limits.max_sources
repeat count times:
  text   source id
  u64    generation
```

### Section 6 — Applied operations

```
u32    count              at most limits.max_applied_operations
repeat count times:
  text   idempotency key  at most limits.max_idempotency_key_bytes
  u64    generation the key was applied at
```

## Semantic validation after decoding

Decoding produces a `CapacityContent`. `CapacityState::build` then validates every
invariant and computes every derived index in one pass. A failure here is a typed
status, never a partially constructed state:

| Condition | Status |
| --- | --- |
| A map key that disagrees with its record identity | `invariant_violation` |
| A declared count above its configured bound | `limit_exceeded` |
| An unknown parent domain | `not_found` |
| A load attached to an unknown domain | `not_found` |
| A group member that does not exist | `not_found` |
| Containment that contains a cycle | `invariant_violation` |
| Containment deeper than `limits.max_tree_depth` | `limit_exceeded` |
| A group whose members are not pairwise independent | `invariant_violation` |
| A domain claimed by two groups | `conflict` |
| `usable > nominal`, a negative capacity, or an inconsistent state and cause | `invalid_argument` |
| A capacity above `limits.max_component_milliwatts` | `limit_exceeded` |
| Any rollup above `limits.max_aggregate_milliwatts` | `limit_exceeded` |
| An all-zero store identity | `invariant_violation` |
| A recorded path longer than `limits.max_path_bytes` | `limit_exceeded` |
