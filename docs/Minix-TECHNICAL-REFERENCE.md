# Minix: technical reference

## History and revision discrimination

MINIX introduced a compact inode-based Unix filesystem in 1987. V1 uses 16-bit zone addresses, V2 expands them to 32 bits, and V3 expands inode counts and directory identifiers and records its block size. The Linux v6.12 reference recognizes V1 magic 0x137F/0x138F and V2 0x2468/0x2478 at superblock byte 16; V3 magic 0x4D5A is at byte 24. The two older magic variants select 14- or 30-byte names. This chapter describes little-endian media and the conventional zero log-zone-size profile; alternate byte order and nonzero zone shifts require version-specific validation. Read the superblock at partition byte 1024, not at a guessed offset based on the later block-size field.

## Superblock binary fields

V1/V2 offsets: 16-bit inode count 0, V1 zone count 2, inode-map blocks 4, zone-map blocks 6, first data zone 8, log zone size 10, 32-bit maximum file bytes 12, 16-bit magic 16 and state 18, V2 32-bit zone count 20. V3: 32-bit inode count 0, reserved 4, 16-bit inode-map blocks 6, zone-map blocks 8, first data zone 10, log zone size 12, reserved 14, maximum file bytes 16, zone count 20, magic 24, reserved 26, block bytes 28 and disk version 30. V1/V2 use 1024-byte blocks; V3 declares B. Verify power-of-two B, map capacity, first data zone and complete inode-table span. Reject geometry whose arithmetic exceeds the image.

## Region organisation and allocation bitmaps

With the ordinary zero zone shift, block 0 is boot storage, block 1 includes the superblock, inode bitmap starts at block 2, zone bitmap follows, then inode table; first data zone is recorded, not inferred from rounding alone. Inode bit n represents inode n; bit zero is reserved. Zone bit z-firstdatazone+1 represents zone z; bit zero is reserved. A set bit denotes allocation. In little-endian bitmap order byte=floor(bit/8), mask=1<<(bit mod 8). For first data zone 50, zone 100 has bit 51, byte 6, bit 3. Split bitmaps at B*8 bits per block and exclude padding and invalid zones. A zone can hold indirect metadata or payload; record both in reverse ownership.

## Inodes and worked location

Root inode is 1. Let I=2+imap_blocks+zmap_blocks and R=32 for V1 or 64 for V2/V3. Inode n is at I*B+(n-1)*R. V1 fields: mode 0, UID 2, size 4, time 8, GID byte 12, link byte 13, nine 16-bit zone pointers at 14. V2/V3: mode 0, links 2, UID 4, GID 6, 32-bit size 8, atime 12, mtime 16, ctime 20, ten 32-bit zone pointers at 24. For B=1024, two inode-map and three zone-map blocks, inode 20 in V3 starts at 7*1024+19*64=8384. Validate allocated inode identity, mode, size and link count. Special devices may use a zone field as a device identifier rather than storage.

## Direct and indirect mapping

Seven direct pointers are followed by single and double indirect; V2/V3 add triple indirect. For V1, fanout is 512 16-bit entries per 1024-byte block. For V2/V3, fanout N=B/4. Logical k<7 selects direct slot; subtract 7, then N and N*N to find indirect depth. At B=1024, V3 logical block 270 falls in double indirect: 270-7-256=7, so use inode pointer 8, index 0, then index 7. Each indirect block must be allocated, within data bounds and visited only under its expected level. Zero pointers represent holes in supported native profiles. The advertised max file size, 32-bit size and addressable tree coverage all constrain valid reads. Do not interpret nonzero zone shift as a simple block number without validating its native mapping.

## Directory records and namespace

V1/V2 entries are 16 or 32 bytes according to magic, with 16-bit inode at 0 and 14 or 30 name bytes at 2. V3 records are 64 bytes with 32-bit inode at 0 and 60 name bytes at 4. A zero inode is unused. Names occupying the entire field need not have a terminator. Match byte-exact bounded names and validate inode range, allocation and dot/dot-dot. Hard links share inode identity; do not duplicate the file for every directory entry. A directory is mapped through the same pointer tree as ordinary file contents. Directory record size must divide its logical data except for explicitly diagnosed corruption.

## Mutation and interrupted updates

Reserve a free inode and necessary zones, initialise data and pointer blocks, then publish namespace reachability. On extension reserve zones before publishing larger size; on deletion clear live pointers before release. V1/V2 state bits 1 and 2 represent valid and error state. The V3 header does not contain the same state field. None provides a general native transaction journal. Relocation copies and verifies bytes before switching the exact owner pointer, with metadata and bitmaps kept coherent. Failure can leak blocks or leave namespace damage, so never advertise all-or-nothing recovery without independent evidence. Formatter establishes all maps, root inode, dot entries and reserved bits, and must validate capacity.

## Independent validation and references

Pinned definitions: https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/minix_fs.h . Algorithms: https://github.com/torvalds/linux/tree/v6.12/fs/minix (inode.c, bitmap.c, itree_v1.c, itree_v2.c, dir.c). Test native mkfs.minix and fsck.minix images for all magic variants, inode and zone bitmap boundaries, maximum-length names, direct/indirect/triple transitions, zero pointers, corrupt maps, and interrupted writes. Compare namespace and hashes with independent mounts. Verify alternate endian and nonzero zone-size profiles separately; this reference does not declare them qualified. The prior manual is retained as historical evidence, not a current implementation guarantee.
