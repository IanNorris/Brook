#include "../../kernel/src/intel_bdw_context.h"
#include "../../kernel/src/intel_bdw_render_wa.h"
#include <cassert>
#include <cstdint>
using namespace brook;
static uint32_t image[20*1024];
static uint32_t saved[20*1024];
static uint32_t wa[1024];
int main() {
    BdwContext64 c{0x100000,0x200000,0x12345000,16384,56,7};uint64_t descriptor=0;
    for(auto& word:image)word=0xa5a5a5a5;
    assert(BdwBuildRcsContext64(c,image,descriptor));
    assert(descriptor==(uint64_t{7}<<32|0x100000|0x13d));
    for(unsigned i=0;i<1024;++i)assert(image[i]==0);
    const auto* r=image+1024;
    assert(r[1]==0x1100101b && r[2]==0x2244 && r[3]==0x000f0009);
    assert(r[4]==0x2034 && r[5]==0 && r[6]==0x2030 && r[7]==56);
    assert(r[8]==0x2038 && r[9]==0x200000 && r[10]==0x203c && r[11]==0x3001);
    assert(r[24]==0x21c0 && r[26]==0x21c4 && r[28]==0x21c8);
    assert(r[33]==0x11001011 && r[34]==0x23a8 && r[48]==0x2274 && r[49]==0);
    assert(r[50]==0x2270 && r[51]==0x12345000);
    assert(r[65]==0x11000001 && r[66]==0x20c8 && r[67]==0 && r[68]==0x05000000);
    for(unsigned i=2048;i<20*1024;++i)assert(image[i]==0);
    for(auto& word:wa)word=0xa5a5a5a5;
    assert(!BdwBuildRenderIndirectWa(c,0x100000,0x301000,image,wa));
    for(auto word:wa)assert(word==0xa5a5a5a5);
    assert(!BdwBuildRenderIndirectWa(c,0x300000,0x300000,image,wa));
    assert(BdwBuildRenderIndirectWa(c,0x300000,0x301000,image,wa));
    assert(r[27]==0x300002 && r[29]==(0x17u<<6));
    assert(wa[0]==0x04000000 && wa[1]==0x12400002 && wa[2]==0xb118 && wa[3]==0x301100);
    assert(wa[5]==0x11000001 && wa[7]==0x40600000 && wa[8]==0x7a000004 && wa[9]==0x100020);
    assert(wa[14]==0x14c00002 && wa[16]==0x301100 && wa[18]==0x7a000004 && wa[19]==0x08304000);
    assert(wa[20]==0xd0 && wa[24]==0x04000001);
    for(unsigned i=25;i<1024;++i)assert(wa[i]==0);
    assert(!BdwBuildRenderIndirectWa(c,0x300000,0x301000,image,wa));
    for(auto& word:saved)word=0xa5a5a5a5;
    saved[1024+0x59]=0xabcdef01;
    assert(BdwBuildRcsContextFromDefault(c,saved,image,descriptor));
    for(unsigned i=0;i<1024;++i)assert(image[i]==0);
    assert(image[2*1024]==0xa5a5a5a5 && image[19*1024]==0xa5a5a5a5);
    r=image+1024;
    assert(r[3]==0x000f0008 && r[5]==0 && r[7]==56 && r[9]==0x200000 && r[11]==0x3001);
    assert(r[25]==0 && r[27]==0 && r[29]==0 && r[35]==0 && r[49]==0 && r[51]==0x12345000);
    for(unsigned i=37;i<=47;i+=2)assert(r[i]==0);
    assert(r[0x59]==((0xabcdef01u&~0x100u)|0x01000000u));
    assert(r[67]==0xa5a5a5a5 && descriptor==(uint64_t{7}<<32|0x100000|0x13d));
    assert(BdwBuildRenderIndirectWa(c,0x300000,0x301000,image,wa));
    assert(r[27]==0x300002 && r[29]==(0x17u<<6));
    assert(!BdwBuildRcsContextFromDefault(c,saved,saved,descriptor));
    // Invalid geometry must leave both outputs untouched, including the full
    // 20-page context overlap that is not covered by the 2-page BCS check.
    for(auto& word:image)word=0xa5a5a5a5;
    descriptor=0x1234;c.ringGgtt=c.contextGgtt+8192;
    assert(!BdwBuildRcsContext64(c,image,descriptor));
    assert(descriptor==0x1234);for(auto word:image)assert(word==0xa5a5a5a5);
    c.ringGgtt=0x200000;c.contextGgtt=(1ull<<32)-19*4096;
    assert(!BdwBuildRcsContext64(c,image,descriptor));
    c.contextGgtt=0x100000;c.pml4Dma=1ull<<39;
    assert(!BdwBuildRcsContext64(c,image,descriptor));
    assert(!BdwBuildRcsContextFromDefault(c,saved,image,descriptor));
    assert(descriptor==0x1234);for(auto word:image)assert(word==0xa5a5a5a5);
}
