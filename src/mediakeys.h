#ifndef TIVI_MEDIAKEYS_H
#define TIVI_MEDIAKEYS_H

// System-wide media-key support (Windows). A small background thread registers a
// low-level keyboard hook; the main loop polls for pending transport actions.
enum { MK_NONE = 0, MK_PLAYPAUSE, MK_STOP, MK_PREV, MK_NEXT };

void mediakeys_start(void);
int  mediakeys_poll(void);   // returns one MK_* action and clears it

#endif
