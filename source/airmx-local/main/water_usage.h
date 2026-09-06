#pragma once
#include <stdint.h>
#include <stdbool.h>
#define WATER_WINDOW 4
/* Latest three level-drop events plus baseline; bounded storage, monotonic clock. */
typedef struct {int64_t at;int level;} water_point;
typedef struct {
 water_point points[WATER_WINDOW];unsigned count;
 int smooth[5];unsigned smooth_count,smooth_next;
 int mode,target,cadr,last_level;int64_t last_feed;bool active;
 const char *reason;
} water_usage;
typedef struct {bool finite;double hours,units_per_hour;unsigned intervals;int64_t next_ms;const char *reason;} water_result;
void water_feed(water_usage *w,int64_t now,bool valid,bool running,int level,int mode,int target,int cadr);
water_result water_estimate(const water_usage *w,int64_t now);
