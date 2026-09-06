#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
bool network_credentials_valid(const char *ssid,const char *password);
esp_err_t network_init(void);
esp_err_t network_configure(const char *ssid,const char *password);
cJSON *network_status(void);
