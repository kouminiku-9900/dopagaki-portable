#ifndef KOMI_OSK_H
#define KOMI_OSK_H

#include <stdbool.h>
#include <stddef.h>

/* Show the firmware keyboard (blocking) and return the text as UTF-8.
   False when the user cancelled. */
bool osk_input(const char *description, const char *initial, char *output,
               size_t capacity);

#endif
