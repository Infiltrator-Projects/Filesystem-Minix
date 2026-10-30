// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media_raw.h"
#include "infiltratr/endian.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UNIX_BLOCK 4096U
static void bit_set(uint8_t *map, uint64_t bit) { map[bit / 8U] |= (uint8_t)(1U << (bit % 8U)); }
static bool bit_get(const uint8_t *map, uint64_t bit)
{
    return (map[bit / 8U] & (uint8_t)(1U << (bit % 8U))) != 0U;
}
static uint32_t file_block(const LdtmRawFile *file, uint64_t index)
{
    return (uint32_t)(index < file->count[0] ? file->start[0] + index
                                             : file->start[1] + index - file->count[0]);
}
static uint32_t pointer_tree(int fd, const LdtmRawFile *file, uint64_t begin, uint64_t count,
                             unsigned depth, uint32_t *cursor, uint8_t *used)
{
    uint8_t raw[UNIX_BLOCK] = {0};
    const uint32_t block = (*cursor)++;
    bit_set(used, block);
    const uint64_t stride = depth == 1U ? 1U : 1024U;
    for (uint64_t n = 0U; n < count; n += stride) {
        uint32_t value;
        if (depth == 1U)
            value = file_block(file, begin + n);
        else {
            value = pointer_tree(fd, file, begin + n, count - n < stride ? count - n : stride,
                                 depth - 1U, cursor, used);
            if (value == 0U)
                return 0U;
        }
        infiltratr_store_le32(raw + (n / stride) * 4U, value);
    }
    return ldtm_raw_write(fd, (uint64_t)block * UNIX_BLOCK, raw, sizeof(raw)) == 0 ? block : 0U;
}
static int inode_pointers(int fd, const LdtmRawFile *file, unsigned direct, uint8_t *pointers,
                          uint32_t *cursor, uint8_t *used)
{
    const uint64_t total = file->count[0] + file->count[1];
    for (uint64_t n = 0U; n < total && n < direct; ++n)
        infiltratr_store_le32(pointers + n * 4U, file_block(file, n));
    uint64_t begin = direct;
    for (unsigned depth = 1U; begin < total && depth <= 2U; ++depth) {
        const uint64_t limit = depth == 1U ? 1024U : UINT64_C(1024) * 1024U;
        const uint64_t count = total - begin < limit ? total - begin : limit;
        const uint32_t tree = pointer_tree(fd, file, begin, count, depth, cursor, used);
        if (tree == 0U)
            return -1;
        infiltratr_store_le32(pointers + (direct + depth - 1U) * 4U, tree);
        begin += count;
    }
    return begin >= total ? 0 : -1;
}
static uint32_t read_pointer(int fd, uint32_t block, uint64_t index, unsigned depth,
                             uint64_t total_blocks)
{
    if (block == 0U || block >= total_blocks)
        return 0U;
    const uint64_t slot = depth == 1U ? index : index / 1024U;
    if (slot >= 1024U)
        return 0U;
    uint8_t raw[4];
    if (ldtm_raw_read(fd, (uint64_t)block * UNIX_BLOCK + slot * 4U, raw, sizeof(raw)) != 0)
        return 0U;
    const uint32_t next = infiltratr_load_le32(raw);
    return depth == 1U ? next : read_pointer(fd, next, index % 1024U, depth - 1U, total_blocks);
}

int ldtm_raw_minix(const LdtmFilesystemSpec *spec, const char *path, bool create)
{
    uint64_t capacity = 0U;
    const int fd = ldtm_raw_open(path, create, &capacity);
    if (fd < 0)
        return -1;
    int result = -1;
    const uint32_t zones = (uint32_t)(capacity / UNIX_BLOCK);
    uint8_t super[64] = {0};
    const uint32_t zmap_blocks = (zones + 32767U) / 32768U;
    const uint32_t inode_table = 3U + zmap_blocks;
    const uint32_t first = inode_table + 2U;
    uint8_t *used = calloc((zones + 7U) / 8U, 1U);
    uint8_t *zmap = calloc(zmap_blocks, UNIX_BLOCK);
    if (used == NULL || zmap == NULL)
        goto done;
    if (create) {
        if (capacity != (uint64_t)spec->size_mib * LDTM_MIB || first > UINT16_MAX)
            goto done;
        LdtmRawPlan plan;
        if (ldtm_raw_plan(spec, capacity, UNIX_BLOCK, (uint64_t)first + 512U, &plan) != 0)
            goto done;
        uint8_t inodes[8192] = {0}, imap[UNIX_BLOCK] = {0}, directory[UNIX_BLOCK] = {0};
        bit_set(imap, 0U);
        bit_set(imap, 1U);
        bit_set(used, first);
        infiltratr_store_le16(inodes, 0040755U);
        infiltratr_store_le16(inodes + 2U, 2U);
        infiltratr_store_le32(inodes + 8U, (LDTM_RAW_FILES + 2U) * 64U);
        infiltratr_store_le32(inodes + 24U, first);
        infiltratr_store_le32(directory, 1U);
        memcpy(directory + 4U, ".", 2U);
        infiltratr_store_le32(directory + 64U, 1U);
        memcpy(directory + 68U, "..", 3U);
        uint32_t cursor = first + 1U;
        for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
            const LdtmRawFile *item = &plan.files[file];
            uint8_t *inode = inodes + (file + 1U) * 64U;
            bit_set(imap, file + 2U);
            infiltratr_store_le16(inode, 0100644U);
            infiltratr_store_le16(inode + 2U, 1U);
            infiltratr_store_le32(inode + 8U, (uint32_t)item->bytes);
            if (inode_pointers(fd, item, 7U, inode + 24U, &cursor, used) != 0)
                goto done;
            for (unsigned part = 0U; part < 2U; ++part)
                for (uint64_t n = 0U; n < item->count[part]; ++n)
                    bit_set(used, item->start[part] + n);
            uint8_t *entry = directory + (file + 2U) * 64U;
            infiltratr_store_le32(entry, file + 2U);
            char name[16];
            ldtm_raw_name(file, name);
            memcpy(entry + 4U, name, strlen(name) + 1U);
        }
        bit_set(zmap, 0U);
        for (uint32_t zone = first; zone < zones; ++zone)
            if (bit_get(used, zone))
                bit_set(zmap, zone - first + 1U);
        infiltratr_store_le32(super, 128U);
        infiltratr_store_le16(super + 6U, 1U);
        infiltratr_store_le16(super + 8U, (uint16_t)zmap_blocks);
        infiltratr_store_le16(super + 10U, (uint16_t)first);
        infiltratr_store_le32(super + 16U, UINT32_C(0x7fffffff));
        infiltratr_store_le32(super + 20U, zones);
        infiltratr_store_le16(super + 24U, 0x4d5aU);
        infiltratr_store_le16(super + 28U, UNIX_BLOCK);
        if (ldtm_raw_write(fd, 1024U, super, sizeof(super)) != 0 ||
            ldtm_raw_write(fd, 2U * UNIX_BLOCK, imap, sizeof(imap)) != 0 ||
            ldtm_raw_write(fd, 3U * UNIX_BLOCK, zmap, (size_t)zmap_blocks * UNIX_BLOCK) != 0 ||
            ldtm_raw_write(fd, (uint64_t)inode_table * UNIX_BLOCK, inodes, sizeof(inodes)) != 0 ||
            ldtm_raw_write(fd, (uint64_t)first * UNIX_BLOCK, directory, sizeof(directory)) != 0 ||
            ldtm_raw_write_payload(fd, &plan) != 0)
            goto done;
        result = 0;
    } else {
        if (ldtm_raw_read(fd, 1024U, super, sizeof(super)) != 0 ||
            infiltratr_load_le16(super + 24U) != 0x4d5aU ||
            infiltratr_load_le16(super + 28U) != UNIX_BLOCK ||
            infiltratr_load_le16(super + 12U) != 0U)
            goto done;
        const uint64_t table =
            2U + infiltratr_load_le16(super + 6U) + infiltratr_load_le16(super + 8U);
        LdtmRawCheck check;
        ldtm_raw_check_init(&check, spec);
        for (unsigned file = 0U; file < LDTM_RAW_FILES; ++file) {
            uint8_t inode[64];
            if (ldtm_raw_read(fd, table * UNIX_BLOCK + (file + 1U) * 64U, inode, sizeof(inode)) !=
                    0 ||
                (infiltratr_load_le16(inode) & 0170000U) != 0100000U ||
                ldtm_raw_check_file(&check, file, infiltratr_load_le32(inode + 8U)) != 0)
                goto done;
            const uint64_t bytes = ldtm_raw_file_bytes(spec, file);
            for (uint64_t logical = 0U; logical < bytes; logical += UNIX_BLOCK) {
                const uint64_t n = logical / UNIX_BLOCK;
                const uint32_t block =
                    n < 7U ? infiltratr_load_le32(inode + 24U + n * 4U)
                    : n < 1031U
                        ? read_pointer(fd, infiltratr_load_le32(inode + 52U), n - 7U, 1U, zones)
                        : read_pointer(fd, infiltratr_load_le32(inode + 56U), n - 1031U, 2U, zones);
                const uint64_t take = bytes - logical < UNIX_BLOCK ? bytes - logical : UNIX_BLOCK;
                if (block == 0U || block >= zones ||
                    ldtm_raw_check_data(&check, fd, file, (uint64_t)block * UNIX_BLOCK, logical,
                                        take) != 0)
                    goto done;
            }
        }
        result = ldtm_raw_check_finish(&check);
    }
done:
    free(used);
    free(zmap);
    if (close(fd) != 0)
        result = -1;
    return result;
}

