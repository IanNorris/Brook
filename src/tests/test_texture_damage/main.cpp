#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../../kernel/src/texture_damage.h"
using namespace brook;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line%d %s\n",__LINE__,#x);std::abort();}}while(0)
namespace {
struct GpuTexture {uint32_t resId=77,w=1920,h=1080;bool owned=false;} texture;
static bool g_controlQueuePoisoned,accept=true;
static unsigned depth;
static constexpr uint32_t COMP_CTX_ID=2;
struct Request {uint32_t x,y,w,h;};
static std::vector<Request> requests;
struct GpuCmdGuard {GpuCmdGuard(){CHECK(!depth);++depth;}~GpuCmdGuard(){--depth;}};
static GpuTexture* CompTex(uint32_t id){return id==1?&texture:nullptr;}
using GpuTexId=uint32_t;
static bool TransferToHost3D(uint32_t ctx,uint32_t res,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t stride,uint32_t rows) {
    CHECK(depth==1 && ctx==2 && res==77 && stride==1920 && rows==1080);
    CHECK(x<stride && w<=stride-x && y<rows && h<=rows-y);
    requests.push_back({x,y,w,h});return accept;
}
#include "../../drivers/virtio_gpu/texture_update.inc"
}
int main() {
    TextureDamage wallpaper,chrome,overlay;bool full=false;
    auto upload=[](uint32_t x,uint32_t y,uint32_t w,uint32_t h){return CompUpdateTexture(1,x,y,w,h);};
    overlay.Mark(1044,1080);
    CHECK(wallpaper.Flush(full,1920,1080,upload) && chrome.Flush(full,1920,1080,upload));
    CHECK(requests.empty()); // taskbar invalidation cannot upload other layers
    accept=false;CHECK(!overlay.Flush(full,1920,1080,upload));
    CHECK(overlay.first==1044 && overlay.last==1080);
    overlay.Mark(600,900);accept=true;
    CHECK(overlay.Flush(full,1920,1080,upload));
    CHECK(requests.back().y==600 && requests.back().h==480);
    auto count=requests.size();CHECK(overlay.Flush(full,1920,1080,upload) && requests.size()==count);
    // New texture/full refresh is retained across a refused/failed transfer.
    full=true;g_controlQueuePoisoned=true;
    CHECK(!wallpaper.Flush(full,1920,1080,upload) && full && requests.size()==count);
    g_controlQueuePoisoned=false;accept=false;
    CHECK(!wallpaper.Flush(full,1920,1080,upload) && full);
    accept=true;CHECK(wallpaper.Flush(full,1920,1080,upload) && !full);
    CHECK(requests.back().y==0 && requests.back().h==1080);
    // Device recreation must request full content even with no pending rows.
    full=true;CHECK(wallpaper.Flush(full,1920,1080,upload));
    CHECK(requests.back().w==1920 && requests.back().h==1080);
    CHECK(CompUpdateTexture(1,1910,1070,~0u,~0u));
    CHECK(requests.back().w==10 && requests.back().h==10);
    count=requests.size();texture.owned=true;CHECK(!CompUpdateTexture(1,0,0,1,1));texture.owned=false;
    CHECK(!CompUpdateTexture(0,0,0,1,1) && !CompUpdateTexture(1,1920,0,1,1));
    CHECK(!CompUpdateTexture(1,0,0,0,1) && requests.size()==count);
    full=true;CHECK(!overlay.Flush(full,0,1080,upload) && full);
    std::puts("TEXTURE_DAMAGE_PASS actual upload response, retry, layer isolation, recreation, overflow");
}
