#ifndef HOME_JAZZ_H
#define HOME_JAZZ_H
#include <stdbool.h>
/* SomaFM internet streams. Nothing is stored on the device. */
void home_jazz_toggle(void);
bool home_jazz_on(void);
/* Hold the upper side button: next station; hold the lower side button: previous. */
void home_jazz_station_step(int delta);
/* Short label for the desk tile (e.g. JAZZ, Lofi). */
const char *home_jazz_station_label(void);
/* Side buttons. Heard immediately; the paper is not redrawn. */
void home_jazz_volume_step(int delta);
/* A short tick. While jazz is playing it is mixed in; otherwise the speaker plays it alone. */
void home_jazz_click(void);
#endif
