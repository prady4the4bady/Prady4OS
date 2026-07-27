"""B-14 — vision pipeline.

Turns images (screenshots, documents, camera frames) into text an agent can
reason about. The security-relevant fact about vision, and the reason this file
is not a thin OCR wrapper:

**Extracted text is untrusted input.** A screenshot can contain
``ignore all previous instructions`` rendered as pixels. OCR launders it into
plain text that looks like it came from the system. So every extraction leaves
this module wrapped in :class:`aether.agents.quarantine.namespace.TaintedContent`
and can only reach a prompt through the quarantine release path, which runs the
B-04 firewall.

A pipeline that returned ``str`` would make that impossible to enforce at the
type level, so it does not.

Backends are injected (OCR engine, caption model). No backend means an explicit
failure, never a silent empty result — an empty caption that looks like "nothing
here" is worse than an error.
"""

from __future__ import annotations

import hashlib
import time
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, Callable, Protocol

from aether.agents.quarantine.namespace import TaintedContent
from aether.kernel.audit.audit_log import AuditLog

__all__ = [
    "ImageSource",
    "VisionResult",
    "VisionPipeline",
    "VisionError",
    "OCRBackend",
    "CaptionBackend",
]

DDR_ID = "B-14"
_FILE = "aether/vision/pipeline.py"

MAX_IMAGE_BYTES = 32 * 1024 * 1024          # 32 MiB


class VisionError(RuntimeError):
    """Vision processing failed."""


class ImageSource(str, Enum):
    SCREENSHOT = "screenshot"
    DOCUMENT = "document"
    CAMERA = "camera"
    UNKNOWN = "unknown"


class OCRBackend(Protocol):
    def __call__(self, image_bytes: bytes) -> str: ...


class CaptionBackend(Protocol):
    def __call__(self, image_bytes: bytes) -> str: ...


@dataclass(slots=True)
class VisionResult:
    """Everything extracted from one image. All text is tainted."""

    source: ImageSource
    sha256: str
    width_hint: int = 0
    height_hint: int = 0
    ocr_text: TaintedContent | None = None
    caption: TaintedContent | None = None
    ts: float = field(default_factory=time.time)
    meta: dict[str, Any] = field(default_factory=dict)

    @property
    def has_text(self) -> bool:
        return self.ocr_text is not None and bool(self.ocr_text.data.strip())


class VisionPipeline:
    """Image -> tainted text. Backends injected; nothing runs implicitly."""

    def __init__(
        self,
        ocr: OCRBackend | None = None,
        caption: CaptionBackend | None = None,
        audit: AuditLog | None = None,
        max_bytes: int = MAX_IMAGE_BYTES,
    ) -> None:
        self.ocr = ocr
        self.caption = caption
        self.audit = audit if audit is not None else AuditLog()
        self.max_bytes = int(max_bytes)

    # -- ingestion -----------------------------------------------------------
    def _load(self, image: bytes | str | Path) -> bytes:
        if isinstance(image, bytes):
            data = image
        else:
            path = Path(image)
            if not path.is_file():
                raise VisionError(f"no such image: {image}")
            data = path.read_bytes()
        if not data:
            raise VisionError("empty image")
        if len(data) > self.max_bytes:
            raise VisionError(
                f"image too large: {len(data)} bytes > {self.max_bytes}"
            )
        return data

    def process(
        self,
        image: bytes | str | Path,
        source: ImageSource = ImageSource.UNKNOWN,
        want_ocr: bool = True,
        want_caption: bool = False,
    ) -> VisionResult:
        """Extract text and/or a caption. All output is tainted."""
        data = self._load(image)
        digest = hashlib.sha256(data).hexdigest()
        origin = f"vision:{source.value}:{digest[:12]}"

        result = VisionResult(source=source, sha256=digest)

        if want_ocr:
            if self.ocr is None:
                raise VisionError("OCR requested but no OCR backend is configured")
            text = self.ocr(data)
            result.ocr_text = TaintedContent(data=text, origin=origin)

        if want_caption:
            if self.caption is None:
                raise VisionError(
                    "caption requested but no caption backend is configured"
                )
            cap = self.caption(data)
            result.caption = TaintedContent(data=cap, origin=origin)

        self.audit.append_action(
            ddr=DDR_ID,
            file=_FILE,
            action="vision.process",
            gate_status="ok",
            source=source.value,
            sha256=digest,
            ocr_chars=len(result.ocr_text.data) if result.ocr_text else 0,
            caption_chars=len(result.caption.data) if result.caption else 0,
        )
        return result

    # -- convenience ---------------------------------------------------------
    def process_many(
        self,
        images: list[bytes | str | Path],
        source: ImageSource = ImageSource.UNKNOWN,
        on_error: Callable[[Exception], None] | None = None,
    ) -> list[VisionResult]:
        """Process a batch. One bad image must not lose the whole batch."""
        out: list[VisionResult] = []
        for image in images:
            try:
                out.append(self.process(image, source=source))
            except (VisionError, OSError) as exc:
                self.audit.append_action(
                    ddr=DDR_ID, file=_FILE, action="vision.error",
                    gate_status="error", error=f"{type(exc).__name__}: {exc}",
                )
                if on_error is not None:
                    on_error(exc)
        return out
