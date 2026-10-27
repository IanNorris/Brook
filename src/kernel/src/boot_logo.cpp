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
    uint32_t* fb=nullptr;uint32_t w=0,h=0,stride=0;
    if(TtyGetFramebuffer(&fb,&w,&h,&stride))
        DrawBootSplash(fb,w,h,stride/4,g_bootLogoData,g_bootLogoWidth,g_bootLogoHeight,percent,true);
    SerialPrintf("BOOT: %u%% %s\n",percent,text?text:"");
}
void BootLogoError(uint8_t,const char* text){active=false;TtySuppressDisplay(false);SerialPrintf("BOOT ERROR: %s\n",text?text:"");}
void BootLogoClear(){active=false;TtySuppressDisplay(false);TtyClear();}
void BootLogoFinish(){active=false;}
bool BootLogoActive(){return active;}
}
