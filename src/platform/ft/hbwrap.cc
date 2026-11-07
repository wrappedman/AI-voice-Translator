/* HarfBuzz amalgamation with the FreeType bridge enabled. */
#ifndef HB_NO_SUBSET
#define HB_NO_SUBSET 1
#endif
#define HAVE_FREETYPE 1
#include <ft2build.h>
#include FT_FREETYPE_H
#include "harfbuzz.cc"
