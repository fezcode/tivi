#ifndef TIVI_SINGLEINST_H
#define TIVI_SINGLEINST_H

#include <stdbool.h>

// Single-instance support (Windows). The first process to launch "wins" and owns
// a named mutex + a hidden message-only window. Any later launch forwards its
// file path(s) to the winner and exits, so there is only ever one tivi window.
//
// Off Windows these degrade to: acquire() always true, the rest no-ops.

bool singleinst_acquire(void);                       // true if this is the FIRST instance
bool singleinst_send_file(const char *utf8_path);    // 2nd inst → hand a file to the winner
bool singleinst_send_focus(void);                    // 2nd inst → just raise the winner
void singleinst_listen_start(void);                  // 1st inst → start listening
bool singleinst_poll_file(char *out, int cap);       // 1st inst → dequeue a forwarded path
bool singleinst_poll_focus(void);                    // 1st inst → pending raise request?

#endif
