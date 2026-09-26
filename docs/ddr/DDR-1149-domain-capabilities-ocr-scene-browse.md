# DDR-1149 — CAP_OCR / CAP_SCENE / CAP_NET_BROWSE: a real capability boundary at submission

Status: DESIGN (committed before code, NON-NEGOTIABLE 5).

## 0. Authority

This implements operator decision 6, PR #17 comment 5845610518, verified at the
primary source as `author_association: OWNER` (2026-09-26):

> Per DDR-982 §5.3, grant all three capability types using the same pattern
> already established for `CAP_EXEC`/`CAP_IPC`: a real capability bit checked
> by `cap_authorize`, paired with an explicit `is_*` field on `struct tcb`,
> kernel-set at spawn, explicitly zeroed in `sched_create` per NON-NEGOTIABLE
> 10, never mintable from ring 3 … Same mutation-testing and real-gate bar.

This answers DDR-982 §5.5's open question, "should the absent action types be
declared, accepting that they can only ever return 'not implemented', in order
to have a capability boundary to enforce?", in the affirmative.

## 1. What a capability can guard here, and what it cannot

`CAP_EXEC` and `CAP_IPC` each guard a **door that does something**: NSI 100
runs a program, and NSI 98 moves a message. The three new bits have **no such
door**, because the subsystems behind them do not exist:

| capability | action | subsystem that does not exist |
|---|---|---|
| `CAP_OCR` | `ACTION_PARSE_DOCUMENT` | a 64 MiB OCR model with no model-shipping path |
| `CAP_SCENE` | `ACTION_QUERY_SCENE` | a scene graph (post-L7) |
| `CAP_NET_BROWSE` | `ACTION_BROWSE_WEB` | the cloud bridge, deferred as a security-posture change (DDR-793) |

**The one door that does exist is action submission** (NSI 31
`SYS_SUBMIT_ACTION`, NSI 92 `SYS_SUBMIT_CHILD_ACTION`). That is where these
checks go. The boundary they enforce is exactly this: **a process may not
propose a domain action unless the kernel granted it that domain.** That is a
real, two-sided, testable property. It is also all this DDR claims.

**This does NOT unblock the domain behaviours.** AHNIS still cannot read a
document, IRIS cannot see a scene, and LUMYN cannot browse. An approved
`PARSE_DOCUMENT` has no executor, just as DDR-1013 §2 records for every action
type (the kernel arbitrates and the agent acts). The operator's phrase
"unblocks AHNIS, IRIS" is true of the **capability layer**, which was the
refused part, and **false of the behaviours**. That gap is flagged here per the
operator's instruction to flag anything thinner than it reads.

## 2. Design

- **Enum.** `ACTION_PARSE_DOCUMENT` (16), `ACTION_QUERY_SCENE` (17) and
  `ACTION_BROWSE_WEB` (18) are **appended**, never inserted.
  - Each is `_Static_assert`-pinned, because the probe hand-copies all three.
  - `aether.h`'s "deliberately ABSENT" comment is corrected at the site.
    `CAPTURE_FRAME`, `SCAN_ENVIRONMENT` and `EXEC_CODE` stay absent.
- **`struct tcb`** gains `is_ocr`/`ocr_cap`, `is_scene`/`scene_cap` and
  `is_browse`/`browse_cap`. All six are explicitly initialised in
  `sched_create_state` (NON-NEGOTIABLE 10).
- **Grants** are `ocr_grant()`, `scene_grant()` and `browse_grant()`. They are
  kernel-only, with the `exec_grant` shape: set the flag, then
  `cap_create(t->caps, RES_DOMAIN, <per-domain res_id>, CAP_<X>)`. There is no
  ring-3 path to any of them.
- **One check helper** is called from **both** submit paths:
  `domain_cap_ok(t, type)`.
  - It returns 1 for every non-domain type.
  - For a domain type it requires the flag **and** `cap_authorize` on the
    matching handle, resource type and right.
  - A refusal returns `-EPERM` and is audited as `AR_CAP_DENIED` with the
    action type. The existing `is_agent` refusal records type 0; this one
    records the type, because a denied domain submission is a different fact
    (DDR-801's rule).
- **Policy is unchanged.** `aether_action_forces_pending()` is not modified.
  Whether `BROWSE_WEB` should force a human decision is a DDR-842 S4 policy
  question. It is recorded for the operator, not taken. Nothing executes the
  type, so no egress can result either way.
- **Per-slot agent grants at spawn are NOT wired.** Roster slots are generic
  (DDR-1022: one agent program), so no slot is "AHNIS". DDR-982 §5.4's
  create-then-init race (`sys_spawn_agent` unblocks before recording the slot)
  would also have to be fixed first. The only callers of the grants today are
  the gate's probes. This is recorded as the next step.

## 3. Gate: `smoke-domcap`, probe key `domcap`

`user/domcaptest.c` is spawned **four** times through `elf_load_args`, with
`argv[1]` naming the role. Every process is `is_agent`:

| role | doors (`is_*`) | caps | expected `ocr scene browse child` |
|---|---|---|---|
| `GRANT` | all three | all three | `ok ok ok ok` |
| `NODOOR` | none | all three | `-1 -1 -1 -1` |
| `NOCAP` | all three | none | `-1 -1 -1 -1` |
| `CROSS` | OCR only | OCR only | `ok -1 -1 ok` |

- `child` submits `PARSE_DOCUMENT` through NSI 92 with `parent = 0`, so the
  second path is covered.
- Accepted actions are polled so their queue slots are released.
- The gate requires the four lines **exactly**, with no `< 0` pattern (DDR-1044).

**Why four roles and not two** (DDR-1033's measured lesson):

- `NODOOR` holds the capability and lacks only the flag.
- `NOCAP` holds the flag and lacks only the capability.
- Each isolates one check, so neither check can be deleted with the gate green.
- `CROSS` is what stops a per-type check collapsing into "any domain flag".

## 4. Mutants (each must fail a different line)

| mutant | change | line expected to fail |
|---|---|---|
| **M1** | flag check removed | `NODOOR` |
| **M2** | `cap_authorize` removed | `NOCAP` |
| **M3** | `SCENE` tests `is_ocr` (a wrong-flag mapping) | `CROSS` |
| **M4** | check dropped on the child path only | `NODOOR`/`NOCAP` `child` field |

## 5. NOT CLAIMED

- No OCR, scene graph or browser exists, and none is claimed.
- No domain action is executed by anything.
- DDR-793's cloud-bridge deferral is untouched.
- `forces_pending` is unchanged.
- No per-slot agent grant exists, and DDR-982 §5.4 is not fixed.
- No existing gate's semantics change, because the three enum values are new.
- `GLOBAL_FORBIDDEN` stays at 77.
