# Stage 1 host-only startup diagnosis

The first authorized Stage 1 run installed the exact reviewed `VDSCRN001`
artifact and exact 128-byte status+screen configuration. VitaCompanion 1.06
reported `Launched.`, but the endpoint never connected to the already-listening
screen receiver and control port 18198 was not observable. The run stopped
without a retry. Cleanup removed the one-session configuration and secrets;
the sealed evidence remains outside the repository.

Host review narrowed the failure to the endpoint's pre-service network startup:

- the installed `eboot.bin` and `param.sfo` matched the authorized VPK;
- the remote configuration was read back byte-for-byte before launch;
- the title remained addressable by exact-title cleanup, ruling out install
  identity failure;
- frame allocation fits VitaSDK's default newlib heap and the package linked
  every required pre-existing SceNet API; and
- both sockets are deliberately closed when screen startup fails, explaining
  why neither port remained observable after the receiver timeout.

The source then revealed two concrete startup defects. It initialized SceNet
and immediately bound the configured interface without initializing NetCtl or
waiting for that exact address to become connected. It also polled `SO_ERROR`
immediately after a nonblocking `connect` and accepted zero as completion
without first receiving a socket-writable event. That behavior differed from
the repository's hardware-qualified Vita TCP transport.

The corrected endpoint:

1. initializes NetCtl after SceNet;
2. waits at most ten seconds for `SCE_NETCTL_STATE_CONNECTED`;
3. reads the interface address and requires an exact match with the configured
   Vita bind IPv4 before creating either service socket;
4. uses SceNet epoll writability/error/hangup events to complete the outbound
   nonblocking screen connection;
5. validates `SO_ERROR` only after readiness; and
6. destroys epoll, NetCtl, SceNet, and the network module in ownership order.

Address mismatch, readiness timeout, event error, and connect error remain
terminal. The endpoint never selects a wildcard, another interface, another
port, or an automatic reconnect path.

This is an evidence-bound root-cause correction, not a hardware pass. No
device was contacted during diagnosis or validation. A later hardware run
requires new explicit authorization and must still exclude suspend and
intentional Wi-Fi/network disruption.
