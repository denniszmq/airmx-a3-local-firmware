#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
bool goose_decode(const uint8_t *p,size_t n,int *temperature,int *humidity);
