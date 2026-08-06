"""Privacy routing, model routing, OCR→memory, context builder (3D #56-59)."""
from __future__ import annotations

import pytest

from aether.agents.budget.compress import CompressionImpossible
from aether.agents.routing import (
    Endpoint,
    ModelSpec,
    PrivacyBlocked,
    PrivacyState,
    Router,
    RoutingError,
    is_local_host,
)
from aether.agents.routing.ocr_memory import (
    MAX_DOCUMENT_BYTES,
    MIN_CONFIDENCE,
    ContextBuilder,
    ContextError,
    Modality,
    OcrError,
    OcrFragment,
    OcrPipeline,
)

LOCAL = Endpoint("127.0.0.1", 11434)
LAN = Endpoint("192.168.1.50", 11434)
REMOTE = Endpoint("api.example.com", 443)

LOCAL_7B = ModelSpec("local-7b", LOCAL, quality=3, est_cost_microcents_per_1k=0,
                     context_window=8192)
BIG = ModelSpec("big-remote", REMOTE, quality=9, est_cost_microcents_per_1k=300,
                context_window=200_000)


# ==========================================================================
# #58 privacy mode (ring-3)
# ==========================================================================
def test_unknown_privacy_state_blocks() -> None:
    """THE fail-closed rule. Assuming off turns an unreadable config into an
    open egress path, and nothing in the logs would say so."""
    assert PrivacyState.UNKNOWN.blocks_remote
    assert PrivacyState.ON.blocks_remote
    assert not PrivacyState.OFF.blocks_remote


def test_remote_model_is_blocked_under_privacy() -> None:
    r = Router([LOCAL_7B, BIG])
    with pytest.raises(PrivacyBlocked):
        r.check_allowed(BIG, PrivacyState.ON)
    with pytest.raises(PrivacyBlocked):
        r.check_allowed(BIG, PrivacyState.UNKNOWN)
    r.check_allowed(BIG, PrivacyState.OFF)  # must not raise


def test_local_model_is_always_allowed() -> None:
    r = Router([LOCAL_7B])
    for state in PrivacyState:
        r.check_allowed(LOCAL_7B, state)


def test_privacy_blocked_is_its_own_type_so_it_is_countable() -> None:
    """Folding it into RoutingError makes "did privacy stop anything?"
    unanswerable — same reasoning as AR_PRIVACY_BLOCKED's own audit code."""
    assert issubclass(PrivacyBlocked, RoutingError)
    assert PrivacyBlocked is not RoutingError


def test_loopback_and_private_ranges_are_local() -> None:
    for host in ("127.0.0.1", "::1", "localhost", "192.168.1.5", "10.0.0.7",
                 "172.16.0.1", "169.254.1.1"):
        assert is_local_host(host), host


def test_public_addresses_are_not_local() -> None:
    for host in ("8.8.8.8", "1.1.1.1", "2001:4860:4860::8888"):
        assert not is_local_host(host), host


def test_unresolved_hostname_is_not_treated_as_local() -> None:
    """Resolution can change between the check and the connection; treating a
    name as local would make classification depend on a DNS answer never seen."""
    for host in ("api.example.com", "ollama.local", "my-localhost-proxy.net", ""):
        assert not is_local_host(host), host


def test_lan_address_counts_as_local() -> None:
    assert ModelSpec("lan", LAN, 5, 0, 8192).endpoint.is_local


# ==========================================================================
# #59 model routing
# ==========================================================================
def test_privacy_forces_the_local_model() -> None:
    r = Router([LOCAL_7B, BIG])
    assert r.route(required_tokens=1000, privacy=PrivacyState.ON).name == "local-7b"
    assert r.route(required_tokens=1000, privacy=PrivacyState.OFF).name == "local-7b"


def test_routing_refuses_when_nothing_fits_rather_than_near_missing() -> None:
    """A caller handed a model that does not fit discovers it as a truncation."""
    r = Router([LOCAL_7B, BIG])
    with pytest.raises(RoutingError, match="no model satisfies"):
        r.route(required_tokens=50_000, privacy=PrivacyState.ON)


def test_rejection_reasons_are_reported() -> None:
    r = Router([LOCAL_7B, BIG])
    with pytest.raises(RoutingError, match="context 8192"):
        r.route(required_tokens=50_000, privacy=PrivacyState.ON)


def test_context_window_is_respected() -> None:
    r = Router([LOCAL_7B, BIG])
    assert r.route(required_tokens=50_000, privacy=PrivacyState.OFF).name == "big-remote"


def test_budget_excludes_a_model_it_cannot_pay_for() -> None:
    r = Router([BIG])
    with pytest.raises(RoutingError, match="budget"):
        r.route(required_tokens=10_000, privacy=PrivacyState.OFF, budget_microcents=100)
    assert r.route(
        required_tokens=10_000, privacy=PrivacyState.OFF, budget_microcents=10_000
    ).name == "big-remote"


def test_min_quality_is_respected() -> None:
    r = Router([LOCAL_7B, BIG])
    assert r.route(
        required_tokens=1000, privacy=PrivacyState.OFF, min_quality=5
    ).name == "big-remote"


def test_routing_is_deterministic_across_registration_order() -> None:
    """A router whose choice depends on dict order makes a bad answer
    impossible to attribute to a model."""
    a = ModelSpec("alpha", LOCAL, quality=5, est_cost_microcents_per_1k=10,
                  context_window=8192)
    b = ModelSpec("beta", LOCAL, quality=5, est_cost_microcents_per_1k=10,
                  context_window=8192)
    r1 = Router([a, b]).route(required_tokens=100, privacy=PrivacyState.ON)
    r2 = Router([b, a]).route(required_tokens=100, privacy=PrivacyState.ON)
    assert r1.name == r2.name == "alpha"


def test_ties_break_toward_higher_quality() -> None:
    lo = ModelSpec("m-lo", LOCAL, quality=2, est_cost_microcents_per_1k=10,
                   context_window=8192)
    hi = ModelSpec("m-hi", LOCAL, quality=8, est_cost_microcents_per_1k=10,
                   context_window=8192)
    assert Router([lo, hi]).route(
        required_tokens=100, privacy=PrivacyState.ON
    ).name == "m-hi"


def test_duplicate_model_name_is_refused() -> None:
    r = Router([LOCAL_7B])
    with pytest.raises(RoutingError, match="already registered"):
        r.register(ModelSpec("local-7b", LAN, 1, 1, 100))


def test_empty_router_and_bad_inputs_are_refused() -> None:
    with pytest.raises(RoutingError, match="no models"):
        Router().route(required_tokens=1, privacy=PrivacyState.ON)
    with pytest.raises(RoutingError):
        Router([LOCAL_7B]).route(required_tokens=0, privacy=PrivacyState.ON)
    with pytest.raises(RoutingError):
        Endpoint("h", 0)
    with pytest.raises(RoutingError):
        ModelSpec("m", LOCAL, -1, 0, 10)
    with pytest.raises(RoutingError):
        ModelSpec("m", LOCAL, 1, 0, 0)


# ==========================================================================
# #56 OCR → memory
# ==========================================================================
def test_low_confidence_text_is_quarantined_not_stored_as_fact() -> None:
    """THE rule. A misrecognised digit arrives looking like a number."""
    p = OcrPipeline()
    p.ingest("invoice.pdf", [
        OcrFragment("Total: 1250.00", 0.97, page=1),
        OcrFragment("Total: 125O.OO", 0.41, page=2),
    ])
    assert len(p.facts()) == 1
    assert len(p.quarantined) == 1
    assert p.quarantined[0].quarantined


def test_ingest_returns_quarantined_records_too() -> None:
    """So a caller cannot read the stored set as "nothing was found there"."""
    p = OcrPipeline()
    out = p.ingest("d.pdf", [OcrFragment("blurry", 0.2, page=1)])
    assert len(out) == 1 and out[0].quarantined


def test_confidence_at_the_floor_is_stored() -> None:
    p = OcrPipeline()
    p.ingest("d.pdf", [OcrFragment("crisp", MIN_CONFIDENCE, page=1)])
    assert len(p.facts()) == 1


def test_provenance_is_mandatory() -> None:
    from aether.agents.routing.ocr_memory import MemoryRecord

    with pytest.raises(OcrError, match="source document"):
        MemoryRecord("t", "  ", 1, 0.9)


def test_records_carry_page_and_document() -> None:
    p = OcrPipeline()
    rec = p.ingest("report.pdf", [OcrFragment("finding", 0.95, page=7)])[0]
    assert rec.source_document == "report.pdf" and rec.page == 7


def test_duplicate_regions_are_not_stored_twice() -> None:
    """Storing them twice makes a repeated line look like corroboration."""
    p = OcrPipeline()
    frags = [OcrFragment("same", 0.95, page=1), OcrFragment("same", 0.95, page=1)]
    assert len(p.ingest("d.pdf", frags)) == 1


def test_same_text_on_a_different_page_is_kept() -> None:
    p = OcrPipeline()
    frags = [OcrFragment("same", 0.95, page=1), OcrFragment("same", 0.95, page=2)]
    assert len(p.ingest("d.pdf", frags)) == 2


def test_oversized_document_is_refused() -> None:
    p = OcrPipeline()
    with pytest.raises(OcrError, match="cap"):
        p.ingest("huge.pdf", [], size_bytes=MAX_DOCUMENT_BYTES + 1)


def test_bad_confidence_and_page_are_refused() -> None:
    with pytest.raises(OcrError):
        OcrFragment("t", 1.5, page=1)
    with pytest.raises(OcrError):
        OcrFragment("t", 0.9, page=-1)
    with pytest.raises(OcrError):
        OcrPipeline(min_confidence=0.0)
    with pytest.raises(OcrError):
        OcrPipeline().ingest("  ", [])


# ==========================================================================
# #57 multi-modal context builder
# ==========================================================================
def test_empty_fragment_is_refused_not_sized_at_zero() -> None:
    """Free content is never evicted, so the one thing nobody could size would
    survive every compression."""
    with pytest.raises(ContextError, match="empty fragment"):
        ContextBuilder().add("k", "   ")


def test_quarantined_memory_enters_marked_and_lowest_priority() -> None:
    """Passing it in clean would launder a low-confidence guess into an
    authoritative-looking line of context."""
    p = OcrPipeline()
    rec = p.ingest("d.pdf", [OcrFragment("maybe 42", 0.3, page=1)])[0]
    seg = ContextBuilder().add_memory(rec, "m1")
    assert "[UNVERIFIED OCR]" in seg.text
    assert seg.priority == -1


def test_confident_memory_carries_provenance_without_the_marker() -> None:
    p = OcrPipeline()
    rec = p.ingest("d.pdf", [OcrFragment("certainly 42", 0.99, page=3)])[0]
    seg = ContextBuilder().add_memory(rec, "m1")
    assert "[UNVERIFIED OCR]" not in seg.text
    assert "d.pdf p3" in seg.text and "0.99" in seg.text


def test_modality_priority_orders_eviction() -> None:
    b = ContextBuilder()
    b.add("task", "task words " * 60, modality=Modality.TEXT)
    b.add("scene", "scene words " * 60, modality=Modality.SCENE)
    dropped = {s.key for s in b.build(0.6).dropped}
    assert dropped == {"scene"}, "scene is re-derivable; task text is not"


def test_pinned_fragment_survives_and_impossible_target_raises() -> None:
    b = ContextBuilder()
    b.add("system", "system words " * 200, pinned=True)
    b.add("chat", "chat", modality=Modality.TEXT)
    with pytest.raises(CompressionImpossible):
        b.build(0.5)


def test_build_respects_the_compression_target() -> None:
    b = ContextBuilder()
    for i in range(10):
        b.add(f"s{i}", f"segment {i} " * 40)
    res = b.build()
    assert res.fraction <= 0.8


def test_duplicate_key_and_empty_context_are_refused() -> None:
    b = ContextBuilder()
    b.add("k", "text here")
    with pytest.raises(ContextError, match="duplicate"):
        b.add("k", "other text")
    with pytest.raises(ContextError, match="empty context"):
        ContextBuilder().build()
    with pytest.raises(ContextError):
        b.add("  ", "text")


def test_unknown_modality_is_refused() -> None:
    with pytest.raises(ContextError, match="unknown modality"):
        ContextBuilder().add("k", "text", modality="ocr")  # type: ignore[arg-type]


def test_tokens_reports_the_uncompressed_size() -> None:
    b = ContextBuilder()
    b.add("a", "one two three four")
    assert b.tokens > 0
