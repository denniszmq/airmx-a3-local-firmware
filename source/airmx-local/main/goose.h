#pragma once
#include "cJSON.h"
#include <stdbool.h>
void goose_init(void);
cJSON *goose_status(void);
bool goose_bind(const char *mac);
void goose_wifi(cJSON *wifi);

bool goose_source(bool external);

typedef struct {int link,signal,temperature,humidity;} goose_values;
void goose_get_values(goose_values *out);
void goose_diagnostics(bool enabled);
