# DIP 228 set/read/restore hardware result

The owner-attended one-shot `VDCP00006` transaction completed successfully on
2026-09-13. The newest valid slot was A, sequence 1, revision 5. It reported:

- state `COMPLETE`, primary result 0, restoration result 0;
- validity flags `0x00003FFF` and core 0;
- exactly one Set call and exactly one Clear call;
- baseline system word `0x00000000`, set-window word `0x00000010`, and restored
  word `0x00000000`;
- direct bit 228 changed from 0 to 1 and returned to 0;
- bit 203 remained 0 and the complete restored snapshot matched the baseline.

SHA-256:

- A: `C1CF826B0C86626EB0FDC7958A6857A1AD988A160935723D8A927302EAA155D2`
- B: `CDB271203E06738D80DEC7E96C72B94A45DB7DBDDC819F642FB7C5B9CB634FCC`

Both slots were pulled again after reboot and matched these hashes exactly. This
proves cached API mutation/readback/restoration only; it does not prove debug
register access or hardware-breakpoint availability.
