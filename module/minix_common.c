/* SPDX-License-Identifier: GPL-3.0-or-later
 * Strict Common V2 Minix filesystem over the canonical V1/V2/V3
 * ON-DISK parser and bounded existing-data writer. Disk format versions
 * are independent of the Common filesystem ABI.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdatomic.h>

/* Protect shared native session state against concurrent and reentrant
 * operations. Never wait while a host callback is executing: report BUSY
 * instead of risking deadlock or teardown of a live volume. This does not
 * qualify parallel I/O across volumes. */
static atomic_flag v2_operation_gate=ATOMIC_FLAG_INIT;
static int v2_enter_operation(void)
{
    return !atomic_flag_test_and_set_explicit(&v2_operation_gate,memory_order_acquire);
}
static int v2_leave_operation(int rc)
{
    atomic_flag_clear_explicit(&v2_operation_gate,memory_order_release);
    return rc;
}

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
    uint32_t allocation_hint;
    int write_fault;
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
static int guarded_probe(const IfsFsHostRuntimeV2 *h,uint32_t *variant)
{
    if (variant) *variant=0u;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(probe(h,variant));
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
    session.allocation_hint=session.media.first_data_zone+1u;
    session.write_fault=0;
    session.active=++session.generation;
    describe(info);
    *out=(void *)session.active;
    return IFS_FS_OK;
}
static int guarded_mount_volume(const IfsFsHostRuntimeV2 *h,void **out,
                        IfsFsVolumeInfoV2 *info)
{
    if (out) *out=NULL;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(mount_volume(h,out,info));
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
static int guarded_unmount_volume(void *volume)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(unmount_volume(volume));
}

static int volume_query(void *volume,IfsFsVolumeInfoV2 *info)
{
    if(!live(volume)||!info||info->structure_bytes<sizeof(*info))
        return IFS_FS_INVALID;
    describe(info);
    return IFS_FS_OK;
}
static int guarded_volume_query(void *volume,IfsFsVolumeInfoV2 *info)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(volume_query(volume,info));
}

static int volume_sync(void *volume)
{
    if(!live(volume))return IFS_FS_INVALID;
    if(session.host.capabilities & IFS_FS_HOST_READ_ONLY)return IFS_FS_OK;
    return host_flush(&session.host)==0?IFS_FS_OK:IFS_FS_IO_ERROR;
}
static int guarded_volume_sync(void *volume)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(volume_sync(volume));
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
    out->name_bytes=0u;
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
static int guarded_stat_path(void *volume,const char *path,IfsFsEntryV2 *out)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(stat_path(volume,path,out));
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
static int guarded_directory_entry(void *volume,const char *path,uint32_t index,
                           IfsFsEntryV2 *out)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(directory_entry(volume,path,index,out));
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
static int guarded_read_file(void *volume,const char *path,uint64_t at,
                     void *data,uint32_t length)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(read_file(volume,path,at,data,length));
}

#if !defined(IFS_MINIX_FIRMWARE_BUILD)
/* V3 non-journalled allocation transaction. Stage complete filesystem
 * blocks, including their original contents, before touching the medium.
 * Ordinary I/O failures roll back all attempted writes; a failed rollback
 * poisons the mounted writer. This is not a power-loss journal. Allocation
 * deliberately continues at the next free zone, so interleaved appends
 * create genuine noncontiguous extents without manufacturing fake maps. */
enum { MUT_PATCH_LIMIT=192, MUT_APPEND_LIMIT=65536 };
typedef struct {
    uint32_t block;
    uint8_t *before,*after;
    int dirty;
} MutPatch;
typedef struct {
    MutPatch patch[MUT_PATCH_LIMIT];
    unsigned count;
    uint32_t hint;
    uint32_t allocated;
} Mutation;
static uint32_t mut_get32(const uint8_t *p)
{
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|
           ((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void mut_put16(uint8_t *p,uint32_t x)
{p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);}
static void mut_put32(uint8_t *p,uint32_t x)
{mut_put16(p,x);mut_put16(p+2,x>>16);}
static uint32_t mut_blocksize(void){return session.media.block_bytes;}
static int mut_allowed(void *volume)
{
    if(!live(volume))return IFS_FS_INVALID;
    if((session.host.capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(session.host.capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(session.host.capabilities&IFS_FS_HOST_DURABLE))return IFS_FS_ACCESS;
    if(session.write_fault)return IFS_FS_IO_ERROR;
    if(session.media.version!=3u)return IFS_FS_UNSUPPORTED;
    return IFS_FS_OK;
}
static void mut_release(Mutation *t)
{
    for(unsigned i=0;i<t->count;++i)
        session.host.release(session.host.context,t->patch[i].before);
}
static int mut_patch(Mutation *t,uint32_t block,MutPatch **out)
{
    if(block>=session.media.zone_count)return IFS_FS_CORRUPT;
    for(unsigned i=0;i<t->count;++i)
        if(t->patch[i].block==block){*out=&t->patch[i];return IFS_FS_OK;}
    if(t->count>=MUT_PATCH_LIMIT)return IFS_FS_NO_MEMORY;
    const uint32_t bs=mut_blocksize();
    uint8_t *mem=session.host.allocate(session.host.context,2u*bs);
    if(!mem)return IFS_FS_NO_MEMORY;
    int rc=ifs_fs_host_read_bytes_v2(&session.host,(uint64_t)block*bs,mem,bs);
    if(rc!=IFS_FS_OK){session.host.release(session.host.context,mem);return rc;}
    memcpy(mem+bs,mem,bs);
    MutPatch *p=&t->patch[t->count++];
    *p=(MutPatch){.block=block,.before=mem,.after=mem+bs};
    *out=p;return IFS_FS_OK;
}
static int mut_commit(Mutation *t)
{
    const IfsFsHostRuntimeV2 *h=&session.host;
    const uint32_t bs=mut_blocksize();
    unsigned attempted=0;
    int rc=IFS_FS_OK;
    if(t->allocated>session.verified_free_zones)return IFS_FS_CORRUPT;
    for(unsigned i=0;i<t->count;++i) {
        if(!t->patch[i].dirty)continue;
        if(h->cancelled && h->cancelled(h->context)){rc=IFS_FS_BUSY;break;}
        attempted=i+1u;
        rc=ifs_fs_host_write_bytes_v2(h,(uint64_t)t->patch[i].block*bs,
                                      t->patch[i].after,bs);
        if(rc!=IFS_FS_OK)break;
    }
    if(rc==IFS_FS_OK && h->flush(h->context)!=0)rc=IFS_FS_IO_ERROR;
    if(rc!=IFS_FS_OK) {
        int failed=0;
        while(attempted) {
            MutPatch *p=&t->patch[--attempted];
            if(p->dirty && ifs_fs_host_write_bytes_v2(h,(uint64_t)p->block*bs,
                                      p->before,bs)!=IFS_FS_OK)failed=1;
        }
        if(h->flush(h->context)!=0)failed=1;
        if(failed)session.write_fault=1;
        return rc;
    }
    session.verified_free_zones-=t->allocated;
    session.allocation_hint=t->hint;
    return IFS_FS_OK;
}
/* The two allocation maps have distinct origins. For zmap, bit one
 * represents the first data zone; inode bit zero is also reserved. */
static int mut_bit(Mutation *t,int zone,uint32_t bit,int set,int *allocated)
{
    const IfsMinixVolume *v=&session.media;
    const uint32_t bits=v->block_bytes*8u;
    const uint32_t count=zone?v->bitmap_zone_blocks:v->bitmap_inode_blocks;
    const uint32_t base=2u+(zone?v->bitmap_inode_blocks:0u);
    if(bit/bits>=count)return IFS_FS_CORRUPT;
    MutPatch *p;
    int rc=mut_patch(t,base+bit/bits,&p);
    if(rc!=IFS_FS_OK)return rc;
    uint32_t local=bit%bits;
    uint8_t mask=(uint8_t)(1u<<(local%8u));
    *allocated=(p->after[local/8u]&mask)!=0u;
    if(set && !*allocated){p->after[local/8u]|=mask;p->dirty=1;}
    return IFS_FS_OK;
}
static int mut_alloc(Mutation *t,int zone,uint32_t *out)
{
    const IfsMinixVolume *v=&session.media;
    const uint32_t lo=zone?v->first_data_zone:1u;
    const uint32_t hi=zone?v->zone_count-1u:v->inode_count;
    uint32_t start=zone?t->hint:2u;
    if(start<lo || start>hi)start=lo;
    uint32_t n=start;
    do {
        if(session.host.cancelled && session.host.cancelled(session.host.context))
            return IFS_FS_BUSY;
        int used=0;
        int rc=mut_bit(t,zone,zone?n-lo+1u:n,1,&used);
        if(rc!=IFS_FS_OK)return rc;
        if(!used) {
            *out=n;
            if(zone) {
                if(t->allocated>=session.verified_free_zones)return IFS_FS_CORRUPT;
                ++t->allocated;
                t->hint=n==hi?lo:n+1u;
                MutPatch *p;
                rc=mut_patch(t,n,&p);
                if(rc!=IFS_FS_OK)return rc;
                memset(p->after,0,v->block_bytes);p->dirty=1;
            }
            return IFS_FS_OK;
        }
        n=n==hi?lo:n+1u;
    }while(n!=start);
    return IFS_FS_NO_SPACE;
}
static int mut_inode(Mutation *t,uint32_t number,MutPatch **p,uint8_t **raw)
{
    const IfsMinixVolume *v=&session.media;
    if(!number || number>v->inode_count)return IFS_FS_CORRUPT;
    uint64_t at=(uint64_t)(number-1u)*64u;
    int rc=mut_patch(t,v->inode_table_block+(uint32_t)(at/v->block_bytes),p);
    if(rc==IFS_FS_OK)*raw=(*p)->after+at%v->block_bytes;
    return rc;
}
static int mut_zone(Mutation *t,uint8_t *inode,uint64_t logical,
                    int allocate,uint32_t *out)
{
    const uint64_t n=mut_blocksize()/4u;
    unsigned levels=0;uint64_t indexes[3]={0};unsigned slot;
    if(logical<7u)slot=(unsigned)logical;
    else if((logical-=7u)<n){slot=7u;levels=1u;indexes[0]=logical;}
    else if((logical-=n)<n*n){slot=8u;levels=2u;indexes[0]=logical/n;indexes[1]=logical%n;}
    else if((logical-=n*n)<n*n*n) {
        slot=9u;levels=3u;indexes[0]=logical/(n*n);
        indexes[1]=(logical/n)%n;indexes[2]=logical%n;
    } else return IFS_FS_NO_SPACE;
    uint8_t *ptr=inode+24u+4u*slot;
    MutPatch *owner=NULL;
    for(unsigned level=0;;++level) {
        uint32_t value=mut_get32(ptr);
        if(!value) {
            if(!allocate){*out=0;return IFS_FS_OK;}
            int rc=mut_alloc(t,1,&value);
            if(rc!=IFS_FS_OK)return rc;
            mut_put32(ptr,value);
            if(owner)owner->dirty=1;
        } else {
            if(value<session.media.first_data_zone || value>=session.media.zone_count)
                return IFS_FS_CORRUPT;
            int used=0;
            int rc=mut_bit(t,1,value-session.media.first_data_zone+1u,0,&used);
            if(rc!=IFS_FS_OK)return rc;
            if(!used)return IFS_FS_CORRUPT;
        }
        if(level==levels){*out=value;return IFS_FS_OK;}
        int rc=mut_patch(t,value,&owner);
        if(rc!=IFS_FS_OK)return rc;
        ptr=owner->after+4u*indexes[level];
    }
}
static void mut_time(uint8_t *raw)
{
    uint64_t now=session.host.realtime_ns?
        session.host.realtime_ns(session.host.context)/UINT64_C(1000000000):0;
    if(now>UINT32_MAX)now=UINT32_MAX;
    mut_put32(raw+16u,(uint32_t)now);
    mut_put32(raw+20u,(uint32_t)now);
}
/* Append into ordinary existing directories, including growing the
 * directory beyond its first zone. Directory records and inode bitmap
 * changes become visible only after the complete staged operation. */
static int create_file(void *volume,const char *path,uint32_t type)
{
    int rc=mut_allowed(volume);
    if(rc!=IFS_FS_OK)return rc;
    if(type!=IFS_FS_ENTRY_FILE)return IFS_FS_UNSUPPORTED;
    if(!path || *path!='/' || !path[1])return IFS_FS_INVALID;
    size_t len=strnlen(path,4096u);
    if(len>=4096u)return IFS_FS_INVALID;
    const char *name=strrchr(path,'/')+1;
    size_t count=strlen(name);
    if(!count || count>session.media.name_bytes ||
       !strcmp(name,".") || !strcmp(name,".."))return IFS_FS_INVALID;
    char parent[4096];
    size_t prefix=(size_t)(name-path)-1u;
    if(!prefix){parent[0]='/';parent[1]=0;}
    else {memcpy(parent,path,prefix);parent[prefix]=0;}
    IfsMinixInode dir;
    last_read_error=IFS_FS_OK;
    rc=resolve(parent,&dir,NULL,NULL);
    if(rc!=IFS_FS_OK)return rc;
    if((dir.mode&0xf000u)!=IFS_MINIX_MODE_DIRECTORY || dir.size_bytes%64u)
        return IFS_FS_INVALID;
    IfsMinixDirent existing;
    int found=native()->lookup(&session.media,&dir,name,(uint32_t)count,&existing);
    if(found==0)return IFS_FS_EXISTS;
    if(found!=1)return last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;
    Mutation *t=session.host.allocate(session.host.context,sizeof(*t));
    if(!t)return IFS_FS_NO_MEMORY;
    memset(t,0,sizeof(*t));t->hint=session.allocation_hint;
    MutPatch *dp,*fp,*data;
    uint8_t *d,*f;
    uint32_t ino,zone;uint64_t at=dir.size_bytes;
    uint8_t record[64];
    for(uint64_t i=0;i<dir.size_bytes;i+=64u) {
        if(native()->read(&session.media,&dir,i,record,sizeof(record))!=0) {
            rc=last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_CORRUPT;goto done;
        }
        if(!mut_get32(record)){at=i;break;}
    }
    if(at>session.media.max_file_bytes-64u){rc=IFS_FS_NO_SPACE;goto done;}
    rc=mut_alloc(t,0,&ino);if(rc!=IFS_FS_OK)goto done;
    rc=mut_inode(t,ino,&fp,&f);if(rc!=IFS_FS_OK)goto done;
    memset(f,0,64);mut_put16(f,0100644u);mut_put16(f+2,1u);
    mut_time(f);memcpy(f+12,f+16,4);fp->dirty=1;
    rc=mut_inode(t,dir.inode_number,&dp,&d);if(rc!=IFS_FS_OK)goto done;
    rc=mut_zone(t,d,at/mut_blocksize(),1,&zone);if(rc!=IFS_FS_OK)goto done;
    rc=mut_patch(t,zone,&data);if(rc!=IFS_FS_OK)goto done;
    uint8_t *entry=data->after+at%mut_blocksize();
    memset(entry,0,64);mut_put32(entry,ino);memcpy(entry+4,name,count);
    data->dirty=1;
    if(at==dir.size_bytes)mut_put32(d+8,(uint32_t)(at+64u));
    mut_time(d);dp->dirty=1;
    rc=mut_commit(t);
done:
    mut_release(t);session.host.release(session.host.context,t);return rc;
}
static int guarded_create_file(void *volume,const char *path,uint32_t type)
{
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(create_file(volume,path,type));
}
/* Extend a V3 regular file without holes. Direct, single, double and
 * triple indirect pointers are allocated through the same bitmap.
 * All bytes beyond the previous end of the file are zeroed before
 * publication. Larger requests may be split by the caller. */
static int grow_file(void *volume,const IfsMinixInode *inode,uint64_t at,
                     const void *data,uint32_t length)
{
    int rc=mut_allowed(volume);
    if(rc!=IFS_FS_OK)return rc;
    if(at>inode->size_bytes || length>MUT_APPEND_LIMIT ||
       at>session.media.max_file_bytes ||
       length>session.media.max_file_bytes-at)return IFS_FS_UNSUPPORTED;
    Mutation *t=session.host.allocate(session.host.context,sizeof(*t));
    if(!t)return IFS_FS_NO_MEMORY;
    memset(t,0,sizeof(*t));t->hint=session.allocation_hint;
    MutPatch *ip;uint8_t *raw;
    rc=mut_inode(t,inode->inode_number,&ip,&raw);
    if(rc!=IFS_FS_OK)goto done;
    const uint8_t *source=data;
    const uint64_t end=at+length;
    uint32_t bs=mut_blocksize();
    while(length) {
        uint32_t zone=0,within=(uint32_t)(at%bs),take=bs-within;
        if(take>length)take=length;
        rc=mut_zone(t,raw,at/bs,1,&zone);if(rc!=IFS_FS_OK)goto done;
        MutPatch *p;rc=mut_patch(t,zone,&p);if(rc!=IFS_FS_OK)goto done;
        if(at<inode->size_bytes && inode->size_bytes-at<take &&
           within+(inode->size_bytes-at)<bs)
            memset(p->after+within+(inode->size_bytes-at),0,
                   bs-within-(size_t)(inode->size_bytes-at));
        else if(at==inode->size_bytes && within)
            memset(p->after+within,0,bs-within);
        memcpy(p->after+within,source,take);p->dirty=1;
        at+=take;source+=take;length-=take;
    }
    mut_put32(raw+8,(uint32_t)end);mut_time(raw);ip->dirty=1;
    rc=mut_commit(t);
done:
    mut_release(t);session.host.release(session.host.context,t);return rc;
}

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
    /* Read errors belong to the current operation, not the previously
     * failed request on this mounted volume. */
    last_read_error=IFS_FS_OK;
    rc=resolve(path,&inode,NULL,NULL);
    if(rc)return rc;
    if((inode.mode&0xf000u)!=IFS_MINIX_MODE_REGULAR)
        return IFS_FS_INVALID;
    if(session.write_fault)return IFS_FS_IO_ERROR;
    if(at>inode.size_bytes)return IFS_FS_UNSUPPORTED;
    if(length>inode.size_bytes-at)
        return grow_file(volume,&inode,at,data,length);
    io.read=minix_io(&session.host);
    io.write_at=host_write;
    io.flush=host_flush;
    return ifs_minix_native_write_ops()->overwrite_existing(
        &io,&session.media,&inode,at,data,length)==0?
        IFS_FS_OK:
        last_read_error!=IFS_FS_OK?last_read_error:IFS_FS_IO_ERROR;
}
static int guarded_write_file(void *volume,const char *path,uint64_t at,
                      const void *data,uint32_t length)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(write_file(volume,path,at,data,length));
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
static int guarded_map_file(void *volume,const char *path,uint64_t first,
                    IfsFsExtentV2 *out,uint32_t capacity,uint32_t *written)
{
    if (written) *written=0u;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(map_file(volume,path,first,out,capacity,written));
}

static const IfsFsDefragOpsV2 defrag_map_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsDefragOpsV2),
             IFS_FS_GROUP_DEFRAG,0u,IFS_FS_DEFRAG_MAP},
    .map=guarded_map_file
};
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
             IFS_FS_GROUP_CORE,0u,
             IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|
#if defined(IFS_MINIX_FIRMWARE_BUILD)
             IFS_FS_CORE_LIST|IFS_FS_CORE_READ},
#else
             IFS_FS_CORE_LIST|IFS_FS_CORE_READ|IFS_FS_CORE_WRITE|IFS_FS_CORE_CREATE},
#endif
    .probe=guarded_probe,.mount=guarded_mount_volume,.unmount=guarded_unmount_volume,
    .stat=guarded_stat_path,.directory_entry=guarded_directory_entry,
    .read=guarded_read_file
#if !defined(IFS_MINIX_FIRMWARE_BUILD)
    ,.write=guarded_write_file,.create=guarded_create_file
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
    /* Variant zero selects the native V3 default. A caller-supplied
     * descriptive label on that default profile is ignored: Minix has no
     * on-media label field. Explicit variant 3 retains its strict contract. */
    if((variant!=0u && variant!=3u) || flags ||
       (variant==3u && label && *label))return IFS_FS_UNSUPPORTED;
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
static int guarded_format_v3(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                     const char *label,uint32_t flags)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(format_v3(h,variant,label,flags));
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
    .query=guarded_volume_query,.sync=guarded_volume_sync
#if !defined(IFS_MINIX_FIRMWARE_BUILD)
    ,.format=guarded_format_v3
#endif
};
/* Report real native ownership and mode, never synthetic POSIX defaults.
 * Minix disk versions differ in ownership width and have no birth time. */
static int metadata_get(void *volume,const char *path,IfsFsMetadataV2 *out)
{
    if (!out || out->structure_bytes<sizeof(*out)) return IFS_FS_INVALID;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    if (!live(volume)) return v2_leave_operation(IFS_FS_INVALID);
    last_read_error=IFS_FS_OK;
    IfsMinixInode inode;
    int rc=resolve(path,&inode,NULL,NULL);
    if (rc==IFS_FS_OK)
        *out=(IfsFsMetadataV2){.structure_bytes=sizeof(*out),
            .mask=IFS_FS_META_GET_MODE,.mode=inode.mode,
            .owner=inode.uid,.group=inode.gid};
    return v2_leave_operation(rc);
}
static const IfsFsMetadataOpsV2 metadata_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(metadata_ops),
             IFS_FS_GROUP_METADATA,0u,IFS_FS_META_GET_MODE},
    .get=metadata_get
};

static const IfsFsGroupHeaderV2 *query_group(
    uint32_t id,uint32_t minimum,uint32_t maximum)
{
    if(minimum>IFS_FS_GROUP_ABI_V2 || maximum<IFS_FS_GROUP_ABI_V2)
        return NULL;
    if(id==IFS_FS_GROUP_CORE)return &core.header;
    if(id==IFS_FS_GROUP_VOLUME)return &volume_ops.header;
    if(id==IFS_FS_GROUP_DEFRAG)return &defrag_map_ops.header;
    if(id==IFS_FS_GROUP_METADATA)return &metadata_ops.header;
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
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG)|
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_METADATA),query_group
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    return &api;
}
