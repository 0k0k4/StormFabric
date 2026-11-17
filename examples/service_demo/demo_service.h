#pragma once
// The contract between service_provider and service_consumer: a header the provider ships
// for the mods that use its service. Append members only; bump the minor version then.
#include <stdint.h>

#define DEMO_COUNTER_SERVICE "demo.counter"
#define DEMO_COUNTER_VERSION SW_SERVICE_VERSION(1, 0)

typedef struct DemoCounterService {
    uint32_t structSize;
    // Adds `amount` to a process-wide counter and returns the new value. Any thread.
    int64_t (*add)(int64_t amount);
    // Name of the providing mod, for logs.
    const char* (*providerName)(void);
} DemoCounterService;
