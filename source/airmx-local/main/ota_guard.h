#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define A3_OTA_PREFIX 288
bool ota_prefix_valid(const uint8_t *p,size_t prefix_len,size_t image_len,size_t capacity);
