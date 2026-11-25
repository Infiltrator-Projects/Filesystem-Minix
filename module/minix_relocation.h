/* SPDX-License-Identifier: GPL-3.0-or-later
 * Hosted native Minix V3 relocation. Included by the Common V2 adapter.
 *
 * The formatter owns the first 1 KiB boot area on its non-bootable
 * data volumes. Its explicit journal identity authorises this provider
 * to use the first logical sector there. Never appropriate the boot
 * code of an externally formatted Minix volume. A single-zone redo
 * transaction copies and verifies data before publishing the intent;
 * it then durably allocates the destination, switches exactly one
 * inode/indirect pointer, and releases the old zone. Recovery replays
 * these idempotent steps. No source zone is reused during a pending
 * transaction, and no other mounted mutation may run during a plan.
 */
#define MR_JOURNAL_BYTES 64u
static const uint8_t mr_magic[8]={'I','F','S','M','R','V','2',0};
typedef struct {
    uint32_t inode,logical,old_zone,new_zone;
} MrIntent;
typedef struct {
    uint32_t inode,bytes,count,target,reserve;
} MrFile;
typedef struct {
    uint32_t count,policy;
    int executed;
    MrFile files[];
} MrPlan;
static uintptr_t mr_generation;
static uint32_t mr_checksum(const uint8_t *p)
{
    uint32_t h=UINT32_C(2166136261);
    for(uint32_t i=0;i<MR_JOURNAL_BYTES-4u;++i)
        h=(h^p[i])*UINT32_C(16777619);
    return h;
}
static int mr_read(uint64_t at,void *out,uint32_t n)
{ return ifs_fs_host_read_bytes_v2(&session.host,at,out,n); }
static int mr_write(uint64_t at,const void *data,uint32_t n)
{
    int rc=ifs_fs_host_write_bytes_v2(&session.host,at,data,n);
    return rc!=IFS_FS_OK?rc:session.host.flush(session.host.context)==0?
        IFS_FS_OK:IFS_FS_IO_ERROR;
}
static int mr_writable(void)
{
    const IfsFsHostRuntimeV2 *h=&session.host;
    return !(h->capabilities&IFS_FS_HOST_READ_ONLY) &&
        (h->capabilities&IFS_FS_HOST_EXCLUSIVE) &&
        (h->capabilities&IFS_FS_HOST_DURABLE) && h->write_at && h->flush;
}
static int mr_zone_valid(uint32_t n)
{ return n>=session.media.first_data_zone && n<session.media.zone_count; }
static int mr_cancelled(void)
{ return session.host.cancelled && session.host.cancelled(session.host.context); }
static void mr_encode(uint8_t raw[MR_JOURNAL_BYTES],uint32_t zones,
                      uint32_t bs,const MrIntent *intent)
{
    memset(raw,0,MR_JOURNAL_BYTES);
    memcpy(raw,mr_magic,sizeof mr_magic);
    mut_put32(raw+8,zones);mut_put32(raw+12,bs);
    if(intent) {
        mut_put32(raw+16,1u);mut_put32(raw+20,intent->inode);
        mut_put32(raw+24,intent->logical);mut_put32(raw+28,intent->old_zone);
        mut_put32(raw+32,intent->new_zone);
    }
    mut_put32(raw+60,mr_checksum(raw));
}
static int mr_journal_read(MrIntent *intent,int *state)
{
    uint8_t raw[MR_JOURNAL_BYTES];
    *state=0;
    if(session.media.version!=3u || session.host.logical_sector_bytes>1024u)
        return IFS_FS_OK;
    int rc=mr_read(0,raw,sizeof raw);
    if(rc!=IFS_FS_OK)return rc;
    if(memcmp(raw,mr_magic,sizeof mr_magic))return IFS_FS_OK;
    if(mut_get32(raw+60)!=mr_checksum(raw) ||
       mut_get32(raw+8)!=session.media.zone_count ||
       mut_get32(raw+12)!=session.media.block_bytes ||
       mut_get32(raw+16)>1u)return IFS_FS_CORRUPT;
    *state=mut_get32(raw+16)?2:1;
    if(*state==2) {
        *intent=(MrIntent){mut_get32(raw+20),mut_get32(raw+24),
                           mut_get32(raw+28),mut_get32(raw+32)};
        if(!intent->inode || intent->inode>session.media.inode_count ||
           !mr_zone_valid(intent->old_zone) || !mr_zone_valid(intent->new_zone) ||
           intent->old_zone==intent->new_zone)
            return IFS_FS_CORRUPT;
    }
    return IFS_FS_OK;
}
static int mr_journal_write(const MrIntent *intent)
{
    uint8_t raw[MR_JOURNAL_BYTES];
    mr_encode(raw,session.media.zone_count,session.media.block_bytes,intent);
    int rc=mr_write(0,raw,sizeof raw);
    if(rc==IFS_FS_OK)session.reloc_state=intent?2:1;
    return rc;
}
static int mr_bit(uint32_t zone,int set,int *used)
{
    const IfsMinixVolume *v=&session.media;
    if(!mr_zone_valid(zone))return IFS_FS_CORRUPT;
    const uint32_t bit=zone-v->first_data_zone+1u;
    const uint64_t at=(uint64_t)(2u+v->bitmap_inode_blocks)*v->block_bytes+bit/8u;
    const uint8_t mask=(uint8_t)(1u<<(bit%8u));
    uint8_t value;
    int rc=mr_read(at,&value,1u);
    if(rc!=IFS_FS_OK)return rc;
    *used=(value&mask)!=0u;
    if(set<0 || *used==set)return IFS_FS_OK;
    value=set?(uint8_t)(value|mask):(uint8_t)(value&~mask);
    return mr_write(at,&value,1u);
}
/* Resolve the precise on-disk pointer slot through the current inode
 * tree. Indirect metadata stays pinned: only its final data reference
 * is changed. During recovery the final data bit may be in transition,
 * so validate every intermediate zone, not the final allocation bit. */
static int mr_pointer(uint32_t inode,uint32_t logical,uint64_t *at,uint32_t *zone)
{
    const IfsMinixVolume *v=&session.media;
    IfsMinixInode f;
    if(native()->inode(v,inode,&f)!=0)return IFS_FS_CORRUPT;
    if((f.mode&0xf000u)!=IFS_MINIX_MODE_REGULAR ||
       (uint64_t)logical*v->block_bytes>=f.size_bytes)return IFS_FS_CORRUPT;
    const uint64_t n=v->block_bytes/4u;
    uint64_t k=logical,ix[3]={0};unsigned slot,levels=0;
    if(k<7u)slot=(unsigned)k;
    else if((k-=7u)<n){slot=7u;levels=1u;ix[0]=k;}
    else if((k-=n)<n*n){slot=8u;levels=2u;ix[0]=k/n;ix[1]=k%n;}
    else if((k-=n*n)<n*n*n){
        slot=9u;levels=3u;ix[0]=k/(n*n);ix[1]=(k/n)%n;ix[2]=k%n;
    }else return IFS_FS_CORRUPT;
    *at=(uint64_t)v->inode_table_block*v->block_bytes+
        (uint64_t)(inode-1u)*64u+24u+4u*slot;
    *zone=f.zone[slot];
    for(unsigned i=0;i<levels;++i) {
        int used=0;
        int rc=mr_bit(*zone,-1,&used);
        if(rc!=IFS_FS_OK)return rc;
        if(!used)return IFS_FS_CORRUPT;
        *at=(uint64_t)*zone*v->block_bytes+4u*ix[i];
        uint8_t raw[4];rc=mr_read(*at,raw,sizeof raw);
        if(rc!=IFS_FS_OK)return rc;
        *zone=mut_get32(raw);
    }
    return mr_zone_valid(*zone)?IFS_FS_OK:IFS_FS_CORRUPT;
}
static int mr_compare(uint32_t old_zone,uint32_t new_zone,int copy)
{
    const uint32_t bs=session.media.block_bytes;
    uint8_t *data=session.host.allocate(session.host.context,(size_t)bs*2u);
    if(!data)return IFS_FS_NO_MEMORY;
    int rc=mr_read((uint64_t)old_zone*bs,data,bs);
    if(rc==IFS_FS_OK && copy)rc=mr_write((uint64_t)new_zone*bs,data,bs);
    if(rc==IFS_FS_OK)rc=mr_read((uint64_t)new_zone*bs,data+bs,bs);
    if(rc==IFS_FS_OK && memcmp(data,data+bs,bs))rc=IFS_FS_CORRUPT;
    session.host.release(session.host.context,data);
    return rc;
}
static int mr_replay(const MrIntent *j)
{
    uint64_t at=0;uint32_t current=0;int old_used=0,new_used=0;
    int rc=mr_pointer(j->inode,j->logical,&at,&current);
    if(rc!=IFS_FS_OK)return rc;
    if(current!=j->old_zone && current!=j->new_zone)return IFS_FS_CORRUPT;
    rc=mr_bit(j->old_zone,-1,&old_used);
    if(rc==IFS_FS_OK)rc=mr_bit(j->new_zone,-1,&new_used);
    if(rc!=IFS_FS_OK)return rc;
    if((current==j->old_zone && !old_used) ||
       (current==j->new_zone && !new_used))return IFS_FS_CORRUPT;
    rc=mr_compare(j->old_zone,j->new_zone,0);
    if(rc!=IFS_FS_OK)return rc;
    if(!new_used){rc=mr_bit(j->new_zone,1,&new_used);if(rc!=IFS_FS_OK)return rc;}
    if(current!=j->new_zone) {
        uint8_t raw[4];mut_put32(raw,j->new_zone);
        rc=mr_write(at,raw,sizeof raw);
        if(rc!=IFS_FS_OK)return rc;
    }
    if(old_used){rc=mr_bit(j->old_zone,0,&old_used);if(rc!=IFS_FS_OK)return rc;}
    return mr_journal_write(NULL);
}
static int mr_mount(void)
{
    MrIntent j={0};
    session.reloc_state=0;session.mount_recovered=0u;
    int rc=mr_journal_read(&j,&session.reloc_state);
    if(rc!=IFS_FS_OK)return rc;
    if(session.reloc_state==2) {
        if(!mr_writable())return IFS_FS_ACCESS;
        rc=mr_replay(&j);
        if(rc!=IFS_FS_OK)return rc;
        session.mount_recovered=1u;
        rc=count_free_zones(&session.media,&session.host,&session.verified_free_zones);
    }
    return rc;
}
static void mr_drop_plan(void)
{
    if(session.reloc_plan)session.host.release(session.host.context,session.reloc_plan);
    session.reloc_plan=NULL;session.reloc_token=0u;
}
static int mr_test_bit(const uint8_t *map,uint32_t bit)
{ return (map[bit/8u]&(uint8_t)(1u<<(bit%8u)))!=0; }
static void mr_mark(uint8_t *map,uint32_t bit)
{ map[bit/8u]|=(uint8_t)(1u<<(bit%8u)); }
/* Claim all data AND indirect metadata zones in each allocated inode.
 * A duplicated or unowned allocation makes a relocation plan unsafe.
 * Ownership is independent of pathname, so hard links are counted once. */
static int mr_claim(uint32_t zone,unsigned depth,const uint8_t *allocated,
                    uint8_t *owned)
{
    if(!zone)return IFS_FS_OK;
    if(mr_cancelled())return IFS_FS_BUSY;
    if(!mr_zone_valid(zone))return IFS_FS_CORRUPT;
    uint32_t bit=zone-session.media.first_data_zone+1u;
    if(!mr_test_bit(allocated,bit) || mr_test_bit(owned,bit))return IFS_FS_CORRUPT;
    mr_mark(owned,bit);
    if(!depth)return IFS_FS_OK;
    const uint32_t bs=session.media.block_bytes;
    uint8_t *block=session.host.allocate(session.host.context,bs);
    if(!block)return IFS_FS_NO_MEMORY;
    int rc=mr_read((uint64_t)zone*bs,block,bs);
    for(uint32_t i=0u;rc==IFS_FS_OK && i<bs;i+=4u)
        rc=mr_claim(mut_get32(block+i),depth-1u,allocated,owned);
    session.host.release(session.host.context,block);
    return rc;
}
static int mr_plan(void *volume,const char *path,uint32_t policy,void **out)
{
    if(!out)return IFS_FS_INVALID;
    *out=NULL;
    if(!live(volume) || !path || (policy!=0u && policy!=10u))return IFS_FS_INVALID;
    if(session.reloc_state!=1 || session.write_fault)return IFS_FS_UNSUPPORTED;
    if(session.reloc_plan || mr_generation==UINTPTR_MAX)return IFS_FS_BUSY;
    const IfsMinixVolume *v=&session.media;
    const uint32_t bs=v->block_bytes;
    uint32_t selected=0;
    if(path[0]!='/' || path[1]) {
        IfsMinixInode f;
        int rc=resolve(path,&f,NULL,NULL);
        if(rc!=IFS_FS_OK)return rc;
        if((f.mode&0xf000u)!=IFS_MINIX_MODE_REGULAR)return IFS_FS_UNSUPPORTED;
        selected=f.inode_number;
    }
    const uint32_t ib=(v->inode_count/8u)+1u;
    const uint32_t zb=(v->zone_count-v->first_data_zone)/8u+1u;
    const uint64_t plan_bytes=sizeof(MrPlan)+(uint64_t)v->inode_count*sizeof(MrFile);
    if(plan_bytes>SIZE_MAX)return IFS_FS_NO_MEMORY;
    uint8_t *imap=session.host.allocate(session.host.context,ib);
    uint8_t *zmap=session.host.allocate(session.host.context,zb);
    uint8_t *owned=session.host.allocate(session.host.context,zb);
    MrPlan *p=session.host.allocate(session.host.context,(size_t)plan_bytes);
    int rc=IFS_FS_NO_MEMORY;
    if(!imap || !zmap || !owned || !p)goto done;
    memset(owned,0,zb);p->count=0u;p->policy=policy;p->executed=0;
    rc=mr_read((uint64_t)2u*bs,imap,ib);
    if(rc==IFS_FS_OK)rc=mr_read((uint64_t)(2u+v->bitmap_inode_blocks)*bs,zmap,zb);
    if(rc!=IFS_FS_OK)goto done;
    for(uint32_t ino=1u;ino<=v->inode_count;++ino) {
        if(mr_cancelled()){rc=IFS_FS_BUSY;goto done;}
        if(!mr_test_bit(imap,ino))continue;
        IfsMinixInode f;
        if(native()->inode(v,ino,&f)!=0){rc=IFS_FS_CORRUPT;goto done;}
        const uint16_t mode=f.mode&0xf000u;
        if(mode!=IFS_MINIX_MODE_REGULAR && mode!=IFS_MINIX_MODE_DIRECTORY &&
           mode!=IFS_MINIX_MODE_SYMLINK) {
            if(f.size_bytes){rc=IFS_FS_UNSUPPORTED;goto done;}
            continue;
        }
        for(unsigned i=0;i<10u;++i) {
            rc=mr_claim(f.zone[i],i<7u?0u:i-6u,zmap,owned);
            if(rc!=IFS_FS_OK)goto done;
        }
        if(mode!=IFS_MINIX_MODE_REGULAR || !f.size_bytes || (selected && ino!=selected))
            continue;
        const uint32_t count=(uint32_t)(((uint64_t)f.size_bytes+bs-1u)/bs);
        uint32_t prev=0u;int fragmented=0;
        for(uint32_t i=0u;i<count;++i) {
            uint32_t z=0;
            if(ifs_minix_data_zone(v,&f,i,&z)!=0){rc=IFS_FS_CORRUPT;goto done;}
            if(!z){rc=IFS_FS_UNSUPPORTED;goto done;}
            if(i && z!=prev+1u)fragmented=1;
            prev=z;
        }
        if(!fragmented && !policy)continue;
        p->files[p->count++]=(MrFile){ino,f.size_bytes,count,0u,
                                      policy?(count+9u)/10u:0u};
    }
    for(uint32_t zone=v->first_data_zone;zone<v->zone_count;++zone) {
        const uint32_t bit=zone-v->first_data_zone+1u;
        if(mr_test_bit(zmap,bit)!=mr_test_bit(owned,bit)){rc=IFS_FS_CORRUPT;goto done;}
    }
    /* Reserve independent non-overlapping target runs in the planning
     * bitmap, including growth slack. No allocation bits are written by
     * planning. Slack must remain genuinely unallocated after execution. */
    uint32_t cursor=v->first_data_zone;
    for(uint32_t i=0u;i<p->count;++i) {
        MrFile *f=&p->files[i];
        const uint64_t need=(uint64_t)f->count+f->reserve;
        uint32_t run=0u;
        for(;cursor<v->zone_count;++cursor) {
            if(mr_cancelled()){rc=IFS_FS_BUSY;goto done;}
            if(mr_test_bit(zmap,cursor-v->first_data_zone+1u))run=0u;
            else if(++run>=need) {
                f->target=cursor-run+1u;
                ++cursor;break;
            }
        }
        if(!f->target){rc=IFS_FS_NO_SPACE;goto done;}
    }
    session.reloc_plan=p;session.reloc_token=++mr_generation;
    *out=(void *)session.reloc_token;p=NULL;rc=IFS_FS_OK;
done:
    if(imap)session.host.release(session.host.context,imap);
    if(zmap)session.host.release(session.host.context,zmap);
    if(owned)session.host.release(session.host.context,owned);
    if(p)session.host.release(session.host.context,p);
    return rc;
}
static MrPlan *mr_get(void *volume,void *token)
{
    return live(volume) && token && (uintptr_t)token==session.reloc_token?
        session.reloc_plan:NULL;
}
static int mr_execute(void *volume,void *token,uint64_t *moved)
{
    if(!moved)return IFS_FS_INVALID;
    *moved=0u;
    MrPlan *p=mr_get(volume,token);
    if(!p || p->executed)return IFS_FS_INVALID;
    if(!mr_writable())return IFS_FS_ACCESS;
    if(session.reloc_state!=1 || session.write_fault)return IFS_FS_IO_ERROR;
    for(uint32_t i=0u;i<p->count;++i) {
        const MrFile *f=&p->files[i];
        for(uint32_t k=0u;k<f->count;++k) {
            if(mr_cancelled())return IFS_FS_BUSY;
            uint64_t at;uint32_t old;int old_used=0,new_used=0;
            int rc=mr_pointer(f->inode,k,&at,&old);
            if(rc!=IFS_FS_OK)return rc;
            MrIntent j={f->inode,k,old,f->target+k};
            rc=mr_bit(old,-1,&old_used);
            if(rc==IFS_FS_OK)rc=mr_bit(j.new_zone,-1,&new_used);
            if(rc!=IFS_FS_OK)return rc;
            if(!old_used || new_used || old==j.new_zone)return IFS_FS_CORRUPT;
            rc=mr_compare(old,j.new_zone,1);
            if(rc!=IFS_FS_OK)return rc;
            /* If journal publication itself fails, its state is uncertain.
             * Poison further mutations until the record is re-read. */
            session.reloc_state=2;
            rc=mr_journal_write(&j);
            if(rc==IFS_FS_OK)rc=mr_replay(&j);
            if(rc!=IFS_FS_OK){session.write_fault=1;return rc;}
            const uint64_t left=(uint64_t)f->bytes-(uint64_t)k*session.media.block_bytes;
            *moved+=left<session.media.block_bytes?left:session.media.block_bytes;
        }
    }
    p->executed=1;
    return count_free_zones(&session.media,&session.host,&session.verified_free_zones);
}
static int mr_verify(void *volume,void *token)
{
    MrPlan *p=mr_get(volume,token);
    if(!p || !p->executed || session.reloc_state!=1 || session.write_fault)
        return IFS_FS_INVALID;
    for(uint32_t i=0u;i<p->count;++i) {
        const MrFile *f=&p->files[i];
        IfsMinixInode inode;
        if(native()->inode(&session.media,f->inode,&inode)!=0 ||
           inode.size_bytes!=f->bytes)return IFS_FS_CORRUPT;
        for(uint32_t k=0u;k<f->count;++k) {
            uint32_t zone=0;
            if(ifs_minix_data_zone(&session.media,&inode,k,&zone)!=0 ||
               zone!=f->target+k)return IFS_FS_CORRUPT;
        }
        for(uint32_t k=0u;k<f->reserve;++k) {
            int used=0;
            int rc=mr_bit(f->target+f->count+k,-1,&used);
            if(rc!=IFS_FS_OK)return rc;
            if(used)return IFS_FS_CORRUPT;
        }
    }
    return IFS_FS_OK;
}
static int mr_recover(void *volume,uint32_t flags,uint64_t *repaired)
{
    if(!repaired)return IFS_FS_INVALID;
    *repaired=0u;
    if(!live(volume) || flags)return IFS_FS_INVALID;
    if(!mr_writable())return IFS_FS_ACCESS;
    if(session.reloc_plan)return IFS_FS_BUSY;
    MrIntent j={0};int state=0;
    int rc=mr_journal_read(&j,&state);
    if(rc!=IFS_FS_OK)return rc;
    if(!state)return IFS_FS_UNSUPPORTED;
    if(state==2) {
        rc=mr_replay(&j);
        if(rc!=IFS_FS_OK)return rc;
        ++session.mount_recovered;
    }
    session.reloc_state=1;session.write_fault=0;
    rc=count_free_zones(&session.media,&session.host,&session.verified_free_zones);
    if(rc!=IFS_FS_OK)return rc;
    *repaired=session.mount_recovered;session.mount_recovered=0u;
    return IFS_FS_OK;
}
static int guarded_mr_plan(void *v,const char *path,uint32_t policy,void **p)
{
    if(p)*p=NULL;
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(mr_plan(v,path,policy,p));
}
static int guarded_mr_execute(void *v,void *p,uint64_t *moved)
{
    if(moved)*moved=0u;
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(mr_execute(v,p,moved));
}
static int guarded_mr_verify(void *v,void *p)
{
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(mr_verify(v,p));
}
static int guarded_mr_recover(void *v,uint32_t flags,uint64_t *repaired)
{
    if(repaired)*repaired=0u;
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(mr_recover(v,flags,repaired));
}
static void guarded_mr_release(void *v,void *p)
{
    if(!v2_enter_operation())return;
    if(mr_get(v,p))mr_drop_plan();
    (void)v2_leave_operation(IFS_FS_OK);
}
