# Minix source consolidation — 6 November 2026

All 14 canonical Minix source files in the central `Filesystems` main source tree at `a253fb724ce1fd760a361219281effb6abd71367` are now represented in this independent repository. Its original 3 matching Minix source files were byte-identical and preserved; the other 11 original Linux VFS source files were imported verbatim with source attribution retained.

`standalone/kernel.json` now uses this repository's own `native/filesystems/minix` tree rather than downloading a pinned historical Filesystems revision. The `.so` module continues to use the repository's original canonical engine and regression suite; `minix.ko` is built locally from its source in this repository.

This is source-ownership consolidation, not proof of full runtime qualification. Continue independently checking the Linux module's ABI, disposable-media read/write/remount tests and native RUN package output. Do not remove the central tree until Filesystems' consumers and CI are detached and their replacement build passes.

## Common V2 native Minix zone bitmap accounting (1.0.11)
The Common V2 CORE mount now checks real on-media zone-allocation bits
using the canonical Minix first_data_zone/bit-1 convention. The verified
count of unallocated data zones, not a synthetic zero, is returned by
VOLUME.QUERY. The read-only scanner uses authenticated host I/O and
rejects truncated, invalid or reserved-bit-corrupt bitmaps. Existing
native Minix file mapping and bounded overwrites remain unchanged;
full transactional V2 DEFRAG.PLAN/EXECUTE/VERIFY/RECOVER remain absent.
