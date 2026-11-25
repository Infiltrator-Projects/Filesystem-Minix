# Filesystem-Minix

## 1. Purpose and format scope

Minix filesystem; variants are selected from native disk signatures. This repository owns the format-specific implementation of `MINIX.so` for InfiltratorOS.

## 2. On-disk architecture and invariants

Superblock geometry, inode and zone bitmaps and direct/indirect zone mappings are format-specific. The V3 mutation path distinguishes appending at EOF from unsupported sparse writes.

## 3. Common V2 implementation and capabilities

The hosted descriptor declares probe, mount, stat, directory enumeration, read, write, create and V3 format. Writable operations check exclusive durable host access. The canonical entry point is `infiltrator_filesystem_api_v2`. Consumers must negotiate the actual profile and operation masks and use bounded, partition-relative host I/O. A source-level callback does not by itself establish that every format variant or host profile is qualified.

## 4. Integrity, mutation, allocation and recovery

Do not generalise V3 formatting and append to other disk versions. Sparse writes remain rejected; general crash recovery and physical boot are separate qualifications. For destructive operations, require the applicable exclusive writable host, durability contract and format-specific validation. Never silently substitute a kernel or FUSE driver or treat a rejected or unavailable operation as success. Preserve original disk-format semantics; relocation and recovery are separate from ordinary file I/O.

## 5. Build, installation and release

The target installation is `/System/Filesystems/MINIX.so` and `/System/Resources/Documentation/Filesystem-Minix.pdf`. The project release policy is a portable amd64 DEB and a CPU-native source-first RUN, with GitHub's automatic source archive; do not treat this policy statement as verification of the current installer payload. The release workflow is the authoritative executable build and package specification. Historical adapters and test artifacts are not additional production filesystem authorities.

## 6. Qualification and limitations

[Recorded qualification workflow](https://github.com/Infiltrator-Projects/Filesystem-Minix/actions/runs/45528280751) · [Current workflows](https://github.com/Infiltrator-Projects/Filesystem-Minix/actions?query=branch%3Amain). A successful workflow establishes the tested scope only, not complete original-format parity, physical-media crash safety, or production BOOT/kernel integration. Use disposable images for destructive qualification.

## 7. Technical references and provenance

- [Provider implementation](module/minix_common.c)
- [Native design or source provenance](native/filesystems/minix/DESIGN.md)
- [Migration and source history](MIGRATION_STATUS.md)
- [Original-format reference source](docs/Minix.pages) and [PDF](docs/Minix.pdf)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

The original-format reference and historical migration notes describe different evidence from the current production provider. When descriptions disagree, the current exported descriptor, implementation, executable tests and exact-revision release evidence take precedence.
