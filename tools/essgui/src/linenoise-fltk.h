/* linenoise.h -- VERSION 1.0
 *
 * Guerrilla line editing library against the idea that a line editing lib
 * needs to be 20,000 lines of C code.
 *
 * See linenoise.c for more information.
 *
 * ------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2014, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 100
#define LINENOISE_MAX_LINE 4096

typedef struct linenoiseCompletions {
    size_t len;
    char **cvec;
} linenoiseCompletions;

/* The linenoiseState structure represents the state during line editing.
 * We pass this state to functions implementing specific editing
 * functionalities. */
struct linenoiseState {
  /* Current mode of line editor state machine */
  enum {
    ln_init = 0,
    ln_read_regular,
    ln_read_esc,
    ln_completion,
    ln_getColumns,
    ln_getColumns_1,
    ln_getColumns_2
  } mode;
  
  /* State for esc sequence handling */
  char seq[3];        /* Esc sequence buffer. */
  size_t seq_idx;     /* Esc sequence read index. */
  
  /* State for completion handling */
  size_t completion_idx; /* Auto-completion selected entry index. */
  linenoiseCompletions lc;
  
  /* State for cursor pos. / column retrieval */
  char cur_pos_buf[32];
  ssize_t cur_pos_idx;
  ssize_t cur_pos_initial;
  
  bool smart_term_connected;
  
  // need to work on this
  int ncolumns;
  
  char *buf;          /* Edited line buffer. */
  size_t buflen;      /* Edited line buffer size. */
  const char *prompt; /* Prompt to display. */
  size_t plen;        /* Prompt length. */
  size_t pos;         /* Current cursor position. */
  size_t oldpos;      /* Previous refresh cursor position. */
  size_t len;         /* Current edited line length. */
  size_t cols;        /* Number of columns in terminal. */
  size_t maxrows;     /* Maximum num of rows used so far (multiline mode) */
  ssize_t history_index;  /* The history index we are currently editing. */
  
  bool mlmode = 0;  /* Multi line mode. Default is single line. */
  size_t history_max_len = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
  size_t history_len = 0;
  char **history = NULL;
};

enum KEY_ACTION {
    KEY_NULL = 0,	    /* NULL */
    CTRL_A = 1,         /* Ctrl+a */
    CTRL_B = 2,         /* Ctrl-b */
    CTRL_C = 3,         /* Ctrl-c */
    CTRL_D = 4,         /* Ctrl-d */
    CTRL_E = 5,         /* Ctrl-e */
    CTRL_F = 6,         /* Ctrl-f */
    CTRL_H = 8,         /* Ctrl-h */
    TAB = 9,            /* Tab */
    CTRL_K = 11,        /* Ctrl+k */
    CTRL_L = 12,        /* Ctrl+l */
    ENTER = 13,         /* Enter */
    CTRL_N = 14,        /* Ctrl-n */
    CTRL_P = 16,        /* Ctrl-p */
    CTRL_T = 20,        /* Ctrl-t */
    CTRL_U = 21,        /* Ctrl+u */
    CTRL_W = 23,        /* Ctrl+w */
    ESC = 27,           /* Escape */
    BACKSPACE =  127    /* Backspace */
};

#ifdef __cplusplus
extern "C" {
#endif

  void lnInitState(struct linenoiseState *l, char *buf, size_t buflen, const char *prompt);
  int lnHandleCharacter(struct linenoiseState *l, char c);
  
  // User-provided console getch() function
  // Should not block and return -1, when no character is received
  int linenoise_getch(void);
  
  // User-provided console write() function
  void linenoise_write(const char *buf, size_t n);
  
  // User-provided timeout functions (optional)
  void linenoise_timeout_set(void);
  bool linenoise_timeout_elapsed(void);
  
  void linenoise_completion(const char *buf, linenoiseCompletions *lc);
  const char **linenoise_hints(const char *buf);
  
  void linenoiseAddCompletion(linenoiseCompletions *, const char *);
  
  int linenoiseEdit(struct linenoiseState *l_state);
  
  int linenoiseHistoryAdd(struct linenoiseState *l_state, const char *line);
  int linenoiseHistorySetMaxLen(struct linenoiseState *l_state, size_t len);
  int linenoiseHistorySave(struct linenoiseState *l_state, const char *filename);
  int linenoiseHistoryLoad(struct linenoiseState *l_state, const char *filename);
  void linenoiseClearScreen(struct linenoiseState *);
  void linenoiseSetMultiLine(struct linenoiseState *ls, bool ml);
  void linenoisePrintKeyCodes(void);
  void linenoiseRefreshEditor(struct linenoiseState *);
  void linenoiseUpdatePrompt(struct linenoiseState *l_state, const char *prompt);
  bool smartTerminalConnected(void);
  
#ifdef __cplusplus
}
#endif
