# RSP admission core curation

**Terminal retail verdict: RSP NOT QUALIFIED.** No hardware pass is claimed.
The retained retail 3.65 records show bounded admission/reset successes and a
later completion attempt that received neither an ACK nor a response to its
first valid `qSupported`. Unrun cases remain unqualified, and the evidence does
not generalize to other firmware, devices, or transports.

## Source and scope

- Base: `39a072467710d53b821d61bb1425872bc4249254`
- Evidence source: `281004aa24dbc36ba82a9a6a0753c6a25ad72e50`
- Local safety ref: `refs/archive/rsp-evidence-281004a`
- Retained evidence:
  [`rsp-admission-confirmation-3.65.json`](hardware/rsp-admission-confirmation-3.65.json)
  and
  [`rsp-completion-terminal-3.65.json`](hardware/rsp-completion-terminal-3.65.json)

| Source change | Curated surface |
| --- | --- |
| `1e58cae` client admission | `src/uvdb.c`, `uvdb.h`, production-TU host coverage, and lifecycle documentation |
| `6a9d066` bounded network lifecycle | `src/uvdb.c`, protocol/frame helpers, forced library/kernel host variants, and focused tests |
| `f6bacc8` stopped-reset recovery | stopped-session normalization and repeated ACK/no-ack reconnect coverage |
| `3a6250a` raw Vita EAGAIN | exact encoded and signed `-35` classification with fatal unrelated negatives |
| `e080f27` unavailable registers | standalone strict legacy `g` reply validator and focused Python tests, without the experimental matrix runner |
| `e8f96fd`, `281004a` evidence | canonical admission and terminal records only |

Excluded source-only surfaces include hardware retry/per-attempt JSON, startup
and admission polling journals, endpoint/self-probe and raw-receive
instrumentation, UDP diagnostics, experimental runners and matrices, and their
tests. None is referenced by the curated production or test code. Phase 1
profiler, mutation, and attach changes from the base are preserved.
