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
#define MSG_PROMPT "msg> "
#define MSG_PORT 2560
#define DB_PROMPT "db> "
#define DB_PORT 2571
#define DSERV_PROMPT "dserv> "
#define DSERV_PORT 4620
#define STIM_PROMPT "stim> "
#define STIM_PORT 4612
#define PG_PROMPT "pg> "
#define PG_PORT 2572
#define GIT_PROMPT "git> "
#define GIT_PORT 2573
#define OPENIRIS_PROMPT "openiris> "
#define OPENIRIS_PORT 2574

void completion(const char *buf, linenoiseCompletions *lc) {
    if (buf[0] == 'h') {
        linenoiseAddCompletion(lc,"hello");
        linenoiseAddCompletion(lc,"hello there");
    }
}

void print_usage(char *prgname) {
    printf("Usage: %s [server] [options]\n", prgname);
    printf("  server        Server address (default: localhost)\n");
    printf("  -c command    Execute command and exit\n");
    printf("  -s service    Target service (ess, msg, db, dserv, stim, pg, git, openiris)\n");
    printf("  -h            Show this help\n");
    printf("\nExamples:\n");
    printf("  %s                    # Interactive mode with localhost\n", prgname);
    printf("  %s server.example.com # Interactive mode with specific server\n", prgname);
    printf("  %s -c \"return 100\"        # Execute 'status' command on localhost\n", prgname);
    printf("  %s -s db -c \"SELECT * FROM users\" # Execute SQL on db service\n", prgname);
    printf("  %s server.com -s msg -c \"expr 5*5\"    # Send message to specific server\n", prgname);
}

int get_port_for_service(char *service) {
    if (!strcmp(service, "ess")) return ESS_PORT;
    if (!strcmp(service, "msg")) return MSG_PORT;
    if (!strcmp(service, "db")) return DB_PORT;
    if (!strcmp(service, "dserv")) return DSERV_PORT;
    if (!strcmp(service, "stim")) return STIM_PORT;
    if (!strcmp(service, "pg")) return PG_PORT;
    if (!strcmp(service, "git")) return GIT_PORT;
    if (!strcmp(service, "openiris")) return OPENIRIS_PORT;
    return -1;
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

char *do_msg_command(char *server, int port, char *line, int n)
{
  char *buf;
  int sock = socket_open(server, port);
  
  if (sock < 0) return NULL;

  if (!sendMessage(sock, line, n)) return NULL;
  if (!receiveMessage(sock, &buf)) return NULL;
  if (buf) {
    if (strlen(buf)) {
      linenoiseHistoryAdd(line); /* Add to the history. */
      linenoiseHistorySave("history.txt"); /* Save the history on disk. */
    }
  }
  return buf;
}

int execute_single_command(char *server, int tcpport, char *command) {
    char *resultstr;
    
    if (tcpport == STIM_PORT || tcpport == MSG_PORT) {
        resultstr = do_msg_command(server, tcpport, command, strlen(command));
    } else {
        resultstr = sock_send(server, tcpport, command, strlen(command));
    }
    
    if (resultstr) {
        if (strlen(resultstr)) {
            printf("%s\n", resultstr);
        }
        if (tcpport == STIM_PORT || tcpport == MSG_PORT) {
            free(resultstr);
        }
        return 0; // Success
    }
    
    fprintf(stderr, "Error: Failed to execute command\n");
    return 1; // Error
}

int main (int argc, char *argv[])
{
  char *line;
  char *prgname = argv[0];
  int async = 0;
  char *server = "localhost";
  int tcpport = MSG_PORT;
  char *resultstr;
  char *prompt = MSG_PROMPT;
  char *command_to_execute = NULL;
  char *target_service = NULL;
  int interactive_mode = 1;
  int i;
  
#ifdef _MSC_VER
  init_w32_socket();
#endif

  // Parse command line arguments
  for (i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
          print_usage(prgname);
          return 0;
      } else if (!strcmp(argv[i], "-c")) {
          if (i + 1 < argc) {
              command_to_execute = argv[++i];
              interactive_mode = 0;
          } else {
              fprintf(stderr, "Error: -c option requires a command\n");
              print_usage(prgname);
              return 1;
          }
      } else if (!strcmp(argv[i], "-s")) {
          if (i + 1 < argc) {
              target_service = argv[++i];
          } else {
              fprintf(stderr, "Error: -s option requires a service name\n");
              print_usage(prgname);
              return 1;
          }
      } else if (argv[i][0] != '-') {
          // Assume it's a server name if it doesn't start with -
          server = argv[i];
      } else {
          fprintf(stderr, "Error: Unknown option %s\n", argv[i]);
          print_usage(prgname);
          return 1;
      }
  }
  
  // Set target service port if specified
  if (target_service) {
      tcpport = get_port_for_service(target_service);
      if (tcpport == -1) {
          fprintf(stderr, "Error: Unknown service '%s'\n", target_service);
          fprintf(stderr, "Valid services: ess, msg, db, dserv, stim, pg, git, openiris\n");
          return 1;
      }
  }
  
  // If command specified, execute it and exit
  if (command_to_execute) {
      int result = execute_single_command(server, tcpport, command_to_execute);
#ifdef _MSC_VER
      cleanup_w32_socket();
#endif
      return result;
  }
  
  // Continue with interactive mode
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

    if (!strcmp(line, "exit")) exit(0);
    
    if (line[0] != '\0' && line[0] != '/') {
      if (tcpport == STIM_PORT || tcpport == MSG_PORT) {
	resultstr = do_msg_command(server, tcpport, line, strlen(line));
	if (resultstr) {
	  if (strlen(resultstr)) printf("%s\n", resultstr);
	  free(resultstr);
	}
      }
      else {
	resultstr = sock_send(server, tcpport, line, strlen(line));
	if (resultstr) {
	  if (strlen(resultstr)) {
	    printf("%s\n", resultstr);
	  }
	}
	linenoiseHistoryAdd(line);
	linenoiseHistorySave("history.txt");
      }
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
    } else if (!strncmp(line, "/msg", 4)) {
      if (strlen(line) > 4) {
	resultstr = do_msg_command(server, MSG_PORT, &line[5],
				   strlen(line)-5);
	if (resultstr) {
	  if (strlen(resultstr)) printf("%s\n", resultstr);
	  free(resultstr);
	}
      }
      else {
	tcpport = MSG_PORT;
	prompt = MSG_PROMPT;
      }
    } else if (!strncmp(line, "/stim", 5)) {
      if (strlen(line) > 5) {
	resultstr = do_msg_command(server, STIM_PORT, &line[6],
				   strlen(line)-6);
	if (resultstr) {
	  if (strlen(resultstr)) printf("%s\n", resultstr);
	  free(resultstr);
	}
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
	prompt = PG_PROMPT;
      }
    } else if (!strncmp(line, "/git", 4)) {
      if (strlen(line) > 4) {
	resultstr = do_command(server, GIT_PORT, &line[5], strlen(line)-5);
	if (resultstr && strlen(resultstr)) printf("%s\n", resultstr);
      }
      else {
	tcpport = GIT_PORT;
	prompt = GIT_PROMPT;
      }
    } else if (!strncmp(line, "/openiris", 9)) {
      if (strlen(line) > 9) {
	resultstr = do_command(server, OPENIRIS_PORT, &line[10], strlen(line)-10);
	if (resultstr && strlen(resultstr)) printf("%s\n", resultstr);
      }
      else {
	tcpport = OPENIRIS_PORT;
	prompt = OPENIRIS_PROMPT;
      }
    } else if (line[0] == '/') {
      printf("Unrecognized command: %s\n", line);
    }
    free(line);
  }

#ifdef _MSC_VER
cleanup_w32_socket();
#endif
  return 0;
}
