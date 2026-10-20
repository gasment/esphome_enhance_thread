#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t thread_br_web_start(uint16_t port, const char *authorization, bool use_psram);
void thread_br_web_stop(void);
typedef struct {
    void *context;
    cJSON *(*get)(void *, const char *, int *);
    cJSON *(*post)(void *, const char *, const cJSON *, int *);
} thread_br_thread_gateway_web_t;
void thread_br_web_set_thread_gateway(const thread_br_thread_gateway_web_t *thread_gateway);
#ifdef __cplusplus
}
#endif
