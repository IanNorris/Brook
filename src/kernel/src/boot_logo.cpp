#include "boot_logo.h"
#include "brook_boot_logo_data.h"
#include "boot_splash.h"
#include "boot_status_text.h"
#include "font_atlas.h"
#include "tty.h"
#include "serial.h"
namespace brook {
static bool active=true;
static uint8_t progress=5;
static char statusLine[80]="Starting kernel";

// One centred status line under the bar (replacing the loader's percentage
// digits): Hack glyphs from the kernel atlas, 2x on 4K-class screens.
static void DrawStatusLine(uint32_t* pixels,uint32_t width,uint32_t height,uint32_t stride)
{
    const auto bar=BootSplashBarRect(width,height,g_bootLogoWidth,g_bootLogoHeight);
    const FontAtlas& font=g_fontAtlas;
    const uint32_t scale=width>=2560?2:1;
    const uint32_t top=bar.y+12,rows=uint32_t(font.lineHeight)*scale+8;
    for(uint32_t y=top;y<top+rows && y<height;++y)for(uint32_t x=0;x<width;++x)pixels[uint64_t(y)*stride+x]=0;
    uint32_t textWidth=0;
    for(const char* c=statusLine;*c;++c){
        const uint32_t i=uint32_t(uint8_t(*c))-font.firstChar;
        if(i<font.glyphCount)textWidth+=uint32_t(font.glyphs[i].advance)*scale;
    }
    int32_t pen=int32_t(width>textWidth?(width-textWidth)/2:0);
    const int32_t baseline=int32_t(top+4)+font.ascent*int32_t(scale);
    for(const char* c=statusLine;*c;++c){
        const uint32_t i=uint32_t(uint8_t(*c))-font.firstChar;
        if(i>=font.glyphCount)continue;
        const GlyphInfo& g=font.glyphs[i];
        for(int32_t gy=g.atlasY0;gy<g.atlasY1;++gy)for(int32_t gx=g.atlasX0;gx<g.atlasX1;++gx){
            const uint32_t cover=font.pixels[uint64_t(gy)*font.atlasWidth+uint32_t(gx)];
            if(!cover)continue;
            const uint32_t r=0xb8*cover/255,gr=0xc9*cover/255,b=0xdf*cover/255,color=(r<<16)|(gr<<8)|b;
            for(uint32_t sy=0;sy<scale;++sy)for(uint32_t sx=0;sx<scale;++sx){
                const int32_t px=pen+(g.bearingX+(gx-g.atlasX0))*int32_t(scale)+int32_t(sx);
                const int32_t py=baseline-(g.bearingY-(gy-g.atlasY0))*int32_t(scale)+int32_t(sy);
                if(px>=0&&py>=0&&uint32_t(px)<width&&uint32_t(py)<height)pixels[uint64_t(py)*stride+uint32_t(px)]=color;
            }
        }
        pen+=g.advance*int32_t(scale);
    }
}

static void Redraw(uint8_t percent)
{
    TtyTryDrawFramebuffer([](uint32_t* pixels, uint32_t width, uint32_t height, uint32_t stride, void* arg) {
        DrawBootSplash(pixels,width,height,stride,g_bootLogoData,g_bootLogoWidth,g_bootLogoHeight,
                       *static_cast<uint8_t*>(arg),true);
        DrawStatusLine(pixels,width,height,stride);
    }, &percent);
}

void BootLogoInit(){active=true;TtySuppressDisplay(true);BootLogoProgress(progress,"Kernel");}
void BootLogoProgress(uint8_t percent,const char* text){
    if(!active)return;
    if(percent>100)percent=100;
    if(percent<progress)percent=progress;
    progress=percent;
    if(const char* status=BootStatusText(text)){
        unsigned n=0;while(status[n]&&n+1<sizeof(statusLine)){statusLine[n]=status[n];++n;}statusLine[n]=0;
    }
    Redraw(percent);
    SerialPrintf("BOOT: %u%% %s\n",percent,text?text:"");
}
void BootLogoStatus(uint8_t percent,const char* status){
    if(!active || !status)return;
    if(percent>100)percent=100;
    if(percent<progress)percent=progress;
    progress=percent;
    unsigned n=0;while(status[n]&&n+1<sizeof(statusLine)){statusLine[n]=status[n];++n;}statusLine[n]=0;
    Redraw(percent);
}
void BootLogoError(uint8_t,const char* text){active=false;TtySuppressDisplay(false);SerialPrintf("BOOT ERROR: %s\n",text?text:"");}
void BootLogoClear(){active=false;TtySuppressDisplay(false);TtyClear();}
void BootLogoFinish(){active=false;}
bool BootLogoActive(){return active;}
}
