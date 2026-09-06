#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_spiffs.h"
#include "protocol.h"
#include "local_credentials.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "ota_guard.h"
#include "network.h"
#include "goose.h"
#include "water_usage.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#define LINK UART_NUM_2
static const char *TAG="A3";
static SemaphoreHandle_t mutex;
static nvs_handle_t store;
static bool storage_ok,fs_ok,armed;
static cJSON *state,*config,*timers,*expected,*last_control,*last_ack;
static int64_t last_ack_at;
static int64_t state_at,command_at,arm_until;
static unsigned state_count,command_id,rx_frames,invalid_frames;
static int64_t now_ms(void);
static bool pending,ack_seen,ota_busy,ota_interrupted,force_wifi_update=true;
static int ota_recv_error;
/* Called with mutex held. Updating requires the STM32 to report stopped. */
static bool ota_safe(void){int p=-1,w=-1,c=-1;return state_at&&now_ms()-state_at<15000&&json_int(state,"power",&p)&&p==0&&json_int(state,"waterflood",&w)&&w==0&&json_int(state,"cadr",&c)&&c==0;}
static char result[192]="等待设备上报",token[33];
static char loglines[12][160];static unsigned logcount,loghead;
static bool diagnostic_enabled,heap_ok=true;
static int64_t last_log_clear,last_heap_check;
static water_usage water_tracker;
static TaskHandle_t serial_handle;
static void clear_logs(void){memset(loglines,0,sizeof(loglines));loghead=logcount=0;cJSON_Delete(last_control);last_control=NULL;cJSON_Delete(last_ack);last_ack=NULL;goose_diagnostics(diagnostic_enabled);}
extern const uint8_t html_start[] asm("_binary_index_html_start");
extern const uint8_t html_end[] asm("_binary_index_html_end");
static int64_t now_ms(void){return esp_timer_get_time()/1000;}
static void note(const char *s){if(!diagnostic_enabled)return;snprintf(loglines[loghead],160,"%lld %s",(long long)now_ms(),s);loghead=(loghead+1)%12;if(logcount<12)logcount++;ESP_LOGI(TAG,"%s",s);}
static cJSON *parse(const char *s){const char *end=NULL;cJSON *j=cJSON_ParseWithOpts(s,&end,true);if(!cJSON_IsObject(j)){cJSON_Delete(j);return NULL;}return j;}
static bool valid_config(cJSON *j){int n;const char *k[]={"humidity","mode","CADR","ONOFF","PIRL","pumpInstantOff","WUD"};for(int i=0;i<7;i++)if(!json_int(j,k[i],&n))return false;return true;}
static cJSON *load_json(const char *key,const char *file){
 char buf[FRAME_CAP];size_t n=sizeof(buf);if(storage_ok&&nvs_get_str(store,key,buf,&n)==ESP_OK)return parse(buf);
 if(!fs_ok)return NULL;
 FILE *f=fopen(file,"rb");if(!f)return NULL;size_t len=fread(buf,1,sizeof(buf)-1,f);bool too_big=fgetc(f)!=EOF;fclose(f);if(too_big)return NULL;buf[len]=0;return parse(buf);
}
static bool save_json(const char *key,cJSON *j){
 /* Calls are serialized by the main mutex. Fixed buffers avoid transient heap churn. */
 static char text[FRAME_CAP],previous[FRAME_CAP];if(!j||!cJSON_PrintPreallocated(j,text,sizeof(text),false))return false;
 size_t n=sizeof(previous);if(storage_ok&&nvs_get_str(store,key,previous,&n)==ESP_OK&&!strcmp(text,previous))return true;
 esp_err_t err=storage_ok?nvs_set_str(store,key,text):ESP_FAIL;if(err==ESP_OK)err=nvs_commit(store);
 if(err!=ESP_OK){note("配置保存失败；重启后可能丢失更改");}
 return err==ESP_OK;
}
static bool send_frame(const char *name,cJSON *j){
 static char wire[FRAME_CAP];int prefix=snprintf(wire,sizeof(wire),"<<%s",name);
 if(prefix<0||prefix>(int)sizeof(wire)-8)return false;
 if(j&&!cJSON_PrintPreallocated(j,wire+prefix,sizeof(wire)-prefix-3,false))return false;
 size_t n=strlen(wire);if(n+3>sizeof(wire))return false;wire[n++]='>';wire[n++]='>';wire[n]=0;
 return uart_write_bytes(LINK,wire,n)==(int)n;
}
static void frame_received(const char *frame,void *ctx){
 (void)ctx;rx_frames++;
 if(!strcmp(frame,"ReadConfig")){note("RX ReadConfig; replying saved configuration");if(config){send_frame("ReadConfigAck",config);force_wifi_update=true;}else note("原厂配置不可用，未发送猜测配置");return;}
 if(!strcmp(frame,"ReadTimerConfig")){if(timers)send_frame("ReadTimerConfigAck",timers);return;}
 const char *brace=strchr(frame,'{');if(!brace){if(!strncmp(frame,"STM32 ",6))note(frame);return;}
 size_t len=(size_t)(brace-frame);if(len>40){invalid_frames++;return;}char kind[41];memcpy(kind,frame,len);kind[len]=0;if(strcmp(kind,"MachineState")&&strcmp(kind,"ControlCmdAck")&&strcmp(kind,"SaveConfig")&&strcmp(kind,"SaveTimerConfig"))return;cJSON *j=parse(brace);if(!j){invalid_frames++;return;}
 if(!strcmp(kind,"MachineState")){
  if(state_valid(j)){int old_power=-1;if(state)json_int(state,"power",&old_power);cJSON_Delete(state);state=j;j=NULL;state_at=now_ms();state_count++;
   int pp=-1,ww=-1,mm=-1,lv=-1,tt=-1,cc=-1;json_int(state,"power",&pp);json_int(state,"waterflood",&ww);json_int(state,"mode",&mm);json_int(state,"cadr",&cc);json_int(state,"hThreshold",&tt);bool water_valid=json_int(state,"water",&lv);
   water_feed(&water_tracker,state_at,water_valid,pp==1&&ww==0,lv,mm,tt,cc);
   if(pending&&request_matches(expected,state)){pending=false;int target;bool saved=true;if(json_int(expected,"hThreshold",&target)){cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(config,"humidity"),target);saved=save_json("config",config);}snprintf(result,sizeof(result),saved?"操作 %u：状态已确认":"操作 %u：已执行但保存失败",command_id);}
   int power=0;json_int(state,"power",&power);if(!power&&old_power==1){armed=false;arm_until=0;}
  }else invalid_frames++;
 }else if(!strcmp(kind,"ControlCmdAck")){cJSON_Delete(last_ack);last_ack=diagnostic_enabled?cJSON_Duplicate(j,true):NULL;last_ack_at=now_ms();if(diagnostic_enabled){char *ack_text=cJSON_PrintUnformatted(j);if(ack_text){note(ack_text);free(ack_text);}}if(pending&&state_valid(j)){ack_seen=true;if(apply_control_ack(expected,j,state)){
 pending=false;snprintf(result,sizeof(result),"操作 %u：状态已确认",command_id);
 }}}
 else if(!strcmp(kind,"SaveConfig")&&valid_config(j)){
  note("RX SaveConfig; saving controller configuration");cJSON_Delete(config);config=j;j=NULL;save_json("config",config);
 }else if(!strcmp(kind,"SaveTimerConfig")){
  cJSON_Delete(timers);timers=j;j=NULL;save_json("timers",timers);
 }
 cJSON_Delete(j);
}
static void serial_task(void *unused){
 (void)unused;ESP_ERROR_CHECK(esp_task_wdt_add(NULL));frame_parser parser={0};uint8_t buf[256];int64_t last_wifi=0,last_diag=0,last_sensor_check=0;goose_values last_values={0};
 for(;;){int count=uart_read_bytes(LINK,buf,sizeof(buf),pdMS_TO_TICKS(100));xSemaphoreTake(mutex,portMAX_DELAY);
  if(count>0)frame_feed(&parser,buf,count,frame_received,NULL);
  int64_t now=now_ms();
  if(ota_busy&&!ota_safe())ota_interrupted=true;
  if(now-state_at>15000||now>arm_until)armed=false;
  if(pending&&now-command_at>12000){pending=false;armed=false;snprintf(result,sizeof(result),"操作 %u：未确认执行，不自动重试",command_id);}
  if(now-last_log_clear>=1800000){clear_logs();last_log_clear=now;}
  if(diagnostic_enabled&&now-last_diag>=60000){
   heap_ok=heap_caps_check_integrity_all(false);last_heap_check=now;
   int power=-1,mode=-1,cadr=-1;json_int(state,"power",&power);json_int(state,"mode",&mode);json_int(state,"cadr",&cadr);
   ESP_LOGI(TAG,"Status v%s partition=%s frames=%u states=%u invalid=%u age_ms=%lld config=%d timers=%d storage=%d power=%d mode=%d cadr=%d ota=%d",esp_app_get_description()->version,esp_ota_get_running_partition()->label,rx_frames,state_count,invalid_frames,(long long)(state_at?now-state_at:-1),config!=NULL,timers!=NULL,storage_ok,power,mode,cadr,ota_busy);
   last_diag=now;
  }
  if(now-last_sensor_check>=1000){
   goose_values values;goose_get_values(&values);last_sensor_check=now;
   bool changed=values.link!=last_values.link||values.temperature!=last_values.temperature||values.humidity!=last_values.humidity;
   if(force_wifi_update||changed||now-last_wifi>=15300){
    cJSON *wifi=cJSON_CreateObject();const char *keys[]={"wifiLink","wifiSignal","bleLink","bleSignal","gooseTemp","gooseHumity","UTCTime"};for(int i=0;i<7;i++)cJSON_AddNumberToObject(wifi,keys[i],0);goose_wifi(wifi);if(send_frame("WiFiState",wifi)){last_wifi=now;last_values=values;force_wifi_update=false;}cJSON_Delete(wifi);
   }
  }
  xSemaphoreGive(mutex);esp_task_wdt_reset();
 }
}
static void headers(httpd_req_t *r){httpd_resp_set_hdr(r,"Cache-Control","no-store");httpd_resp_set_hdr(r,"X-Content-Type-Options","nosniff");httpd_resp_set_hdr(r,"X-Frame-Options","DENY");}
static esp_err_t reply(httpd_req_t *r,cJSON *j){
 static char response[8192];bool ok=j&&cJSON_PrintPreallocated(j,response,sizeof(response),false);cJSON_Delete(j);
 headers(r);httpd_resp_set_type(r,"application/json");if(!ok){httpd_resp_set_status(r,"503 Service Unavailable");return httpd_resp_sendstr(r,"{\"error\":\"响应资源不足，请稍后重试\"}");}return httpd_resp_sendstr(r,response);
}
static esp_err_t error(httpd_req_t *r,const char *status,const char *msg){httpd_resp_set_status(r,status);cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"error",msg);return reply(r,j);}
static esp_err_t index_get(httpd_req_t *r){headers(r);httpd_resp_set_hdr(r,"Content-Security-Policy","default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'");httpd_resp_set_type(r,"text/html; charset=utf-8");return httpd_resp_send(r,(const char*)html_start,html_end-html_start-1);}
static esp_err_t status_get(httpd_req_t *r){
 if(esp_get_free_heap_size()<12000){httpd_resp_set_status(r,"503 Service Unavailable");return httpd_resp_sendstr(r,"{\"error\":\"资源不足，请稍后重试\"}");}
 xSemaphoreTake(mutex,portMAX_DELAY);cJSON *j=cJSON_CreateObject();int64_t age=state_at?now_ms()-state_at:-1;bool fresh=age>=0&&age<=15000;
 cJSON_AddStringToObject(j,"version",esp_app_get_description()->version);cJSON_AddBoolToObject(j,"otaReady",ota_safe()&&!pending&&!ota_busy);cJSON_AddBoolToObject(j,"otaBusy",ota_busy);cJSON_AddStringToObject(j,"runningPartition",esp_ota_get_running_partition()->label);
 cJSON_AddItemToObject(j,"network",network_status());cJSON_AddItemToObject(j,"goose",goose_status());
 cJSON *diag=cJSON_AddObjectToObject(j,"controlDiagnostic");if(last_control)cJSON_AddItemToObject(diag,"lastSent",cJSON_Duplicate(last_control,true));if(last_ack)cJSON_AddItemToObject(diag,"lastAck",cJSON_Duplicate(last_ack,true));cJSON_AddNumberToObject(diag,"lastAckAgeMs",last_ack_at?(double)(now_ms()-last_ack_at):-1);cJSON_AddBoolToObject(diag,"ackSeen",ack_seen);
 if(diagnostic_enabled&&config)cJSON_AddItemToObject(diag,"savedDeviceConfig",cJSON_Duplicate(config,true));
 cJSON *cfg=cJSON_AddObjectToObject(diag,"savedConfig");const char *ck[]={"ONOFF","mode","CADR","humidity","h_m","PIRL","WUD","pumpInstantOff"};for(unsigned i=0;i<sizeof(ck)/sizeof(ck[0]);i++){int n;if(json_int(config,ck[i],&n))cJSON_AddNumberToObject(cfg,ck[i],n);}

 cJSON_AddBoolToObject(j,"controlReady",fresh&&state_count>=3&&config&&timers&&storage_ok&&!pending&&!ota_busy);cJSON_AddStringToObject(j,"token",token);cJSON_AddBoolToObject(j,"connected",fresh);cJSON_AddBoolToObject(j,"armed",armed&&now_ms()<arm_until&&fresh);cJSON_AddBoolToObject(j,"pending",pending);cJSON_AddBoolToObject(j,"configReady",config&&timers&&storage_ok);cJSON_AddNumberToObject(j,"ageMs",(double)age);cJSON_AddNumberToObject(j,"rxFrames",rx_frames);cJSON_AddNumberToObject(j,"invalidFrames",invalid_frames);cJSON_AddNumberToObject(j,"commandId",command_id);cJSON_AddStringToObject(j,"result",result);
 if(state)cJSON_AddItemToObject(j,"state",cJSON_Duplicate(state,true));else cJSON_AddNullToObject(j,"state");
 cJSON_AddBoolToObject(j,"diagnosticsEnabled",diagnostic_enabled);
 cJSON *health=cJSON_AddObjectToObject(j,"health");cJSON_AddNumberToObject(health,"uptimeSeconds",now_ms()/1000);
 water_result wr=water_estimate(&water_tracker,now_ms());cJSON *water=cJSON_AddObjectToObject(health,"waterUsage");cJSON_AddBoolToObject(water,"infinite",!wr.finite);cJSON_AddStringToObject(water,"reason",wr.reason);cJSON_AddNumberToObject(water,"intervals",wr.intervals);cJSON_AddNumberToObject(water,"nextSampleSeconds",wr.next_ms/1000);cJSON_AddNumberToObject(water,"samples",water_tracker.count);
 if(wr.finite){cJSON_AddNumberToObject(water,"hours",wr.hours);cJSON_AddNumberToObject(water,"unitsPerHour",wr.units_per_hour);}else cJSON_AddNullToObject(water,"hours");
 if(diagnostic_enabled){cJSON_AddNumberToObject(health,"freeHeap",esp_get_free_heap_size());cJSON_AddNumberToObject(health,"minFreeHeap",esp_get_minimum_free_heap_size());cJSON_AddNumberToObject(health,"largestFreeBlock",heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));if(last_heap_check)cJSON_AddBoolToObject(health,"heapIntegrity",heap_ok);else cJSON_AddNullToObject(health,"heapIntegrity");cJSON_AddNumberToObject(health,"heapCheckAgeMs",last_heap_check?now_ms()-last_heap_check:-1);cJSON_AddNumberToObject(health,"resetReason",esp_reset_reason());cJSON_AddNumberToObject(health,"serialStackFree",serial_handle?uxTaskGetStackHighWaterMark(serial_handle):0);}
 cJSON *logs=cJSON_AddArrayToObject(j,"logs");if(diagnostic_enabled)for(unsigned i=0;i<logcount;i++)cJSON_AddItemToArray(logs,cJSON_CreateString(loglines[(loghead+12-logcount+i)%12]));
 xSemaphoreGive(mutex);return reply(r,j);
}
static bool authorized(httpd_req_t *r){char got[40];return httpd_req_get_hdr_value_str(r,"X-A3-Token",got,sizeof(got))==ESP_OK&&!strcmp(got,token);}
static esp_err_t arm_post(httpd_req_t *r){
 if(!authorized(r))return error(r,"403 Forbidden","请重新打开本地页面");
 xSemaphoreTake(mutex,portMAX_DELAY);bool ok=state_count>=3&&state_at&&now_ms()-state_at<15000&&config&&timers&&storage_ok&&!pending&&!ota_busy;
 if(ok){armed=true;arm_until=now_ms()+60000;snprintf(result,sizeof(result),"已准备本次操作");}
 xSemaphoreGive(mutex);if(!ok)return error(r,"409 Conflict","等待有效状态和完整配置后再启用");cJSON *j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"ok",true);return reply(r,j);
}
static esp_err_t control_post(httpd_req_t *r){
 if(!authorized(r))return error(r,"403 Forbidden","请重新打开本地页面");
 if(r->content_len<=0||r->content_len>160)return error(r,"400 Bad Request","无效操作");
 char buf[161];int total=0;while(total<r->content_len){int n=httpd_req_recv(r,buf+total,r->content_len-total);if(n<=0)return error(r,"408 Request Timeout","请求读取失败");total+=n;}buf[total]=0;
 cJSON *req=parse(buf);const cJSON *action=cJSON_GetObjectItemCaseSensitive(req,"action");int value=0;
 if(!cJSON_IsString(action)||!json_int(req,"value",&value)){cJSON_Delete(req);return error(r,"400 Bad Request","无效操作");}
 xSemaphoreTake(mutex,portMAX_DELAY);
 if(ota_busy||!armed||now_ms()>arm_until||!state_at||now_ms()-state_at>15000||pending){xSemaphoreGive(mutex);cJSON_Delete(req);return error(r,"409 Conflict","操作准备已过期，请等待设备最新状态后重试");}
 bool target=!strcmp(action->valuestring,"target");
 cJSON *cmd=NULL;const char *frame_name="ControlCmd";
 if(target){cmd=make_target_config(state,config,value);frame_name="ReadConfigAck";
 }else cmd=make_control(state,config,action->valuestring,value);
 if(!cmd){xSemaphoreGive(mutex);cJSON_Delete(req);return error(r,"409 Conflict","当前状态不允许该操作，请先解除保护或结束加水");}
 cJSON_Delete(expected);expected=cJSON_CreateObject();
 if(target)cJSON_AddNumberToObject(expected,"hThreshold",value);
 else if(!strcmp(action->valuestring,"power"))cJSON_AddNumberToObject(expected,"power",value);
 else if(!strcmp(action->valuestring,"mode"))cJSON_AddNumberToObject(expected,"mode",value);
 else if(!strcmp(action->valuestring,"airflow")){cJSON_AddNumberToObject(expected,"mode",0);cJSON_AddNumberToObject(expected,"cadr",value);}
 else {int report[]={14,29,64,85,100};cJSON_AddNumberToObject(expected,"mode",0);cJSON_AddNumberToObject(expected,"cadr",report[value-1]);}
 cJSON_Delete(last_control);last_control=diagnostic_enabled?cJSON_Duplicate(cmd,true):NULL;note(target?"TX ReadConfigAck target; awaiting state":"TX ControlCmd; awaiting controller response");
 bool sent=send_frame(frame_name,cmd);if(sent&&target)force_wifi_update=true;cJSON_Delete(cmd);command_id++;pending=sent;ack_seen=false;command_at=now_ms();armed=false; /* Each user action requires a fresh arm request. */
 snprintf(result,sizeof(result),sent?"操作 %u：已发送，等待应答与状态":"操作 %u：发送失败",command_id);
 cJSON *j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"sent",sent);cJSON_AddNumberToObject(j,"commandId",command_id);xSemaphoreGive(mutex);cJSON_Delete(req);return reply(r,j);
}

static void reboot_task(void *arg){(void)arg;vTaskDelay(pdMS_TO_TICKS(2000));esp_restart();}
static bool ota_still_safe(void){xSemaphoreTake(mutex,portMAX_DELAY);bool ok=ota_safe()&&!ota_interrupted;xSemaphoreGive(mutex);return ok;}
/* A socket receive timeout is temporary, not a disconnected peer. Keep both
 * an overall deadline and an idle bound; recheck STM32 safety on every retry. */
static int ota_receive(httpd_req_t *r,char *buf,size_t length,int64_t deadline,int64_t *last_data){
 while(ota_still_safe()&&now_ms()<deadline&&now_ms()-*last_data<20000){
  int n=httpd_req_recv(r,buf,length);
  if(n==HTTPD_SOCK_ERR_TIMEOUT)continue;
  if(n<=0)ota_recv_error=n;
  if(n>0)*last_data=now_ms();
  return n;
 }
 ota_recv_error= !ota_still_safe()?-10:now_ms()>=deadline?-11:-12;
 return -1;
}
static esp_err_t ota_post(httpd_req_t *r){
 if(!authorized(r))return error(r,"403 Forbidden","请重新打开本地页面");
 char type[48];if(httpd_req_get_hdr_value_str(r,"Content-Type",type,sizeof(type))!=ESP_OK||strcmp(type,"application/octet-stream"))return error(r,"400 Bad Request","请上传应用 bin 文件");
 const esp_partition_t *running=esp_ota_get_running_partition(),*target=esp_ota_get_next_update_partition(NULL);
 if(!running||!target||running->address==target->address||target->size!=0x1e0000||!((running->address==0x10000&&target->address==0x1f0000)||(running->address==0x1f0000&&target->address==0x10000)))return error(r,"409 Conflict","分区布局不符合本机升级方案");
 if(r->content_len<A3_OTA_PREFIX+32||r->content_len>target->size)return error(r,"400 Bad Request","固件大小不符合应用分区");
 xSemaphoreTake(mutex,portMAX_DELAY);bool ready=ota_safe()&&!pending&&!ota_busy;
 if(ready){ota_recv_error=0;ota_busy=true;ota_interrupted=false;armed=false;snprintf(result,sizeof(result),"正在接收固件，暂不接受控制");}xSemaphoreGive(mutex);
 if(!ready)return error(r,"409 Conflict","请先关机并等待停止状态，再更新固件");
 esp_ota_handle_t handle=0;bool started=false;const char *failure="上传中断，启动分区未更改";uint8_t buf[1024];size_t received=0;int64_t last_data=now_ms(),deadline=last_data+180000;
 /* Inspect the complete prefix before touching the inactive application. */
 while(received<A3_OTA_PREFIX){int n=ota_receive(r,(char*)buf+received,A3_OTA_PREFIX-received,deadline,&last_data);if(n<=0)goto failed;received+=(size_t)n;}
 if(!ota_prefix_valid(buf,received,r->content_len,target->size)){failure="不是本项目 ESP32 应用镜像，未写入";goto failed;}
 if(!ota_still_safe()||now_ms()>deadline){failure="设备状态改变，升级已取消";goto failed;}
 if(esp_ota_begin(target,OTA_WITH_SEQUENTIAL_WRITES,&handle)!=ESP_OK){failure="备用分区准备失败";goto failed;}started=true;
 if(esp_ota_write(handle,buf,received)!=ESP_OK){failure="固件写入失败，启动分区未更改";goto failed;}
 while(received<r->content_len){
  if(!ota_still_safe()||now_ms()>deadline){failure="状态变化或上传超时，启动分区未更改";goto failed;}
  size_t need=r->content_len-received;if(need>sizeof(buf))need=sizeof(buf);int n=ota_receive(r,(char*)buf,need,deadline,&last_data);if(n<=0)goto failed;
  if(!ota_still_safe()||now_ms()>deadline)goto failed;
  if(esp_ota_write(handle,buf,n)!=ESP_OK){failure="固件写入失败，启动分区未更改";goto failed;}received+=(size_t)n;
 }
 if(!ota_still_safe()){failure="设备状态变化，启动分区未更改";goto failed;}
 esp_err_t verified=esp_ota_end(handle);started=false;
 if(verified!=ESP_OK){failure="固件完整性校验失败，启动分区未更改";goto failed;}
 if(!ota_still_safe()){failure="设备状态变化，启动分区未更改";goto failed;}
 if(esp_ota_set_boot_partition(target)!=ESP_OK){failure="启动分区设置失败，请保留串口恢复条件";goto failed;}
 xSemaphoreTake(mutex,portMAX_DELAY);snprintf(result,sizeof(result),"固件已校验，准备重启");xSemaphoreGive(mutex);
 cJSON *j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"ok",true);cJSON_AddStringToObject(j,"partition",target->label);
 bool rebooting=xTaskCreate(reboot_task,"ota_reboot",2048,NULL,5,NULL)==pdPASS;cJSON_AddBoolToObject(j,"rebooting",rebooting);cJSON_AddStringToObject(j,"message",rebooting?"校验成功，正在重启；重新连接热点后核对版本":"校验成功，请手动断电再通电以启动新版");return reply(r,j);
failed:
 if(started)esp_ota_abort(handle);
 char detail[192];xSemaphoreTake(mutex,portMAX_DELAY);ota_busy=false;
 snprintf(detail,sizeof(detail),"%s [rx=%u/%u code=%d age=%lld latch=%d]",failure,(unsigned)received,(unsigned)r->content_len,ota_recv_error,(long long)(now_ms()-state_at),ota_interrupted);
 snprintf(result,sizeof(result),"%s",detail);xSemaphoreGive(mutex);
 /* Close after early rejection so unread binary bytes cannot form a new request. */
 httpd_resp_set_hdr(r,"Connection","close");return error(r,"400 Bad Request",detail);
}

static esp_err_t network_post(httpd_req_t *r){
 if(!authorized(r))return error(r,"403 Forbidden","请重新打开本地页面");
 if(r->content_len<=0||r->content_len>1024)return error(r,"400 Bad Request","网络配置过长");
 char body[1025];int total=0;while(total<r->content_len){int n=httpd_req_recv(r,body+total,r->content_len-total);if(n<=0)return error(r,"408 Request Timeout","请求读取失败");total+=n;}body[total]=0;
 cJSON *j=parse(body);memset(body,0,sizeof(body));cJSON *ssid=cJSON_GetObjectItemCaseSensitive(j,"ssid"),*password=cJSON_GetObjectItemCaseSensitive(j,"password");
 if(!cJSON_IsString(ssid)||!cJSON_IsString(password)||!network_credentials_valid(ssid->valuestring,password->valuestring)){cJSON_Delete(j);return error(r,"400 Bad Request","Wi-Fi 名称需为 1–32 字节，密码需为 8–63 字节");}
 xSemaphoreTake(mutex,portMAX_DELAY);esp_err_t err=ota_busy||pending?ESP_ERR_INVALID_STATE:network_configure(ssid->valuestring,password->valuestring);if(err==ESP_OK)armed=false;xSemaphoreGive(mutex);
 memset(password->valuestring,0,strlen(password->valuestring));cJSON_Delete(j);
 if(err!=ESP_OK)return error(r,"409 Conflict","网络配置暂不可保存，请等待更新或控制完成后重试");
 cJSON *out=cJSON_CreateObject();cJSON_AddBoolToObject(out,"ok",true);return reply(r,out);
}

static esp_err_t goose_post(httpd_req_t *r){
 if(!authorized(r))return error(r,"403 Forbidden","请刷新页面");
 if(r->content_len<=0||r->content_len>64)return error(r,"400 Bad Request","无效绑定请求");
 char buf[65];int n=0;while(n<r->content_len){int k=httpd_req_recv(r,buf+n,r->content_len-n);if(k<=0)return error(r,"408 Request Timeout","读取失败");n+=k;}buf[n]=0;cJSON *j=parse(buf);const cJSON *m=cJSON_GetObjectItemCaseSensitive(j,"mac");
 xSemaphoreTake(mutex,portMAX_DELAY);const cJSON *source=cJSON_GetObjectItemCaseSensitive(j,"source");bool source_ok=cJSON_IsString(source)&&(!strcmp(source->valuestring,"external")||!strcmp(source->valuestring,"internal"));
 bool ok=!pending&&!ota_busy&&(source_ok?goose_source(!strcmp(source->valuestring,"external")):cJSON_IsString(m)&&goose_bind(m->valuestring));
 if(ok&&source_ok){cJSON_ReplaceItemInObject(config,"BLE",cJSON_CreateNumber(!strcmp(source->valuestring,"external")?1:0));ok=save_json("config",config);}
 if(ok&&source_ok){cJSON *w=cJSON_CreateObject();const char *keys[]={"wifiLink","wifiSignal","bleLink","bleSignal","gooseTemp","gooseHumity","UTCTime"};for(int i=0;i<7;i++)cJSON_AddNumberToObject(w,keys[i],0);goose_wifi(w);ok=send_frame("WiFiState",w);cJSON_Delete(w);}xSemaphoreGive(mutex);cJSON_Delete(j);if(!ok)return error(r,"409 Conflict","请等待操作完成并选择最近收到数据的温湿度计");cJSON *o=cJSON_CreateObject();cJSON_AddBoolToObject(o,"ok",true);return reply(r,o);
}
static esp_err_t diagnostic_post(httpd_req_t *r){
 if(!authorized(r))return error(r,"403 Forbidden","请刷新页面");
 if(r->content_len<=0||r->content_len>64)return error(r,"400 Bad Request","无效设置");
 char buf[65];int got=0;while(got<r->content_len){int n=httpd_req_recv(r,buf+got,r->content_len-got);if(n<=0)return error(r,"408 Request Timeout","读取失败");got+=n;}buf[got]=0;
 cJSON *j=parse(buf);const cJSON *enabled=cJSON_GetObjectItemCaseSensitive(j,"enabled"),*clear=cJSON_GetObjectItemCaseSensitive(j,"clear");
 if(!cJSON_IsBool(enabled)&&!cJSON_IsTrue(clear)){cJSON_Delete(j);return error(r,"400 Bad Request","无效设置");}
 xSemaphoreTake(mutex,portMAX_DELAY);bool ok=!ota_busy;
 if(ok&&cJSON_IsBool(enabled)){bool next=cJSON_IsTrue(enabled);ok=storage_ok&&nvs_set_u8(store,"diagnostic",next)==ESP_OK&&nvs_commit(store)==ESP_OK;if(ok)diagnostic_enabled=next;}
 if(ok){if(!diagnostic_enabled||cJSON_IsTrue(clear))clear_logs();last_log_clear=now_ms();goose_diagnostics(diagnostic_enabled);esp_log_level_set("*",diagnostic_enabled?ESP_LOG_INFO:ESP_LOG_WARN);}
 xSemaphoreGive(mutex);cJSON_Delete(j);if(!ok)return error(r,"409 Conflict","当前无法保存诊断设置");cJSON *res=cJSON_CreateObject();cJSON_AddBoolToObject(res,"ok",true);cJSON_AddBoolToObject(res,"enabled",diagnostic_enabled);return reply(r,res);
}
void app_main(void){
 ESP_LOGI(TAG,"Boot v%s running=%s",esp_app_get_description()->version,esp_ota_get_running_partition()->label);
 mutex=xSemaphoreCreateMutex();ESP_ERROR_CHECK(mutex?ESP_OK:ESP_ERR_NO_MEM);
 /* No erase-on-error or eFuse changes. OTA writes only on authenticated upload. */
 esp_err_t err=nvs_flash_init();storage_ok=err==ESP_OK&&nvs_open("a3local",NVS_READWRITE,&store)==ESP_OK;
 esp_vfs_spiffs_conf_t fs={.base_path="/factory",.partition_label="spiffs",.max_files=3,.format_if_mount_failed=false};fs_ok=esp_vfs_spiffs_register(&fs)==ESP_OK;
 uint8_t diag_pref=0;if(storage_ok&&nvs_get_u8(store,"diagnostic",&diag_pref)==ESP_OK)diagnostic_enabled=diag_pref==1;esp_log_level_set("*",diagnostic_enabled?ESP_LOG_INFO:ESP_LOG_WARN);
 config=load_json("config","/factory/airwater_config.txt");timers=load_json("timers","/factory/airwater_timer_config.txt");if(config&&!valid_config(config)){cJSON_Delete(config);config=NULL;}
 ESP_LOGI(TAG,"Startup configuration: storage=%d spiffs=%d config=%d timers=%d",storage_ok,fs_ok,config!=NULL,timers!=NULL);
 if(!config||!timers)note("配置未能读取：仅观察，控制保持禁用");
 uart_config_t uart={.baud_rate=115200,.data_bits=UART_DATA_8_BITS,.parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,.flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
 ESP_ERROR_CHECK(uart_driver_install(LINK,4096,2048,0,NULL,0));ESP_ERROR_CHECK(uart_param_config(LINK,&uart));ESP_ERROR_CHECK(uart_set_pin(LINK,17,16,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE));
 ESP_ERROR_CHECK(xTaskCreate(serial_task,"a3_uart",8192,NULL,8,&serial_handle)==pdPASS?ESP_OK:ESP_ERR_NO_MEM);
 ESP_ERROR_CHECK(network_init());goose_init();goose_diagnostics(diagnostic_enabled);
 uint8_t random[16];esp_fill_random(random,sizeof(random));for(int i=0;i<16;i++)snprintf(token+i*2,3,"%02x",random[i]);
 httpd_config_t hc=HTTPD_DEFAULT_CONFIG();hc.stack_size=8192;hc.max_uri_handlers=8;hc.recv_wait_timeout=5;httpd_handle_t server=NULL;ESP_ERROR_CHECK(httpd_start(&server,&hc));
 httpd_uri_t routes[]={{.uri="/",.method=HTTP_GET,.handler=index_get},{.uri="/api/status",.method=HTTP_GET,.handler=status_get},{.uri="/api/arm",.method=HTTP_POST,.handler=arm_post},{.uri="/api/control",.method=HTTP_POST,.handler=control_post},{.uri="/api/ota",.method=HTTP_POST,.handler=ota_post},{.uri="/api/network",.method=HTTP_POST,.handler=network_post},{.uri="/api/goose",.method=HTTP_POST,.handler=goose_post},{.uri="/api/diagnostics",.method=HTTP_POST,.handler=diagnostic_post}};
 for(size_t i=0;i<sizeof(routes)/sizeof(routes[0]);i++)ESP_ERROR_CHECK(httpd_register_uri_handler(server,&routes[i]));
 ESP_LOGI(TAG,"Local AP ready: %s; http://192.168.4.1 ; control requires explicit arming",A3_AP_SSID);
}
