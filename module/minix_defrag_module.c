/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_minix_defrag_v1.h"
#include "ifs_minix_fixture_v1.h"
int ifs_minix_worker_main(int,char **);
int ifs_minix_fixture_run(const void *,const char *,int,const void *);
int ifs_bind_services(const IfsDefragServicesV1 *);
void ifs_unbind_services(void);
static int run(int argc,char **argv,const IfsDefragServicesV1 *s){
 if(ifs_bind_services(s)!=0)return 1;
 int result=ifs_minix_worker_main(argc,argv);
 ifs_unbind_services();return result;
}
static int fixture(const void *spec,const char *path,int create,
 const IfsDefragServicesV1 *s,const IfsMinixFixtureHostV1 *h){
 if(ifs_bind_services(s)!=0)return -1;
 int result=ifs_minix_fixture_run(spec,path,create,h);
 ifs_unbind_services();return result;
}
static const IfsMinixDefragV1 api={IFS_MINIX_DEFRAG_ABI_V1,sizeof(IfsMinixDefragV1),
 "minix",IFS_MINIX_DEFRAG_CAP_RUN|IFS_MINIX_DEFRAG_CAP_FIXTURE,run,fixture};
const IfsMinixDefragV1 *infiltrator_filesystem_minix_defrag_v1(void){return &api;}
