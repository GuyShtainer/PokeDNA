/* host shim: gb_art_source.c needs only the fixed-width ints from tonc.h (+ siprintf) */
#ifndef HOSTSHIM_TONC_H
#define HOSTSHIM_TONC_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define siprintf sprintf
#endif
