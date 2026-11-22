#include "nvn/vulkan/vk.h"
#include "nvn/rqueue.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
int main(void){
 TexBackend b={.format=VK_FORMAT_R8G8B8A8_UNORM};NvnTexture t={.backend=&b};
 unsigned char *a=malloc(64);memset(a,0x63,64);
 backend_upload_now(&t,RQ_UPLOAD_RGBA,a,4,4,16,0);
 assert(b.pending_rgba==a && !b.pending_nv12 && b.pending_stride==16 && b.gpu_owned);
 assert(((unsigned char*)b.pending_rgba)[63]==0x63);
 unsigned char *uv=malloc(24);memset(uv,0x79,24);
 backend_upload_now(&t,RQ_UPLOAD_NV12,uv,4,4,4,16);
 assert(b.pending_rgba==uv && b.pending_nv12 && b.pending_uv_off==16);
 assert(((unsigned char*)b.pending_rgba)[23]==0x79);
 b.format=VK_FORMAT_D32_SFLOAT;
 backend_upload_now(&t,RQ_UPLOAD_NV12,malloc(24),4,4,4,16);
 assert(b.pending_rgba==uv);
 t.backend=NULL;backend_upload_now(&t,RQ_UPLOAD_RGBA,malloc(64),4,4,16,0);
 free(b.pending_rgba);puts("PASS: queued upload ownership, replacement, invalid format and missing texture");
}
