#include "ota_guard.h"
#include <string.h>
/* ESP32 image header (24), first segment header (8), app descriptor (256).
 * Only an early format filter; esp_ota_end performs the full image validation. */
bool ota_prefix_valid(const uint8_t *p,size_t n,size_t total,size_t capacity){
 if(!p||n<A3_OTA_PREFIX||total<A3_OTA_PREFIX+32||total>capacity)return false;
 if(p[0]!=0xe9||p[1]<1||p[1]>16||p[12]!=0||p[13]!=0||p[23]!=1)return false;
 if(p[32]!=0x32||p[33]!=0x54||p[34]!=0xcd||p[35]!=0xab)return false;
 return !memcmp(p+80,"airmx_local\0",12);
}
