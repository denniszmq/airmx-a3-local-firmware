#include "goose.h"
#include "goose_decode.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
static SemaphoreHandle_t lock;
static nvs_handle_t storage;
static bool writable,started,diagnostic;
static uint8_t use_external=1;
static int scan_error;
static unsigned packets, advertisements, fdcd_packets, fe95_packets, fff9_packets;
static char bound_raw[125];
static int64_t bound_seen;
static char bound[18];
typedef struct{char mac[18];int temp,hum,rssi;int64_t at;} sample;
static sample candidates[8];
static int64_t now(void){return esp_timer_get_time()/1000;}
static bool normalize(const char *in,char *out){if(!in||strlen(in)!=17)return false;for(int i=0;i<17;i++){if(i%3==2){if(in[i]!=':')return false;out[i]=':';}else{if(!isxdigit((unsigned char)in[i]))return false;out[i]=(char)tolower((unsigned char)in[i]);}}out[17]=0;return true;}
static sample *current(void){for(int i=0;i<8;i++)if(bound[0]&&!strcmp(bound,candidates[i].mac))return &candidates[i];return NULL;}
static int event(struct ble_gap_event *e,void *arg){(void)arg;
 if(e->type!=BLE_GAP_EVENT_DISC)return 0;
 const uint8_t *p=e->disc.data;size_t n=e->disc.length_data;
 xSemaphoreTake(lock,portMAX_DELAY);
 if(diagnostic){char seen_mac[18];const uint8_t *addr=e->disc.addr.val;
  snprintf(seen_mac,sizeof(seen_mac),"%02x:%02x:%02x:%02x:%02x:%02x",addr[5],addr[4],addr[3],addr[2],addr[1],addr[0]);advertisements++;
  if(bound[0]&&!strcmp(bound,seen_mac)){bound_seen=now();size_t count=n>62?62:n;for(size_t k=0;k<count;k++)snprintf(bound_raw+2*k,3,"%02x",p[k]);}
 }xSemaphoreGive(lock);
 for(size_t i=0;i<n;){size_t len=p[i];if(!len||len>n-i-1)break;
  if(len>=3&&p[i+1]==0x16){xSemaphoreTake(lock,portMAX_DELAY);if(diagnostic){if(p[i+2]==0xcd&&p[i+3]==0xfd)fdcd_packets++;if(p[i+2]==0x95&&p[i+3]==0xfe)fe95_packets++;if(p[i+2]==0xf9&&p[i+3]==0xff)fff9_packets++;}xSemaphoreGive(lock);}
  if(len>=3&&p[i+1]==0x16&&((p[i+2]==0xcd&&p[i+3]==0xfd)||(p[i+2]==0xf9&&p[i+3]==0xff))){int t,h;if(goose_decode(p+i+4,len-3,&t,&h)){
   char mac[18];const uint8_t *a=e->disc.addr.val;snprintf(mac,sizeof(mac),"%02x:%02x:%02x:%02x:%02x:%02x",a[5],a[4],a[3],a[2],a[1],a[0]);
   xSemaphoreTake(lock,portMAX_DELAY);packets++;int slot=-1;for(int j=0;j<8;j++)if(!strcmp(mac,candidates[j].mac)){slot=j;break;}
   if(slot<0){int64_t oldest=INT64_MAX;for(int j=0;j<8;j++)if((!bound[0]||strcmp(bound,candidates[j].mac))&&candidates[j].at<oldest){oldest=candidates[j].at;slot=j;}}
   if(slot>=0){sample *s=&candidates[slot];strcpy(s->mac,mac);s->temp=t;s->hum=h;s->rssi=e->disc.rssi;s->at=now();}xSemaphoreGive(lock);
  }}i+=len+1;
 }return 0;
}
static void synced(void){uint8_t own;int rc=ble_hs_util_ensure_addr(0);if(!rc)rc=ble_hs_id_infer_auto(0,&own);if(!rc){struct ble_gap_disc_params p={0};p.passive=1;p.itvl=160;p.window=80;p.filter_duplicates=0;rc=ble_gap_disc(own,BLE_HS_FOREVER,&p,event,NULL);}xSemaphoreTake(lock,portMAX_DELAY);scan_error=rc;started=rc==0;xSemaphoreGive(lock);}
static void reset(int reason){xSemaphoreTake(lock,portMAX_DELAY);started=false;scan_error=reason;for(int i=0;i<8;i++)candidates[i].at=0;xSemaphoreGive(lock);}
static void task(void *arg){(void)arg;nimble_port_run();nimble_port_freertos_deinit();}
void goose_init(void){lock=xSemaphoreCreateMutex();if(!lock)return;writable=nvs_open("a3goose",NVS_READWRITE,&storage)==ESP_OK;if(writable){uint8_t v;if(nvs_get_u8(storage,"external",&v)==ESP_OK&&v<=1)use_external=v;}size_t n=sizeof(bound);char raw[32]={0};
 if(writable&&nvs_get_str(storage,"mac",raw,&n)==ESP_OK){normalize(raw,bound);}else{FILE *f=fopen("/factory/Goose_MAC.txt","rb");if(f){fread(raw,1,sizeof(raw)-1,f);fclose(f);raw[strcspn(raw,"\r\n ")]=0;normalize(raw,bound);}}
 int rc=nimble_port_init();if(rc){scan_error=rc;return;}ble_hs_cfg.sync_cb=synced;ble_hs_cfg.reset_cb=reset;nimble_port_freertos_init(task);
}
bool goose_bind(const char *mac){char norm[18];if(!lock||!normalize(mac,norm))return false;xSemaphoreTake(lock,portMAX_DELAY);bool found=false;for(int i=0;i<8;i++)if(!strcmp(norm,candidates[i].mac)&&candidates[i].at&&now()-candidates[i].at<=120000)found=true;
 bool ok=found&&writable&&nvs_set_str(storage,"mac",norm)==ESP_OK&&nvs_commit(storage)==ESP_OK;if(ok)strcpy(bound,norm);xSemaphoreGive(lock);return ok;}
cJSON *goose_status(void){cJSON *j=cJSON_CreateObject();if(!lock)return j;xSemaphoreTake(lock,portMAX_DELAY);sample *s=current();bool fresh=started&&s&&s->at&&now()-s->at<=120000;
 if(diagnostic){ cJSON_AddNumberToObject(j,"advertisements",advertisements);cJSON_AddNumberToObject(j,"fdcdPackets",fdcd_packets);cJSON_AddNumberToObject(j,"fff9Packets",fff9_packets);cJSON_AddNumberToObject(j,"fe95Packets",fe95_packets);cJSON_AddNumberToObject(j,"boundSeenAgeMs",bound_seen?now()-bound_seen:-1);cJSON_AddStringToObject(j,"boundRaw",bound_raw);}
 cJSON_AddStringToObject(j,"selectedSource",use_external?"external":"internal");cJSON_AddStringToObject(j,"effectiveSource",use_external&&fresh?"external":"internal");cJSON_AddBoolToObject(j,"scanning",started);cJSON_AddNumberToObject(j,"error",scan_error);cJSON_AddNumberToObject(j,"packets",packets);cJSON_AddStringToObject(j,"boundMac",bound);cJSON_AddBoolToObject(j,"online",fresh);cJSON_AddNumberToObject(j,"ageMs",s&&s->at?now()-s->at:-1);
 if(fresh){cJSON_AddNumberToObject(j,"temperature",s->temp);cJSON_AddNumberToObject(j,"humidity",s->hum);cJSON_AddNumberToObject(j,"rssi",s->rssi);}cJSON *arr=cJSON_AddArrayToObject(j,"candidates");for(int i=0;i<8;i++){s=&candidates[i];if(!s->at||now()-s->at>120000)continue;cJSON *c=cJSON_CreateObject();cJSON_AddStringToObject(c,"mac",s->mac);cJSON_AddNumberToObject(c,"temperature",s->temp);cJSON_AddNumberToObject(c,"humidity",s->hum);cJSON_AddNumberToObject(c,"rssi",s->rssi);cJSON_AddItemToArray(arr,c);}xSemaphoreGive(lock);return j;}
bool goose_source(bool external){if(!lock)return false;xSemaphoreTake(lock,portMAX_DELAY);sample *s=current();bool fresh=started&&s&&s->at&&now()-s->at<=120000;bool ok=(!external||fresh)&&writable&&nvs_set_u8(storage,"external",external?1:0)==ESP_OK&&nvs_commit(storage)==ESP_OK;if(ok)use_external=external?1:0;xSemaphoreGive(lock);return ok;}
void goose_diagnostics(bool enabled){if(!lock)return;xSemaphoreTake(lock,portMAX_DELAY);diagnostic=enabled;bound_raw[0]=0;bound_seen=0;advertisements=fdcd_packets=fff9_packets=fe95_packets=0;xSemaphoreGive(lock);}
void goose_get_values(goose_values *out){*out=(goose_values){.temperature=99999,.humidity=99999};if(!lock)return;xSemaphoreTake(lock,portMAX_DELAY);sample *s=current();bool fresh=use_external&&started&&s&&s->at&&now()-s->at<=120000;if(fresh)*out=(goose_values){1,s->rssi,s->temp,s->hum};xSemaphoreGive(lock);}
void goose_wifi(cJSON *wifi){goose_values v;goose_get_values(&v);cJSON_ReplaceItemInObject(wifi,"bleLink",cJSON_CreateNumber(v.link));cJSON_ReplaceItemInObject(wifi,"bleSignal",cJSON_CreateNumber(v.signal));cJSON_ReplaceItemInObject(wifi,"gooseTemp",cJSON_CreateNumber(v.temperature));cJSON_ReplaceItemInObject(wifi,"gooseHumity",cJSON_CreateNumber(v.humidity));}
