#pragma once

#include <stdbool.h>
#include "esp_err.h"

enum { AGENT_INPUT_BYTES = 769, AGENT_ID_BYTES = 80 };
typedef enum { AGENT_SEND, AGENT_SYNC, AGENT_INBOX, AGENT_READ, AGENT_CAPTURE } agent_action_t;

typedef struct agent_reply {
    char id[AGENT_ID_BYTES];
    char *text;
    bool unread;
    bool notification;
    struct agent_reply *next;
} agent_reply_t;

typedef struct {
    char status[112];
    char message_id[AGENT_ID_BYTES];
    char question[AGENT_INPUT_BYTES];
    agent_reply_t *replies;
    bool poll_job;
    bool sent;
    bool acknowledged;
} agent_result_t;

esp_err_t agent_client_init(void);
// All three functions below run on the UI task. One outstanding operation at a time.
bool agent_client_start(agent_action_t action, const char *value);
agent_result_t *agent_client_take_result(void);
void agent_result_free(agent_result_t *result);
