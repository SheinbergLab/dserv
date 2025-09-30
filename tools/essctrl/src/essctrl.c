#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <string.h>
#include "linenoise.h"
#include "sockapi.h"

#define LEGACY_PROMPT "legacy> "
#define LEGACY_PORT 2570
#define ESS_PROMPT "ess> "
#define ESS_PORT 2560
#define DB_PROMPT "db> "
#define DB_PORT 2571
#define DSERV_PROMPT "dserv> "
#define DSERV_PORT 4620
#define VSTREAM_PROMPT "vstream> "
#define VSTREAM_PORT 4630
#define STIM_PROMPT "stim> "
#define STIM_PORT 4612
#define PG_PROMPT "pg> "
#define PG_PORT 2572
#define GIT_PROMPT "git> "
#define GIT_PORT 2573
#define OPENIRIS_PROMPT "openiris> "
#define OPENIRIS_PORT 2574

// ANSI color codes
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_RESET   "\x1b[0m"

// Error constants
#define ERROR_PREFIX "!TCL_ERROR "
#define ERROR_PREFIX_LEN 11

// Global flag to track if we're in interactive mode
int g_interactive_mode = 1;

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
    printf("  -s service    Target service (ess, legacy, db, dserv, stim, pg, git, openiris)\n");
    printf("  -h            Show this help\n");
    printf("\nExamples:\n");
    printf("  %s                    # Interactive mode with localhost\n", prgname);
    printf("  %s server.example.com # Interactive mode with specific server\n", prgname);
    printf("  %s -c \"return 100\"        # Execute 'status' command on localhost\n", prgname);
    printf("  %s -s db -c \"SELECT * FROM users\" # Execute SQL on db service\n", prgname);
    printf("  %s server.com -s ess -c \"expr 5*5\"    # Send message to specific server\n", prgname);
}

int get_port_for_service(char *service) {
    if (!strcmp(service, "ess")) return ESS_PORT;
    if (!strcmp(service, "legacy")) return LEGACY_PORT;
    if (!strcmp(service, "db")) return DB_PORT;
    if (!strcmp(service, "dserv")) return DSERV_PORT;
    if (!strcmp(service, "vstream")) return VSTREAM_PORT;
    if (!strcmp(service, "stim")) return STIM_PORT;
    if (!strcmp(service, "pg")) return PG_PORT;
    if (!strcmp(service, "git")) return GIT_PORT;
    if (!strcmp(service, "openiris")) return OPENIRIS_PORT;
    return -1;
}

// Check if terminal supports colors
int supports_color() {
#ifdef _WIN32
    // On Windows, check if we're in a modern terminal
    char* term = getenv("TERM");
    return (term != NULL);
#else
    // On Unix/Linux, check if stdout is a terminal and TERM is set
    return isatty(STDOUT_FILENO) && getenv("TERM") != NULL;
#endif
}


// Check if string contains only whitespace characters
int is_whitespace_only(char *str) {
    if (!str) return 1;
    while (*str) {
        if (*str != ' ' && *str != '\t' && *str != '\n' && *str != '\r') {
            return 0;
        }
        str++;
    }
    return 1;
}

// Process server response and handle errors
// Returns 0 for success, 1 for error
int process_response(char *response, int interactive) {
  if (!response || strlen(response) == 0) {
    return 0; // Empty response is not an error
  }
  
  // Check if response is only whitespace - not an error
  if (is_whitespace_only(response)) {
    if (strlen(response) > 0) {
      printf("%s", response); // Print the whitespace as-is (including newlines)
    }
    return 0; // Whitespace-only response is not an error
  }
  
  // Check if response starts with error prefix
  if (strncmp(response, ERROR_PREFIX, ERROR_PREFIX_LEN) == 0) {
    // Extract error message (skip the prefix)
    char *error_msg = response + ERROR_PREFIX_LEN;
    
    if (interactive && supports_color()) {
      // Print in red for interactive mode with color support
      printf("%s%s%s\n", ANSI_COLOR_RED, error_msg, ANSI_COLOR_RESET);
    } else {
      // Just print the error message without prefix
      printf("%s\n", error_msg);
    }
    return 1; // Error occurred
  } else {
    // Normal response, print as-is
    printf("%s\n", response);
    return 0; // Success
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

char *do_msg_command(char *server, int port, char *line, int n)
{
  char *buf;
  int sock = socket_open(server, port);
  
  if (sock < 0) return NULL;

  if (!sendMessage(sock, line, n)) {
    socket_close(sock);
    return NULL;
  }
  if (!receiveMessage(sock, &buf)) {
    socket_close(sock);
    return NULL;
  }
  
  if (buf) {
    if (strlen(buf)) {
      linenoiseHistoryAdd(line); /* Add to the history. */
      linenoiseHistorySave("history.txt"); /* Save the history on disk. */
    }
  }
  
  socket_close(sock);

  return buf;
}

int execute_single_command(char *server, int tcpport, char *command) {
    char *resultstr;
    int error_occurred = 0;
    
    if (tcpport == STIM_PORT || tcpport == ESS_PORT) {
        resultstr = do_msg_command(server, tcpport, command, strlen(command));
    } else {
        resultstr = sock_send(server, tcpport, command, strlen(command));
    }
    
    if (resultstr) {
      error_occurred = process_response(resultstr, 0);
      if (tcpport == STIM_PORT || tcpport == ESS_PORT) {
	free(resultstr);
      }
      return error_occurred; // Return 0 for success, 1 for error
    }
    else {
      return 0;
    }
    
    fprintf(stderr, "Error: Failed to execute command\n");
    return 1; // Connection/communication error
}

int is_stdin_available() {
#ifdef _WIN32
    // Windows implementation
    return !_isatty(_fileno(stdin));
#else
    // Unix/Linux implementation
    return !isatty(STDIN_FILENO);
#endif
}

int process_stdin_commands(char *server, int tcpport) {
    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    char *resultstr;
    int commands_processed = 0;
    int any_errors = 0;

    while ((read = getline(&line, &len, stdin)) != -1) {
        // Remove newline if present
        if (read > 0 && line[read-1] == '\n') {
            line[read-1] = '\0';
            read--;
        }
        
        // Skip empty lines
        if (read == 0 || line[0] == '\0') continue;
        
        // Process the command
        if (tcpport == STIM_PORT || tcpport == ESS_PORT) {
            resultstr = do_msg_command(server, tcpport, line, strlen(line));
            if (resultstr) {
                if (strlen(resultstr)) {
                    if (process_response(resultstr, 0)) { // Not interactive
                        any_errors = 1;
                    }
                }
                free(resultstr);
            }
        } else {
            resultstr = sock_send(server, tcpport, line, strlen(line));
            if (resultstr) {
                if (strlen(resultstr)) {
                    if (process_response(resultstr, 0)) { // Not interactive
                        any_errors = 1;
                    }
                }
            }
        }
        
        commands_processed++;
    }
    
    if (line) free(line);
    
    // Return appropriate exit code
    if (commands_processed == 0) return 1; // No commands processed
    return any_errors ? 1 : 0;  // Return 1 if any errors occurred
}

int main (int argc, char *argv[])
{
  char *line;
  char *prgname = argv[0];
  int async = 0;
  char *server = "localhost";
  int tcpport = ESS_PORT;
  char *resultstr;
  char *prompt = ESS_PROMPT;
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
  
  // Set global interactive mode flag
  g_interactive_mode = interactive_mode;
  
  // Set target service port if specified
  if (target_service) {
      tcpport = get_port_for_service(target_service);
      if (tcpport == -1) {
          fprintf(stderr, "Error: Unknown service '%s'\n", target_service);
          fprintf(stderr, "Valid services: ess, legacy, db, dserv, stim, pg, git, openiris\n");
          return 1;
      }
  }

  // Check if we should read from stdin
  if (!command_to_execute && is_stdin_available()) {
      g_interactive_mode = 0; // Stdin mode is not interactive
      int result = process_stdin_commands(server, tcpport);
#ifdef _MSC_VER
      cleanup_w32_socket();
#endif
      return result;
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
  g_interactive_mode = 1;
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
      if (tcpport == STIM_PORT || tcpport == ESS_PORT) {
	resultstr = do_msg_command(server, tcpport, line, strlen(line));
	if (resultstr) {
	  if (strlen(resultstr)) {
              process_response(resultstr, 1); // Interactive mode
          }
	  free(resultstr);
	}
      }
      else {
	resultstr = sock_send(server, tcpport, line, strlen(line));
	if (resultstr) {
	  if (strlen(resultstr)) {
	    process_response(resultstr, 1); // Interactive mode
	  }
	}
	linenoiseHistoryAdd(line);
	linenoiseHistorySave("history.txt");
      }
    } else if (!strncmp(line,"/historylen",11)) {
      /* The "/historylen" command will change the history len. */
      int len = atoi(line+11);
      linenoiseHistorySetMaxLen(len);
    } else if (!strncmp(line, "/legacy", 7)) {
      if (strlen(line) > 7) {
	resultstr = do_command(server, LEGACY_PORT, &line[8], strlen(line)-8);
	if (resultstr && strlen(resultstr)) {
            process_response(resultstr, 1); // Interactive mode
        }
      }
      else {
	tcpport = LEGACY_PORT;
	prompt = LEGACY_PROMPT;
      }
    } else if (!strncmp(line, "/ess", 4)) {
      if (strlen(line) > 4) {
	resultstr = do_msg_command(server, ESS_PORT, &line[5],
				   strlen(line)-5);
	if (resultstr) {
	  if (strlen(resultstr)) {
              process_response(resultstr, 1); // Interactive mode
          }
	  free(resultstr);
	}
      }
      else {
	tcpport = ESS_PORT;
	prompt = ESS_PROMPT;
      }
      if (strlen(line) > 6) {
	resultstr = do_command(server, DSERV_PORT, &line[7], strlen(line)-7);
	if (resultstr && strlen(resultstr)) {
            process_response(resultstr, 1); // Interactive mode
        }
      }
      else {
	tcpport = DSERV_PORT;
	prompt = DSERV_PROMPT;
      }
    } else if (!strncmp(line, "/dserv", 6)) {
      if (strlen(line) > 5) {
	resultstr = do_msg_command(server, STIM_PORT, &line[6],
				   strlen(line)-6);
	if (resultstr) {
	  if (strlen(resultstr)) {
              process_response(resultstr, 1); // Interactive mode
          }
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
	if (resultstr && strlen(resultstr)) {
            process_response(resultstr, 1); // Interactive mode
        }
      }
      else {
	tcpport = DB_PORT;
	prompt = DB_PROMPT;
      }
    } else if (!strncmp(line, "/stim", 5)) {
      if (strlen(line) > 5) {
	resultstr = do_command(server, STIM_PORT, &line[6], strlen(line)-6);
	if (resultstr && strlen(resultstr)) {
            process_response(resultstr, 1); // Interactive mode
        }
      }
      else {
	tcpport = STIM_PORT;
	prompt = STIM_PROMPT;
      }
    } else if (!strncmp(line, "/vstream", 5)) {
      if (strlen(line) > 8) {
	resultstr = do_command(server, VSTREAM_PORT, &line[9], strlen(line)-9);
	if (resultstr && strlen(resultstr)) {
            process_response(resultstr, 1); // Interactive mode
        }
      }
      else {
	tcpport = VSTREAM_PORT;
	prompt = VSTREAM_PROMPT;
      }
    } else if (!strncmp(line, "/pg", 3)) {
      if (strlen(line) > 3) {
	resultstr = do_command(server, PG_PORT, &line[4], strlen(line)-4);
	if (resultstr && strlen(resultstr)) {
            process_response(resultstr, 1); // Interactive mode
        }
      }
      else {
	tcpport = PG_PORT;
	prompt = PG_PROMPT;
      }
    } else if (!strncmp(line, "/git", 4)) {
      if (strlen(line) > 4) {
	resultstr = do_command(server, GIT_PORT, &line[5], strlen(line)-5);
	if (resultstr && strlen(resultstr)) {
            process_response(resultstr, 1); // Interactive mode
        }
      }
      else {
	tcpport = GIT_PORT;
	prompt = GIT_PROMPT;
      }
    } else if (!strncmp(line, "/openiris", 9)) {
      if (strlen(line) > 9) {
	resultstr = do_command(server, OPENIRIS_PORT, &line[10], strlen(line)-10);
	if (resultstr && strlen(resultstr)) {
            process_response(resultstr, 1); // Interactive mode
        }
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
