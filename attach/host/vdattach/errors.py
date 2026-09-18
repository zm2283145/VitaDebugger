"""Errors raised by the read-only VitaDebugger attach discovery scaffold."""


class AttachError(RuntimeError):
    """Base error for attach discovery and transport failures."""


class ProtocolError(AttachError):
    """A wire record was malformed, ambiguous, or unsupported."""


class TransportError(AttachError):
    """The bounded TCP exchange failed."""


class CapabilityError(AttachError):
    """The peer cannot satisfy the fail-closed discovery contract."""


class RemoteError(AttachError):
    """The broker returned an explicit non-success result."""


class AuthenticationError(AttachError):
    """Mutual peer authentication failed closed."""


class RateLimitError(AuthenticationError):
    """Authentication is temporarily blocked after repeated failures."""

    def __init__(self, retry_after_ms: int):
        super().__init__(
            f"authenticated attach retry is blocked for {retry_after_ms} ms"
        )
        self.retry_after_ms = retry_after_ms
