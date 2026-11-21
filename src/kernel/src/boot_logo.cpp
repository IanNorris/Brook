#include "boot_logo.h"
#include "brook_boot_logo_data.h"
#include "boot_splash.h"
#include "tty.h"
#include "serial.h"
namespace brook {
static bool active=true;
static uint8_t progress=5;
void BootLogoInit(){active=true;TtySuppressDisplay(true);BootLogoProgress(progress,"Kernel");}
void BootLogoProgress(uint8_t percent,const char* text){
    if(!active)return;
    if(percent>100)percent=100;
    if(percent<progress)percent=progress;
    progress=percent;
    TtyTryDrawFramebuffer([](uint32_t* pixels, uint32_t width, uint32_t height, uint32_t stride, void* arg) {
        DrawBootSplash(pixels,width,height,stride,g_bootLogoData,g_bootLogoWidth,g_bootLogoHeight,
                       *static_cast<uint8_t*>(arg),true);
    }, &percent);
    SerialPrintf("BOOT: %u%% %s\n",percent,text?text:"");
}
void BootLogoError(uint8_t,const char* text){active=false;TtySuppressDisplay(false);SerialPrintf("BOOT ERROR: %s\n",text?text:"");}
void BootLogoClear(){active=false;TtySuppressDisplay(false);TtyClear();}
void BootLogoFinish(){active=false;}
bool BootLogoActive(){return active;}
}
