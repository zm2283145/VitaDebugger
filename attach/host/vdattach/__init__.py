"""Read-only attach discovery protocol and host client."""

from .client import AttachDiscoveryClient, DiscoverySnapshot
from .errors import AttachError, CapabilityError, ProtocolError, RemoteError, TransportError

__all__ = [
    "AttachDiscoveryClient",
    "DiscoverySnapshot",
    "AttachError",
    "CapabilityError",
    "ProtocolError",
    "RemoteError",
    "TransportError",
]
