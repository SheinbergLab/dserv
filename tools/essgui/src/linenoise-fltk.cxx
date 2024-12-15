/* linenoise.c -- guerrilla line editing library against the idea that a
 * line editing lib needs to be 20,000 lines of C code.
 *
 * You can find the latest source code at:
 *
 *   http://github.com/antirez/linenoise
 *
 * Does a number of crazy assumptions that happen to be true in 99.9999% of
 * the 2010 UNIX computers around.
 *
 * ------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2016, Salvatore Sanfilippo <antirez at gmail dot com>
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
 *
 * ------------------------------------------------------------------------
 *
 * References:
 * - http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
 * - http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html
 *
 * Todo list:
 * - Filter bogus Ctrl+<char> combinations.
 * - Win32 support
 *
 * Bloat:
 * - History search like Ctrl+r in readline?
 *
 * List of escape sequences used by this program, we do everything just
 * with three sequences. In order to be so cheap we may have some
 * flickering effect with some slow terminal, but the lesser sequences
 * the more compatible.
 *
 * EL (Erase Line)
 *    Sequence: ESC [ n K
 *    Effect: if n is 0 or missing, clear from cursor to end of line
 *    Effect: if n is 1, clear from beginning of line to cursor
 *    Effect: if n is 2, clear entire line
 *
 * CUF (CUrsor Forward)
 *    Sequence: ESC [ n C
 *    Effect: moves cursor forward n chars
 *
 * CUB (CUrsor Backward)
 *    Sequence: ESC [ n D
 *    Effect: moves cursor backward n chars
 *
 * The following is used to get the terminal width if getting
 * the width with the TIOCGWINSZ ioctl fails
 *
 * DSR (Device Status Report)
 *    Sequence: ESC [ 6 n
 *    Effect: reports the current cusor position as ESC [ n ; m R
 *            where n is the row and m is the column
 *
 * When multi line mode is enabled, we also use an additional escape
 * sequence. However multi line editing is disabled by default.
 *
 * CUU (Cursor Up)
 *    Sequence: ESC [ n A
 *    Effect: moves cursor up of n chars.
 *
 * CUD (Cursor Down)
 *    Sequence: ESC [ n B
 *    Effect: moves cursor down of n chars.
 *
 * When linenoiseClearScreen() is called, two additional escape sequences
 * are used in order to clear the screen and position the cursor at home
 * position.
 *
 * CUP (Cursor position)
 *    Sequence: ESC [ H
 *    Effect: moves the cursor to upper left corner
 *
 * ED (Erase display)
 *    Sequence: ESC [ 2 J
 *    Effect: clear the whole screen
 *
 */


#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <iostream>

#include "linenoise-fltk.h"

#define PROMPT_HDR "\x1b[1;37;49m"
#define PROMPT_TLR "\x1b[0m"

#if defined(_MSC_VER)
#define ATTR_WEAK
#else
#define ATTR_WEAK __attribute__((weak))
#endif

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

// Dummy timeout functions for query of screen dimensions (to detect a dumb terminal)
// Can be overridden by user code.

void ATTR_WEAK linenoise_timeout_set(void)
{
}

bool ATTR_WEAK linenoise_timeout_elapsed(void)
{
    // Default timeout implementation never elapses.
    return false;
}

// Dummy functions provided for completion and hints, can be overridden by user code.

void ATTR_WEAK linenoise_completion(const char *buf, linenoiseCompletions *lc)
{
    (void)buf;
    (void)lc;
}

const char** ATTR_WEAK linenoise_hints(const char *buf)
{
    (void)buf;
    return NULL;
}

static void refreshLine(struct linenoiseState *l);

/* Debugging macro. */
#if 0
FILE *lndebug_fp = NULL;
#define lndebug(...) \
    do { \
        if (!lndebug_fp) { \
            lndebug_fp = fopen("lndebug.txt","a"); \
            if (lndebug_fp) { \
                fprintf(lndebug_fp, \
                    "[%u %u %u] p: %u, rows: %u, rpos: %u, max: %u, oldmax: %u\n", \
                    l->len,l->pos,l->oldpos,plen,rows,rpos,l->maxrows,old_rows); \
            } \
        } \
        fprintf(lndebug_fp, ", " __VA_ARGS__); \
        fflush(lndebug_fp); \
    } while (0)
#else
#define lndebug(...)
#endif

static inline void linenoise_write_string(const char *str)
{
  linenoise_write(str, strlen(str));
}

/* Set if to use or not the multi line mode. */
void linenoiseSetMultiLine(struct linenoiseState *ls, bool ml)
{
  ls->mlmode = ml;
}

/* Try to get the number of columns in the current terminal, or assume 80
 * if it fails. */
static int getColumns(struct linenoiseState *ls)
{
  return ls->cols;
}

/* Clear the screen. Used to handle ctrl+l */
void linenoiseClearScreen(struct linenoiseState *l_state)
{
    linenoise_write_string("\x1b[H\x1b[2J");
    l_state->mode = linenoiseState::ln_getColumns;
}

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
static void linenoiseBeep(void)
{
    fprintf(stderr, "\x7");
    fflush(stderr);
}

/* ============================== Completion ================================ */

/* Free a list of completion option populated by linenoiseAddCompletion(). */
static void freeCompletions(linenoiseCompletions *lc)
{
    size_t i;

    for (i = 0; i < lc->len; i++) {
        free(lc->cvec[i]);
    }

    if (lc->cvec != NULL) {
        free(lc->cvec);
    }

    lc->len = 0;
}

static void lnShowCompletion(struct linenoiseState *ls)
{
    struct linenoiseState saved = *ls;
    /* Find next completion not identical with current line buffer */
    while (ls->completion_idx < ls->lc.len) {
        if (strcmp(ls->buf, ls->lc.cvec[ls->completion_idx])) {
            break;
        }
        ls->completion_idx = (ls->completion_idx + 1) % (ls->lc.len + 1);
    }
    if (ls->completion_idx < ls->lc.len) {
        ls->len = ls->pos = strlen(ls->lc.cvec[ls->completion_idx]);
        ls->buf = ls->lc.cvec[ls->completion_idx];
    }
    /* Show completion or original buffer */
    refreshLine(ls);
    ls->len = saved.len;
    ls->pos = saved.pos;
    ls->buf = saved.buf;
}

/* This is an helper function for linenoiseEdit() and is called when the
 * user types the <tab> key in order to complete the string currently in the
 * input.
 *
 * The state of the editing is encapsulated into the pointed linenoiseState
 * structure as described in the structure definition. */
static void completeLine(struct linenoiseState *ls)
{
    ls->lc.len = 0;
    ls->lc.cvec = NULL;

    linenoise_completion(ls->buf, &ls->lc);

    if (ls->lc.len == 0) {
        linenoiseBeep();
        freeCompletions(&ls->lc);
    } else {
        ls->completion_idx = 0;
        ls->mode = linenoiseState::ln_completion;
        lnShowCompletion(ls);
    }
}


/* This function is used by the callback function registered by the user
 * in order to add completion options given the input string when the
 * user typed <tab>. See the example.c source code for a very easy to
 * understand example. */
void linenoiseAddCompletion(linenoiseCompletions *lc, const char *str)
{
    size_t len = strlen(str);
    char *copy, **cvec;

    copy = (char *) malloc(len + 1);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, str, len + 1);
    cvec = (char **) realloc(lc->cvec, sizeof(char*) * (lc->len + 1));

    if (cvec == NULL) {
        free(copy);
        return;
    }

    lc->cvec = cvec;
    lc->cvec[lc->len++] = copy;
}

/* =========================== Line editing ================================= */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
    char *b;
    size_t len;
};

static void abInit(struct abuf *ab)
{
    ab->b = NULL;
    ab->len = 0;
}

static void abAppendN(struct abuf *ab, const char *s, size_t len)
{
  char *newab = (char *) realloc(ab->b, ab->len + len);

    if (newab == NULL) {
        return;
    }

    memcpy(newab + ab->len, s, len);
    ab->b = newab;
    ab->len += len;
}

static void abAppend(struct abuf *ab, const char *s)
{
    abAppendN(ab, s, strlen(s));
}

static void abFree(struct abuf *ab)
{
    free(ab->b);
}

/* Helper of refreshSingleLine() and refreshMultiLine() to show hints
 * to the right of the prompt. */
static void refreshShowHints(struct abuf *ab, struct linenoiseState *l, size_t plen)
{
  ssize_t cols_avail = (ssize_t)(l->cols - (plen + l->len + 1));
  if (cols_avail > 0) {
    const char **hints = linenoise_hints(l->buf);
    if (hints) {
      // By convention, it returns a char*[2]
      // hints[0] = cmd args [optional]
      // hints[1] = cmd desc
      abAppend(ab, " \033[0;35;49m");
      if (hints[0] && *hints[0] != '\0') {
	size_t abLen = MIN(strlen(hints[0]), (size_t)cols_avail);
	if(strchr(l->buf, ' ')) {
	  // We got spaces, so try to locate which argument we are at
	  size_t arg_id = 0;
	  for(size_t pos = 0; pos < strlen(l->buf); pos++) {
	    if(' ' == l->buf[pos]) {
	      arg_id++;
	    }
	  }
	  
	  size_t arg_start = 0;
	  const char* hint_ptr = hints[0];
	  while(arg_id--) {
	    while(*hint_ptr && '[' != *hint_ptr) {
	      hint_ptr++;
	    }
	    if(*hint_ptr) {
	      hint_ptr++;
	    }
	    arg_start = (size_t)(hint_ptr - hints[0]);
	  }
	  
	  size_t arg_end = 0;
	  if(arg_start) {
	    while(*hint_ptr && ' ' != *hint_ptr && ']' != *hint_ptr) {
	      hint_ptr++;
	    }
	    arg_end = (size_t)(hint_ptr - hints[0]);
	  }
	  
	  if(arg_start != arg_end) {
	    abAppendN(ab, hints[0], MIN(abLen, arg_start));
	    abAppend(ab, "\033[7;35;49m");
	    abAppendN(ab, hints[0] + arg_start, abLen < arg_start ? 0 : (abLen < arg_end ? abLen - arg_start : arg_end - arg_start));
	    abAppend(ab, "\033[0;35;49m");
	    abAppendN(ab, hints[0] + arg_end, abLen < arg_end ? 0 : abLen - arg_end);
	  } else {
	    abAppendN(ab, hints[0], abLen);
	  }
	} else {
	  abAppendN(ab, hints[0], abLen);
	}
	cols_avail -= (ssize_t)abLen;
	if (cols_avail > 0) {
	  abAppend(ab, " ");
	  cols_avail--;
	}
      }
      if (cols_avail > 0 && hints[1] && *hints[1] != '\0') {
	abAppend(ab, "\033[1;35;49m");
	size_t abLen = MIN(strlen(hints[1]), (size_t)cols_avail);
	abAppendN(ab, hints[1], abLen);
      }
      abAppend(ab, "\033[0m");
    }
  }
}

/* Single line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static void refreshSingleLine(struct linenoiseState *l, bool showHints)
{
  size_t plen = strlen(l->prompt);
  
  char *buf = l->buf;
  size_t len = l->len;
  size_t pos = l->pos;

  //  printf("refreshSingleLine: plen(%d) pos(%d) l->cols(%d) l->buf(%s) l->len(%d)\n",
  //	 plen, pos, l->cols, l->buf, l->len);
  
  while((plen + pos) >= l->cols) {
    buf++;
    len--;
    pos--;
  }
  
  while (plen + len > l->cols) {
    len--;
  }
  
  struct abuf ab;
  abInit(&ab);
  
  /* Cursor to left edge */
  abAppend(&ab, "\r");
  
  /* Write the prompt and the current buffer content */
  abAppend(&ab, PROMPT_HDR);
  abAppend(&ab, l->prompt);
  abAppend(&ab, PROMPT_TLR);
  
  abAppendN(&ab, buf, len);
  
  if (showHints) {
    /* Show hits if any. */
    refreshShowHints(&ab, l, plen);
  }
  
  /* Erase to right */
  abAppend(&ab, "\x1b[0K");
  
  /* Move cursor to original position. */
  char seq[20];
  snprintf(seq, sizeof(seq), "\r\x1b[%dC", (int)(pos + plen));
  abAppend(&ab, seq);
  
  linenoise_write(ab.b, ab.len);
  
  abFree(&ab);
}

/* Multi line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static void refreshMultiLine(struct linenoiseState *l, bool showHints)
{
    char seq[20];
    size_t plen = strlen(l->prompt);
    size_t rows = (plen + l->len + l->cols - 1) / l->cols; /* rows used by current buf. */
    size_t rpos = (plen + l->oldpos + l->cols) / l->cols; /* cursor relative row. */
    size_t rpos2; /* rpos after refresh. */
    size_t col; /* colum position, zero-based. */
    size_t old_rows = l->maxrows;
    size_t j;

    /* Update maxrows if needed. */
    if (rows > l->maxrows) {
        l->maxrows = rows;
    }

    /* First step: clear all the lines used before. To do so start by
     * going to the last row. */
    struct abuf ab;
    abInit(&ab);
    if (old_rows - rpos > 0) {
        lndebug("go down %u", old_rows - rpos);
        snprintf(seq, sizeof(seq), "\x1b[%uB", (unsigned int) (old_rows - rpos));
        abAppend(&ab, seq);
    }

    /* Now for every row clear it, go up. */
    for (j = 0; j < old_rows - 1; j++) {
        lndebug("clear+up");
        abAppend(&ab, "\r\x1b[0K\x1b[1A");
    }

    /* Clean the top line. */
    lndebug("clear");
    abAppend(&ab, "\r\x1b[0K");

    /* Write the prompt and the current buffer content */
    abAppend(&ab, PROMPT_HDR);
    abAppend(&ab, l->prompt);
    abAppend(&ab, PROMPT_TLR);

    abAppendN(&ab, l->buf, l->len);

    if (showHints) {
        /* Show hits if any. */
        refreshShowHints(&ab, l, plen);
    }

    /* If we are at the very end of the screen with our prompt, we need to
     * emit a newline and move the prompt to the first column. */
    if (l->pos &&
            l->pos == l->len &&
            (l->pos + plen) % l->cols == 0) {
        lndebug("<newline>");
        abAppend(&ab, "\n\r"); // <-- This is intentional: new line and move to start in that order

        rows++;
        if (rows > l->maxrows) {
            l->maxrows = rows;
        }
    }

    /* Move cursor to right position. */
    rpos2 = (plen + l->pos + l->cols) / l->cols; /* current cursor relative row. */
    lndebug("rpos2 %d", rpos2);

    /* Go up till we reach the expected positon. */
    if (rows - rpos2 > 0) {
        lndebug("go-up %u", rows - rpos2);
        snprintf(seq, sizeof(seq), "\x1b[%uA", (unsigned int) (rows - rpos2));
        abAppend(&ab, seq);
    }

    /* Set column. */
    col = (plen + l->pos) % l->cols;
    lndebug("set col %u", 1 + col);
    if (col) {
      snprintf(seq, sizeof(seq), "\r\x1b[%uC", (unsigned int) col);
    } else {
        snprintf(seq, sizeof(seq), "\r");
    }
    abAppend(&ab, seq);

    lndebug("\n");
    l->oldpos = l->pos;

    linenoise_write(ab.b, ab.len);
    abFree(&ab);
}

/* Calls the two low level functions refreshSingleLine() or
 * refreshMultiLine() according to the selected mode. */
static void refreshLine(struct linenoiseState *l)
{
  if (l->mlmode) {
    refreshMultiLine(l, true);
  } else {
    refreshSingleLine(l, true);
  }
}

static void refreshLineNoHints(struct linenoiseState *l)
{
    if (l->mlmode) {
        refreshMultiLine(l, false);
    } else {
        refreshSingleLine(l, false);
    }
}

/* Insert the character 'c' at cursor current position.
 *
 * On error writing to the terminal -1 is returned, otherwise 0. */
static int linenoiseEditInsert(struct linenoiseState *l, char c)
{
    if (l->len < l->buflen) {
        if (l->len == l->pos) {
            l->buf[l->pos] = c;
            l->pos++;
            l->len++;
            l->buf[l->len] = '\0';
            refreshLine(l);
        } else {
            memmove(l->buf + l->pos + 1, l->buf + l->pos, l->len - l->pos);
            l->buf[l->pos] = c;
            l->len++;
            l->pos++;
            l->buf[l->len] = '\0';
            refreshLine(l);
        }
    }
    return 0;
}

/* Move cursor on the left. */
static void linenoiseEditMoveLeft(struct linenoiseState *l)
{
    if (l->pos > 0) {
        l->pos--;
        refreshLine(l);
    }
}

/* Move cursor on the right. */
static void linenoiseEditMoveRight(struct linenoiseState *l)
{
    if (l->pos != l->len) {
        l->pos++;
        refreshLine(l);
    }
}

/* Move cursor to the start of the line. */
static void linenoiseEditMoveHome(struct linenoiseState *l)
{
    if (l->pos != 0) {
        l->pos = 0;
        refreshLine(l);
    }
}

/* Move cursor to the end of the line. */
static void linenoiseEditMoveEnd(struct linenoiseState *l)
{
    if (l->pos != l->len) {
        l->pos = l->len;
        refreshLine(l);
    }
}

/* Substitute the currently edited line with the next or previous history
 * entry as specified by 'dir'. */
#define LINENOISE_HISTORY_NEXT 0
#define LINENOISE_HISTORY_PREV 1
static void linenoiseEditHistoryNext(struct linenoiseState *l, int dir)
{
    if (l->history_len > 1) {
        /* Update the current history entry before to
         * overwrite it with the next one. */
        free(l->history[l->history_len - 1 - (size_t)l->history_index]);
        l->history[l->history_len - 1 - (size_t)l->history_index] = strdup(l->buf);
        /* Show the new entry */
        l->history_index += (dir == LINENOISE_HISTORY_PREV) ? 1 : -1;
        if (l->history_index < 0) {
            l->history_index = 0;
            return;
        } else if ((size_t)l->history_index >= l->history_len) {
            l->history_index = (ssize_t)l->history_len - 1;
            return;
        }
        strncpy(l->buf, l->history[l->history_len - 1 - (size_t)l->history_index], l->buflen);
        l->buf[l->buflen - 1] = '\0';
        l->len = l->pos = strlen(l->buf);
        refreshLine(l);
    }
}

/* Delete the character at the right of the cursor without altering the cursor
 * position. Basically this is what happens with the "Delete" keyboard key. */
static void linenoiseEditDelete(struct linenoiseState *l)
{
    if (l->len > 0 && l->pos < l->len) {
        memmove(l->buf + l->pos, l->buf + l->pos + 1, l->len - l->pos - 1);
        l->len--;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

/* Backspace implementation. */
static void linenoiseEditBackspace(struct linenoiseState *l)
{
    if (l->pos > 0 && l->len > 0) {
        memmove(l->buf + l->pos - 1, l->buf + l->pos, l->len - l->pos);
        l->pos--;
        l->len--;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

/* Delete the previous word, maintaining the cursor at the start of the
 * current word. */
static void linenoiseEditDeletePrevWord(struct linenoiseState *l)
{
    size_t old_pos = l->pos;
    size_t diff;

    while (l->pos > 0 && l->buf[l->pos - 1] == ' ') {
        l->pos--;
    }

    while (l->pos > 0 && l->buf[l->pos - 1] != ' ') {
        l->pos--;
    }

    diff = old_pos - l->pos;
    memmove(l->buf + l->pos, l->buf + old_pos, l->len - old_pos + 1);
    l->len -= diff;
    refreshLine(l);
}

void lnInitState(struct linenoiseState *l, char *buf, size_t buflen, const char *prompt)
{
    /* Populate the linenoise state that we pass to functions implementing
    * specific editing functionalities. */
    l->buf = buf;
    l->buflen = buflen;
    l->prompt = prompt;
    l->plen = strlen(prompt);
    l->oldpos = l->pos = 0;
    l->len = 0;
    l->maxrows = 0;
    l->history_index = 0;

    /* Buffer starts empty. */
    l->buf[0] = '\0';
    l->buflen--; /* Make sure there is always space for the nulterm */

    /* The latest history entry is always our current buffer, that
    * initially is just an empty string. */
    linenoiseHistoryAdd(l, "");

    if (l->smart_term_connected) {
        linenoise_write_string(PROMPT_HDR);
        linenoise_write_string(prompt);
        linenoise_write_string(PROMPT_TLR);
    } else {
        linenoise_write_string(prompt);
    }

    l->mode = linenoiseState::ln_read_regular;
}

static int lnReadEscSequence(struct linenoiseState *l)
{
    /* Read at least two bytes representing the escape sequence */
    int c = linenoise_getch();
    if (c < 0) {
        return -1;
    }
    if (l->seq_idx >= sizeof(l->seq)) {
        // This should never happen ...
        return -1;
    }
    l->seq[l->seq_idx++] = (char)c;
    if (l->seq_idx < 2) {
        // We need at least 2 characters
        return -1;
    }

    /* ESC [ sequences. */
    if (l->seq[0] == '[') {
        if (l->seq[1] >= '0' && l->seq[1] <= '9') {
            /* Extended escape, make sure we have read the additional byte. */
            if (l->seq_idx < 3) {
                return -1;
            }
            if (l->seq[2] == '~') {
                switch(l->seq[1]) {
                case '3': /* Delete key. */
                    linenoiseEditDelete(l);
                    break;
                default:
                    break;
                }
            }
        } else {
            switch(l->seq[1]) {
            case 'A': /* Up */
                linenoiseEditHistoryNext(l, LINENOISE_HISTORY_PREV);
                break;
            case 'B': /* Down */
                linenoiseEditHistoryNext(l, LINENOISE_HISTORY_NEXT);
                break;
            case 'C': /* Right */
                linenoiseEditMoveRight(l);
                break;
            case 'D': /* Left */
                linenoiseEditMoveLeft(l);
                break;
            case 'H': /* Home */
                linenoiseEditMoveHome(l);
                break;
            case 'F': /* End*/
                linenoiseEditMoveEnd(l);
                break;
            default:
                break;
            }
        }
    }

    /* ESC O sequences. */
    else if (l->seq[0] == 'O') {
        switch(l->seq[1]) {
        case 'H': /* Home */
            linenoiseEditMoveHome(l);
            break;
        case 'F': /* End*/
            linenoiseEditMoveEnd(l);
            break;
        default:
            break;
        }
    }

    l->mode = linenoiseState::ln_read_regular;
    return -1;
}

static void lnRestartState(struct linenoiseState *l)
{
  l->mode = linenoiseState::ln_getColumns;
}

int lnHandleCharacter(struct linenoiseState *l, char c)
{
    /* Only autocomplete when the callback is set. It returns < 0 when
        * there was an error reading from fd. Otherwise it will return the
        * character that should be handled next. */
    if (c == '\t') {
        completeLine(l);
        return -1;
    }

    switch(c) {
    case ENTER:    /* enter */
        l->history_len--;
        free(l->history[l->history_len]);
        if (l->mlmode) {
            linenoiseEditMoveEnd(l);
        }
        /* Force a refresh without hints to leave the previous
            * line as the user typed it after a newline. */
        refreshLineNoHints(l);
        lnRestartState(l);
        return (int)l->len;
    case CTRL_C:     /* ctrl-c */
        errno = EAGAIN;
        return -1;
    case BACKSPACE:   /* backspace */
    case 8:     /* ctrl-h */
        linenoiseEditBackspace(l);
        break;
    case CTRL_D:     /* ctrl-d, remove char at right of cursor, or if the
                        line is empty, act as end-of-file. */
        if (l->len > 0) {
            linenoiseEditDelete(l);
        } else {
            l->history_len--;
            free(l->history[l->history_len]);
            return -1;
        }
        break;
    case CTRL_T:    /* ctrl-t, swaps current character with previous. */
        if (l->pos > 0 && l->pos < l->len) {
            char aux = l->buf[l->pos - 1];
            l->buf[l->pos - 1] = l->buf[l->pos];
            l->buf[l->pos] = aux;
            if (l->pos != l->len - 1) {
                l->pos++;
            }
            refreshLine(l);
        }
        break;
    case CTRL_B:     /* ctrl-b */
        linenoiseEditMoveLeft(l);
        break;
    case CTRL_F:     /* ctrl-f */
        linenoiseEditMoveRight(l);
        break;
    case CTRL_P:    /* ctrl-p */
        linenoiseEditHistoryNext(l, LINENOISE_HISTORY_PREV);
        break;
    case CTRL_N:    /* ctrl-n */
        linenoiseEditHistoryNext(l, LINENOISE_HISTORY_NEXT);
        break;
    case ESC:    /* escape sequence */
        l->seq_idx = 0;
        l->mode = linenoiseState::ln_read_esc;
        break;
    default:
        if (linenoiseEditInsert(l, (char)c)) {
            return -1;
        }
        break;
    case CTRL_U: /* Ctrl+u, delete the whole line. */
        l->buf[0] = '\0';
        l->pos = l->len = 0;
        refreshLine(l);
        break;
    case CTRL_K: /* Ctrl+k, delete from current to end of line. */
        l->buf[l->pos] = '\0';
        l->len = l->pos;
        refreshLine(l);
        break;
    case CTRL_A: /* Ctrl+a, go to the start of the line */
        linenoiseEditMoveHome(l);
        break;
    case CTRL_E: /* ctrl+e, go to the end of the line */
        linenoiseEditMoveEnd(l);
        break;
    case CTRL_L: /* ctrl+l, clear screen */
        linenoiseClearScreen(l);
        refreshLine(l);
        break;
    case CTRL_W: /* ctrl+w, delete previous word */
        linenoiseEditDeletePrevWord(l);
        break;
    }
    return -1;
}

static int lnCompletion(struct linenoiseState *ls)
{
    int c = linenoise_getch();
    if (c < 0) {
        return -1;
    }

    switch(c) {
    case 9: /* tab */
        ls->completion_idx = (ls->completion_idx + 1) % (ls->lc.len + 1);
        if (ls->completion_idx == ls->lc.len) {
            linenoiseBeep();
        }
        lnShowCompletion(ls);
        return -1;
    case 27: /* escape */
        /* Re-show original buffer */
        if (ls->completion_idx < ls->lc.len) {
            refreshLine(ls);
        }
        break;
    default:
        /* Update buffer and return */
        if (ls->completion_idx < ls->lc.len) {
            ls->len = ls->pos = (size_t)snprintf(ls->buf, ls->buflen, "%s", ls->lc.cvec[ls->completion_idx]);
        }
        break;
    }

    ls->mode = linenoiseState::ln_read_regular;
    freeCompletions(&ls->lc);
    return lnHandleCharacter(ls, (char)c);
}

static int lnReadUserInput(struct linenoiseState *l)
{
    int c = linenoise_getch();

    if (c < 0) {
        return -1;
    }

    return lnHandleCharacter(l, (char)c);
}

/* This function is the core of the line editing capability of linenoise.
 * It expects 'fd' to be already in "raw mode" so that every key pressed
 * will be returned ASAP to read().
 *
 * The resulting string is put into 'buf' when the user type enter, or
 * when ctrl+d is typed.
 *
 * The function returns the length of the current buffer. */
int linenoiseEdit(struct linenoiseState *l_state)
{
  char *buf = l_state->buf;
  size_t buflen = l_state->buflen;
  const char *prompt = l_state->prompt;
  
  switch (l_state->mode) {
  default:
  case linenoiseState::ln_getColumns:
  case linenoiseState::ln_getColumns_1:
  case linenoiseState::ln_getColumns_2:
        if (getColumns(l_state) < 0) {
            return -1;
        }
	// fall through
  case linenoiseState::ln_init:
    lnInitState(l_state, buf, buflen, prompt);
    // fall through
  case linenoiseState::ln_read_regular:
        return lnReadUserInput(l_state);
  case linenoiseState::ln_read_esc:
        return lnReadEscSequence(l_state);
  case linenoiseState::ln_completion:
    return lnCompletion(l_state);
  }
  return -1;
}

void linenoiseRefreshEditor(struct linenoiseState *l_state)
{
    switch (l_state->mode) {
    case linenoiseState::ln_init:
    case linenoiseState::ln_getColumns:
    case linenoiseState::ln_getColumns_1:
    case linenoiseState::ln_getColumns_2:
        // Don't refresh if line editor not active (yet).
        break;
    case linenoiseState::ln_completion:
        lnShowCompletion(l_state);
        break;
    case linenoiseState::ln_read_regular:
    case linenoiseState::ln_read_esc:
    default:
        refreshLine(l_state);
        break;
    }
}

void linenoiseUpdatePrompt(struct linenoiseState *l_state, const char *prompt)
{
    l_state->prompt = prompt;
    l_state->plen = strlen(prompt);
    linenoiseRefreshEditor(l_state);
}

/* This special mode is used by linenoise in order to print scan codes
 * on screen for debugging / development purposes. It is implemented
 * by the linenoise_example program using the --keycodes option. */
void linenoisePrintKeyCodes(void)
{
    char quit[4];

    printf("Linenoise key codes debugging mode.\n"
           "Press keys to see scan codes. Type 'quit' at any time to exit.\n");

    memset(quit, ' ', sizeof(quit));
    while(1) {
        int c;

        do {
            c = linenoise_getch();
        } while (c < 0);

        memmove(quit, quit + 1, sizeof(quit) - 1); /* shift string to left. */
        quit[sizeof(quit) - 1] = (char)c; /* Insert current char on the right. */
        if (memcmp(quit, "quit", sizeof(quit)) == 0) {
            break;
        }

        printf("'%c' %02x (%d) (type quit to exit)\n",
               isprint(c) ? c : '?', (int)c, (int)c);
        printf("\r"); /* Go left edge manually, we are in raw mode. */
        fflush(stdout);
    }
}

/* ================================ History ================================= */

/* This is the API call to add a new entry in the linenoise history.
 * It uses a fixed array of char pointers that are shifted (memmoved)
 * when the history max length is reached in order to remove the older
 * entry and make room for the new one, so it is not exactly suitable for huge
 * histories, but will work well for a few hundred of entries.
 *
 * Using a circular buffer is smarter, but a bit more complex to handle. */
int linenoiseHistoryAdd(struct linenoiseState *l, const char *line)
{
    char *linecopy;

    if (l->history_max_len == 0) {
        return 0;
    }

    /* Initialization on first call. */
    if (l->history == NULL) {
      l->history = (char **) calloc(l->history_max_len, sizeof(char*));
        if (l->history == NULL) {
            return 0;
        }
    }

    /* Don't add duplicated lines. */
    if (l->history_len && !strcmp(l->history[l->history_len - 1], line)) {
        return 0;
    }

    /* Add an heap allocated copy of the line in the history.
     * If we reached the max length, remove the older line. */
    linecopy = strdup(line);

    if (!linecopy) {
        return 0;
    }
    if (l->history_len == l->history_max_len) {
        free(l->history[0]);
        memmove(l->history, l->history + 1, sizeof(char*) * (l->history_max_len - 1));
        l->history_len--;
    }

    l->history[l->history_len] = linecopy;
    l->history_len++;
    return 1;
}

/* Set the maximum length for the history. This function can be called even
 * if there is already some history, the function will make sure to retain
 * just the latest 'len' elements if the new history length value is smaller
 * than the amount of items already inside the history. */
int linenoiseHistorySetMaxLen(struct linenoiseState *l, size_t len)
{
  char **newh;
  
  if (len < 1) {
    return 0;
  }
  
  if (l->history) {
    size_t tocopy = l->history_len;
    
    newh = (char **) malloc(sizeof(char*)*len);
    if (newh == NULL) {
      return 0;
    }
    
    /* If we can't copy everything, free the elements we'll not use. */
    if (len < tocopy) {
      size_t j;
      
      for (j = 0; j < tocopy - len; j++) {
	free(l->history[j]);
      }
      
      tocopy = len;
    }
    
    memset(newh, 0, sizeof(char*)*len);
    memcpy(newh, l->history + (l->history_len - tocopy), sizeof(char*)*tocopy);
    free(l->history);
    l->history = newh;
  }
  
  l->history_max_len = len;
  
  if (l->history_len > l->history_max_len) {
    l->history_len = l->history_max_len;
  }
  
  return 1;
}

/* Save the history in the specified file. On success 0 is returned
 * otherwise -1 is returned. */
int linenoiseHistorySave(struct linenoiseState *l, const char *filename)
{
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        return -1;
    }

    for (size_t j = 0; j < l->history_len; j++) {
        fprintf(fp, "%s\n", l->history[j]);
    }

    fclose(fp);
    return 0;
}

/* Load the history from the specified file. If the file does not exist
 * zero is returned and no operation is performed.
 *
 * If the file exists and the operation succeeded 0 is returned, otherwise
 * on error -1 is returned. */
int linenoiseHistoryLoad(struct linenoiseState *l, const char *filename)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        return -1;
    }

    char buf[LINENOISE_MAX_LINE];

    while (fgets(buf, LINENOISE_MAX_LINE, fp) != NULL) {
        char *p;

        p = strchr(buf, '\r');
        if (!p) {
            p = strchr(buf, '\n');
        }
        if (p) {
            *p = '\0';
        }

        linenoiseHistoryAdd(l, buf);
    }

    fclose(fp);
    return 0;
}
