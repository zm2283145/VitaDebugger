"""Small isolated client for Vita Companion's command port."""

from __future__ import annotations

import socket
from dataclasses import dataclass

from .errors import DeploymentError
from .paths import validate_title_id


@dataclass
class VitaCompanionClient:
    host: str
    port: int = 1338
    timeout: float = 5.0

    def command(self, command: str) -> str:
        if (
            not command
            or len(command) > 128
            or not command.isascii()
            or any(char in command for char in "\r\n;")
        ):
            raise DeploymentError("refusing unsafe Vita Companion command text")
        try:
            with socket.create_connection((self.host, self.port), timeout=self.timeout) as connection:
                connection.settimeout(self.timeout)
                connection.sendall(command.encode("ascii") + b"\n")
                chunks: list[bytes] = []
                size = 0
                while True:
                    block = connection.recv(2048)
                    if not block:
                        break
                    size += len(block)
                    if size > 8192:
                        raise DeploymentError("Vita Companion response exceeds 8192 bytes")
                    chunks.append(block)
        except OSError as exc:
            raise DeploymentError(f"Vita Companion command failed: {exc}") from exc
        try:
            return b"".join(chunks).decode("utf-8", "strict").strip()
        except UnicodeDecodeError as exc:
            raise DeploymentError("Vita Companion returned invalid UTF-8") from exc

    def version(self) -> str:
        response = self.command("version")
        if not response or response.startswith("Error:"):
            raise DeploymentError(f"Vita Companion version check failed: {response or 'empty response'}")
        return response

    def kill(self, title_id: str, *, require_success: bool = False) -> str:
        response = self.command(f"kill {validate_title_id(title_id)}")
        if require_success and response != "Killed.":
            raise DeploymentError(f"Vita Companion could not kill {title_id}: {response}")
        return response

    def destroy(self) -> str:
        """Close open user applications so LiveArea cannot block a launch."""

        response = self.command("destroy")
        if response != "Apps destroyed.":
            raise DeploymentError(f"Vita Companion could not close open applications: {response}")
        return response

    def launch(self, title_id: str) -> str:
        response = self.command(f"launch {validate_title_id(title_id)}")
        if response != "Launched.":
            raise DeploymentError(f"Vita Companion could not launch {title_id}: {response}")
        return response
