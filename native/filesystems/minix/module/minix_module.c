/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minix V1/V2/V3 read-only on-media engine.
 * Interprets the real Linux Minix source at ../linux/{inode,dir,itree_v1,itree_v2}.c
 * using partition-relative host I/O, not a Linux VFS, mount or .ko proxy.
 * V1 uses 16-bit zones, 32-byte inodes and 16/32-byte dirents.
 * V2 uses 32-bit zones, 64-byte inodes and 16/32-byte dirents.
 * V3 uses 32-bit zones, 64-byte inodes and 64-byte dirents.
 */
#include "minix_module.h"
#include <stddef.h>

#define BLOCK_MIN 1024u
#define MINIX_V1_14 0x137fu
#define MINIX_V1_30 0x138fu
#define MINIX_V2_14 0x2468u
#define MINIX_V2_30 0x2478u
#define MINIX_V3_60 0x4d5au

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int bytes_ok(const IfsMinixVolume *v, uint64_t pos, uint32_t len)
{
    return v && pos <= v->volume_bytes &&
           (uint64_t)len <= v->volume_bytes - pos &&
           pos <= v->io.device_bytes &&
           (uint64_t)len <= v->io.device_bytes - pos;
}
static int read_at(const IfsMinixVolume *v, uint64_t pos,
                   void *dest, uint32_t len)
{
    return dest && v->io.read_at && bytes_ok(v, pos, len) &&
           v->io.read_at(v->io.user, pos, dest, len) == 0 ? 0 : -1;
}
static int zone_valid(const IfsMinixVolume *v, uint32_t zone)
{
    return zone >= v->first_data_zone && zone < v->zone_count;
}
static int zone_allocated(const IfsMinixVolume *v, uint32_t zone)
{
    uint8_t bitmap;
    uint64_t bit, offset;
    if (!zone_valid(v, zone)) return 0;
    /* Linux Minix bitmap rule: bit 1 maps to first_data_zone. */
    bit = (uint64_t)zone - v->first_data_zone + 1u;
    offset = (2u + (uint64_t)v->bitmap_inode_blocks) *
             v->block_bytes + (bit >> 3);
    return read_at(v, offset, &bitmap, 1u) == 0 &&
           (bitmap & (uint8_t)(1u << (bit & 7u))) != 0;
}
static int get_inode(const IfsMinixVolume *, uint32_t, IfsMinixInode *);
static int mount_volume(IfsMinixVolume *v, const IfsMinixIo *io)
{
    uint8_t sb[64], raw[64];
    uint32_t inode_blocks, dir_bytes, block_bytes, inode_bytes;
    uint32_t ninodes, zones, first, maxsize, imap, zmap, version, pointer;
    uint16_t magic, logzone;
    uint64_t inode_table, bitmap_bytes;
    IfsMinixVolume result;
    IfsMinixInode root;
    if (!v || !io || !io->read_at || io->device_bytes < 5u * BLOCK_MIN ||
        io->read_at(io->user, BLOCK_MIN, sb, sizeof sb) != 0)
        return -1;
    magic = u16(sb + 16u);
    version = 0u;
    if (magic == MINIX_V1_14 || magic == MINIX_V1_30)
        version = 1u;
    else if (magic == MINIX_V2_14 || magic == MINIX_V2_30)
        version = 2u;
    else if (u16(sb + 24u) == MINIX_V3_60)
        version = 3u;
    if (!version) return -1;
    block_bytes = version == 3u ? u16(sb + 28u) : BLOCK_MIN;
    inode_bytes = version == 1u ? 32u : 64u;
    pointer = version == 1u ? 2u : 4u;
    dir_bytes = version == 3u ? 64u :
        (magic == MINIX_V1_30 || magic == MINIX_V2_30 ? 32u : 16u);
    if (version == 3u) {
        ninodes = u32(sb);
        imap = u16(sb + 6u);
        zmap = u16(sb + 8u);
        first = u16(sb + 10u);
        logzone = u16(sb + 12u);
        maxsize = u32(sb + 16u);
        zones = u32(sb + 20u);
    } else {
        ninodes = u16(sb);
        imap = u16(sb + 4u);
        zmap = u16(sb + 6u);
        first = u16(sb + 8u);
        logzone = u16(sb + 10u);
        maxsize = u32(sb + 12u);
        zones = version == 1u ? u16(sb + 2u) : u32(sb + 20u);
    }
    if (block_bytes < BLOCK_MIN || block_bytes > 4096u ||
        (block_bytes & (block_bytes - 1u)) != 0u ||
        logzone != 0u || !ninodes || !imap || !zmap ||
        first < 5u || zones <= first || !maxsize)
        return -1;
    inode_blocks = (uint32_t)(((uint64_t)ninodes * inode_bytes +
                               block_bytes - 1u) / block_bytes);
    inode_table = 2u + (uint64_t)imap + zmap;
    if (inode_table + inode_blocks > first ||
        (uint64_t)zones * block_bytes > io->device_bytes)
        return -1;
    bitmap_bytes = (uint64_t)imap * block_bytes * 8u;
    if (bitmap_bytes <= ninodes ||
        (uint64_t)zmap * block_bytes * 8u <= zones - first)
        return -1;
    result.io = *io;
    result.volume_bytes = (uint64_t)zones * block_bytes;
    result.version = version;
    result.inode_count = ninodes;
    result.zone_count = zones;
    result.first_data_zone = first;
    result.block_bytes = block_bytes;
    result.inode_bytes = inode_bytes;
    result.directory_bytes = dir_bytes;
    result.name_bytes = dir_bytes - (version == 3u ? 4u : 2u);
    result.inode_table_block = (uint32_t)inode_table;
    result.max_file_bytes = maxsize;
    result.bitmap_inode_blocks = (uint16_t)imap;
    result.bitmap_zone_blocks = (uint16_t)zmap;
    result.pointer_bytes = (uint8_t)pointer;
    /* Check that inode #1 is allocated and describes a readable root. */
    if (read_at(&result, (uint64_t)2u * block_bytes, raw, 1u) != 0 ||
        (raw[0] & 2u) == 0u ||
        !bytes_ok(&result, (uint64_t)result.inode_table_block * block_bytes,
                  inode_bytes) ||
        get_inode(&result, 1u, &root) != 0) return -1;
    if ((root.mode & 0xf000u) != IFS_MINIX_MODE_DIRECTORY ||
        root.size_bytes % result.directory_bytes != 0u) return -1;
    *v = result;
    return 0;
}
static int get_inode(const IfsMinixVolume *v, uint32_t number, IfsMinixInode *out)
{
    uint8_t raw[64], bitmap;
    uint64_t pos, bit, map_pos;
    if (!v || !out || number == 0u || number > v->inode_count)
        return -1;
    bit = number;
    map_pos = (uint64_t)2u * v->block_bytes + (bit >> 3);
    if (read_at(v, map_pos, &bitmap, 1u) != 0 ||
        (bitmap & (uint8_t)(1u << (bit & 7u))) == 0u)
        return -1;
    pos = (uint64_t)v->inode_table_block * v->block_bytes +
          (uint64_t)(number - 1u) * v->inode_bytes;
    if (read_at(v, pos, raw, v->inode_bytes) != 0) return -1;
    out->inode_number = number;
    out->mode = u16(raw);
    out->size_bytes = u32(raw + (v->version == 1u ? 4u : 8u));
    if (out->size_bytes > v->max_file_bytes) return -1;
    for (uint32_t j = 0; j < 10u; ++j) {
        if (v->version == 1u)
            out->zone[j] = j < 9u ? u16(raw + 14u + j * 2u) : 0u;
        else out->zone[j] = u32(raw + 24u + j * 4u);
    }
    return 0;
}
static int indirect(const IfsMinixVolume *v, uint32_t pointer,
                    uint32_t index, uint32_t *next)
{
    uint8_t raw[4];
    uint64_t pos;
    if (!next || index >= v->block_bytes / v->pointer_bytes) return -1;
    if (!pointer) { *next = 0u; return 0; }
    if (!zone_allocated(v, pointer)) return -1;
    pos = (uint64_t)pointer * v->block_bytes +
          (uint64_t)index * v->pointer_bytes;
    if (read_at(v, pos, raw, v->pointer_bytes) != 0) return -1;
    *next = v->pointer_bytes == 2u ? u16(raw) : u32(raw);
    if (*next && !zone_allocated(v, *next)) return -1;
    return 0;
}
static int data_zone(const IfsMinixVolume *v, const IfsMinixInode *f,
                     uint64_t block, uint32_t *zone)
{
    const uint64_t n = v->block_bytes / v->pointer_bytes;
    uint32_t ptr;
    if (block < 7u) ptr = f->zone[block];
    else {
        block -= 7u;
        if (block < n) {
            if (indirect(v, f->zone[7], (uint32_t)block, &ptr) != 0)
                return -1;
        } else {
            block -= n;
            if (block < n * n) {
                uint32_t branch;
                if (indirect(v, f->zone[8], (uint32_t)(block / n),
                             &branch) != 0 ||
                    indirect(v, branch, (uint32_t)(block % n), &ptr) != 0)
                    return -1;
            } else {
                block -= n * n;
                if (v->version == 1u || block >= n * n * n)
                    return -1;
                uint32_t branch, leaf;
                if (indirect(v, f->zone[9], (uint32_t)(block / (n*n)),
                             &branch) != 0 ||
                    indirect(v, branch, (uint32_t)((block/n)%n),
                             &leaf) != 0 ||
                    indirect(v, leaf, (uint32_t)(block % n), &ptr) != 0)
                    return -1;
            }
        }
    }
    if (ptr && !zone_allocated(v, ptr)) return -1;
    *zone = ptr;
    return 0;
}
static int read_file(const IfsMinixVolume *v, const IfsMinixInode *f,
                     uint64_t position, void *buffer, uint32_t bytes)
{
    uint8_t *dst = (uint8_t *)buffer;
    IfsMinixInode live;
    if (!v || !f || (!dst && bytes) || f->inode_number == 0u ||
        f->inode_number > v->inode_count ||
        ((f->mode & 0xf000u) != IFS_MINIX_MODE_DIRECTORY &&
         (f->mode & 0xf000u) != IFS_MINIX_MODE_REGULAR &&
         (f->mode & 0xf000u) != IFS_MINIX_MODE_SYMLINK) ||
        f->size_bytes > v->max_file_bytes ||
        position > f->size_bytes || bytes > (uint64_t)f->size_bytes-position)
        return -1;
    /* Caller-supplied handles cannot grant access to unrelated sectors. */
    if (get_inode(v, f->inode_number, &live) != 0 ||
        live.mode != f->mode || live.size_bytes != f->size_bytes)
        return -1;
    for (uint32_t i = 0u; i < 10u; ++i)
        if (live.zone[i] != f->zone[i]) return -1;
    while (bytes) {
        const uint32_t within = (uint32_t)(position % v->block_bytes);
        uint32_t take = v->block_bytes - within, zone;
        if (take > bytes) take = bytes;
        if (data_zone(v, f, position / v->block_bytes, &zone) != 0)
            return -1;
        if (zone) {
            if (read_at(v, (uint64_t)zone * v->block_bytes + within,
                        dst, take) != 0) return -1;
        } else for (uint32_t i = 0u; i < take; ++i) dst[i] = 0u;
        dst += take;
        position += take;
        bytes -= take;
    }
    return 0;
}
static int raw_dir(const IfsMinixVolume *v, const IfsMinixInode *dir,
                   uint32_t slot, IfsMinixDirent *out)
{
    uint8_t rec[64];
    uint32_t ino;
    if ((dir->mode & 0xf000u) != IFS_MINIX_MODE_DIRECTORY ||
        dir->size_bytes % v->directory_bytes != 0u)
        return -1;
    if (slot >= dir->size_bytes / v->directory_bytes) return 1;
    if (read_file(v, dir, (uint64_t)slot * v->directory_bytes,
                  rec, v->directory_bytes) != 0) return -1;
    ino = v->version == 3u ? u32(rec) : u16(rec);
    if (ino > v->inode_count) return -1;
    out->inode_number = ino;
    const uint32_t prefix = v->version == 3u ? 4u : 2u;
    uint32_t i = 0u;
    for (; i < v->name_bytes && rec[prefix + i]; ++i)
        out->name[i] = (char)rec[prefix + i];
    out->name[i] = '\0';
    if (ino && !i) return -1;
    return 0;
}
static int directory_entry(const IfsMinixVolume *v,
                           const IfsMinixInode *dir, uint32_t nth,
                           IfsMinixDirent *out)
{
    IfsMinixDirent found;
    uint32_t occupied = 0u;
    if (!v || !dir || !out ||
        (dir->mode & 0xf000u) != IFS_MINIX_MODE_DIRECTORY ||
        dir->size_bytes % v->directory_bytes) return -1;
    const uint32_t count = dir->size_bytes / v->directory_bytes;
    for (uint32_t slot = 0; slot < count; ++slot) {
        if (raw_dir(v, dir, slot, &found) != 0) return -1;
        if (found.inode_number && occupied++ == nth) {
            *out = found;
            return 0;
        }
    }
    return 1;
}
static int directory_lookup(const IfsMinixVolume *v,
                            const IfsMinixInode *dir, const char *name,
                            uint32_t name_length, IfsMinixDirent *out)
{
    IfsMinixDirent found;
    if (!v || !dir || !name || !out || !name_length ||
        name_length > v->name_bytes ||
        (dir->mode & 0xf000u) != IFS_MINIX_MODE_DIRECTORY ||
        dir->size_bytes % v->directory_bytes) return -1;
    const uint32_t count = dir->size_bytes / v->directory_bytes;
    for (uint32_t slot = 0; slot < count; ++slot) {
        uint32_t i = 0u;
        if (raw_dir(v, dir, slot, &found) != 0) return -1;
        if (!found.inode_number) continue;
        while (i < name_length && found.name[i] == name[i] &&
               found.name[i]) ++i;
        if (i == name_length && found.name[i] == '\0') {
            *out = found; return 0;
        }
    }
    return 1;
}
/* Validate every destination block before the first mutation. The live
 * filesystem geometry and complete inode zone pointers must match the
 * previously observed file. Sparse holes may not be materialised by V2.
 * The host must guarantee exclusive medium access and identical callbacks.
 */
static int overwrite_existing(const IfsMinixWritableIo *io,
                              const IfsMinixVolume *expected,
                              const IfsMinixInode *file,
                              uint64_t position, const void *input,
                              uint32_t length)
{
    IfsMinixVolume live_volume;
    IfsMinixInode live_file;
    const uint8_t *source = (const uint8_t *)input;
    uint64_t cursor, remaining;
    if (!io || !expected || !file || !io->read.read_at ||
        !io->write_at || !io->flush || (!source && length != 0u) ||
        (file->mode & 0xf000u) != IFS_MINIX_MODE_REGULAR ||
        position > file->size_bytes ||
        (uint64_t)length > file->size_bytes - position ||
        mount_volume(&live_volume, &io->read) != 0)
        return -1;

    if (live_volume.volume_bytes != expected->volume_bytes ||
        live_volume.version != expected->version ||
        live_volume.inode_count != expected->inode_count ||
        live_volume.zone_count != expected->zone_count ||
        live_volume.first_data_zone != expected->first_data_zone ||
        live_volume.block_bytes != expected->block_bytes ||
        live_volume.inode_bytes != expected->inode_bytes ||
        live_volume.directory_bytes != expected->directory_bytes ||
        live_volume.name_bytes != expected->name_bytes ||
        live_volume.inode_table_block != expected->inode_table_block ||
        live_volume.max_file_bytes != expected->max_file_bytes ||
        live_volume.bitmap_inode_blocks != expected->bitmap_inode_blocks ||
        live_volume.bitmap_zone_blocks != expected->bitmap_zone_blocks ||
        live_volume.pointer_bytes != expected->pointer_bytes ||
        get_inode(&live_volume, file->inode_number, &live_file) != 0 ||
        live_file.mode != file->mode ||
        live_file.size_bytes != file->size_bytes)
        return -1;
    for (uint32_t i = 0u; i < 10u; ++i)
        if (live_file.zone[i] != file->zone[i]) return -1;

    if (length == 0u) return 0;

    /* First pass: no write until every destination zone is allocated,
     * mapped, and fully inside both the partition and the host device. */
    cursor = position;
    remaining = length;
    while (remaining != 0u) {
        uint32_t zone, within, count;
        uint64_t offset;
        within = (uint32_t)(cursor % live_volume.block_bytes);
        count = live_volume.block_bytes - within;
        if ((uint64_t)count > remaining) count = (uint32_t)remaining;
        if (data_zone(&live_volume, &live_file,
                      cursor / live_volume.block_bytes, &zone) != 0 ||
            zone == 0u || !zone_allocated(&live_volume, zone))
            return -1;
        offset = (uint64_t)zone * live_volume.block_bytes + within;
        if (!bytes_ok(&live_volume, offset, count)) return -1;
        cursor += count;
        remaining -= count;
    }

    cursor = position;
    remaining = length;
    while (remaining != 0u) {
        uint32_t zone, within, count;
        uint64_t offset;
        within = (uint32_t)(cursor % live_volume.block_bytes);
        count = live_volume.block_bytes - within;
        if ((uint64_t)count > remaining) count = (uint32_t)remaining;
        if (data_zone(&live_volume, &live_file,
                      cursor / live_volume.block_bytes, &zone) != 0 ||
            zone == 0u || !zone_allocated(&live_volume, zone))
            return -1;
        offset = (uint64_t)zone * live_volume.block_bytes + within;
        if (!bytes_ok(&live_volume, offset, count) ||
            io->write_at(io->read.user, offset, source, count) != 0)
            return -1;
        cursor += count;
        source += count;
        remaining -= count;
    }
    return io->flush(io->read.user) == 0 ? 0 : -1;
}

__attribute__((used, section(".ifsmeta")))
static const char module_identity[] = "IFS-MODULE:minix:1";
static const IfsMinixModule module = {
    IFS_MINIX_ABI_VERSION, sizeof(IfsMinixModule),
    "minix", "Minix filesystem V1/V2/V3",
    IFS_MINIX_CAP_PROBE | IFS_MINIX_CAP_DIRECTORY |
    IFS_MINIX_CAP_FILE_READ,
    mount_volume, get_inode, directory_entry, directory_lookup, read_file
};
__attribute__((visibility("default")))
const IfsMinixModule *infiltrator_filesystem_module_v1(void)
{
    return &module;
}

__attribute__((used, section(".ifsmeta")))
static const char module_identity_v2[] = "IFS-MODULE:minix:2";
static const IfsMinixModuleV2 module_v2 = {
    IFS_MINIX_V2_ABI_VERSION, sizeof(IfsMinixModuleV2),
    "minix", "Minix V1/V2/V3 bounded existing-file overwrite",
    IFS_MINIX_V2_CAP_OVERWRITE_EXISTING, &module,
    overwrite_existing
};
__attribute__((visibility("default")))
const IfsMinixModuleV2 *infiltrator_filesystem_module_v2(void)
{
    return &module_v2;
}
