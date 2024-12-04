#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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

char *hints(const char *buf, int *color, int *bold) {
    if (!strcasecmp(buf,"hello")) {
        *color = 35;
        *bold = 0;
        return " World";
    }
    return NULL;
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
  
  if (argc < 2) {
    server = "localhost";
  }
  else server = argv[1];
  

  /* Parse options, with --multiline we enable multi line editing. */
  while(argc > 2) {
    argc--;
    argv++;
    if (!strcmp(*argv,"--multiline")) {
      linenoiseSetMultiLine(1);
      printf("Multi-line mode enabled.\n");
    } else if (!strcmp(*argv,"--keycodes")) {
      linenoisePrintKeyCodes();
      exit(0);
    } else if (!strcmp(*argv,"--async")) {
      async = 1;
    } else {
      fprintf(stderr, "Usage: %s [--multiline] [--keycodes] [--async]\n", prgname);
      exit(1);
    }
  }
  
  /* Set the completion callback. This will be called every time the
   * user uses the <tab> key. */
  linenoiseSetCompletionCallback(completion);
  linenoiseSetHintsCallback(hints);
  
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
    if (!async) {
      line = linenoise(prompt);
      if (line == NULL) break;
    } else {
      /* Asynchronous mode using the multiplexing API: wait for
       * data on stdin, and simulate async data coming from some source
       * using the select(2) timeout. */
      struct linenoiseState ls;
      char buf[1024];
      linenoiseEditStart(&ls,-1,-1,buf,sizeof(buf), prompt);
      while(1) {
	fd_set readfds;
	struct timeval tv;
	int retval;
	
	FD_ZERO(&readfds);
	FD_SET(ls.ifd, &readfds);
	tv.tv_sec = 1; // 1 sec timeout
	tv.tv_usec = 0;
	
	retval = select(ls.ifd+1, &readfds, NULL, NULL, &tv);
	if (retval == -1) {
	  perror("select()");
	  exit(1);
	} else if (retval) {
	  line = linenoiseEditFeed(&ls);
	  /* A NULL return means: line editing is continuing.
	   * Otherwise the user hit enter or stopped editing
	   * (CTRL+C/D). */
	  if (line != linenoiseEditMore) break;
	} else {
	  // Timeout occurred
	  static int counter = 0;
	  linenoiseHide(&ls);
	  printf("Async output %d.\n", counter++);
	  linenoiseShow(&ls);
	}
      }
      linenoiseEditStop(&ls);
      if (line == NULL) exit(0); /* Ctrl+D/C. */
    }
    
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
    } else if (!strncmp(line, "/mask", 5)) {
      linenoiseMaskModeEnable();
    } else if (!strncmp(line, "/unmask", 7)) {
      linenoiseMaskModeDisable();
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
  return 0;
}
