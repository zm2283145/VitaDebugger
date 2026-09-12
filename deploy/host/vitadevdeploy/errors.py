"""Shared exceptions for the host tools."""


class VitaDevDeployError(Exception):
    """Base class for expected deployment failures."""


class VpkValidationError(VitaDevDeployError):
    """The VPK or extracted package failed a safety check."""


class SfoFormatError(VpkValidationError):
    """A param.sfo file is malformed or missing required metadata."""


class ProtocolError(VitaDevDeployError):
    """A challenge, request, manifest, or result violates the protocol."""


class CryptoUnavailableError(VitaDevDeployError):
    """No supported Ed25519 implementation is available."""


class DeploymentError(VitaDevDeployError):
    """A network or remote deployment operation failed."""
