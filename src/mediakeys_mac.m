// mediakeys_mac.m — macOS backend for mediakeys.h (see timp for the original).
// Uses MPRemoteCommandCenter (the sanctioned route since macOS 10.12.2): the
// system routes the keyboard transport keys / AirPods taps / Now Playing
// widget to whichever app registered handlers and reports a playback state —
// no Input Monitoring permission, and it coexists with other media apps
// exactly like the Windows low-level-hook approach. Compile with -fobjc-arc.
#ifdef __APPLE__

#import <MediaPlayer/MediaPlayer.h>
#include <stdatomic.h>
#include "mediakeys.h"

static _Atomic int g_action = MK_NONE;

void mediakeys_start(void) {
    MPRemoteCommandCenter *c = [MPRemoteCommandCenter sharedCommandCenter];

    [c.togglePlayPauseCommand addTargetWithHandler:^(MPRemoteCommandEvent *e) {
        (void)e; atomic_store(&g_action, MK_PLAYPAUSE); return MPRemoteCommandHandlerStatusSuccess; }];
    [c.playCommand addTargetWithHandler:^(MPRemoteCommandEvent *e) {
        (void)e; atomic_store(&g_action, MK_PLAYPAUSE); return MPRemoteCommandHandlerStatusSuccess; }];
    [c.pauseCommand addTargetWithHandler:^(MPRemoteCommandEvent *e) {
        (void)e; atomic_store(&g_action, MK_PLAYPAUSE); return MPRemoteCommandHandlerStatusSuccess; }];
    [c.stopCommand addTargetWithHandler:^(MPRemoteCommandEvent *e) {
        (void)e; atomic_store(&g_action, MK_STOP); return MPRemoteCommandHandlerStatusSuccess; }];
    [c.previousTrackCommand addTargetWithHandler:^(MPRemoteCommandEvent *e) {
        (void)e; atomic_store(&g_action, MK_PREV); return MPRemoteCommandHandlerStatusSuccess; }];
    [c.nextTrackCommand addTargetWithHandler:^(MPRemoteCommandEvent *e) {
        (void)e; atomic_store(&g_action, MK_NEXT); return MPRemoteCommandHandlerStatusSuccess; }];

    // The system only targets us with remote commands once we look like a
    // playing app. Timp has no play-state plumbing into this module, so report
    // "playing" up front — good enough for key routing, matching the Windows
    // hook's always-on behaviour.
    MPNowPlayingInfoCenter *np = [MPNowPlayingInfoCenter defaultCenter];
    np.nowPlayingInfo = @{ MPMediaItemPropertyTitle : @"tivi" };
    np.playbackState = MPNowPlayingPlaybackStatePlaying;
}

int mediakeys_poll(void) {
    return atomic_exchange(&g_action, MK_NONE);
}

#endif // __APPLE__
