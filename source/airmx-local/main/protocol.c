#include "protocol.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
void frame_feed(frame_parser *p,const uint8_t *bytes,size_t count,frame_fn cb,void *ctx){
 for(size_t i=0;i<count;i++) {char c=(char)bytes[i];
  if(p->prev=='<' && c=='<'){p->active=true;p->n=0;p->prev=0;continue;}
  if(p->active){
   if(p->prev=='>' && c=='>'){if(p->n)p->n--;p->data[p->n]=0;cb(p->data,ctx);p->active=false;p->n=0;p->prev=0;continue;}
   if(p->n+1>=FRAME_CAP){p->active=false;p->n=0;}else p->data[p->n++]=c;
  }
  p->prev=c;
 }
}
bool json_int(const cJSON *j,const char *key,int *v){const cJSON *x=cJSON_GetObjectItemCaseSensitive(j,key);if(!cJSON_IsNumber(x)||!isfinite(x->valuedouble)||x->valuedouble<-1000000||x->valuedouble>1000000||x->valuedouble!=(double)x->valueint)return false;*v=x->valueint;return true;}
bool state_valid(const cJSON *s){
 const char *keys[]={"power","mode","cadr","uvlamp","waterflood","lock","anion"};int v;
 if(!cJSON_IsObject(s))return false;
 for(int i=0;i<7;i++){if(!json_int(s,keys[i],&v))return false;if(i==1){if(v<0||v>10)return false;}else if(i==2){if(v<0||v>100)return false;}else if(v!=0&&v!=1)return false;}
 return true;
}
int configured_speed(int v){const int report[]={14,29,64,85,100},cfg[]={15,30,64,86,100};for(int i=0;i<5;i++)if(v==report[i])return cfg[i];return v;}
cJSON *make_control(const cJSON *s,const cJSON *cfg,const char *act,int v){
 if(!state_valid(s)||!act)return NULL;
 bool power=!strcmp(act,"power"),speed=!strcmp(act,"speed"),mode=!strcmp(act,"mode"),airflow=!strcmp(act,"airflow");
 if(!(power||speed||mode||airflow))return NULL;
 if(power&&(v<0||v>1))return NULL;
 if(airflow&&(v<15||v>100))return NULL;
 if(speed&&(v<1||v>5))return NULL;
 if(mode&&(v<0||v>2))return NULL;
 int ison=0,md=0,lock=0,wf=0;json_int(s,"power",&ison);json_int(s,"mode",&md);json_int(s,"lock",&lock);json_int(s,"waterflood",&wf);
 /* Do not turn a setting change into a hidden power-on or clear a local protection. */
 if(!power&&(!ison||lock||(md!=0&&md!=1&&md!=2)))return NULL;
 if((speed||mode||airflow)&&wf)return NULL;
 cJSON *out=cJSON_CreateObject();const char *keys[]={"power","mode","cadr","uvlamp","waterflood","lock","anion"};
 for(int i=0;i<7;i++){int n=0;json_int(s,keys[i],&n);cJSON_AddNumberToObject(out,keys[i],n);}
 if(power){cJSON_SetNumberValue(cJSON_GetObjectItem(out,"power"),v);if(v){int saved=0;if(md>2||!json_int(cfg,"CADR",&saved)||saved<0||saved>100){cJSON_Delete(out);return NULL;}cJSON_SetNumberValue(cJSON_GetObjectItem(out,"cadr"),saved?saved:15);}else cJSON_SetNumberValue(cJSON_GetObjectItem(out,"cadr"),0);}
 if(speed){int values[]={15,30,64,86,100};cJSON_SetNumberValue(cJSON_GetObjectItem(out,"mode"),0);cJSON_SetNumberValue(cJSON_GetObjectItem(out,"cadr"),values[v-1]);}
 if(airflow){cJSON_SetNumberValue(cJSON_GetObjectItem(out,"mode"),0);cJSON_SetNumberValue(cJSON_GetObjectItem(out,"cadr"),v);}
 if(mode){int saved=0;if(!json_int(cfg,"CADR",&saved)||saved<0||saved>100){cJSON_Delete(out);return NULL;}cJSON_SetNumberValue(cJSON_GetObjectItem(out,"mode"),v);cJSON_SetNumberValue(cJSON_GetObjectItem(out,"cadr"),saved?saved:15);}
 return out;
}
bool request_matches(const cJSON *expected,const cJSON *s){
 if(!cJSON_IsObject(expected)||!expected->child||!cJSON_IsObject(s))return false;
 const cJSON *x=NULL;cJSON_ArrayForEach(x,expected){int v;if(!x->string||!json_int(s,x->string,&v)||!cJSON_IsNumber(x) )return false;if(!strcmp(x->string,"cadr")){if(abs(v-x->valueint)>1)return false;}else if(v!=x->valueint)return false;}return true;
}

bool apply_control_ack(const cJSON *expected,const cJSON *ack,cJSON *state){
 if(!cJSON_IsObject(expected)||!expected->child||!state_valid(state)||!state_valid(ack)||!request_matches(expected,ack))return false;
 const char *keys[]={"power","mode","cadr","uvlamp","waterflood","lock","anion"};
 for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++)cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(state,keys[i]),cJSON_GetObjectItemCaseSensitive(ack,keys[i])->valueint);
 return true;
}

cJSON *make_target_config(const cJSON *s,const cJSON *cfg,int v){
 int power,mode,lock,wf,cp,cm,old;
 if(!state_valid(s)||v<40||v>60||!json_int(s,"power",&power)||!json_int(s,"mode",&mode)||!json_int(s,"lock",&lock)||lock||!json_int(s,"waterflood",&wf)||wf||!json_int(cfg,"ONOFF",&cp)||cp!=power||!json_int(cfg,"mode",&cm)||cm!=mode||!json_int(cfg,"humidity",&old)||mode>2||(mode!=1&&power!=0))return NULL;
 cJSON *out=cJSON_Duplicate(cfg,true);if(out)cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(out,"humidity"),v);return out;
}
