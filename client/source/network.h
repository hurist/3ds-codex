#ifndef NETWORK_H
#define NETWORK_H

#include "composer.h"
#include <stddef.h>

#define RESPONSE_CAPACITY (120 * 1024)

typedef struct {
    char address[16];
    unsigned port;
    char identity[17];
    unsigned char key[32];
    size_t key_length;
} BridgeEndpoint;

typedef struct {
    BridgeEndpoint endpoint;
    char path[256];
    char session[33];
    char operation[33];
    char message[MESSAGE_CAPACITY];
    const void *audio;
    size_t audio_length;
    bool rejected;
    size_t response_length;
    char response[608 * 1024];
    char error[128];
} BridgeRequest;

void fetch_bridge(void *context);

#endif
