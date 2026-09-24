/* tilefinch's persistent log, with its body compiled in. src/psp_log.c
   reduces to stubs unless TILEFINCH_PSP_VALIDATION_LOG is set, and setting
   that for the whole player would change struct layouts it shares with
   tilefinch_core; this one translation unit is safe to build with it. */
#define TILEFINCH_PSP_VALIDATION_LOG 1
#include "psp_log.c"
