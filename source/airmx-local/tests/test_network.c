#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
bool network_credentials_valid(const char*,const char*);
int main(void){char ssid[34],password[65];memset(ssid,'s',33);ssid[33]=0;memset(password,'p',64);password[64]=0;
 assert(!network_credentials_valid(NULL,"12345678"));assert(!network_credentials_valid("home",NULL));assert(!network_credentials_valid("","12345678"));assert(!network_credentials_valid("home","1234567"));assert(!network_credentials_valid(ssid,"12345678"));ssid[32]=0;assert(network_credentials_valid(ssid,"12345678"));assert(!network_credentials_valid("home",password));password[63]=0;assert(network_credentials_valid("home",password));assert(network_credentials_valid("家庭 Wi-Fi","12345678"));puts("PASS: network credential byte limits, nulls, empty values and UTF-8");}
