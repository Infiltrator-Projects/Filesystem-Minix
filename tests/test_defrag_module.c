/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_minix_defrag_v1.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"failed: %s\n",#x);return 1;}}while(0)
int main(void){
 const IfsMinixDefragV1 *api=infiltrator_filesystem_minix_defrag_v1();
 CHECK(api&&api->abi_version==IFS_MINIX_DEFRAG_ABI_V1);
 CHECK(api->structure_bytes>=sizeof(*api));
 CHECK(api->format_id&&strcmp(api->format_id,"minix")==0);
 CHECK(api->capabilities==(IFS_MINIX_DEFRAG_CAP_RUN|IFS_MINIX_DEFRAG_CAP_FIXTURE));
 CHECK(api->run&&api->fixture);
 char *args[]={"defragmenter-minix-worker","--version",NULL};
 CHECK(api->run(2,args,NULL)!=0);
 return 0;
}
