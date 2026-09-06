#include "water_usage.h"
#include <string.h>
#include <stdlib.h>
#define MIN_WINDOW 1800000LL
static void reset(water_usage *w,const char *why){memset(w,0,sizeof(*w));w->reason=why;}
void water_feed(water_usage *w,int64_t now,bool valid,bool running,int level,int mode,int target,int cadr){
 if(!valid||level<0||level>100){reset(w,"stale");return;}
 if(!running||mode>2||level<=10){reset(w,"stopped");return;}
 const char *why="collecting";
 if(w->active){
  if(now-w->last_feed>15000){reset(w,"stale");why="stale";}
  else if(mode!=w->mode||target!=w->target||(mode==0&&abs(cadr-w->cadr)>2)){reset(w,"changed");why="changed";}
 }
 if(!w->active){w->active=true;w->mode=mode;w->target=target;w->cadr=cadr;w->reason=why;}
 w->last_feed=now;w->smooth[w->smooth_next++%5]=level;if(w->smooth_count<5)w->smooth_count++;
 int sorted[5];memcpy(sorted,w->smooth,w->smooth_count*sizeof(int));
 for(unsigned i=1;i<w->smooth_count;i++)for(unsigned j=i;j>0&&sorted[j]<sorted[j-1];j--){int t=sorted[j];sorted[j]=sorted[j-1];sorted[j-1]=t;}
 int median=sorted[w->smooth_count/2];
 if(w->smooth_count<5)return;
 if(w->count&&median>=w->last_level+5){w->count=0;w->reason="refilled";}
 w->last_level=median;
 if(!w->count||median<=w->points[w->count-1].level-5){
  if(w->count==WATER_WINDOW){memmove(w->points,w->points+1,(WATER_WINDOW-1)*sizeof(water_point));w->count--;}
  w->points[w->count++]=(water_point){.at=now,.level=median};
 }
 /* Avoid even a theoretical counter wrap affecting the median ring. */
 w->smooth_next%=5;
}
water_result water_estimate(const water_usage *w,int64_t now){
 water_result r={.reason=w->reason?w->reason:"collecting",.next_ms=0};
 if(!w->active)return r;
 if(now-w->last_feed>15000){r.reason="stale";return r;}
 if(w->count<2)return r;
 r.intervals=w->count-1;
 int drop=w->points[0].level-w->last_level;
 int64_t elapsed=now-w->points[0].at;
 /* Retain unchanged plateaus, even for hours. No fixed 45-minute expiry.
    Include time since the last drop so an old high rate cannot persist. */
 if(drop<10||elapsed<MIN_WINDOW){r.reason="collecting";return r;}
 r.units_per_hour=drop/(elapsed/3600000.0);
 if(r.units_per_hour<=0){r.reason="no_drop";return r;}
 r.hours=(w->last_level-10)/r.units_per_hour;
 r.finite=true;r.reason="estimate";return r;
}
