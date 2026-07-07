#ifndef SENDCLIENT_H
#define SENDCLIENT_H

#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <functional>
#include <memory>
#include <string>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <queue>
#include <cstring>

#include "sharedqueue.h"
#include "Datapoint.h"
#include "MatchDict.h"
#include "ClientRequest.h"

/*
 * SendClient
 *
 *  Owned by shared_ptr: the SendTable holds one reference and the
 * client's own thread holds another, so a pointer obtained from the
 * table can never dangle — operations on a client that has already
 * been shut down are harmless no-ops.
 */
class SendClient {
 public:
  enum sendclient_type { SOCKET_CLIENT, QUEUE_CLIENT };

  sendclient_type type;
  std::atomic<int> active{1};	/* set to 0 if connection bad;
				   written by the client thread,
				   read by the send thread */
  std::string key;		/* send_table key, set at registration */
  char *host = nullptr;
  int port = 0;
  int fd = -1;			/* socket to write to         */
  int send_binary = 0;
  int send_json = 0;
  //  json_encoder_t *json_encoder;
  // point queue for incoming notifications
  SharedQueue<ds_datapoint_t *> dpoint_queue;

  // client_request queue to push points to
  SharedQueue<client_request_t> *client_queue = nullptr;

  ds_datapoint_t shutdown_dpoint = {}; /* dpoint signal shutdown   */

  MatchDict matches;

  static void
    send_client_process(std::shared_ptr<SendClient> sendclient);

  SendClient(int socket, char *hoststr, int port, uint8_t flags);
  SendClient(SharedQueue<client_request_t> *client_queue);
  ~SendClient();
  int send_dpoint(ds_datapoint_t *dpoint);
};

#endif
