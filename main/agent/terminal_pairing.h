#pragma once

#include <stdbool.h>
#include <stddef.h>

enum { TERMINAL_SERVER_SIZE = 256, TERMINAL_TOKEN_SIZE = 44 };

void terminal_load_pairing(void);
void terminal_seed_pairing(void);
bool terminal_is_paired(void);
const char *terminal_server_url(void);
const char *terminal_device_token(void);
bool terminal_pair_device(const char *address, const char *code, char *message, size_t size);
