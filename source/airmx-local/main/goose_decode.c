#include "goose_decode.h"
/* Qingping FDCD / legacy CGG1 FFF9 service data, UUID excluded. Match original product allowlist,
 * then require a complete type 01 / length 04 temperature-humidity record. */
bool goose_decode(const uint8_t *p,size_t n,int *temperature,int *humidity){
 if(!p||!temperature||!humidity||n<14)return false;
 if(p[1]!=1&&p[1]!=7&&p[1]!=16&&p[1]!=22)return false;
 for(size_t i=8;i+2<=n;){size_t len=p[i+1];if(len>n-i-2)return false;
  if(p[i]==1&&len==4){int t=(int16_t)((unsigned)p[i+2]|((unsigned)p[i+3]<<8));int h=p[i+4]|(p[i+5]<<8);if(t < -400||t>850||h>999)return false;*temperature=t*10;*humidity=h*10;return true;}i+=2+len;
 }return false;
}
