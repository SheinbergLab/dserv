#ifndef CLIENT_REQUEST_H
#define CLIENT_REQUEST_H

#include "sharedqueue.h"

/*
 * client_request
 *   requests to main process can be:
 *     REQ_SCRIPT: tcl script
 *     REQ_SCRIPT_NOREPLY: tcl script but don't wait for reply
 *     REQ_SCRIPT_WS_ASYNC: tcl script, send result back via WebSocket (non-blocking)
 *     REQ_DPOINT: add datapoint
 *     REQ_DPOINT_SCRIPT: datapoint for trigger processing
 *     REQ_TIMER: timer id
 *     REQ_REWARD_TIMER: timer pin
 *     REQ_SHUTDOWN: shutdown message
 */

enum request_t { REQ_SCRIPT, REQ_SCRIPT_NOREPLY,
		 REQ_SCRIPT_WS_ASYNC,
		 REQ_TRIGGER, REQ_DPOINT, REQ_DPOINT_SCRIPT, REQ_TIMER,
		 REQ_REWARD_TIMER, REQ_ADC_TIMER, REQ_SHUTDOWN };

typedef struct client_request_s {
  request_t type;
  int timer_id;
  std::string script;
  SharedQueue<std::string> *rqueue;
  ds_datapoint_t *dpoint;
  int socket_fd = -1;           // Socket FD if request came from socket (-1 if not)
  std::string websocket_id;     // WebSocket ID if request came from websocket (empty if not)
  std::string request_id;       // Client-provided request ID for async WebSocket responses
} client_request_t;

#endif
