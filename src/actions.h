#ifndef EDGES_ACTIONS_H
#define EDGES_ACTIONS_H

/*
 * Runs the side effects of gestures (volume, brightness, OSD, media) on
 * worker threads, so a slow command can never stall the touchpad.
 * Slider updates are coalesced: only the newest target is applied.
 */

#include "config.h"
#include "engine.h"

int eb_actions_start(const eb_config *c);
void eb_actions_set_config(const eb_config *c);
void eb_actions_post(const eb_action *a);
void eb_actions_stop(void);

#endif
