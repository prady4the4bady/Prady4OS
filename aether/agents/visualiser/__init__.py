"""Replayable run visualiser (Section 3D #65, DDR-856).

Spec: *"replayable run visualizer (optional ring-3 web UI)"*.

Reads the **existing** JSONL trajectory (`agents/budget/trajectory.py`,
DDR-849) and renders it. It does not define a second log format, and it reuses
`read_trajectory` rather than re-implementing the parse — a second parser would
drift from the writer, and the first symptom would be a run that renders subtly
wrong rather than failing.

Three properties, each because of a specific way a viewer goes wrong:

- **It never un-redacts.** Secrets are scrubbed at write time; a viewer that
  reached for the raw log to show "what really happened" would undo that on a
  page someone screenshots. The renderer has no access to anything but the
  redacted records, and a test asserts redaction markers survive.
- **Output is deterministic.** Same trajectory, byte-identical HTML. "Replayable"
  is the requirement; a page with a render timestamp in it is not replayable,
  and diffing two runs stops working the moment output varies for reasons the
  runs did not.
- **Self-contained, no network.** Inline CSS, no fonts, no scripts, no images.
  This is an offline OS; a viewer that fetches anything is broken exactly when
  it is needed, and on a privacy-mode machine it is also an egress attempt.

Everything from the trajectory is HTML-escaped. A field can contain markup —
agent output routinely does — and an unescaped record is a stored-XSS vector in
a page built from untrusted content.
"""

from __future__ import annotations

import html
import json
from typing import Any, Iterable, TextIO

from ..budget.trajectory import REDACTED, read_trajectory

__all__ = ["render_html", "render_summary", "VisualiserError"]


class VisualiserError(ValueError):
    """A trajectory that cannot be rendered."""


_CSS = """
:root{color-scheme:light dark}
body{font:14px/1.5 system-ui,sans-serif;margin:0;padding:1.5rem;
 background:#fff;color:#111}
@media(prefers-color-scheme:dark){body{background:#111;color:#eee}}
h1{font-size:1.15rem;margin:0 0 .25rem}
.meta{opacity:.7;font-size:.85rem;margin-bottom:1rem}
table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}
th,td{text-align:left;padding:.35rem .6rem;border-bottom:1px solid #8883;
 vertical-align:top}
th{font-weight:600;white-space:nowrap}
td.seq{opacity:.6;white-space:nowrap}
td.ev{font-weight:600;white-space:nowrap}
pre{margin:0;white-space:pre-wrap;word-break:break-word;font:12px/1.45
 ui-monospace,monospace}
.redacted{color:#b00;font-weight:600}
@media(prefers-color-scheme:dark){.redacted{color:#f88}}
.wrap{overflow-x:auto}
"""


def _fields(record: dict[str, Any]) -> str:
    """Render the non-structural fields as escaped JSON."""
    body = {k: v for k, v in record.items() if k not in ("run_id", "seq", "event")}
    if not body:
        return ""
    # sort_keys so the same record always renders identically.
    text = json.dumps(body, sort_keys=True, indent=2, default=str)
    escaped = html.escape(text, quote=True)
    return escaped.replace(
        html.escape(REDACTED), f'<span class="redacted">{html.escape(REDACTED)}</span>'
    )


def render_summary(records: list[dict[str, Any]]) -> dict[str, Any]:
    """Counts per event type plus the run id. Deterministic ordering."""
    if not records:
        raise VisualiserError(
            "empty trajectory — refusing to render a page that would look like "
            "a run that did nothing rather than a file that was not found"
        )
    run_ids = {r.get("run_id") for r in records}
    if len(run_ids) > 1:
        raise VisualiserError(
            f"trajectory mixes run ids {sorted(map(str, run_ids))} — rendering "
            "them as one run would invent a sequence that never happened"
        )
    counts: dict[str, int] = {}
    for r in records:
        counts[str(r.get("event", "?"))] = counts.get(str(r.get("event", "?")), 0) + 1
    return {
        "run_id": next(iter(run_ids)),
        "events": len(records),
        "by_event": dict(sorted(counts.items())),
    }


def render_html(source: TextIO | Iterable[dict[str, Any]], title: str = "PRADYOS run") -> str:
    """Render a trajectory to a self-contained HTML page.

    `source` may be an open JSONL stream (parsed with the writer's own reader)
    or an already-parsed sequence of records.
    """
    if hasattr(source, "read"):
        records = list(read_trajectory(source))  # type: ignore[arg-type]
    else:
        records = list(source)  # type: ignore[arg-type]

    summary = render_summary(records)
    rows = []
    for r in records:
        fields = _fields(r)
        rows.append(
            "<tr>"
            f'<td class="seq">{html.escape(str(r.get("seq", "")))}</td>'
            f'<td class="ev">{html.escape(str(r.get("event", "")))}</td>'
            f"<td>{'<pre>' + fields + '</pre>' if fields else ''}</td>"
            "</tr>"
        )

    by_event = ", ".join(f"{html.escape(k)}&nbsp;{v}"
                         for k, v in summary["by_event"].items())
    return (
        "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        f"<title>{html.escape(title)}</title><style>{_CSS}</style></head><body>"
        f"<h1>{html.escape(title)}</h1>"
        f'<p class="meta">run <code>{html.escape(str(summary["run_id"]))}</code>'
        f" &middot; {summary['events']} events &middot; {by_event}</p>"
        '<div class="wrap"><table><thead><tr><th>#</th><th>event</th>'
        "<th>fields</th></tr></thead><tbody>"
        + "".join(rows)
        + "</tbody></table></div></body></html>\n"
    )
