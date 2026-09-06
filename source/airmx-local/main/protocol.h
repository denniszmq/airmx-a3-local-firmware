#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#define FRAME_CAP 2048
typedef struct {char data[FRAME_CAP];size_t n;bool active;char prev;} frame_parser;
typedef void (*frame_fn)(const char *,void *);
void frame_feed(frame_parser *,const uint8_t *,size_t,frame_fn,void *);
bool json_int(const cJSON *,const char *,int *);
bool state_valid(const cJSON *);
int configured_speed(int reported);
/* action power(0/1), speed(1..5), mode(0..2), refill(0/1). No arbitrary fields. */
cJSON *make_control(const cJSON *state,const cJSON *config,const char *action,int value);
bool request_matches(const cJSON *expected,const cJSON *state);

/* Accept only a complete matching controller ACK; preserve telemetry/config. */
bool apply_control_ack(const cJSON *expected,const cJSON *ack,cJSON *state);

cJSON *make_target_config(const cJSON *state,const cJSON *config,int target);
