/* ----------------------------------------------------------------------- */
/*  Minimal SBCS code conversion functions for FatFs LFN (ASCII only)       */
/*                                                                         */
/*  This is a simplified replacement of ChaN's ccsbcs.c.                   */
/*  It is enough for file names like "backup.json" (ASCII).                */
/*  Non-ASCII national characters will be treated as invalid (return 0).   */
/* ----------------------------------------------------------------------- */

#include "ff.h"

#if _USE_LFN

/*-----------------------------------------------------------------------*/
/* Convert OEM code <==> Unicode (for SBCS)                               */
/*  dir = 0: OEM  -> Unicode                                              */
/*  dir = 1: Unicode -> OEM                                               */
/*-----------------------------------------------------------------------*/
WCHAR ff_convert (WCHAR chr, UINT dir)
{
    (void)dir;

    /* Accept basic ASCII only */
    if (chr < 0x80) {
        return chr;
    }
    return 0; /* invalid character */
}

/*-----------------------------------------------------------------------*/
/* Unicode upper-case conversion                                          */
/*-----------------------------------------------------------------------*/
WCHAR ff_wtoupper (WCHAR chr)
{
    if (chr >= 'a' && chr <= 'z') {
        chr = (WCHAR)(chr - 'a' + 'A');
    }
    return chr;
}

#endif /* _USE_LFN */
