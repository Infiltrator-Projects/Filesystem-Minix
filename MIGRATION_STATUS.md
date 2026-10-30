# Minix independent filesystem migration (2026-10-30)

Status: initial independent standalone module + native regression test, **not** a declaration of full filesystem/defrag completeness.

- Canonical portable Minix V1/V2/V3 .so source and its test copied **unchanged** from Infiltrator-Projects/Filesystems at ca2fa1210c3c1f2b6f52cd15b3da6dfa83d453ad.
- Defragmenter's separate Minix relocation/analysis code is retained unchanged under `migration/defragmenter/` for later behaviour-preserving reconciliation, from Defragmenter at 30af892d8897d3aeaeefeac1bfcdb9620a0a5c72.
- `make` builds MINIX.so, `make test` builds and executes the existing synthetic Minix on-media test.
- Existing legacy ABI v1/v2 is retained. Common's new uniform ABI, Defrag V2 and Boot/kernel integration are not yet wired here.
- No old code has been removed from Filesystems or Defragmenter. Their current builds must remain intact until this standalone module is independently qualified.
