// On a PC, program memory IS memory. These macros make the font tables in
// glcdfont.c and gfxfont.h read as plain arrays.
#pragma once
#include <stdint.h>
#include <string.h>

#define PROGMEM
#define PGM_P const char *
#define PSTR(s) (s)
#define pgm_read_byte(a)      (*(const uint8_t *)(a))
#define pgm_read_byte_near(a) (*(const uint8_t *)(a))
#define pgm_read_word(a)      (*(const uint16_t *)(a))
#define pgm_read_dword(a)     (*(const uint32_t *)(a))
#ifndef pgm_read_pointer
#define pgm_read_pointer(a)   (*(void *const *)(a))
#endif
#define memcpy_P memcpy
#define strlen_P strlen
#define strcpy_P strcpy
