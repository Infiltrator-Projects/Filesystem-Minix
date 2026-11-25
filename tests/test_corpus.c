/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercise the published hosted module with the same file geometry and
 * two-phase interleaved append sequence as Defragmenter Test Media.
 * An independent checker consumes the resulting image, not a fixture.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <infiltratr/filesystem_module_v2.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
typedef struct {int fd;uint32_t sector;uint64_t size;int fail_after;} Media;
static int rd(void *c,uint64_t off,void *p,uint32_t n)
{
    Media *m=c;
    if(off%m->sector || n%m->sector || n>65536u ||
       off>m->size || n>m->size-off)return -1;
    return pread(m->fd,p,n,(off_t)off)==(ssize_t)n?0:-1;
}
static int wr(void *c,uint64_t off,const void *p,uint32_t n)
{
    Media *m=c;
    if(off%m->sector || n%m->sector || n>65536u ||
       off>m->size || n>m->size-off)return -1;
    if(m->fail_after==0){m->fail_after=-1;return -1;}
    if(m->fail_after>0)--m->fail_after;
    return pwrite(m->fd,p,n,(off_t)off)==(ssize_t)n?0:-1;
}
static int sync_media(void *c){return fsync(((Media*)c)->fd);}
static void *al(void *c,size_t n){(void)c;return malloc(n);}
static void re(void *c,void *p){(void)c;free(p);}
static uint64_t realtime(void *c)
{(void)c;struct timespec t;clock_gettime(CLOCK_REALTIME,&t);return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;}
static uint64_t monotonic(void *c)
{(void)c;struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;}
static void payload(uint8_t *p,uint32_t n,unsigned file,uint64_t at)
{
    for(uint32_t i=0;i<n;++i) {
        uint64_t x=at+i+UINT64_C(0x9e3779b97f4a7c15)*(file+1u);
        x=(x^(x>>30))*UINT64_C(0xbf58476d1ce4e5b9);
        x=(x^(x>>27))*UINT64_C(0x94d049bb133111eb);
        p[i]=(uint8_t)(x^(x>>31));
    }
}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"MINIX corpus check %d: %s\n",__LINE__,#x);return 1;}}while(0)
#define RC(x) do{int rc=(x);if(rc!=IFS_FS_OK){fprintf(stderr,"MINIX corpus line %d: %s returned %d\n",__LINE__,#x,rc);return 1;}}while(0)
int main(int argc,char **argv)
{
    CHECK(argc==4);
    void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);CHECK(lib);
    void *sym=dlsym(lib,IFS_FS_COMMON_EXPORT_V2);CHECK(sym);
    IfsFsModuleApiEntryV2 entry=NULL;memcpy(&entry,&sym,sizeof(entry));
    const IfsFsModuleApiV2 *api=entry();CHECK(ifs_fs_api_v2_valid(api,"minix"));
    const IfsFsCoreOpsV2 *core=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *vol=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    const IfsFsDefragOpsV2 *map=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    CHECK(core && core->create && core->write && core->read && vol && vol->format && map && map->map);
    Media m={.fd=open(argv[2],O_CREAT|O_EXCL|O_NOFOLLOW|O_RDWR,0600),
             .sector=(uint32_t)strtoul(argv[3],NULL,10),
             .size=UINT64_C(2)*1024u*1024u*1024u,.fail_after=-1};
    CHECK(m.fd>=0 && ftruncate(m.fd,(off_t)m.size)==0);
    IfsFsHostRuntimeV2 h={.abi_version=IFS_FS_HOST_ABI_V2,.structure_bytes=sizeof(h),
        .capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE|IFS_FS_HOST_CLOCK,
        .volume_bytes=m.size,.logical_sector_bytes=m.sector,.physical_sector_bytes=m.sector,
        .maximum_transfer_bytes=65536u,.context=&m,.read_at=rd,.write_at=wr,.flush=sync_media,
        .allocate=al,.release=re,.realtime_ns=realtime,.monotonic_ns=monotonic};
    IfsFsHostRuntimeV2 ro=h;ro.capabilities=IFS_FS_HOST_READ_ONLY|IFS_FS_HOST_CLOCK;
    ro.write_at=NULL;ro.flush=NULL;
    RC(vol->format(&h,0u,"LD_MINIX",0u));
    void *v=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    RC(core->mount(&h,&v,&info));CHECK(info.variant==3u);
    const uint32_t bs=info.block_bytes;
    const uint64_t lengths[]={4194304,6291456,10485760,16777216,27262976,35651584,46137344,62914560,0,1,511,512,513,4095,4096,4097};
    char names[16][32];uint8_t data[65536],out[65536];
    /* Inject a write failure during directory creation; the transaction
     * must leave no newly allocated inode or published entry behind. */
    m.fail_after=1;
    CHECK(core->create(v,"/FAILED.BIN",IFS_FS_ENTRY_FILE)!=IFS_FS_OK);
    IfsFsEntryV2 e={.structure_bytes=sizeof(e),.name=names[0],.name_capacity=sizeof(names[0])};
    CHECK(core->stat(v,"/FAILED.BIN",&e)==IFS_FS_NOT_FOUND);
    for(unsigned f=0;f<16;++f) {
        snprintf(names[f],sizeof(names[f]),"/F%06u.BIN",f);
        RC(core->create(v,names[f],IFS_FS_ENTRY_FILE));
    }
    for(unsigned phase=0;phase<2;++phase)for(unsigned f=0;f<16;++f) {
        uint64_t blocks=(lengths[f]+bs-1u)/bs;
        uint64_t half=(blocks>1u?blocks/2u:blocks)*bs;
        if(half>lengths[f])half=lengths[f];
        uint64_t at=phase?half:0,end=phase?lengths[f]:half;
        while(at<end) {
            uint32_t n=(uint32_t)(end-at>sizeof(data)?sizeof(data):end-at);
            payload(data,n,f,at);RC(core->write(v,names[f],at,data,n));at+=n;
        }
    }
    RC(vol->sync(v));RC(core->unmount(v));RC(core->mount(&ro,&v,&info));
    unsigned seen=0;
    for(uint32_t i=0;;++i) {
        char name[128];e=(IfsFsEntryV2){.structure_bytes=sizeof(e),.name=name,.name_capacity=sizeof(name)};
        int rc=core->directory_entry(v,"/",i,&e);
        if(rc==IFS_FS_END)break;
        CHECK(rc==IFS_FS_OK && i<32u);
        for(unsigned f=0;f<16;++f)if(!strcmp(name,names[f]+1)) {
            CHECK(!(seen&(1u<<f)) && e.type==IFS_FS_ENTRY_FILE && e.size_bytes==lengths[f]);
            seen|=1u<<f;
        }
    }
    CHECK(seen==65535u);
    for(unsigned f=0;f<16;++f) {
        for(uint64_t at=0;at<lengths[f];) {
            uint32_t n=(uint32_t)(lengths[f]-at>sizeof(data)?sizeof(data):lengths[f]-at);
            payload(data,n,f,at);RC(core->read(v,names[f],at,out,n));
            CHECK(!memcmp(data,out,n));at+=n;
        }
        if(f<8u) {
            uint64_t half=((lengths[f]+bs-1u)/bs)/2u;
            IfsFsExtentV2 ext[2];uint32_t n=0;
            RC(map->map(v,names[f],half-1u,ext,2u,&n));
            CHECK(n==2u && ext[1].physical_offset!=ext[0].physical_offset+ext[0].length);
        }
    }
    RC(core->unmount(v));close(m.fd);dlclose(lib);
    puts("MINIX: all sixteen files, byte-identical reopen, eight fragmented files, rollback and allocation passed");
    return 0;
}
