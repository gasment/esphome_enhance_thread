#pragma once
#include "cJSON.h"

// Read-only snapshot; never replaces the advertising-proxy callbacks.
cJSON *thread_br_srp_snapshot(void);
