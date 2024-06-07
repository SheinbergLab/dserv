#ifndef SENDCLIENT_H
#define SENDCLIENT_H

#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <queue>
#include <cstring>

#include "sharedqueue.h"
#include "Datapoint.h"
#include "MatchDict.h"
#include "ClientRequest.h"

class SendClient {
 public:
  enum sendclient_type { SOCKET_CLIENT, QUEUE_CLIENT };

  sendclient_type type;
  int active;			/* set to 0 if connection bad */
  char *host;		
  int port;
  int fd;			/* socket to write to         */
  int send_binary;
  int send_json;
  //  json_encoder_t *json_encoder;
  // point queue for incoming notifications
  SharedQueue<ds_datapoint_t *> dpoint_queue;

  // client_request queue to push points to
  SharedQueue<client_request_t> *client_queue;
    
  ds_datapoint_t shutdown_dpoint; /* dpoint signal shutdown   */
  
  MatchDict matches;

  static void
    send_client_process(SendClient *);
  
  SendClient(int socket, char *hoststr, int port, uint8_t flags);
  SendClient(SharedQueue<client_request_t> *client_queue);
  ~SendClient();
  int send_dpoint(ds_datapoint_t *dpoint);
};

#endif
