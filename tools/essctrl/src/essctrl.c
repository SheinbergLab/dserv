#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <string.h>
#include "linenoise.h"
#include "sockapi.h"

#define ESS_PROMPT "ess> "
#define ESS_PORT 2570
#define DB_PROMPT "db> "
#define DB_PORT 2571
#define DSERV_PROMPT "dserv> "
#define DSERV_PORT 4620
#define STIM_PROMPT "stim> "
#define STIM_PORT 4610
#define PG_PROMPT "pg> "
#define PG_PORT 2572

void completion(const char *buf, linenoiseCompletions *lc) {
    if (buf[0] == 'h') {
        linenoiseAddCompletion(lc,"hello");
        linenoiseAddCompletion(lc,"hello there");
    }
}

char *do_command(char *server, int tcpport, char *line, int n)
{
  char *resultstr = sock_send(server, tcpport, line, strlen(line));
  if (resultstr) {
    if (strlen(resultstr)) {
      linenoiseHistoryAdd(line); /* Add to the history. */
      linenoiseHistorySave("history.txt"); /* Save the history on disk. */
    }
   }
  return resultstr;
}


int main (int argc, char *argv[])
{
  char *line;
  char *prgname = argv[0];
  int async = 0;
  char *server;
  int tcpport = ESS_PORT;;
  char *resultstr;
  char *prompt = "ess> ";
  
#ifdef _MSC_VER
  init_w32_socket();
#endif

  if (argc < 2) {
    server = "localhost";
  }
  else server = argv[1];
  
  linenoiseInstallWindowChangeHandler();

  /* Set the completion callback. This will be called every time the
   * user uses the <tab> key. */
  linenoiseSetCompletionCallback(completion);
  
  /* Load history from file. The history file is just a plain text file
   * where entries are separated by newlines. */
  linenoiseHistoryLoad("history.txt"); /* Load the history at startup */
  
  /* Now this is the main loop of the typical linenoise-based application.
   * The call to linenoise() will block as long as the user types something
   * and presses enter.
   *
   * The typed string is returned as a malloc() allocated string by
   * linenoise, so the user needs to free() it. */
  
  while(1) {
      line = linenoise(prompt);
      if (line == NULL) break;
    /* Do something with the string. */
    if (!strcmp(line, "exit")) exit(0);
    
    if (line[0] != '\0' && line[0] != '/') {
      resultstr = sock_send(server, tcpport, line, strlen(line));
      if (resultstr) {
      	if (strlen(resultstr)) {
	       printf("%s\n", resultstr);
      	}
      }
      linenoiseHistoryAdd(line); /* Add to the history. */
      linenoiseHistorySave("history.txt"); /* Save the history on disk. */
    } else if (!strncmp(line,"/historylen",11)) {
      /* The "/historylen" command will change the history len. */
      int len = atoi(line+11);
      linenoiseHistorySetMaxLen(len);
    } else if (!strncmp(line, "/ess", 4)) {
      if (strlen(line) > 4) {
	resultstr = do_command(server, ESS_PORT, &line[5], strlen(line)-5);
	if (resultstr && strlen(resultstr)) printf("%s\n", resultstr);
      }
      else {
	tcpport = ESS_PORT;
	prompt = ESS_PROMPT;
      }
    } else if (!strncmp(line, "/dserv", 6)) {
      if (strlen(line) > 6) {
	resultstr = do_command(server, DSERV_PORT, &line[7], strlen(line)-7);
	if (resultstr && strlen(resultstr)) printf("%s\n", resultstr);
      }
      else {
	tcpport = DSERV_PORT;
	prompt = DSERV_PROMPT;
      }
    } else if (!strncmp(line, "/stim", 5)) {
      if (strlen(line) > 5) {
	resultstr = do_command(server, STIM_PORT, &line[6], strlen(line)-6);
	if (resultstr && strlen(resultstr)) printf("%s\n", resultstr);
      }
      else {
	tcpport = STIM_PORT;
	prompt = STIM_PROMPT;
      }
    } else if (!strncmp(line, "/db", 3)) {
      if (strlen(line) > 3) {
	resultstr = do_command(server, DB_PORT, &line[4], strlen(line)-4);
	if (resultstr && strlen(resultstr)) printf("%s\n", resultstr);
      }
      else {
	tcpport = DB_PORT;
	prompt = DB_PROMPT;
      }
    } else if (!strncmp(line, "/pg", 3)) {
      if (strlen(line) > 3) {
	resultstr = do_command(server, PG_PORT, &line[4], strlen(line)-4);
	if (resultstr && strlen(resultstr)) printf("%s\n", resultstr);
      }
      else {
	tcpport = PG_PORT;
	prompt =PG_PROMPT;
      }
    } else if (line[0] == '/') {
      printf("Unreconized command: %s\n", line);
    }
    free(line);
  }

#ifdef _MSC_VER
cleanup_w32_socket();
#endif
  return 0;
}
