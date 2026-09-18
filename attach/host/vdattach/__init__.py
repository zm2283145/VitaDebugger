"""Read-only discovery and authentication-only attach clients."""

from .auth import AuthenticatedAttachClient, AuthenticatedSession
from .auth_keys import JsonKeyStore, UnavailableKeyStore
from .client import AttachDiscoveryClient, DiscoverySnapshot
from .errors import (
    AttachError,
    AuthenticationError,
    CapabilityError,
    ProtocolError,
    RateLimitError,
    RemoteError,
    TransportError,
)

__all__ = [
    "AuthenticatedAttachClient",
    "AuthenticatedSession",
    "AttachDiscoveryClient",
    "DiscoverySnapshot",
    "JsonKeyStore",
    "UnavailableKeyStore",
    "AttachError",
    "AuthenticationError",
    "CapabilityError",
    "ProtocolError",
    "RateLimitError",
    "RemoteError",
    "TransportError",
]
