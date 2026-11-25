/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Independent synthetic Minix V1/V2/V3 media, bounds and indirect-tree tests. */
#include "minix_module.h"
#ifdef IFS_TEST_COMMON_V2
#include <infiltratr/filesystem_module_v2.h>
#include <stdlib.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define BS 1024u
#define ZONES 64u
static uint8_t disk[ZONES * BS];
static unsigned reads;
static void w16(uint8_t *p, uint32_t x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x>>8); }
static void w32(uint8_t *p, uint32_t x) {
    p[0] = (uint8_t)x; p[1] = (uint8_t)(x>>8);
    p[2] = (uint8_t)(x>>16); p[3] = (uint8_t)(x>>24);
}
static int io_read(void *u, uint64_t off, void *dst, uint32_t n)
{
    (void)u;
    if (off > sizeof disk || n > sizeof disk - off) return -1;
    memcpy(dst, disk + (size_t)off, n);
    ++reads;
    return 0;
}
static unsigned writes, flushes;
static int io_write(void *u, uint64_t off, const void *src, uint32_t n)
{
    (void)u;
    /* Only the actual file's allocated data blocks may ever be written. */
    if (off < 10u * BS || off > sizeof disk || n > sizeof disk - off)
        return -1;
    memcpy(disk + (size_t)off, src, n);
    ++writes;
    return 0;
}
static int io_flush(void *u)
{
    (void)u;
    ++flushes;
    return 0;
}
#ifdef IFS_TEST_COMMON_V2
static uint32_t common_sector=512u;
static uint64_t fail_read_at=UINT64_MAX;
static const IfsFsCoreOpsV2 *unmount_during_read;
static void *active_volume;
static int observed_unmount;
static int common_read(void *ctx,uint64_t at,void *dst,uint32_t length)
{
    if (unmount_during_read) {
        const IfsFsCoreOpsV2 *ops=unmount_during_read;
        unmount_during_read=NULL;
        observed_unmount=ops->unmount(active_volume);
        if (observed_unmount!=IFS_FS_BUSY) return -1;
    }
    if(at==fail_read_at || at%common_sector ||
       length%common_sector || length>common_sector)
        return -1;
    return io_read(ctx,at,dst,length);
}
static int common_write(void *ctx,uint64_t at,const void *src,uint32_t length)
{
    if(at%common_sector || length%common_sector || length>common_sector)
        return -1;
    return io_write(ctx,at,src,length);
}
static void *common_alloc(void *ctx, size_t bytes)
{
    (void)ctx;
    return malloc(bytes);
}
static void common_free(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}
#endif
static int fail(const char *why) { fprintf(stderr, "MINIX: %s\n", why); return 1; }
static void inode_init(uint8_t *p, uint32_t version, uint16_t mode,
                       uint32_t bytes, uint32_t first)
{
    w16(p, mode);
    if (version == 1u) {
        w32(p + 4, bytes);
        p[13] = 1u;
        w16(p + 14, first);
    } else {
        w16(p + 2, 1u);
        w32(p + 8, bytes);
        w32(p + 24, first);
    }
}
static void inode_zone(uint8_t *inode, uint32_t version,
                       uint32_t slot, uint32_t zone)
{
    if (version == 1u) w16(inode + 14u + slot*2u, zone);
    else w32(inode + 24u + slot*4u, zone);
}
static void put_directory(uint8_t *p, uint32_t version, uint32_t stride,
                          uint32_t slot, uint32_t ino, const char *name)
{
    p += slot * stride;
    if (version == 3u) { w32(p, ino); p += 4; }
    else { w16(p, ino); p += 2; }
    while (*name) *p++ = (uint8_t)*name++;
}
static void fixture(uint32_t version, uint32_t namelen)
{
    uint8_t *sb, *root, *file, *dir;
    uint32_t stride = version == 3u ? 64u : namelen + 2u;
    uint32_t inode_size = version == 1u ? 32u : 64u;
    memset(disk, 0, sizeof disk);
    sb = disk + BS;
    if (version == 3u) {
        w32(sb, 64u);                  /* inode count */
        w16(sb + 6u, 1u);             /* inode bitmap blocks */
        w16(sb + 8u, 1u);             /* zone bitmap blocks */
        w16(sb + 10u, 8u);            /* first data zone */
        w32(sb + 16u, 100000u);       /* maximum file bytes */
        w32(sb + 20u, ZONES);
        w16(sb + 24u, 0x4d5au);
        w16(sb + 28u, BS);
    } else {
        w16(sb, 64u);
        w16(sb + 2u, ZONES);
        w16(sb + 4u, 1u);
        w16(sb + 6u, 1u);
        w16(sb + 8u, 8u);
        w32(sb + 12u, 100000u);
        w16(sb + 16u, version == 1u ?
            (namelen == 14u ? 0x137fu : 0x138fu) :
            (namelen == 14u ? 0x2468u : 0x2478u));
        if (version == 2u) w32(sb + 20u, ZONES);
    }
    disk[2u * BS] = 7u;               /* reserved, root and file inodes */
    memset(disk + 3u * BS, 0xffu, BS);
    /* Two unowned zones (40,41); preserve all indirect data/pointer
     * fixtures, including the nested triple-indirect chain at 28..31.
     * Bit 1 corresponds to first_data_zone=8; bit 0 is reserved. */
    disk[3u*BS+4u] &= (uint8_t)~((1u<<1u)|(1u<<2u));
    root = disk + 4u*BS;
    file = root + inode_size;
    inode_init(root, version, 0x41edu, 3u * stride, 8u);
    inode_init(file, version, 0x81a4u, 9u*BS+5u, 10u);
    if (version==1u) { w16(file+2u,1234u); file[12]=201u; }
    else { w16(file+4u,1234u); w16(file+6u,4321u); }
    for (uint32_t i = 0u; i < 7u; ++i)
        inode_zone(file, version, i, 10u + i);
    inode_zone(file, version, 7u, 24u);   /* single indirect */
    uint8_t *ind = disk + 24u*BS;
    for (uint32_t i = 0u; i < 3u; ++i) {
        if (version == 1u) w16(ind + 2u*i, 20u + i);
        else w32(ind + 4u*i, 20u + i);
    }
    for (uint32_t i = 0u; i < 10u; ++i) {
        uint32_t zone = i < 7u ? 10u + i : 20u + i - 7u;
        memset(disk + zone*BS, 'A'+(int)i, BS);
    }
    dir = disk + 8u*BS;
    put_directory(dir, version, stride, 0u, 1u, ".");
    put_directory(dir, version, stride, 1u, 1u, "..");
    put_directory(dir, version, stride, 2u, 2u, "HELLO.TXT");
}
static int run_variant(uint32_t version, uint32_t name_bytes)
{
    const IfsMinixModule *api = ifs_minix_native_read_ops();
    IfsMinixVolume volume;
    IfsMinixInode root, file, forged;
    IfsMinixDirent dent;
    IfsMinixIo io = {io_read, NULL, sizeof disk};
    uint8_t bytes[8];
    fixture(version, name_bytes);
    if (!api || api->abi_version != IFS_MINIX_NATIVE_TABLE_VERSION ||
        api->structure_bytes != sizeof *api ||
        api->capabilities != (IFS_MINIX_CAP_PROBE | IFS_MINIX_CAP_DIRECTORY |
                              IFS_MINIX_CAP_FILE_READ))
        return fail("ABI/capabilities mismatch");
    if (api->mount(&volume, &io) != 0 ||
        volume.version != version || volume.name_bytes != name_bytes)
        return fail("real superblock variant was rejected");
    if (api->inode(&volume, 1u, &root) != 0 ||
        api->inode(&volume, 2u, &file) != 0)
        return fail("allocated root/file inode not readable");
    if (api->entry(&volume, &root, 0u, &dent) != 0 ||
        dent.inode_number != 1u || strcmp(dent.name, ".") != 0 ||
        api->entry(&volume, &root, 2u, &dent) != 0 ||
        strcmp(dent.name, "HELLO.TXT") != 0 ||
        api->entry(&volume, &root, 3u, &dent) != 1)
        return fail("directory enumeration mismatch");
    if (api->lookup(&volume, &root, "HELLO.TXT", 9u, &dent) != 0 ||
        dent.inode_number != 2u ||
        api->lookup(&volume, &root, "hello.txt", 9u, &dent) != 1 ||
        api->lookup(&volume, &root, "wrong", 5u, &dent) != 1)
        return fail("directory lookup mismatch");
#ifdef IFS_TEST_COMMON_V2
    /* Test real Minix V1/V2/V3 on-media content through ONE strict Common
     * V2 interface. The native disk variants are not ABI compatibility. */
    {
        const IfsFsModuleApiV2 *common=infiltrator_filesystem_api_v2();
        const IfsFsCoreOpsV2 *core;
        const IfsFsVolumeOpsV2 *volume_ops;
        const IfsFsDefragOpsV2 *defrag_map;
        IfsFsHostRuntimeV2 host={0};
        IfsFsVolumeInfoV2 volume_info={0};
        IfsFsEntryV2 name_info={0};
        char filename[64]={0};
        unsigned char byte=0u;
        void *opened=NULL,*old=NULL;
        uint32_t detected=0u;
        host.abi_version=IFS_FS_HOST_ABI_V2;
        host.structure_bytes=sizeof(host);
        host.capabilities=IFS_FS_HOST_READ_ONLY;
        host.volume_bytes=sizeof disk;
        host.logical_sector_bytes=512u;
        host.physical_sector_bytes=512u;
        host.maximum_transfer_bytes=512u;
        host.read_at=common_read;
        host.allocate=common_alloc;
        host.release=common_free;
        volume_info.structure_bytes=sizeof(volume_info);
        name_info.structure_bytes=sizeof(name_info);
        name_info.name=filename;
        name_info.name_capacity=sizeof(filename);
        if(!ifs_fs_host_v2_valid(&host) ||
           !ifs_fs_api_v2_valid(common,"minix") ||
           common->group_mask!=(IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
                                 IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)|
                                 IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG)|
                                 IFS_FS_GROUP_BIT(IFS_FS_GROUP_METADATA)) ||
           (common->runtime_profiles & IFS_FS_PROFILE_KERNEL))
            return fail("strict Common V2 ABI or host invalid");
        core=(const IfsFsCoreOpsV2 *)ifs_fs_group_v2(common,IFS_FS_GROUP_CORE);
        volume_ops=(const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(common,IFS_FS_GROUP_VOLUME);
        defrag_map=(const IfsFsDefragOpsV2 *)ifs_fs_group_v2(common,IFS_FS_GROUP_DEFRAG);
        if(!core || !core->probe || !core->mount || !core->unmount ||
           !core->stat || !core->directory_entry || !core->read || !core->write ||
           !volume_ops || !volume_ops->query || !volume_ops->sync ||
           !volume_ops->format || !defrag_map || !defrag_map->map ||
           defrag_map->header.capabilities!=IFS_FS_DEFRAG_ALL ||
           !defrag_map->plan || !defrag_map->execute || !defrag_map->verify ||
           !defrag_map->recover || !defrag_map->release_plan)
            return fail("Missing strict V2 functionality or false capability");
        /* A true host transport failure is not a missing filesystem.
         * The failed 512-byte transfer must propagate through V2. */
        fail_read_at=1024u;
        detected=UINT32_MAX;
        if(core->probe(&host,&detected)!=IFS_FS_IO_ERROR || detected!=0u)
            return fail("Common V2 probe hid an I/O error or stale variant");
        fail_read_at=UINT64_MAX;
        {
            IfsFsHostRuntimeV2 invalid_host=host;
            invalid_host.abi_version=1u;
            detected=UINT32_MAX;
            if(core->probe(&invalid_host,&detected)!=IFS_FS_INVALID || detected!=0u)
                return fail("Invalid V2 host retained stale detected format");
        }
        if(core->probe(&host,&detected)!=IFS_FS_OK || detected!=version)
            return fail("Common V2 probe did not recover after I/O failure");
        /* A reserved zmap bit is not allocatable data. The hosted V2
         * bitmap verifier must refuse the corrupted map, not claim
         * measured zero free space or accept a partial transaction. */
        disk[3u*BS] &= (uint8_t)~1u;
        volume_info.structure_bytes=sizeof(volume_info);
        if(core->mount(&host,&opened,&volume_info)!=IFS_FS_CORRUPT || opened)
            return fail("Common V2 accepted a corrupt zone bitmap");
        disk[3u*BS] |= 1u;
        volatile size_t short_bytes=IFS_FS_HOST_V2_BASE_BYTES;
        IfsFsHostRuntimeV2 *short_host=malloc(short_bytes);
        if(!short_host)return fail("Short V2 host allocation");
        memcpy(short_host,&host,IFS_FS_HOST_V2_BASE_BYTES);
        short_host->structure_bytes=IFS_FS_HOST_V2_BASE_BYTES;
        if(core->mount(short_host,&opened,&volume_info)!=IFS_FS_OK || !opened ||
           core->unmount(opened)!=IFS_FS_OK)
            return fail("Original short V2 host rejected");
        free(short_host);
        opened=NULL;volume_info.structure_bytes=sizeof(volume_info);
        if(core->probe(&host,&detected)!=IFS_FS_OK || detected!=version ||
           core->mount(&host,&opened,&volume_info)!=IFS_FS_OK || !opened ||
           volume_info.variant!=version || volume_info.free_bytes!=2u*BS)
            return fail("V2 probe/mount failed on real Minix media");
        old=opened;
        if(core->stat(opened,"/HELLO.TXT",&name_info)!=IFS_FS_OK ||
           strcmp(filename,"HELLO.TXT") ||
           core->directory_entry(opened,"/",0u,&name_info)!=IFS_FS_OK ||
           strcmp(filename,"HELLO.TXT") ||
           core->directory_entry(opened,"/",1u,&name_info)!=IFS_FS_END ||
           core->read(opened,"/HELLO.TXT",0u,&byte,1u)!=IFS_FS_OK ||
           byte!='A')
            return fail("V2 native directory or file data mismatch");
        const IfsFsMetadataOpsV2 *meta=(const void *)ifs_fs_group_v2(common,IFS_FS_GROUP_METADATA);
        IfsFsMetadataV2 metadata={.structure_bytes=sizeof(metadata)};
        if (!meta || !meta->get || meta->set ||
            meta->header.capabilities!=IFS_FS_META_GET_MODE ||
            meta->get(opened,"/HELLO.TXT",&metadata)!=IFS_FS_OK ||
            metadata.mask!=IFS_FS_META_GET_MODE || metadata.mode!=0100644u ||
            metadata.owner!=1234u || metadata.group!=(version==1u?201u:4321u) ||
            meta->get(opened,"/ABSENT",&metadata)!=IFS_FS_NOT_FOUND)
            return fail("Common V2 native Minix ownership or mode mismatch");
        active_volume=opened;
        unmount_during_read=core;
        observed_unmount=IFS_FS_INVALID;
        if (core->read(opened,"/HELLO.TXT",0u,&byte,1u)!=IFS_FS_OK ||
            unmount_during_read || observed_unmount!=IFS_FS_BUSY)
            return fail("reentrant unmount must report BUSY");
        /* A second fully valid dirent with the same exact name must not
         * let either the native reader or Common V2 silently choose an
         * inode. Minix disk versions V1/V2/V3 all use this same decoder.
         * Modifications affect only the test's disposable in-memory disk. */
        {
            const uint32_t stride=volume.directory_bytes;
            uint8_t *root_bytes=disk+4u*BS;
            uint8_t *fourth=disk+8u*BS+3u*stride;
            const uint32_t root_size_at=version==1u?4u:8u;
            put_directory(disk+8u*BS,version,stride,3u,2u,"HELLO.TXT");
            w32(root_bytes+root_size_at,4u*stride);
            root.size_bytes=4u*stride;
            if(api->lookup(&volume,&root,"HELLO.TXT",9u,&dent)!=-1 ||
               core->stat(opened,"/HELLO.TXT",&name_info)!=IFS_FS_CORRUPT ||
               core->read(opened,"/HELLO.TXT",0u,&byte,1u)!=IFS_FS_CORRUPT)
                return fail("V2 accepted duplicate native Minix pathname");
            memset(fourth,0,stride);
            w32(root_bytes+root_size_at,3u*stride);
            root.size_bytes=3u*stride;
            if(api->lookup(&volume,&root,"HELLO.TXT",9u,&dent)!=0 ||
               core->stat(opened,"/HELLO.TXT",&name_info)!=IFS_FS_OK)
                return fail("V2 did not recover after Minix duplicate removal");
        }
        /* All supported Minix disk variants must report a short
         * STAT/LIST name buffer as NO_SPACE, leaving output untouched. */
        {
            char narrow[9] = {'X','X','X','X','X','X','X','X','X'};
            const char untouched[9] = {'X','X','X','X','X','X','X','X','X'};
            IfsFsEntryV2 small={0};
            small.structure_bytes=sizeof(small);
            small.name=narrow;
            small.name_capacity=sizeof(narrow);
            small.name_bytes=99u;
            if(core->stat(opened,"/HELLO.TXT",&small)!=IFS_FS_NO_SPACE ||
               memcmp(narrow,untouched,sizeof(narrow))!=0 ||
               core->directory_entry(opened,"/",0u,&small)!=IFS_FS_NO_SPACE ||
               memcmp(narrow,untouched,sizeof(narrow))!=0 ||
               small.name_bytes!=0u)
                return fail("Minix V2 short name buffer did not fail safely");
        }
        /* Exact physical mapping via Common V2, paginated across the
         * direct and indirect Minix zone pointers on real synthetic media. */
        {
            IfsFsExtentV2 page[4]={{0}};
            uint32_t n=99u;
            if(defrag_map->map(opened,"/HELLO.TXT",0u,page,4u,&n)!=IFS_FS_OK ||
               n!=4u || page[0].physical_offset!=10u*BS ||
               page[0].logical_offset!=0u || page[0].length!=BS ||
               page[3].physical_offset!=13u*BS ||
               page[3].logical_offset!=3u*BS ||
               defrag_map->map(opened,"/HELLO.TXT",7u,page,4u,&n)!=IFS_FS_OK ||
               n!=3u || page[0].physical_offset!=20u*BS ||
               page[2].physical_offset!=22u*BS || page[2].length!=5u ||
               defrag_map->map(opened,"/HELLO.TXT",10u,page,4u,&n)!=IFS_FS_END ||
               n!=0u ||
               defrag_map->map(opened,"/",0u,page,4u,&n)!=IFS_FS_UNSUPPORTED ||
               defrag_map->map(opened,"/../HELLO.TXT",0u,page,4u,&n)!=IFS_FS_INVALID)
                return fail("strict Common V2 extent-map pagination/identity failed");
        }
        if(core->write(opened,"/HELLO.TXT",0u,"X",1u)!=IFS_FS_ACCESS ||
           volume_ops->query(opened,&volume_info)!=IFS_FS_OK ||
           volume_info.variant!=version ||
           volume_info.free_bytes!=2u*BS || !volume_ops->format ||
           !core->create || core->truncate ||
           core->create(opened,"/NEW.TXT",IFS_FS_ENTRY_FILE)!=IFS_FS_ACCESS)
            return fail("V2 unsupported mutation capability falsely accepted");
        {
            void *duplicate=NULL;
            if(core->mount(&host,&duplicate,&volume_info)!=IFS_FS_BUSY ||
               duplicate) return fail("V2 admitted duplicate volume session");
        }
        /* Existing-file bytes can be overwritten only on an exclusive,
         * durably flushed V2 host. The native writer validates every zone. */
        if(core->unmount(opened)!=IFS_FS_OK ||
           core->unmount(old)!=IFS_FS_INVALID)
            return fail("V2 unmount / stale handle acceptance");
        host.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE;
        host.write_at=common_write;
        host.flush=io_flush;
        if(!ifs_fs_host_v2_valid(&host))return fail("Writable V2 host invalid");
        volume_info.structure_bytes=sizeof(volume_info);
        if(core->mount(&host,&opened,&volume_info)!=IFS_FS_OK || !opened ||
           opened==old) return fail("V2 exclusive mount failed");
        /* Faulted inode lookup must propagate I/O, but a later
         * checksummed-independent structural failure must not inherit it. */
        writes=flushes=0u;
        fail_read_at=4u*BS;
        if(core->write(opened,"/HELLO.TXT",0u,"X",1u)!=IFS_FS_IO_ERROR ||
           writes) return fail("V2 write lookup lost native I/O status");
        fail_read_at=UINT64_MAX;
        {
            const uint32_t stride=volume.directory_bytes;
            uint8_t *root_bytes=disk+4u*BS;
            uint8_t *fourth=disk+8u*BS+3u*stride;
            const uint32_t root_size_at=version==1u?4u:8u;
            put_directory(disk+8u*BS,version,stride,3u,2u,"HELLO.TXT");
            w32(root_bytes+root_size_at,4u*stride);
            if(core->write(opened,"/HELLO.TXT",0u,"X",1u)!=IFS_FS_CORRUPT ||
               writes) return fail("V2 write inherited stale I/O status");
            memset(fourth,0,stride);
            w32(root_bytes+root_size_at,3u*stride);
        }
        if(core->write(opened,"/HELLO.TXT",7u*BS-3u,"wXYZfoo",7u)!=IFS_FS_OK ||
           !writes || !flushes ||
           core->read(opened,"/HELLO.TXT",7u*BS-3u,
                      bytes,7u)!=IFS_FS_OK ||
           memcmp(bytes,"wXYZfoo",7u))
            return fail("Strict V2 bounded native write/read mismatch");
        if(core->write(opened,"/HELLO.TXT",9u*BS+6u,"X",1u)!=IFS_FS_UNSUPPORTED ||
           core->write(opened,"/../HELLO.TXT",0u,"X",1u)!=IFS_FS_INVALID ||
           volume_ops->sync(opened)!=IFS_FS_OK ||
           core->unmount(opened)!=IFS_FS_OK ||
           volume_ops->query(old,&volume_info)!=IFS_FS_INVALID)
            return fail("V2 unsafe path/write/sync/stale check failed");
        fixture(version,name_bytes);
        /* 4Kn read-only host: byte-oriented Minix inode and bitmap accesses
         * are reconstructed by Common without unaligned device callbacks. */
        host.capabilities=IFS_FS_HOST_READ_ONLY;
        host.write_at=NULL;host.flush=NULL;
        host.logical_sector_bytes=4096u;
        host.physical_sector_bytes=4096u;
        host.maximum_transfer_bytes=4096u;
        common_sector=4096u;
        if(!ifs_fs_host_v2_valid(&host) ||
           core->probe(&host,&detected)!=IFS_FS_OK || detected!=version ||
           core->mount(&host,&opened,&volume_info)!=IFS_FS_OK ||
           core->read(opened,"/HELLO.TXT",7u*BS-3u,
                      bytes,7u)!=IFS_FS_OK ||
           memcmp(bytes,"GGGHHHH",7u) ||
           core->unmount(opened)!=IFS_FS_OK)
            return fail("4Kn Common V2 byte offset access failed");
        common_sector=512u;
    }
#endif
    if (api->read(&volume, &file, 7u*BS-3u, bytes, 7u) != 0 ||
        memcmp(bytes, "GGGHHHH", 7u) != 0 ||
        api->read(&volume, &file, 9u*BS+4u, bytes, 1u) != 0 ||
        bytes[0] != 'J')
        return fail("direct/single-indirect read mismatch");
    if (api->read(&volume, &file, 9u*BS+5u, bytes, 1u) == 0)
        return fail("accepted read beyond EOF");
    {
        const IfsMinixModuleV2 *v2 = ifs_minix_native_write_ops();
        IfsMinixWritableIo rw = {{io_read, NULL, sizeof disk},
                                  io_write, io_flush};
        writes = flushes = 0u;
        if (!v2 || v2->abi_version != IFS_MINIX_NATIVE_WRITER_VERSION ||
            v2->structure_bytes != sizeof *v2 ||
            v2->readonly_engine != api ||
            v2->capabilities != IFS_MINIX_V2_CAP_OVERWRITE_EXISTING)
            return fail("V2 ABI/capability compatibility mismatch");
        if (v2->overwrite_existing(&rw, &volume, &file,
                                   7u*BS-3u, "wXYZfoo", 7u) != 0 ||
            writes != 2u || flushes != 1u ||
            api->read(&volume, &file, 7u*BS-3u, bytes, 7u) != 0 ||
            memcmp(bytes, "wXYZfoo", 7u) != 0)
            return fail("V2 existing data cross-zone write/flush mismatch");
        if (v2->overwrite_existing(&rw, &volume, &file,
                                   file.size_bytes, "X", 1u) == 0 ||
            writes != 2u)
            return fail("V2 wrote beyond existing file end");
        if (v2->overwrite_existing(&rw, &volume, &root,
                                   0u, "X", 1u) == 0 || writes != 2u)
            return fail("V2 wrote directory metadata");
        forged = file;
        forged.zone[7] = 65u;
        if (v2->overwrite_existing(&rw, &volume, &forged,
                                   7u*BS, "X", 1u) == 0 || writes != 2u)
            return fail("V2 trusted a forged inode mapping");
        rw.flush = NULL;
        if (v2->overwrite_existing(&rw, &volume, &file,
                                   0u, "X", 1u) == 0 || writes != 2u)
            return fail("V2 wrote without flush support");
        rw.flush = io_flush;
        rw.write_at = NULL;
        if (v2->overwrite_existing(&rw, &volume, &file,
                                   0u, "X", 1u) == 0 || writes != 2u)
            return fail("V2 wrote without explicit writer");
        rw.write_at = io_write;
        rw.read.device_bytes = sizeof disk - BS;
        if (v2->overwrite_existing(&rw, &volume, &file,
                                   0u, "X", 1u) == 0 || writes != 2u)
            return fail("V2 ignored truncated partition");
        rw.read.device_bytes = sizeof disk;
        /* Never allocate through a hole or write later allocated zones
         * before learning that the complete write range contains a hole. */
        if (version == 1u)
            w16(disk + 4u*BS + 32u + 14u + 7u*2u, 0u);
        else
            w32(disk + 4u*BS + 64u + 24u + 7u*4u, 0u);
        if (api->inode(&volume, 2u, &forged) != 0 ||
            v2->overwrite_existing(&rw, &volume, &forged,
                                   7u*BS-3u, "ABCDEFG", 7u) == 0 ||
            writes != 2u)
            return fail("V2 failed to preflight sparse destination");
        fixture(version, name_bytes);
        if (api->mount(&volume, &io) != 0 ||
            api->inode(&volume, 2u, &file) != 0)
            return fail("V2 restored media was not readable");
    }
    forged = file;
    forged.zone[7] = 65u;
    if (api->read(&volume, &forged, 7u*BS, bytes, 4u) == 0)
        return fail("accepted out-of-volume indirect zone");
    /* Holes must exist in the real inode on disk, not a forged handle. */
    if (version == 1u)
        w16(disk + 4u*BS + 32u + 14u + 7u*2u, 0u);
    else
        w32(disk + 4u*BS + 64u + 24u + 7u*4u, 0u);
    if (api->inode(&volume, 2u, &forged) != 0)
        return fail("cannot reload sparse inode");
    memset(bytes, 0xa5u, sizeof bytes);
    if (api->read(&volume, &forged, 7u*BS, bytes, 4u) != 0 ||
        bytes[0] != 0u || bytes[1] != 0u ||
        bytes[2] != 0u || bytes[3] != 0u)
        return fail("sparse hole was not zero");
    io.device_bytes = sizeof disk - BS;
    /* An image that is shorter than its on-disk declared block count fails. */
    if (api->mount(&volume, &io) == 0)
        return fail("truncated device accepted");
    io.device_bytes = sizeof disk;
    if (version == 3u) w16(disk + BS + 12u, 1u);
    else w16(disk + BS + 10u, 1u);
    if (api->mount(&volume, &io) == 0)
        return fail("unsupported zone-shift accepted");
    fixture(version, name_bytes);
    if (version == 3u) w16(disk + BS + 24u, 0u);
    else w16(disk + BS + 16u, 0u);
    if (api->mount(&volume, &io) == 0)
        return fail("unknown format magic accepted");
    fixture(version, name_bytes);
    /* Allocated flag is authoritative; an unallocated root is invalid. */
    disk[2u*BS] = 1u;
    if (api->mount(&volume, &io) == 0)
        return fail("unallocated root accepted");
    fixture(version, name_bytes);
    if (api->mount(&volume, &io) != 0 ||
        api->inode(&volume, 1u, &root) != 0)
        return fail("cannot reload allocation test");
    disk[3u*BS] &= (uint8_t)~2u; /* first data zone bit 1 */
    if (api->entry(&volume, &root, 0u, &dent) == 0)
        return fail("accepted root data from unallocated zone");

    /* Independently exercise the deep pointer trees with sparse logical
     * files: high offsets do not require allocating gigabytes of media. */
    fixture(version, name_bytes);
    {
        const uint32_t pointers = version == 1u ? 512u : 256u;
        const uint32_t double_block = 7u + pointers;
        const uint32_t triple_block = double_block + pointers * pointers;
        const uint32_t logical_block =
            version == 1u ? double_block : triple_block;
        const uint32_t size = (logical_block + 1u) * BS;
        uint8_t *raw_file = disk + 4u*BS +
            (version == 1u ? 32u : 64u);
        uint8_t *sb = disk + BS;
        w32(sb + (version == 3u ? 16u : 12u), size + BS);
        w32(raw_file + (version == 1u ? 4u : 8u), size);
        inode_zone(raw_file, version, 8u, 25u);
        if (version == 1u) {
            w16(disk + 25u*BS, 26u);
            w16(disk + 26u*BS, 27u);
            disk[27u*BS] = (uint8_t)'D';
        } else {
            inode_zone(raw_file, version, 9u, 28u);
            w32(disk + 28u*BS, 29u);
            w32(disk + 29u*BS, 30u);
            w32(disk + 30u*BS, 31u);
            disk[31u*BS] = (uint8_t)'T';
        }
        if (api->mount(&volume, &io) != 0 ||
            api->inode(&volume, 2u, &file) != 0 ||
            api->read(&volume, &file,
                      (uint64_t)logical_block * BS, bytes, 1u) != 0 ||
            bytes[0] != (uint8_t)(version == 1u ? 'D' : 'T'))
            return fail("double/triple-indirect deep file mapping failed");
        /* Corrupt an on-disk indirect pointer: never read outside volume. */
        if (version == 1u) w16(disk + 26u*BS, 65u);
        else w32(disk + 30u*BS, 65u);
        if (api->read(&volume, &file,
                      (uint64_t)logical_block * BS, bytes, 1u) == 0)
            return fail("out-of-range deep indirect pointer accepted");
    }
    return 0;
}
#ifdef IFS_TEST_COMMON_V2
/* Unlike the existing-file writer test, this callback permits metadata
 * writes only inside our disposable in-memory disk. */
static int format_write(void *ctx,uint64_t at,const void *src,uint32_t n)
{
    (void)ctx;
    if(at%common_sector || n%common_sector || n>common_sector ||
       at>sizeof(disk) || n>sizeof(disk)-at)return -1;
    memcpy(disk+(size_t)at,src,n);++writes;return 0;
}
static int test_format(void)
{
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    const IfsFsCoreOpsV2 *core=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *vol=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    if(!vol || !vol->format)return fail("missing V3 format");
    for(uint32_t sector=512u;sector<=4096u;sector*=2u) {
        common_sector=sector;fail_read_at=UINT64_MAX;
        memset(disk,0xa5,sizeof(disk));writes=flushes=0;
        IfsFsHostRuntimeV2 h={
            .abi_version=IFS_FS_HOST_ABI_V2,.structure_bytes=sizeof(h),
            .capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE,
            .volume_bytes=sizeof(disk),.logical_sector_bytes=sector,
            .physical_sector_bytes=sector,.maximum_transfer_bytes=sector,
            .read_at=common_read,.write_at=format_write,.flush=io_flush,
            .allocate=common_alloc,.release=common_free
        };
        IfsFsHostRuntimeV2 ro=h;
        ro.capabilities=IFS_FS_HOST_READ_ONLY;ro.write_at=NULL;ro.flush=NULL;
        if(vol->format(&ro,3u,NULL,0u)!=IFS_FS_ACCESS ||
           vol->format(&h,2u,NULL,0u)!=IFS_FS_UNSUPPORTED ||
           vol->format(&h,3u,"unsupported label",0u)!=IFS_FS_UNSUPPORTED ||
           writes || flushes)return fail("format rejected request wrote media");
        if(sector==4096u) {
            /* A 64 KiB fixture is too small for 64 4 KiB zones. */
            if(vol->format(&h,3u,NULL,0u)!=IFS_FS_UNSUPPORTED || writes)
                return fail("format accepted undersized geometry");
            continue;
        }
        if(sector==2048u) {
            if(vol->format(&h,3u,NULL,0u)!=IFS_FS_UNSUPPORTED || writes)
                return fail("format accepted undersized geometry");
            continue;
        }
        if(vol->format(&h,3u,NULL,0u)!=IFS_FS_OK || flushes<3u)
            return fail("V3 format or publication");
        uint32_t v=0;
        IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
        void *m=NULL;
        char name[64];
        IfsFsEntryV2 root={.structure_bytes=sizeof(root),.name=name,
                           .name_capacity=sizeof(name)};
        if(core->probe(&ro,&v)!=IFS_FS_OK || v!=3u ||
           core->mount(&ro,&m,&info)!=IFS_FS_OK || !m ||
           info.variant!=3u || info.total_bytes!=sizeof(disk) ||
           !info.free_bytes || info.free_bytes>=info.total_bytes ||
           core->stat(m,"/",&root)!=IFS_FS_OK ||
           root.type!=IFS_FS_ENTRY_DIRECTORY ||
           core->directory_entry(m,"/",0u,&root)!=IFS_FS_END ||
           vol->format(&h,3u,NULL,0u)!=IFS_FS_BUSY ||
           core->unmount(m)!=IFS_FS_OK)return fail("formatted root validation");
        if(vol->format(&h,0u,"LD_MINIX",0u)!=IFS_FS_OK ||
           core->mount(&h,&m,&info)!=IFS_FS_OK)return fail("default format");
        uint8_t data[1024],copy[1024];memset(data,0x37,sizeof(data));
        const uint64_t initial=info.free_bytes;
        if(core->create(m,"/A.BIN",IFS_FS_ENTRY_FILE)!=IFS_FS_OK ||
           core->create(m,"/B.BIN",IFS_FS_ENTRY_FILE)!=IFS_FS_OK ||
           core->create(m,"/A.BIN",IFS_FS_ENTRY_FILE)!=IFS_FS_EXISTS ||
           core->write(m,"/A.BIN",0,data,sizeof(data))!=IFS_FS_OK ||
           core->write(m,"/B.BIN",0,data,sizeof(data))!=IFS_FS_OK ||
           core->write(m,"/A.BIN",sizeof(data),data,sizeof(data))!=IFS_FS_OK ||
           vol->query(m,&info)!=IFS_FS_OK || info.free_bytes!=initial-3u*sizeof(data) ||
           core->unmount(m)!=IFS_FS_OK ||
           core->mount(&ro,&m,&info)!=IFS_FS_OK ||
           core->read(m,"/A.BIN",sizeof(data),copy,sizeof(copy))!=IFS_FS_OK ||
           memcmp(data,copy,sizeof(data)))return fail("V3 persistent interleaved append");
        const IfsFsDefragOpsV2 *map=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
        IfsFsExtentV2 ext[2];uint32_t n=0;
        if(map->map(m,"/A.BIN",0,ext,2,&n)!=IFS_FS_OK || n!=2 ||
           ext[1].physical_offset==ext[0].physical_offset+ext[0].length ||
           core->unmount(m)!=IFS_FS_OK)return fail("V3 real fragmented extents");
        for(uint32_t policy=0u;policy<=10u;policy+=10u) {
            if(core->mount(&h,&m,&info)!=IFS_FS_OK)return fail("relocation mount");
            void *plan=NULL;uint64_t moved=0u,repaired=UINT64_MAX;
            if(map->plan(m,"/",policy,&plan)!=IFS_FS_OK || !plan ||
               map->execute(m,plan,&moved)!=IFS_FS_OK || !moved ||
               map->verify(m,plan)!=IFS_FS_OK)return fail("native V3 relocation");
            map->release_plan(m,plan);
            if(map->recover(m,0u,&repaired)!=IFS_FS_OK || repaired ||
               map->map(m,"/A.BIN",0,ext,2,&n)!=IFS_FS_OK || n!=2 ||
               ext[1].physical_offset!=ext[0].physical_offset+ext[0].length ||
               core->read(m,"/A.BIN",sizeof(data),copy,sizeof(copy))!=IFS_FS_OK ||
               memcmp(data,copy,sizeof(data)) ||
               core->read(m,"/B.BIN",0,copy,sizeof(copy))!=IFS_FS_OK ||
               memcmp(data,copy,sizeof(data)) ||
               vol->query(m,&info)!=IFS_FS_OK || info.free_bytes!=initial-3u*sizeof(data) ||
               core->unmount(m)!=IFS_FS_OK)return fail("V3 relocation integrity");
        }
    }
    common_sector=512u;
    puts("MINIX V3 native format, read-only mount and empty namespace PASS");
    return 0;
}
#endif

int main(void)
{
#ifdef IFS_TEST_COMMON_V2
    if(test_format())return 1;
#endif
    if (run_variant(1u, 14u) || run_variant(1u, 30u) ||
        run_variant(2u, 14u) || run_variant(2u, 30u) ||
        run_variant(3u, 60u)) return 1;
    printf("MINIX-V1-V2-V3-ROOT-INDIRECT-BOUNDS-PASS (%u reads)\n", reads);
    return 0;
}
