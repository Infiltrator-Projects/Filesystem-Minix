/* SPDX-License-Identifier: GPL-3.0-or-later
 * Common V1/V2 bridge to the existing native Minix V1/V2/V3 engine.
 * All media I/O is partition-relative and delegated to that engine.
 */
#include <infiltratr/filesystem_module_v2.h>
#include "../native/filesystems/minix/module/minix_module.h"
#include <stdint.h>
#include <string.h>

#define COMMON_MINIX_LIVE UINT32_C(0x4d494e58)
typedef struct {
    uint32_t live;
    IfsFsHostIoV1 host;
    IfsMinixVolume media;
} CommonMinix;

static const IfsMinixModule *native(void)
{
    return infiltrator_filesystem_module_v1();
}
static int host_ok(const IfsFsHostIoV1 *h)
{
    return h && h->structure_bytes >= sizeof(*h) && h->read_at &&
        h->volume_bytes >= 5120u &&
        !(h->flags & ~(IFS_FS_IO_READ_ONLY | IFS_FS_IO_EXCLUSIVE));
}
static IfsMinixIo minix_io(const IfsFsHostIoV1 *h)
{
    IfsMinixIo io = {h->read_at, h->context, h->volume_bytes};
    return io;
}
static int probe(const IfsFsHostIoV1 *h, uint32_t *variant)
{
    IfsMinixVolume volume;
    IfsMinixIo io;
    if (!host_ok(h) || !variant) return IFS_FS_INVALID;
    io = minix_io(h);
    if (native()->mount(&volume, &io) != 0) return IFS_FS_NOT_FOUND;
    *variant = volume.version;
    return IFS_FS_OK;
}
static int mounted(const CommonMinix *v)
{
    return v && v->live == COMMON_MINIX_LIVE &&
        v->host.read_at && v->media.io.user == v->host.context;
}
static void describe(const CommonMinix *v, IfsFsVolumeInfoV1 *info)
{
    info->variant = v->media.version;
    info->block_bytes = v->media.block_bytes;
    info->max_name_bytes = v->media.name_bytes;
    info->total_bytes = v->media.volume_bytes;
    /* This read-only geometry engine does not expose an allocation count. */
    info->free_bytes = 0u;
}
static int mount(const IfsFsHostIoV1 *h, void **out,
                 IfsFsVolumeInfoV1 *info)
{
    CommonMinix *v;
    IfsMinixIo io;
    if (!host_ok(h) || !out || !info ||
        info->structure_bytes < sizeof(*info) ||
        !h->allocate || !h->release) return IFS_FS_INVALID;
    *out = NULL;
    v = h->allocate(h->context, sizeof(*v));
    if (!v) return IFS_FS_NO_MEMORY;
    memset(v, 0, sizeof(*v));
    v->host = *h;
    io = minix_io(&v->host);
    if (native()->mount(&v->media, &io) != 0) {
        h->release(h->context, v);
        return IFS_FS_CORRUPT;
    }
    v->live = COMMON_MINIX_LIVE;
    describe(v, info);
    *out = v;
    return IFS_FS_OK;
}
static int unmount(void *volume)
{
    CommonMinix *v = volume;
    IfsFsHostIoV1 host;
    if (!mounted(v)) return IFS_FS_INVALID;
    host = v->host;
    v->live = 0u;
    host.release(host.context, v);
    return IFS_FS_OK;
}
/* Reject unnormalised traversal, including dot segments. Never follow links. */
static int resolve(CommonMinix *v, const char *path, IfsMinixInode *inode,
                   const char **basename, uint32_t *bytes)
{
    const char *p;
    if (!mounted(v) || !path || path[0] != '/' || !inode ||
        native()->inode(&v->media, 1u, inode) != 0)
        return IFS_FS_INVALID;
    p = path + 1;
    if (!*p) {
        if (basename) *basename = "/";
        if (bytes) *bytes = 1u;
        return IFS_FS_OK;
    }
    while (*p) {
        IfsMinixDirent dirent = {0};
        const char *start = p;
        size_t n;
        int rc;
        while (*p && *p != '/') ++p;
        n = (size_t)(p - start);
        if (!n || n > v->media.name_bytes ||
            (n == 1u && *start == '.') ||
            (n == 2u && start[0] == '.' && start[1] == '.'))
            return IFS_FS_INVALID;
        if ((inode->mode & 0xf000u) != IFS_MINIX_MODE_DIRECTORY)
            return IFS_FS_NOT_FOUND;
        rc = native()->lookup(&v->media, inode, start, (uint32_t)n, &dirent);
        if (rc != 0) return rc == 1 ? IFS_FS_NOT_FOUND : IFS_FS_CORRUPT;
        if (native()->inode(&v->media, dirent.inode_number, inode) != 0)
            return IFS_FS_CORRUPT;
        if (basename) *basename = start;
        if (bytes) *bytes = (uint32_t)n;
        if (*p == '/' && !*++p) return IFS_FS_INVALID;
    }
    return IFS_FS_OK;
}
static int fill(IfsFsEntryV1 *out, const IfsMinixInode *inode,
                const char *name, uint32_t bytes)
{
    uint32_t type;
    if (!out || out->structure_bytes < sizeof(*out) ||
        !out->name || !name || bytes >= out->name_capacity)
        return IFS_FS_INVALID;
    switch (inode->mode & 0xf000u) {
    case IFS_MINIX_MODE_REGULAR: type = IFS_FS_ENTRY_FILE; break;
    case IFS_MINIX_MODE_DIRECTORY: type = IFS_FS_ENTRY_DIRECTORY; break;
    case IFS_MINIX_MODE_SYMLINK: type = IFS_FS_ENTRY_LINK; break;
    default: return IFS_FS_UNSUPPORTED;
    }
    memcpy(out->name, name, bytes);
    out->name[bytes] = '\0';
    out->name_bytes = bytes;
    out->type = type;
    out->attributes = inode->mode;
    out->size_bytes = inode->size_bytes;
    return IFS_FS_OK;
}
static int stat_path(void *volume, const char *path, IfsFsEntryV1 *out)
{
    CommonMinix *v = volume;
    IfsMinixInode inode;
    const char *name = NULL;
    uint32_t n = 0;
    int rc = resolve(v, path, &inode, &name, &n);
    return rc ? rc : fill(out, &inode, name, n);
}
static int directory_entry(void *volume, const char *path, uint32_t index,
                           IfsFsEntryV1 *out)
{
    CommonMinix *v = volume;
    IfsMinixInode directory, file;
    uint32_t returned = 0u;
    int rc = resolve(v, path, &directory, NULL, NULL);
    if (rc) return rc;
    if ((directory.mode & 0xf000u) != IFS_MINIX_MODE_DIRECTORY)
        return IFS_FS_INVALID;
    for (uint32_t i = 0; ; ++i) {
        IfsMinixDirent e = {0};
        const char *zero;
        uint32_t len;
        rc = native()->entry(&v->media, &directory, i, &e);
        if (rc) return rc == 1 ? IFS_FS_END : IFS_FS_CORRUPT;
        /* The Minix engine returns a bounded NUL-terminated name. */
        zero = memchr(e.name, '\0', sizeof(e.name));
        if (!zero) return IFS_FS_CORRUPT;
        len = (uint32_t)(zero - e.name);
        if ((len == 1u && e.name[0] == '.') ||
            (len == 2u && e.name[0] == '.' && e.name[1] == '.'))
            continue;
        if (returned++ != index) continue;
        if (native()->inode(&v->media, e.inode_number, &file) != 0)
            return IFS_FS_CORRUPT;
        return fill(out, &file, e.name, len);
    }
}
static int read_file(void *volume, const char *path, uint64_t offset,
                     void *data, uint32_t length)
{
    CommonMinix *v = volume;
    IfsMinixInode inode;
    int rc;
    if (!mounted(v) || (!data && length)) return IFS_FS_INVALID;
    rc = resolve(v, path, &inode, NULL, NULL);
    if (rc) return rc;
    if ((inode.mode & 0xf000u) != IFS_MINIX_MODE_REGULAR)
        return IFS_FS_INVALID;
    if (offset > inode.size_bytes || length > inode.size_bytes - offset)
        return IFS_FS_UNSUPPORTED;
    return native()->read(&v->media, &inode, offset, data, length) == 0 ?
        IFS_FS_OK : IFS_FS_IO_ERROR;
}
static int write_file(void *volume, const char *path, uint64_t offset,
                      const void *data, uint32_t length)
{
    CommonMinix *v = volume;
    IfsMinixInode inode;
    IfsMinixWritableIo io = {0};
    int rc;
    if (!mounted(v) || (!data && length)) return IFS_FS_INVALID;
    if ((v->host.flags & IFS_FS_IO_READ_ONLY) ||
        !(v->host.flags & IFS_FS_IO_EXCLUSIVE) ||
        !v->host.write_at || !v->host.flush) return IFS_FS_ACCESS;
    rc = resolve(v, path, &inode, NULL, NULL);
    if (rc) return rc;
    if ((inode.mode & 0xf000u) != IFS_MINIX_MODE_REGULAR)
        return IFS_FS_INVALID;
    if (offset > inode.size_bytes || length > inode.size_bytes - offset)
        return IFS_FS_UNSUPPORTED;
    io.read = minix_io(&v->host);
    io.write_at = v->host.write_at;
    io.flush = v->host.flush;
    return infiltrator_filesystem_module_v2()->overwrite_existing(
        &io, &v->media, &inode, offset, data, length) == 0 ?
        IFS_FS_OK : IFS_FS_IO_ERROR;
}
/* The underlying Minix API currently provides bounded overwrite only. */
static const IfsFsModuleApiV1 common_v1 = {
    .abi_version=IFS_FS_COMMON_ABI_V1,
    .structure_bytes=sizeof(IfsFsModuleApiV1),
    .format_id="minix",
    .capabilities=IFS_FS_CAP_PROBE | IFS_FS_CAP_MOUNT |
        IFS_FS_CAP_LIST | IFS_FS_CAP_READ | IFS_FS_CAP_WRITE,
    .probe=probe, .mount=mount, .unmount=unmount,
    .stat=stat_path, .directory_entry=directory_entry,
    .read=read_file, .write=write_file
};
__attribute__((visibility("default")))
const IfsFsModuleApiV1 *infiltrator_filesystem_api_v1(void)
{
    return &common_v1;
}
static int query_volume(void *volume, IfsFsVolumeInfoV1 *out)
{
    CommonMinix *v = volume;
    if (!mounted(v) || !out || out->structure_bytes < sizeof(*out))
        return IFS_FS_INVALID;
    describe(v, out);
    return IFS_FS_OK;
}
static int sync_volume(void *volume)
{
    CommonMinix *v = volume;
    if (!mounted(v)) return IFS_FS_INVALID;
    if ((v->host.flags & IFS_FS_IO_READ_ONLY) || !v->host.write_at)
        return IFS_FS_OK;
    return v->host.flush && v->host.flush(v->host.context) == 0 ?
        IFS_FS_OK : IFS_FS_IO_ERROR;
}
static const IfsFsVolumeOpsV1 volume_ops = {
    .header={IFS_FS_GROUP_ABI_V1, sizeof(IfsFsVolumeOpsV1),
             IFS_FS_GROUP_VOLUME, 0u,
             IFS_FS_VOLUME_QUERY | IFS_FS_VOLUME_SYNC},
    .query=query_volume, .sync=sync_volume
};
static const IfsFsGroupHeaderV2 *query_group(uint32_t id,
                                              uint32_t minimum,
                                              uint32_t maximum)
{
    return id == IFS_FS_GROUP_VOLUME && minimum <= IFS_FS_GROUP_ABI_V1 &&
        maximum >= IFS_FS_GROUP_ABI_V1 ? &volume_ops.header : NULL;
}
static const IfsFsModuleApiV2 common_v2 = {
    IFS_FS_COMMON_ABI_V2, sizeof(IfsFsModuleApiV2), "minix",
    IFS_FS_PROFILE_HOSTED, IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),
    &common_v1, query_group
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    return &common_v2;
}
