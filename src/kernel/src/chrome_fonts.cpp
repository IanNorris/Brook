// The chrome font sets: one baked atlas per type role (ChromeFont) at each
// desktop scale. The atlases are generated at build time
// (cmake/ChromeFonts.cmake: scripts/bake_font.py and scripts/bake_icons.py).
// Freestanding.
#include "chrome_raster.h"

namespace brook {

#define BROOK_CHROME_FONT_SET(scale) \
    extern const FontAtlas g_chromeBody##scale, g_chromeLabel##scale, g_chromeSmall##scale, g_chromeStrong##scale, \
        g_chromeLead##scale, g_chromeHero##scale, g_chromeMono##scale, g_chromeIcons##scale; \
    extern const ChromeFontSet g_chromeFonts##scale = {{&g_chromeBody##scale, &g_chromeLabel##scale, \
        &g_chromeSmall##scale, &g_chromeStrong##scale, &g_chromeLead##scale, &g_chromeHero##scale, \
        &g_chromeMono##scale, &g_chromeIcons##scale}};

BROOK_CHROME_FONT_SET(100)
BROOK_CHROME_FONT_SET(150)
BROOK_CHROME_FONT_SET(200)

} // namespace brook
