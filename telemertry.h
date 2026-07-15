#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>

typedef struct {
    uint32_t device_id;
    char     channel_name[32]; // Populated using OID .1.3.6.1.4.1.40297.1.2.4.9.0
    float    temperature;
    float    voltage;
    uint16_t status_flags;
} RepeaterTelemetry;

#endif
