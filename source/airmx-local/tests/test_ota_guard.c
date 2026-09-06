#include "ota_guard.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv){assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);unsigned char b[A3_OTA_PREFIX],copy[A3_OTA_PREFIX];assert(fread(b,1,sizeof(b),f)==sizeof(b));fclose(f);assert(ota_prefix_valid(b,sizeof(b),size,0x1e0000));memcpy(copy,b,sizeof(b));
 int positions[]={0,12,13,23,32,80};for(unsigned i=0;i<sizeof(positions)/sizeof(positions[0]);i++){memcpy(b,copy,sizeof(b));b[positions[i]]^=0x80;assert(!ota_prefix_valid(b,sizeof(b),size,0x1e0000));}
 memcpy(b,copy,sizeof(b));assert(!ota_prefix_valid(b,sizeof(b)-1,size,0x1e0000));assert(!ota_prefix_valid(b,sizeof(b),4194304,0x1e0000));assert(!ota_prefix_valid(b,sizeof(b),100,0x1e0000));assert(!ota_prefix_valid(NULL,sizeof(b),size,0x1e0000));puts("PASS: real image accepted; wrong chip/project/magic/hash flag, truncated and oversized images rejected");}
