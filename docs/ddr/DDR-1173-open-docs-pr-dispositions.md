# DDR-1173: dispositions of open docs PRs #27, #31, #36

Status: decided. Checklist item C0.1 ("Open docs PRs #27, #31, #36: read,
merge or close, record why"), issue #45 comment 6201120568 item 1(c).

## Rule applied

A document either records history (dated, frozen, cited by later DDRs) or
reports current status. History is merged: it cannot go stale because it
claims nothing about the present. A second status list beside an existing
authority is closed: DDR-1114 measured that duplicated status rows drift and
that a stale copy is worse than none. The status authorities are
`docs/V1_MASTER_CHECKLIST.md` (work order) and `docs/FEATURE_STATUS.md`
(what exists).

## Dispositions

| PR | Content | Disposition | Reason |
|---|---|---|---|
| #31 | `docs/AUDIT_LIFETIME_2026-10.md`, read-only lifetime/UAF audit | **merge** | Dated audit record; DDR-1155 and DDR-1158 cite its findings. Claims nothing about current state, so it cannot drift. U1/U2/U3 remain open; merging the audit does not close them. |
| #27 | `OPERATOR_PLAN_2026-09-29.md` + `T1_REQUIREMENTS_REGISTER.md` | **close** | Superseded by the issue #45 OWNER orders (6177490081 onward) and the master checklist. The decisions it recorded already live in DDR-1143..1150, each citing its source comment. A second requirements register would drift from the checklist. |
| #36 | `OS_COMPONENT_CHECKLIST.md`, per-component status | **close** | A third status list beside the master checklist and FEATURE_STATUS. Its useful content (component inventory) is covered by FEATURE_STATUS and will be extended by `OS_GAP_ANALYSIS.md` (addendum 6201467383 item 3). |

Closed PRs stay readable on GitHub; nothing is deleted.

## Not claimed

No code change, no gate, no open issue moves.
