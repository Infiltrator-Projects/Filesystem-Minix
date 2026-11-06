# Minix source consolidation — 6 November 2026

All 14 canonical Minix source files in the central `Filesystems` main source tree at `a253fb724ce1fd760a361219281effb6abd71367` are now represented in this independent repository. Its original 3 matching Minix source files were byte-identical and preserved; the other 11 original Linux VFS source files were imported verbatim with source attribution retained.

`standalone/kernel.json` now uses this repository's own `native/filesystems/minix` tree rather than downloading a pinned historical Filesystems revision. The `.so` module continues to use the repository's original canonical engine and regression suite; `minix.ko` is built locally from its source in this repository.

This is source-ownership consolidation, not proof of full runtime qualification. Continue independently checking the Linux module's ABI, disposable-media read/write/remount tests and native RUN package output. Do not remove the central tree until Filesystems' consumers and CI are detached and their replacement build passes.
