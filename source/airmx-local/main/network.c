#include "network.h"
#include "local_credentials.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
typedef struct {char ssid[33];char password[64];} credentials;
static SemaphoreHandle_t lock;
static QueueHandle_t changes;
static nvs_handle_t storage;
static bool persistent,connected,changing,radio_ready;
static credentials saved;
static char ip[16],message[128]="尚未配置家庭 Wi-Fi";
static int reason;
static int64_t retry_at;
static void event(void *arg,esp_event_base_t base,int32_t id,void *data){
 (void)arg;xSemaphoreTake(lock,portMAX_DELAY);
 if(base==IP_EVENT&&id==IP_EVENT_STA_GOT_IP){ip_event_got_ip_t *e=data;snprintf(ip,sizeof(ip),IPSTR,IP2STR(&e->ip_info.ip));connected=true;reason=0;snprintf(message,sizeof(message),"已连接家庭 Wi-Fi");ESP_LOGI("A3-NET","LAN address http://%s",ip);}
 else if(base==IP_EVENT&&id==IP_EVENT_STA_LOST_IP){connected=false;ip[0]=0;snprintf(message,sizeof(message),"正在重新获取局域网地址");}
 else if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_DISCONNECTED){connected=false;ip[0]=0;reason=((wifi_event_sta_disconnected_t*)data)->reason;retry_at=esp_timer_get_time()+10000000;snprintf(message,sizeof(message),"连接未成功或已断开，将自动重连；可通过设备热点修改密码");}
 xSemaphoreGive(lock);
}
static void worker(void *arg){
 (void)arg;credentials next;
 for(;;){bool changed=xQueueReceive(changes,&next,pdMS_TO_TICKS(1000))==pdTRUE;
  if(changed){
   esp_wifi_disconnect();wifi_config_t sta={0};memcpy(sta.sta.ssid,next.ssid,strlen(next.ssid));memcpy(sta.sta.password,next.password,strlen(next.password));sta.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;sta.sta.pmf_cfg.capable=true;
   esp_err_t err=esp_wifi_set_config(WIFI_IF_STA,&sta);
   xSemaphoreTake(lock,portMAX_DELAY);connected=false;ip[0]=0;retry_at=0;changing=false;radio_ready=err==ESP_OK;snprintf(message,sizeof(message),err==ESP_OK?"正在连接家庭 Wi-Fi":"网络配置失败，请通过设备热点重试");xSemaphoreGive(lock);
   memset(&sta,0,sizeof(sta));memset(&next,0,sizeof(next));
  }
  xSemaphoreTake(lock,portMAX_DELAY);bool attempt=radio_ready&&saved.ssid[0]&&!connected&&!changing&&esp_timer_get_time()>=retry_at;if(attempt)retry_at=esp_timer_get_time()+15000000;xSemaphoreGive(lock);
  if(attempt)esp_wifi_connect();
 }
}
esp_err_t network_configure(const char *ssid,const char *password){
 if(!network_credentials_valid(ssid,password))return ESP_ERR_INVALID_ARG;
 credentials next={0};memcpy(next.ssid,ssid,strlen(ssid));memcpy(next.password,password,strlen(password));
 xSemaphoreTake(lock,portMAX_DELAY);
 if(!persistent||changing){xSemaphoreGive(lock);memset(&next,0,sizeof(next));return ESP_ERR_INVALID_STATE;}
 esp_err_t err=nvs_set_blob(storage,"wifi",&next,sizeof(next));if(err==ESP_OK)err=nvs_commit(storage);
 if(err==ESP_OK){saved=next;changing=true;snprintf(message,sizeof(message),"配置已保存，正在切换网络");xQueueOverwrite(changes,&next);}
 xSemaphoreGive(lock);memset(&next,0,sizeof(next));return err;
}
cJSON *network_status(void){
 xSemaphoreTake(lock,portMAX_DELAY);cJSON *j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"configured",saved.ssid[0]!=0);cJSON_AddBoolToObject(j,"connected",connected);cJSON_AddBoolToObject(j,"canConfigure",persistent&&!changing);cJSON_AddStringToObject(j,"ssid",saved.ssid);cJSON_AddStringToObject(j,"ip",ip);cJSON_AddStringToObject(j,"apIp","192.168.4.1");cJSON_AddStringToObject(j,"message",message);cJSON_AddNumberToObject(j,"disconnectReason",reason);xSemaphoreGive(lock);return j;
}
esp_err_t network_init(void){
 lock=xSemaphoreCreateMutex();changes=xQueueCreate(1,sizeof(credentials));if(!lock||!changes)return ESP_ERR_NO_MEM;
 persistent=nvs_open("a3network",NVS_READWRITE,&storage)==ESP_OK;
 size_t n=sizeof(saved);if(!persistent||nvs_get_blob(storage,"wifi",&saved,&n)!=ESP_OK||n!=sizeof(saved)||!memchr(saved.ssid,0,sizeof(saved.ssid))||!memchr(saved.password,0,sizeof(saved.password))||!network_credentials_valid(saved.ssid,saved.password))memset(&saved,0,sizeof(saved));
 ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());esp_netif_create_default_wifi_ap();esp_netif_t *sta=esp_netif_create_default_wifi_sta();esp_netif_set_hostname(sta,"airmx-a3");
 ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,event,NULL));ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,ESP_EVENT_ANY_ID,event,NULL));
 wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&init));ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
 wifi_config_t ap={.ap={.ssid=A3_AP_SSID,.password=A3_AP_PASSWORD,.ssid_len=sizeof(A3_AP_SSID)-1,.channel=6,.max_connection=2,.authmode=WIFI_AUTH_WPA2_PSK,.pmf_cfg={.required=false}}};
 ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&ap));ESP_ERROR_CHECK(esp_wifi_start());
 if(saved.ssid[0]){changing=true;xQueueOverwrite(changes,&saved);}
 return xTaskCreate(worker,"a3_network",4096,NULL,4,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
