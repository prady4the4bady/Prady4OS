"""I-09 — the real D-08 wiring for D-06…D-15.

Each of D-06…D-15 was built with an ``alignment_tap`` slot and shipped against a
stub. This module is what the D-15 slice promised: the slot now carries a real
:class:`~aether.agents.safety.superalignment_monitor.SuperalignmentMonitor`.

The one design point worth stating. :func:`wire` registers **every** agent in
:data:`TAPPED_AGENTS` before handing out taps, rather than registering an agent
the first time it reports. Registering on first report is the natural-looking
version and it is exactly wrong: an agent that never starts never reports, so it
is never registered, so the monitor never notices it is missing. The failure
D-08 exists to catch — silence — would be invisible for precisely the agents
that failed hardest. Declaring the roster up front means a dead agent is a
missed deadline within one tick.

Intervals are per-agent because the loops run at different rates: D-06 observes
continuously, D-14 loads once at startup. A single global interval would either
be too tight for the slow agents or too loose to catch the fast ones going
quiet.
"""

from __future__ import annotations

from typing import Any, Callable

from aether.agents.safety.superalignment_monitor import SuperalignmentMonitor

__all__ = ["TAPPED_AGENTS", "wire", "taps_for"]

#: DDR id -> maximum silence, in seconds, before D-08 raises.
#: A value here is a claim about how often that agent's loop reports; it is
#: deliberately explicit per agent rather than one global number.
TAPPED_AGENTS: dict[str, float] = {
    "D-06": 60.0,      # causal engine — observes continuously
    "D-07": 120.0,     # hypothesis generation — bursty
    "D-09": 300.0,     # self-model revision — infrequent by design
    "D-10": 120.0,     # experiment outcomes — follows experiment cadence
    "D-11": 300.0,     # knowledge consolidation — periodic sweep
    "D-12": 60.0,      # goal prioritisation — every scheduling round
    "D-13": 600.0,     # failure registry — only on failure
    "D-14": 900.0,     # lineage load — once at startup
    "D-15": 600.0,     # skill composition — occasional
}


def wire(monitor: SuperalignmentMonitor) -> dict[str, Callable[[str, dict[str, Any]], None]]:
    """Register the whole roster with ``monitor`` and return per-agent taps.

    Registration happens for every agent up front — see the module docstring for
    why registering lazily on first report defeats the monitor.
    """
    for agent, interval in TAPPED_AGENTS.items():
        monitor.register(agent, max_interval_s=interval)
    return taps_for(monitor)


def taps_for(
    monitor: SuperalignmentMonitor,
) -> dict[str, Callable[[str, dict[str, Any]], None]]:
    """Per-agent tap callables, without touching registration.

    Every agent gets the same ``monitor.observe`` — it routes on the ``DDR.step``
    prefix the agents already emit, so no agent needs to know which monitor it
    is talking to.
    """
    return {agent: monitor.observe for agent in TAPPED_AGENTS}
