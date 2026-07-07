#pragma once
#include <stdbool.h>

enum { SND_JUMP, SND_FALL, SND_COUNT };

/* Loads the wav files under assets/ and starts the mixer thread. Returns
 * false (and the game keeps running silently) if audio is unavailable. */
bool sound_init(void);
void sound_play(int clip);
void sound_shutdown(void);
