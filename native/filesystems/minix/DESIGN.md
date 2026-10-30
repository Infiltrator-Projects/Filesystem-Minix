# Minix filesystem — implementation and qualification

## Format and source authority

Minix V1, V2 and V3 are variants of one on-disk filesystem identity,
`minix`, not three catalogue entries. The checked-in Linux VFS implementation
under `native/filesystems/minix/linux/` is original editable Linux source
with its own attribution. It is a genuine kernel implementation and remains
subject to independent host-kernel build and runtime qualification.

The portable C module under `native/filesystems/minix/module/` is
Filesystems' genuine, freestanding userspace engine for parsing Minix V1/V2/V3
on-media structures. It is **not** a copied Linux kernel binary, a proxy for
`minix.ko`, or a generic wrapper around `mount`. Both code paths interpret the
same format but their semantics are not yet consolidated into one shared
canonical core. That consolidation remains an explicit architectural task.

## Implemented application module

- `MINIX.so` contains the module's C engine, identity metadata and V1
  read-only entry point. Its V1 ABI is unchanged by later capability additions.
- The V1 parser validates superblocks, inode allocation, directory records,
  file sizes, direct zones and single/double/triple-indirect tree mappings
  as applicable to the media version.
- The V2 entry point independently advertises a *restricted* existing-file
  overwrite operation, not a general writable filesystem.
- V2 requires an allocated regular file, unchanged live inode pointers and
  geometry, complete destination-zone preflight, range-bounded write callbacks,
  and a successful host flush. Sparse holes, metadata/directory writes,
  file extension, missing flush or untrusted caller inode pointers are refused.
- The host is responsible for exclusive medium ownership, ensuring read/write
  callbacks access the same partition and preventing concurrent metadata
  changes. Once a callback begins writing, failures can leave partially
  committed data: V2 does not implement rollback or crash recovery.
- The app-owned source and compile helper install under
  `/Programs/Filesystems/Sources/minix/module` and
  `/Programs/Filesystems/Helpers/minix-module-helper`; the selected binary
  goes directly to `/System/Filesystems/MINIX.so`. Selecting MINIX does
  not install a Linux kernel driver, EFI image or unrelated filesystem.

## Independent qualifications

`tests/minix_dropin_module_test.c` constructs versioned media and exercises
directory traversal, direct and indirect blocks, sparse reads, corrupt
allocation metadata and V2 existing-data writes/refusals. The synthetic
writer tests cross allocated zone boundaries and checks that an attempted
operation spanning an unallocated hole performs **no data writes**.

`tests/minix_real_mkfs_test.c` checks independently generated disposable
`mkfs.minix` images for V1 14/30-character, V2 14/30-character, and V3
60-character directory formats. The separate release-gated workflow compiles
the actual freestanding `MINIX.so`, requires both versioned ELF symbols,
the matching nonexecuted `.ifsmeta` identity records and no dynamic
`NEEDED` dependencies. Real `mkfs.minix` tests currently qualify **reads**
and superblock/directory interoperability, not V2 on-media writes.

Linux `minix.ko` has its own kernel build/runtime qualification and is
not implicitly substituted for this application `.so`.

## Unfinished work — do not mark complete

General file creation, allocation, extension, truncation, deletion, rename,
permissions/ownership changes, timestamps, link management, full writable
mount behaviour, metadata journalling, recovery and physical-device write
qualification are **not** supplied by the standalone module.

A Minix-specific `.efi` firmware driver, root-filesystem bootstrap, and
cross-product common load/dispatch ABI are not completed. The Linux module
and the freestanding `.so` are distinct ABI artifacts and cannot simply
be renamed to obtain firmware support.

The canonical engine must ultimately reconcile shared format semantics
between the userspace engine, Linux VFS adapter and future EFI reader without
copying diverging rules into three independent implementations.

## Preservation

Keep Linux VFS integration and its original attribution. Do not overwrite
working FAT12/FAT16 V3/V4 sources, defragmentation code, kernel modules,
EFI drivers, existing boot partitions or active filesystem volumes as part
of Minix development. Use disposable media and independently qualified
replacements before any production storage migration.
