#ifndef PAIRING_H
#define PAIRING_H

#include "network.h"

typedef enum { SETUP_MENU, SETUP_FORM, SETUP_BUSY, SETUP_DISCOVER, SETUP_CLOSED } SetupPhase;
typedef enum { SETUP_IDLE, SETUP_CONNECT, SETUP_PAIR, SETUP_SEARCH } SetupAction;

typedef struct {
    SetupPhase phase;
    BridgeEndpoint saved;
    char address[16];
    char port[6];
    char code[21];
    unsigned field;
    unsigned row;
    unsigned column;
    bool pairing;
    bool keypad_focus;
    char notice[128];
} Setup;

void setup_load(Setup *setup);
SetupAction setup_input(Setup *setup, u32 pressed, u32 repeat, BridgeEndpoint *endpoint);
bool setup_discovered(Setup *setup, const BridgeRequest *request, BridgeEndpoint *endpoint);
bool setup_receive(Setup *setup, const BridgeRequest *request, BridgeEndpoint *endpoint);
void setup_draw(Screen top, Screen bottom, const Setup *setup, u32 held);

#endif
