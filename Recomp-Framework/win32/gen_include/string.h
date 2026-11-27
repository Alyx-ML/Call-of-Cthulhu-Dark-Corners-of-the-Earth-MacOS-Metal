// The <string.h> of the translated game code (gen/*.c) when the app compiles it on the player's Mac (see math.h in
// this folder). The prototypes of the C standard; libSystem gives the functions when the game library loads.
#pragma once
#include <stddef.h>

void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
