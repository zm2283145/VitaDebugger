# Pre-DBGVCR read-only baseline

This `VDCP00005` sample was taken immediately before the one-shot DIP 228 +
DBGVCR experiment on 2026-09-13. Both journal slots validate. The newest slot
was B, revision 12, sequence 4. It reported result zero, validity flags
`0x0000000F`, core 1, zero raw/debug/system words, and direct bits 203 and 228
both clear.

SHA-256:

- A: `3A85980FC0672F894E3B651981B211755180D4A9EDCBEB7BE795AA8E7F5DE8B4BB`
- B: `D00081A759E2B5AF1E49BBC22A525B37AD027224A9E15DACFDEF1B9C739C0642`

This establishes the clean cached DIP state immediately before the hazardous
probe. These files are raw checksummed probe journals, not synthesized
fixtures.

Related evidence:

- [DBGVCR hardware run](../../../dipsw-dbgvcr-probe/hardware-results/2026-09-13-first-run/README.md)
- [Post-reboot read-only recovery](../2026-09-13-post-dbgvcr-reboot/README.md)
