/* SPDX-License-Identifier: GPL-3.0-or-later
 * Strict Common V2 Minix filesystem over the canonical V1/V2/V3
 * ON-DISK parser and bounded existing-data writer. Disk format versions
 * are independent of the Common filesystem ABI.
 */
#include <infiltratr/filesystem_module_v2.h>
#include "../native/filesystems/minix/module/minix_module.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* The source-owned engine is per-volume and uses host-neutral I/O.
 * One native shared-object instance owns one active mounted volume. */
static struct {
    uintptr_t generation, active;
    IfsFsHostRuntimeV2 host;
    IfsMinixVolume media;
    uint64_t verified_free_zones;
} session;
/* The native parser's callback is boolean, but Common V2 must preserve
 * transport errors instead of reporting a missing file or corrupt media. */
static int last_read_error = IFS_FS_OK;

static const IfsMinixModule *native(void)
{
    return ifs_minix_native_read_ops();
}
static int host_ok(const IfsFsHostRuntimeV2 *host)
{
    return ifs_fs_host_v2_valid(host) && host->volume_bytes >= 5120u;
}
static int host_read(void *context,uint64_t position,void *data,uint32_t n)
{
    const int rc = ifs_fs_host_read_bytes_v2(
        (const IfsFsHostRuntimeV2 *)context,position,data,n);
    if(rc!=IFS_FS_OK)last_read_error=rc;
    return rc==IFS_FS_OK?0:-1;
}
#if !defined(IFS_MINIX_FIRMWARE_BUILD)
/* The existing Minix overwrite engine can request byte-granular writes.
 * V2's only authorised device writes are complete logical sectors, so
 * preserve every neighbouring byte with a bounded read/modify/write.
 * This does NOT claim crash rollback after a failed host write. */
static int host_write(void *context,uint64_t position,
                      const void *buffer,uint32_t length)
{
    const IfsFsHostRuntimeV2 *h=context;
    const unsigned char *source=buffer;
    const uint32_t sector=h ? h->logical_sector_bytes : 0u;
    uint8_t *scratch=NULL;
    uint32_t n,skip;
    int result=-1;
    if(!host_ok(h) || !h->write_at || !h->flush ||
       (h->capabilities & IFS_FS_HOST_READ_ONLY) ||
       !(h->capabilities & IFS_FS_HOST_EXCLUSIVE) ||
       !(h->capabilities & IFS_FS_HOST_DURABLE) ||
       (!source && length) || position>h->volume_bytes ||
       length>h->volume_bytes-position)
        return -1;
    if(!length)return 0;
    scratch=h->allocate(h->context,sector);
    if(!scratch)return -1;
    while(length) {
        uint64_t base=position-position%sector;
        skip=(uint32_t)(position%sector);
        n=sector-skip;
        if(n>length)n=length;
        if(base>h->volume_bytes || sector>h->volume_bytes-base)
            goto done;
        if(skip || n!=sector) {
            if(h->read_at(h->context,base,scratch,sector)!=0)
                goto done;
            memcpy(scratch+skip,source,n);
        } else
            memcpy(scratch,source,sector);
        if(h->write_at(h->context,base,scratch,sector)!=0)
            goto done;
        position+=n;
        source+=n;
        length-=n;
    }
    result=0;
done:
    h->release(h->context,scratch);
    return result;
}
#endif /* !IFS_MINIX_FIRMWARE_BUILD */

static int host_flush(void *context)
{
    const IfsFsHostRuntimeV2 *h=context;
    return host_ok(h) && h->flush && h->flush(h->context)==0 ? 0 : -1;
}
static IfsMinixIo minix_io(const IfsFsHostRuntimeV2 *h)
{
    IfsMinixIo io={host_read,(void *)h,h->volume_bytes};
    return io;
}
static int probe(const IfsFsHostRuntimeV2 *h,uint32_t *variant)
{
    IfsMinixVolume media={0};
    IfsMinixIo io;
    uint8_t superblock[64];
    uint16_t legacy_magic, v3_magic;
    int rc;
    if(!variant)return IFS_FS_INVALID;
    *variant=0u;
    if(!host_ok(h))return IFS_FS_INVALID;
    /* The V1/V2 and V3 Minix superblocks have distinct magic offsets.
     * First inspect the on-disk identity through the V2 host, so absence,
     * corruption and block transport errors are distinguishable. */
    rc=ifs_fs_host_read_bytes_v2(h,1024u,superblock,sizeof(superblock));
    if(rc!=IFS_FS_OK)return rc;
    legacy_magic=(uint16_t)(superblock[16] | ((uint16_t)superblock[17]<<8));
    v3_magic=(uint16_t)(superblock[24] | ((uint16_t)superblock[25]<<8));
    if(legacy_magic!=0x137fu && legacy_magic!=0x138fu &&
       legacy_magic!=0x2468u && legacy_magic!=0x2478u &&
       v3_magic!=0x4d5au)
        return IFS_FS_NOT_FOUND;
    io=minix_io(h);
    last_read_error=IFS_FS_OK;
    if(native()->mount(&media,&io)!=0)
        return last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
    *variant=media.version;
    return IFS_FS_OK;
}
static int live(void *volume)
{
    return session.active!=0u && (uintptr_t)volume==session.active;
}
/* Count actual unallocated Minix data zones using the authoritative
 * first_data_zone -> zmap bit-1 rule from the native Minix inode reader.
 * Only Common V2 sector-aligned host I/O is used, never a pathname or
 * retired V1 services. A malformed/short bitmap cannot yield free_bytes. */
static int count_free_zones(const IfsMinixVolume *v,
                            const IfsFsHostRuntimeV2 *h,
                            uint64_t *free_zones)
{
    const uint64_t data_zones=(uint64_t)v->zone_count-v->first_data_zone;
    const uint64_t needed_bits=data_zones+1u;
    const uint64_t bitmap_bits=(uint64_t)v->bitmap_zone_blocks *
                                v->block_bytes * 8u;
    const uint64_t map_base=(2u+(uint64_t)v->bitmap_inode_blocks) *
                             v->block_bytes;
    uint64_t free_total=0u;
    if(!h || !free_zones || !v->block_bytes || !v->bitmap_zone_blocks ||
       v->first_data_zone>=v->zone_count || needed_bits>bitmap_bits ||
       map_base>h->volume_bytes ||
       (uint64_t)v->bitmap_zone_blocks*v->block_bytes >
           h->volume_bytes-map_base)
        return IFS_FS_CORRUPT;
    uint8_t *raw=h->allocate(h->context,v->block_bytes);
    if(!raw)return IFS_FS_NO_MEMORY;
    int rc=IFS_FS_OK;
    for(uint32_t group=0u;group<v->bitmap_zone_blocks;++group) {
        uint64_t first=(uint64_t)group*v->block_bytes*8u;
        if(first>=needed_bits)break;
        uint64_t count=needed_bits-first;
        if(count>(uint64_t)v->block_bytes*8u)
            count=(uint64_t)v->block_bytes*8u;
        rc=ifs_fs_host_read_bytes_v2(
            h,map_base+(uint64_t)group*v->block_bytes,
            raw,v->block_bytes);
        if(rc!=IFS_FS_OK)break;
        if(group==0u && !(raw[0]&1u)) {
            rc=IFS_FS_CORRUPT;break; /* reserved zmap bit zero */
        }
        for(uint64_t bit=0u;bit<count;++bit) {
            if(first+bit==0u)continue;
            free_total+=(raw[bit>>3u] &
                (uint8_t)(1u<<(bit&7u)))==0u;
        }
    }
    h->release(h->context,raw);
    if(rc==IFS_FS_OK)*free_zones=free_total;
    return rc;
}
static void describe(IfsFsVolumeInfoV2 *info)
{
    info->structure_bytes=sizeof(*info);
    info->variant=session.media.version;
    info->block_bytes=session.media.block_bytes;
    info->max_name_bytes=session.media.name_bytes;
    info->total_bytes=session.media.volume_bytes;
    /* Only the verified data-zone allocation bitmap is counted. */
    info->free_bytes=session.verified_free_zones*
                     session.media.block_bytes;
}
static int mount_volume(const IfsFsHostRuntimeV2 *h,void **out,
                        IfsFsVolumeInfoV2 *info)
{
    IfsMinixIo io;
    uint32_t variant;
    int probe_rc;
    if(!out || !info || info->structure_bytes<sizeof(*info) || !host_ok(h))
        return IFS_FS_INVALID;
    *out=NULL;
    if(session.active || session.generation==UINTPTR_MAX)
        return IFS_FS_BUSY;
    probe_rc=probe(h,&variant);
    if(probe_rc!=IFS_FS_OK)return probe_rc;
    /* Preserve the caller's size-versioned V2 prefix without reading
     * optional fields that an earlier host did not allocate. */
    size_t host_bytes=h->structure_bytes<sizeof(session.host) ?
        h->structure_bytes : sizeof(session.host);
    memset(&session.host,0,sizeof(session.host));
    memcpy(&session.host,h,host_bytes);
    session.host.structure_bytes=(uint32_t)host_bytes;
    io=minix_io(&session.host);
    last_read_error=IFS_FS_OK;
    if(native()->mount(&session.media,&io)!=0) {
        const int result=last_read_error!=IFS_FS_OK?
            last_read_error:IFS_FS_CORRUPT;
        memset(&session.host,0,sizeof(session.host));
        memset(&session.media,0,sizeof(session.media));
        return result;
    }
    int bitmap_rc=count_free_zones(&session.media,&session.host,
                                   &session.verified_free_zones);
    if(bitmap_rc!=IFS_FS_OK) {
        memset(&session.host,0,sizeof(session.host));
        memset(&session.media,0,sizeof(session.media));
        session.verified_free_zones=0u;
        return bitmap_rc;
    }
    session.active=++session.generation;
    describe(info);
    *out=(void *)session.active;
    return IFS_FS_OK;
}
static int unmount_volume(void *volume)
{
    if(!live(volume))return IFS_FS_INVALID;
    session.active=0u;
    memset(&session.host,0,sizeof(session.host));
    memset(&session.media,0,sizeof(session.media));
    session.verified_free_zones=0u;
    return IFS_FS_OK;
}
static int volume_query(void *volume,IfsFsVolumeInfoV2 *info)
{
    if(!live(volume)||!info||info->structure_bytes<sizeof(*info))
        return IFS_FS_INVALID;
    describe(info);
    return IFS_FS_OK;
}
static int volume_sync(void *volume)
{
    if(!live(volume))return IFS_FS_INVALID;
    if(session.host.capabilities & IFS_FS_HOST_READ_ONLY)return IFS_FS_OK;
    return host_flush(&session.host)==0?IFS_FS_OK:IFS_FS_IO_ERROR;
}

/* No symlink traversal; reject dot segments and trailing slashes. */
static int resolve(const char *path,IfsMinixInode *inode,
                   const char **basename,uint32_t *bytes)
{
    const char *p;
    if(!session.active || !path || path[0]!='/' || !inode)
        return IFS_FS_INVALID;
    if(native()->inode(&session.media,1u,inode)!=0)
        return last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
    p=path+1;
    if(!*p) {
        if(basename)*basename="/";
        if(bytes)*bytes=1u;
        return IFS_FS_OK;
    }
    while(*p) {
        IfsMinixDirent dent={0};
        const char *start=p;
        size_t n;
        int rc;
        while(*p && *p!='/')++p;
        n=(size_t)(p-start);
        if(!n || n>session.media.name_bytes ||
           (n==1u && *start=='.') ||
           (n==2u && start[0]=='.' && start[1]=='.'))
            return IFS_FS_INVALID;
        if((inode->mode&0xf000u)!=IFS_MINIX_MODE_DIRECTORY)
            return IFS_FS_NOT_FOUND;
        rc=native()->lookup(&session.media,inode,start,(uint32_t)n,&dent);
        if(rc!=0)return rc==1?IFS_FS_NOT_FOUND:
            last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
        if(native()->inode(&session.media,dent.inode_number,inode)!=0)
            return last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
        if(basename)*basename=start;
        if(bytes)*bytes=(uint32_t)n;
        if(*p=='/' && !*++p)return IFS_FS_INVALID;
    }
    return IFS_FS_OK;
}
static int fill(IfsFsEntryV2 *out,const IfsMinixInode *inode,
                const char *name,uint32_t bytes)
{
    uint32_t type;
    if(!out || out->structure_bytes<sizeof(*out) ||
       !out->name || !name)
        return IFS_FS_INVALID;
    /* An otherwise valid STAT/LIST request needs a larger caller buffer.
     * Return capacity failure without writing a partial name or metadata. */
    if(bytes>=out->name_capacity)
        return IFS_FS_NO_SPACE;
    switch(inode->mode&0xf000u) {
    case IFS_MINIX_MODE_REGULAR:type=IFS_FS_ENTRY_FILE;break;
    case IFS_MINIX_MODE_DIRECTORY:type=IFS_FS_ENTRY_DIRECTORY;break;
    case IFS_MINIX_MODE_SYMLINK:type=IFS_FS_ENTRY_LINK;break;
    default:return IFS_FS_UNSUPPORTED;
    }
    memcpy(out->name,name,bytes);
    out->name[bytes]='\0';
    out->name_bytes=bytes;
    out->type=type;
    out->attributes=inode->mode;
    out->size_bytes=inode->size_bytes;
    return IFS_FS_OK;
}
static int stat_path(void *volume,const char *path,IfsFsEntryV2 *out)
{
    IfsMinixInode inode;
    const char *name=NULL;
    uint32_t bytes=0;
    int rc;
    if(!live(volume))return IFS_FS_INVALID;
    last_read_error=IFS_FS_OK;
    rc=resolve(path,&inode,&name,&bytes);
    return rc?rc:fill(out,&inode,name,bytes);
}
static int directory_entry(void *volume,const char *path,uint32_t index,
                           IfsFsEntryV2 *out)
{
    IfsMinixInode directory,file;
    uint32_t returned=0u;
    int rc;
    if(!live(volume))return IFS_FS_INVALID;
    last_read_error=IFS_FS_OK;
    rc=resolve(path,&directory,NULL,NULL);
    if(rc)return rc;
    if((directory.mode&0xf000u)!=IFS_MINIX_MODE_DIRECTORY)
        return IFS_FS_INVALID;
    for(uint32_t i=0u;;++i) {
        IfsMinixDirent dent={0};
        const char *zero;
        uint32_t len;
        rc=native()->entry(&session.media,&directory,i,&dent);
        if(rc)return rc==1?IFS_FS_END:
            last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
        zero=memchr(dent.name,'\0',sizeof(dent.name));
        if(!zero)return IFS_FS_CORRUPT;
        len=(uint32_t)(zero-dent.name);
        if((len==1u && dent.name[0]=='.') ||
           (len==2u && dent.name[0]=='.' && dent.name[1]=='.'))
            continue;
        if(returned++!=index)continue;
        if(native()->inode(&session.media,dent.inode_number,&file)!=0)
            return last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
        return fill(out,&file,dent.name,len);
    }
}
static int read_file(void *volume,const char *path,uint64_t at,
                     void *data,uint32_t length)
{
    IfsMinixInode inode;
    int rc;
    if(!live(volume) || (!data && length))return IFS_FS_INVALID;
    last_read_error=IFS_FS_OK;
    rc=resolve(path,&inode,NULL,NULL);
    if(rc)return rc;
    if((inode.mode&0xf000u)!=IFS_MINIX_MODE_REGULAR)
        return IFS_FS_INVALID;
    if(at>inode.size_bytes || length>inode.size_bytes-at)
        return IFS_FS_UNSUPPORTED;
    return native()->read(&session.media,&inode,at,data,length)==0?
        IFS_FS_OK:
        last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
}
#if !defined(IFS_MINIX_FIRMWARE_BUILD)
static int write_file(void *volume,const char *path,uint64_t at,
                      const void *data,uint32_t length)
{
    IfsMinixInode inode;
    IfsMinixWritableIo io={0};
    int rc;
    if(!live(volume) || (!data && length))return IFS_FS_INVALID;
    if((session.host.capabilities & IFS_FS_HOST_READ_ONLY) ||
       !(session.host.capabilities & IFS_FS_HOST_EXCLUSIVE) ||
       !(session.host.capabilities & IFS_FS_HOST_DURABLE) ||
       !session.host.write_at || !session.host.flush)
        return IFS_FS_ACCESS;
    rc=resolve(path,&inode,NULL,NULL);
    if(rc)return rc;
    if((inode.mode&0xf000u)!=IFS_MINIX_MODE_REGULAR)
        return IFS_FS_INVALID;
    if(at>inode.size_bytes || length>inode.size_bytes-at)
        return IFS_FS_UNSUPPORTED;
    io.read=minix_io(&session.host);
    io.write_at=host_write;
    io.flush=host_flush;
    return ifs_minix_native_write_ops()->overwrite_existing(
        &io,&session.media,&inode,at,data,length)==0?
        IFS_FS_OK:IFS_FS_IO_ERROR;
}
#endif /* !IFS_MINIX_FIRMWARE_BUILD */

/* Strict Common V2 per-file mapping. An unsupported sparse file is NOT
 * reported as a fake physical extent. Every returned zone comes from the
 * original Minix parser and its allocation bitmap validation. */
static int map_file(void *volume,const char *path,uint64_t first,
                    IfsFsExtentV2 *out,uint32_t capacity,uint32_t *written)
{
    IfsMinixInode inode;
    uint32_t zones[64];
    uint64_t total,count;
    int rc;
    if(!live(volume)||!path||!out||!written||!capacity||capacity>64u)
        return IFS_FS_INVALID;
    *written=0u;
    last_read_error=IFS_FS_OK;
    rc=resolve(path,&inode,NULL,NULL);
    if(rc)return rc;
    if((inode.mode&0xf000u)!=IFS_MINIX_MODE_REGULAR ||
       !session.media.block_bytes)
        return IFS_FS_UNSUPPORTED;
    total=((uint64_t)inode.size_bytes+session.media.block_bytes-1u)/
          session.media.block_bytes;
    if(first>=total)return IFS_FS_END;
    count=total-first;
    if(count>capacity)count=capacity;
    /* Preflight a whole page before exposing any output. No I/O writes. */
    for(uint64_t i=0u;i<count;++i) {
        uint64_t position;
        if(ifs_minix_data_zone(&session.media,&inode,first+i,&zones[i])!=0)
            return IFS_FS_CORRUPT;
        if(!zones[i])return IFS_FS_UNSUPPORTED;
        position=(uint64_t)zones[i]*session.media.block_bytes;
        if(position>=session.media.volume_bytes ||
           session.media.block_bytes>session.media.volume_bytes-position)
            return IFS_FS_CORRUPT;
    }
    for(uint64_t i=0u;i<count;++i) {
        const uint64_t logical=(first+i)*session.media.block_bytes;
        const uint64_t remaining=(uint64_t)inode.size_bytes-logical;
        out[i].logical_offset=logical;
        out[i].physical_offset=(uint64_t)zones[i]*session.media.block_bytes;
        out[i].length=remaining<session.media.block_bytes?
                      remaining:session.media.block_bytes;
        out[i].flags=0u;
        out[i].reserved=0u;
    }
    *written=(uint32_t)count;
    return IFS_FS_OK;
}
static const IfsFsDefragOpsV2 defrag_map_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsDefragOpsV2),
             IFS_FS_GROUP_DEFRAG,0u,IFS_FS_DEFRAG_MAP},
    .map=map_file
};
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
             IFS_FS_GROUP_CORE,0u,
             IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|
#if defined(IFS_MINIX_FIRMWARE_BUILD)
             IFS_FS_CORE_LIST|IFS_FS_CORE_READ},
#else
             IFS_FS_CORE_LIST|IFS_FS_CORE_READ|IFS_FS_CORE_WRITE},
#endif
    .probe=probe,.mount=mount_volume,.unmount=unmount_volume,
    .stat=stat_path,.directory_entry=directory_entry,
    .read=read_file
#if !defined(IFS_MINIX_FIRMWARE_BUILD)
    ,.write=write_file
#endif
};
#if !defined(IFS_MINIX_FIRMWARE_BUILD)
/* Empty Minix V3 volume, using the same on-disk geometry as the native
 * reader. Formatting is exclusively a provider operation. No pathname,
 * external mkfs, kernel driver or retired ABI participates. */
static void format_le16(uint8_t *p,uint32_t x)
{ p[0]=(uint8_t)x; p[1]=(uint8_t)(x>>8); }
static void format_le32(uint8_t *p,uint32_t x)
{ format_le16(p,x); format_le16(p+2,x>>16); }

static int format_v3(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                     const char *label,uint32_t flags)
{
    if(!host_ok(h))return IFS_FS_INVALID;
    if((h->capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(h->capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(h->capabilities&IFS_FS_HOST_DURABLE))
        return IFS_FS_ACCESS;
    if(session.active)return IFS_FS_BUSY;
    /* Minix has no volume label field. Only V3 creation is implemented;
     * older on-disk variants remain readable. */
    if(variant!=3u || flags || (label && *label))
        return IFS_FS_UNSUPPORTED;
    const uint32_t bs=h->logical_sector_bytes>1024u?
                      h->logical_sector_bytes:1024u;
    if(bs>4096u || h->volume_bytes%bs ||
       h->volume_bytes/bs<64u || h->volume_bytes/bs>UINT32_MAX)
        return IFS_FS_UNSUPPORTED;
    const uint64_t zones=h->volume_bytes/bs;
    const uint32_t bits=bs*8u;
    /* Reserve roughly one inode per four zones; cap metadata so the
     * 16-bit first-data-zone field remains representable. */
    uint32_t inodes=(uint32_t)(zones/4u);
    if(inodes>65520u)inodes=65520u;
    if(inodes<16u)inodes=16u;
    const uint32_t imap=(inodes+1u+bits-1u)/bits;
    const uint32_t itable=(inodes*64u+bs-1u)/bs;
    uint64_t zmap=(zones+bits-1u)/bits;
    const uint64_t base=2u+imap+itable;
    /* Find the smallest bitmap covering the data region, including its
     * reserved bit zero. Its own size reduces the size of that region. */
    while(zmap>1u && base+zmap-1u<zones &&
          (zmap-1u)*bits>=zones-(base+zmap-1u)+1u)
        --zmap;
    const uint64_t first=base+zmap;
    if(!zmap || zmap>UINT16_MAX || first>UINT16_MAX ||
       first>=zones || zmap*bits<zones-first+1u)
        return IFS_FS_UNSUPPORTED;

    uint8_t block[4096]={0}, super[32]={0};
    int rc;
    /* Invalidate old superblock before any metadata mutation. A failure
     * before the final publication leaves no new valid Minix signature. */
    rc=ifs_fs_host_write_bytes_v2(h,0u,block,bs);
    if(rc!=IFS_FS_OK)return rc;
    rc=ifs_fs_host_write_bytes_v2(h,bs,block,bs);
    if(rc!=IFS_FS_OK)return rc;
    if(h->flush(h->context)!=0)return IFS_FS_IO_ERROR;

    /* Bitmap bit zero and root inode/zone bit one are reserved. Bits
     * outside the declared addressable range must never be allocatable. */
    for(uint32_t kind=0u;kind<2u;++kind) {
        const uint64_t count=kind?zmap:imap;
        const uint64_t valid=kind?zones-first+1u:inodes+1u;
        const uint64_t start=kind?2u+imap:2u;
        for(uint64_t i=0u;i<count;++i) {
            memset(block,0,bs);
            if(!i)block[0]=3u;
            for(uint32_t bit=0u;bit<bits;++bit)
                if(i*bits+bit>=valid)
                    block[bit/8u]|=(uint8_t)(1u<<(bit%8u));
            rc=ifs_fs_host_write_bytes_v2(h,(start+i)*bs,block,bs);
            if(rc!=IFS_FS_OK)return rc;
        }
    }
    /* Root inode is the only allocated inode. Its single data zone
     * contains the two required directory entries, "." and "..". */
    for(uint32_t i=0u;i<itable;++i) {
        memset(block,0,bs);
        if(!i) {
            format_le16(block,0040755u);
            format_le16(block+2,2u);
            format_le32(block+8,128u);
            format_le32(block+24,(uint32_t)first);
        }
        rc=ifs_fs_host_write_bytes_v2(h,(2u+imap+zmap+i)*bs,block,bs);
        if(rc!=IFS_FS_OK)return rc;
    }
    memset(block,0,bs);
    format_le32(block,1u);block[4]='.';
    format_le32(block+64,1u);block[68]='.';block[69]='.';
    rc=ifs_fs_host_write_bytes_v2(h,first*bs,block,bs);
    if(rc!=IFS_FS_OK)return rc;
    if(h->flush(h->context)!=0)return IFS_FS_IO_ERROR;
    format_le32(super,inodes);
    format_le16(super+6,imap);
    format_le16(super+8,(uint32_t)zmap);
    format_le16(super+10,(uint32_t)first);
    format_le32(super+16,UINT32_C(0x7fffffff));
    format_le32(super+20,(uint32_t)zones);
    format_le16(super+24,0x4d5au);
    format_le16(super+28,bs);
    rc=ifs_fs_host_write_bytes_v2(h,1024u,super,sizeof(super));
    if(rc!=IFS_FS_OK)return rc;
    if(h->flush(h->context)!=0) {
        memset(super,0,sizeof(super));
        (void)ifs_fs_host_write_bytes_v2(h,1024u,super,sizeof(super));
        (void)h->flush(h->context);
        return IFS_FS_IO_ERROR;
    }
    uint32_t observed=0u;
    rc=probe(h,&observed);
    return rc!=IFS_FS_OK?rc:observed==3u?IFS_FS_OK:IFS_FS_CORRUPT;
}
#endif /* hosted format */

static const IfsFsVolumeOpsV2 volume_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsVolumeOpsV2),
             IFS_FS_GROUP_VOLUME,0u,
             IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_SYNC
#if !defined(IFS_MINIX_FIRMWARE_BUILD)
             |IFS_FS_VOLUME_FORMAT
#endif
    },
    .query=volume_query,.sync=volume_sync
#if !defined(IFS_MINIX_FIRMWARE_BUILD)
    ,.format=format_v3
#endif
};
static const IfsFsGroupHeaderV2 *query_group(
    uint32_t id,uint32_t minimum,uint32_t maximum)
{
    if(minimum>IFS_FS_GROUP_ABI_V2 || maximum<IFS_FS_GROUP_ABI_V2)
        return NULL;
    if(id==IFS_FS_GROUP_CORE)return &core.header;
    if(id==IFS_FS_GROUP_VOLUME)return &volume_ops.header;
    if(id==IFS_FS_GROUP_DEFRAG)return &defrag_map_ops.header;
    return NULL;
}
__attribute__((used,section(".ifsmeta")))
static const char identity[]="IFS-MODULE:minix:common2";
static const IfsFsModuleApiV2 api={
    IFS_FS_COMMON_ABI_V2,sizeof(IfsFsModuleApiV2),"minix",
#if defined(IFS_MINIX_FIRMWARE_BUILD)
    IFS_FS_PROFILE_FREESTANDING|IFS_FS_PROFILE_FIRMWARE,
#else
    IFS_FS_PROFILE_HOSTED,
#endif
    IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)|
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG),query_group
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    return &api;
}
