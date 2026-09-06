#include "water_usage.h"
#include <assert.h>
#include <stdio.h>
static void feed(water_usage *w,int64_t from,int64_t to,int level,int speed){for(int64_t t=from;t<=to;t+=5000)water_feed(w,t,true,true,level,0,50,speed);}
int main(void){
 water_usage w={0};feed(&w,0,3600000,90,56);assert(w.count==1&&!water_estimate(&w,3600000).finite);
 feed(&w,3605000,5400000,85,56);assert(w.count==2&&!water_estimate(&w,5400000).finite);
 feed(&w,5405000,7200000,80,56);water_result r=water_estimate(&w,7200000);assert(r.finite&&r.hours>13&&r.hours<15);
 feed(&w,7205000,10800000,80,56);r=water_estimate(&w,10800000);assert(r.finite&&r.hours>20&&r.hours<22); /* unchanged plateau retained */
 feed(&w,10805000,10840000,85,56);assert(w.count==1&&!water_estimate(&w,10840000).finite);
 feed(&w,10845000,10880000,80,70);assert(!water_estimate(&w,10880000).finite);assert(!water_estimate(&w,10900000).finite);
 water_feed(&w,10905000,true,false,80,0,50,70);assert(!w.active);
 water_usage fast={0};feed(&fast,0,30000,90,56);feed(&fast,35000,65000,80,56);assert(!water_estimate(&fast,65000).finite);
 water_feed(&fast,70000,true,true,110,0,50,56);assert(!fast.active); /* uncalibrated out-of-range */
 for(int64_t t=0;t<5000000000LL;t+=5000){water_feed(&w,t,true,true,60,0,50,56);assert(w.count<=4&&w.smooth_count<=5&&w.smooth_next<5);}assert(!water_estimate(&w,4999995000LL).finite);
 puts("PASS: event-based water windows, multi-hour plateaus, minimum decline/time, refill/change/stale reset, million samples bounded");
}
