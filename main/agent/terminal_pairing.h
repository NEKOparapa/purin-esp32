#pragma once

#include <stdbool.h>
#include <stddef.h>

enum { TERMINAL_SERVER_SIZE = 256, TERMINAL_TOKEN_SIZE = 44 };
enum { TERMINAL_PAIR_LINK_SIZE = TERMINAL_SERVER_SIZE + sizeof("/#wm-pair=12345-12345") };

void terminal_load_pairing(void);
void terminal_seed_pairing(void);
bool terminal_is_paired(void);
const char *terminal_server_url(void);
const char *terminal_device_token(void);
const char *terminal_pair_link_invitation(const char *link);
bool terminal_pair_device(const char *link, char *message, size_t size);
