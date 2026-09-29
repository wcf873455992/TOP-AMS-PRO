#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void dns_server_start(const char *ap_ip);
void dns_server_stop(void);

#ifdef __cplusplus
}
#endif
