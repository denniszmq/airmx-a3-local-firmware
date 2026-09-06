#include <stdbool.h>
#include <string.h>
bool network_credentials_valid(const char *ssid,const char *password){
 if(!ssid||!password)return false;
 size_t s=strlen(ssid),p=strlen(password);
 return s>=1&&s<=32&&p>=8&&p<=63;
}
