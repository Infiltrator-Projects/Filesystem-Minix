/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Independent, read-only Minix V1/V2/V3 format module. */
#ifndef IFS_MINIX_MODULE_H
#define IFS_MINIX_MODULE_H
#include <stdint.h>
#define IFS_MINIX_ABI_VERSION 1u
#define IFS_MINIX_CAP_PROBE UINT64_C(1)
#define IFS_MINIX_CAP_DIRECTORY UINT64_C(2)
#define IFS_MINIX_CAP_FILE_READ UINT64_C(4)
#define IFS_MINIX_MODE_DIRECTORY 0x4000u
#define IFS_MINIX_MODE_REGULAR 0x8000u
#define IFS_MINIX_MODE_SYMLINK 0xa000u

typedef int (*IfsMinixReadAt)(void *user, uint64_t offset,
                             void *buffer, uint32_t bytes);
typedef struct {
    IfsMinixReadAt read_at;
    void *user;
    uint64_t device_bytes; /* partition-relative */
} IfsMinixIo;
typedef struct {
    IfsMinixIo io;
    uint64_t volume_bytes;
    uint32_t version, inode_count, zone_count, first_data_zone;
    uint32_t block_bytes, inode_bytes, directory_bytes, name_bytes;
    uint32_t inode_table_block, max_file_bytes;
    uint16_t bitmap_inode_blocks, bitmap_zone_blocks;
    uint8_t pointer_bytes;
} IfsMinixVolume;
typedef struct {
    uint32_t inode_number, size_bytes;
    uint16_t mode;
    uint32_t zone[10];
} IfsMinixInode;
typedef struct {
    uint32_t inode_number;
    char name[61];
} IfsMinixDirent;
typedef struct {
    uint32_t abi_version, structure_bytes;
    const char *format_id, *format_name;
    uint64_t capabilities;
    /* Return 0 on success, -1 on unsupported/corrupt geometry. */
    int (*mount)(IfsMinixVolume *, const IfsMinixIo *);
    int (*inode)(const IfsMinixVolume *, uint32_t, IfsMinixInode *);
    /* nth occupied entry: 0=found, 1=end, -1=media/format error. */
    int (*entry)(const IfsMinixVolume *, const IfsMinixInode *,
                 uint32_t, IfsMinixDirent *);
    /* exact case-sensitive names; 0=found, 1=absent, -1=error. */
    int (*lookup)(const IfsMinixVolume *, const IfsMinixInode *,
                  const char *, uint32_t, IfsMinixDirent *);
    /* Existing allocated/hole file content only; no mutation. */
    int (*read)(const IfsMinixVolume *, const IfsMinixInode *,
                uint64_t, void *, uint32_t);
} IfsMinixModule;
const IfsMinixModule *infiltrator_filesystem_module_v1(void);

/* Optional V2 API: only allocated bytes of existing REGULAR files can
 * be overwritten. V1 remains byte-for-byte compatible and read-only.
 * Caller owns exclusive volume access and guarantees that read/write/flush
 * address the SAME underlying medium, without concurrent metadata mutation.
 * No rollback is claimed after a failed host I/O operation.
 */
#define IFS_MINIX_V2_ABI_VERSION 2u
#define IFS_MINIX_V2_CAP_OVERWRITE_EXISTING UINT64_C(8)
typedef int (*IfsMinixWriteAt)(void *user, uint64_t offset,
                              const void *buffer, uint32_t bytes);
typedef int (*IfsMinixFlush)(void *user);
typedef struct {
    IfsMinixIo read;
    IfsMinixWriteAt write_at;
    IfsMinixFlush flush;
} IfsMinixWritableIo;
typedef struct {
    uint32_t abi_version, structure_bytes;
    const char *format_id, *format_name;
    uint64_t capabilities;
    const IfsMinixModule *readonly_v1;
    /* 0 = complete write+flush, -1 = rejected/failed; partial writes
     * may have occurred on I/O failure. Never changes allocation metadata. */
    int (*overwrite_existing)(const IfsMinixWritableIo *,
                              const IfsMinixVolume *,
                              const IfsMinixInode *,
                              uint64_t position, const void *bytes,
                              uint32_t length);
} IfsMinixModuleV2;
const IfsMinixModuleV2 *infiltrator_filesystem_module_v2(void);
#endif
