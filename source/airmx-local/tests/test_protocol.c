#include "protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned frames;static char got[FRAME_CAP];
static void receive(const char *s,void *ctx){(void)ctx;frames++;snprintf(got,sizeof(got),"%s",s);}
int main(int argc,char **argv){
 char deep[128]="{\"a\":";for(int i=0;i<20;i++)strcat(deep,"[");strcat(deep,"0");for(int i=0;i<20;i++)strcat(deep,"]");strcat(deep,"}");assert(!cJSON_Parse(deep));
 assert(!request_matches(NULL,NULL));cJSON *empty=cJSON_CreateObject();assert(!request_matches(empty,empty));cJSON_Delete(empty);
 cJSON *cached=cJSON_Parse("{\"power\":1,\"mode\":0,\"cadr\":40,\"uvlamp\":1,\"waterflood\":0,\"lock\":0,\"anion\":0,\"hThreshold\":45,\"humidity\":5200}");
 cJSON *full=cJSON_Parse("{\"ONOFF\":1,\"mode\":1,\"humidity\":50,\"PIRL\":1,\"unknownFutureField\":123}");cJSON_SetNumberValue(cJSON_GetObjectItem(cached,"mode"),1);
 cJSON *changed=make_target_config(cached,full,47);assert(changed);assert(cJSON_GetObjectItem(changed,"humidity")->valueint==47);assert(cJSON_GetObjectItem(changed,"PIRL")->valueint==1);assert(cJSON_GetObjectItem(changed,"unknownFutureField")->valueint==123);assert(cJSON_GetObjectItem(full,"humidity")->valueint==50);cJSON_Delete(changed);
 cJSON_SetNumberValue(cJSON_GetObjectItem(cached,"power"),0);cJSON_SetNumberValue(cJSON_GetObjectItem(full,"ONOFF"),0);cJSON_SetNumberValue(cJSON_GetObjectItem(cached,"mode"),5);cJSON_SetNumberValue(cJSON_GetObjectItem(full,"mode"),5);assert(!make_target_config(cached,full,47));cJSON_SetNumberValue(cJSON_GetObjectItem(cached,"mode"),1);cJSON_SetNumberValue(cJSON_GetObjectItem(full,"mode"),1);cJSON_SetNumberValue(cJSON_GetObjectItem(cached,"power"),1);cJSON_SetNumberValue(cJSON_GetObjectItem(full,"ONOFF"),1);assert(!make_target_config(cached,full,39));assert(!make_target_config(cached,full,61));cJSON_SetNumberValue(cJSON_GetObjectItem(cached,"power"),0);assert(!make_target_config(cached,full,47));cJSON_SetNumberValue(cJSON_GetObjectItem(cached,"power"),1);cJSON_SetNumberValue(cJSON_GetObjectItem(cached,"mode"),0);cJSON_Delete(full);
 cJSON *want=cJSON_Parse("{\"mode\":0,\"cadr\":63}");
 cJSON *ack=cJSON_Parse("{\"power\":1,\"mode\":0,\"cadr\":62,\"uvlamp\":1,\"waterflood\":0,\"lock\":0,\"anion\":0,\"humidity\":5700}");
 assert(apply_control_ack(want,ack,cached));assert(cJSON_GetObjectItem(cached,"cadr")->valueint==62);assert(cJSON_GetObjectItem(cached,"hThreshold")->valueint==45);assert(cJSON_GetObjectItem(cached,"humidity")->valueint==5200);
 cJSON_SetNumberValue(cJSON_GetObjectItem(ack,"cadr"),58);assert(!apply_control_ack(want,ack,cached));assert(cJSON_GetObjectItem(cached,"cadr")->valueint==62);
 cJSON_DeleteItemFromObject(ack,"lock");assert(!apply_control_ack(want,ack,cached));cJSON_Delete(want);cJSON_Delete(ack);cJSON_Delete(cached);

 frame_parser p={0};const char *text="noise<<ReadConfig>><<MachineState{\"power\":1}>>";
 for(size_t i=0;i<strlen(text);i++)frame_feed(&p,(const uint8_t*)text+i,1,receive,NULL);
 assert(frames==2&&!strcmp(got,"MachineState{\"power\":1}"));
 char *large=malloc(6000);memset(large,'x',6000);frame_feed(&p,(const uint8_t*)"<<",2,receive,NULL);frame_feed(&p,(uint8_t*)large,6000,receive,NULL);frame_feed(&p,(const uint8_t*)"<<ReadConfig>>",14,receive,NULL);assert(!strcmp(got,"ReadConfig"));free(large);
 cJSON *s=cJSON_Parse("{\"power\":1,\"mode\":0,\"cadr\":14,\"uvlamp\":1,\"waterflood\":0,\"lock\":0,\"anion\":0}");cJSON *cfg=cJSON_Parse("{\"CADR\":15}");assert(state_valid(s));
 int vals[]={15,30,64,86,100};for(int i=1;i<=5;i++){cJSON *cmd=make_control(s,cfg,"speed",i);assert(cmd);assert(cJSON_GetObjectItem(cmd,"cadr")->valueint==vals[i-1]);assert(cJSON_GetObjectItem(cmd,"uvlamp")->valueint==1);assert(cJSON_GetArraySize(cmd)==7);cJSON_Delete(cmd);}
 assert(!make_control(s,cfg,"speed",0));assert(!make_control(s,cfg,"speed",6));assert(!make_control(s,cfg,"ReSet",1));
 cJSON *cmd=NULL;assert(!make_control(s,cfg,"refill",1));assert(!make_control(s,cfg,"refill",0));
 cJSON_SetNumberValue(cJSON_GetObjectItem(s,"power"),0);assert(!make_control(s,cfg,"speed",2));assert(!make_control(s,cfg,"refill",0));cmd=make_control(s,cfg,"power",1);assert(cmd&&cJSON_GetObjectItem(cmd,"cadr")->valueint==15);cJSON_Delete(cmd);
 cJSON_SetNumberValue(cJSON_GetObjectItem(s,"power"),1);cJSON_SetNumberValue(cJSON_GetObjectItem(s,"waterflood"),1);assert(!make_control(s,cfg,"mode",0));cJSON_SetNumberValue(cJSON_GetObjectItem(s,"waterflood"),0);cJSON_SetNumberValue(cJSON_GetObjectItem(s,"lock"),1);assert(!make_control(s,cfg,"speed",1));cmd=make_control(s,cfg,"power",0);assert(cmd);cJSON_Delete(cmd);
 cJSON_SetNumberValue(cJSON_GetObjectItem(cfg,"CADR"),0);cmd=make_control(s,cfg,"power",1);assert(cmd&&cJSON_GetObjectItem(cmd,"cadr")->valueint==15);cJSON_Delete(cmd);
 cJSON_SetNumberValue(cJSON_GetObjectItem(s,"lock"),0);for(int i=15;i<=100;i++){cmd=make_control(s,cfg,"airflow",i);assert(cmd&&cJSON_GetObjectItem(cmd,"cadr")->valueint==i);cJSON_Delete(cmd);}assert(!make_control(s,cfg,"airflow",14)&&!make_control(s,cfg,"airflow",101));
 cJSON_DeleteItemFromObject(s,"uvlamp");assert(!state_valid(s)&&!make_control(s,cfg,"power",1));cJSON_Delete(s);cJSON_Delete(cfg);
 unsigned captures=0;for(int i=1;i<argc;i++){FILE *f=fopen(argv[i],"rb");assert(f);frame_parser parser={0};unsigned char buf[37];size_t n;while((n=fread(buf,1,sizeof(buf),f)))frame_feed(&parser,buf,n,receive,NULL);fclose(f);captures++;}
 printf("PASS: protocol bounds, fragmented frames, overflow resync, command allowlist, protected-state rejection, %u real captures replayed (%u frames)\n",captures,frames);
}
