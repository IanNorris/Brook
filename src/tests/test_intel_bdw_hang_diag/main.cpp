#include "intel_bdw_hang_diag.h"
#include "intel_bdw_render_ring.h"
#include "intel_bdw_bcs_init.h"
#include "bridge_reply.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
using namespace brook;

static std::string g_out;
static void Collect(const char* s){g_out+=s;}
using Writer=BridgeReplyBuffer<Collect>;

// Fake device: registers default to offset*3, overridable; pages are plain arrays.
struct FakeIo {
    std::map<uint32_t,uint32_t> regs;uint32_t failReg=0;bool pagesOk=true,ptesOk=true;
    alignas(4096) uint8_t pages[37][4096]{};
    bool reg(uint32_t off,uint32_t& v){if(off==failReg)return false;auto it=regs.find(off);v=it==regs.end()?off*3:it->second;return true;}
    const uint8_t* page(unsigned i){return pagesOk && i<37?pages[i]:nullptr;}
    bool pte(unsigned i,uint64_t& v){if(!ptesOk)return false;v=(0x1000000ull+4096ull*i)|3;return true;}
    uint64_t dma(unsigned i){return 0x1000000ull+4096ull*i;}
    void word(unsigned page,unsigned dword,uint32_t v){std::memcpy(pages[page]+4*dword,&v,4);}
};

static bool Balanced(const std::string& s){
    int depth=0;bool str=false;
    for(size_t i=0;i<s.size();++i){char c=s[i];
        if(str){if(c=='\\')++i;else if(c=='"')str=false;continue;}
        if(c=='"')str=true;else if(c=='{'||c=='[')++depth;else if(c=='}'||c==']'){if(--depth<0)return false;}
        if((c=='}'||c==']') && i && s[i-1]==',')return false;
    }
    return depth==0 && !str;
}

// A failed job as the worker would publish it: ELSP written, marker on.
static void Submitted(BdwHangCapture& c,FakeIo& io,uint32_t nonce){
    BdwHangClear(c);c.kind=1;c.nonce=nonce;c.runFailSite=2;c.submitSite=10;c.khz=2000000;
    c.diag.portWrites=4;c.diag.marker=true;c.diag.originTsc=1000;c.diag.elspTsc=1000+2000*50;
    io.regs[0x2034]=0;io.regs[0x2030]=0xe0;io.regs[0x23a0]=0x505;io.regs[0x2234]=0x1;io.regs[0x2068]=0;
    io.regs[0x4094]=0;io.regs[0x40a0]=0;io.regs[0x20b0]=0;io.regs[0x2140]=0;io.regs[0x2168]=0;
    io.word(24,0x1f,5);
}

int main(){
    // 1. Breadcrumb ring: numbering, wrap-around and bounds.
    {static BdwHangCrumbRing<128> ring;
     for(uint32_t i=0;i<200;++i){BdwHangBreadcrumb b{};b.pid=i*3;b.nonce=0x42525000+i;ring.Append(b);}
     assert(ring.Count()==200);BdwHangBreadcrumb b{};
     assert(ring.Read(199,b) && b.job==200 && b.pid==199*3 && b.nonce==0x42525000+199);
     assert(ring.Read(72,b) && b.job==73 && b.pid==72*3);
     assert(!ring.Read(71,b) && !ring.Read(200,b) && !ring.Read(0,b));}
    // 2. Concurrent reader never sees a torn record.
    {static BdwHangCrumbRing<16> ring;std::atomic<bool> done{false};uint64_t good=0,torn=0;
     std::thread reader([&]{while(!done.load()){const uint64_t n=ring.Count();if(!n)continue;BdwHangBreadcrumb b{};
         if(ring.Read(n-1,b)){if(b.pid!=b.job*7 || b.nonce!=~b.job || b.batch!=uint64_t(b.job)<<20)++torn;else ++good;}}});
     for(uint32_t i=1;i<=300000;++i){BdwHangBreadcrumb b{};b.pid=i*7;b.nonce=~i;b.batch=uint64_t(i)<<20;ring.Append(b);}
     done=true;reader.join();assert(!torn);std::printf("seqlock reads ok=%llu torn=0\n",(unsigned long long)good);}
    // 3. Append cost (always-on path, host terms).
    {static BdwHangCrumbRing<128> ring;BdwHangBreadcrumb b{};const unsigned n=2000000;
     const auto start=std::chrono::steady_clock::now();
     for(unsigned i=0;i<n;++i){b.pid=i;ring.Append(b);}
     const auto ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/n;
     assert(ring.Count()==n);std::printf("breadcrumb append %.1f ns (record %zu bytes, capture %zu bytes)\n",ns,sizeof(BdwHangBreadcrumb),sizeof(BdwHangCapture));}
    // 4. Capture: registers, CSB registers, pages and PTEs land where expected.
    static FakeIo io;static BdwHangCapture c;
    {io.regs[0x2074]=0x10014abc;io.word(1,7,0xe0);io.word(20,39,0x18800101);io.word(24,0x1f,0x00000003);
     io.word(25,0,0x42525111);io.word(25,4,0x42525111);io.word(0,0x34,0xdead);
     BdwHangClear(c);BdwHangCaptureDevice(io,c);
     assert(c.regsValid && c.pagesValid && c.ptesValid && c.version==1);
     assert(BdwHangReg32(c,0x2074)==0x10014abc && BdwHangReg32(c,0x2068)==0x2068*3 && BdwHangReg32(c,0x130044)==0x130044*3);
     assert(c.csbRegs[0]==0x2370*3 && c.csbRegs[11]==0x239c*3);
     assert(c.lrc[7]==0xe0 && c.ring[39]==0x18800101 && c.hws[0xf]==3 && c.completion[0]==0x42525111 &&
            c.completion[4]==0x42525111 && c.pphwsp[4]==0xdead);
     assert(c.pte[28]==((0x1000000ull+4096ull*28)|3) && c.dma[24]==0x1000000ull+4096ull*24);
     io.failReg=0x2068;io.pagesOk=false;io.ptesOk=false;BdwHangClear(c);BdwHangCaptureDevice(io,c);
     assert(!c.regsValid && !c.pagesValid && !c.ptesValid && BdwHangReg32(c,0x2068)==0xffffffffu && c.pte[0]==~uint64_t{0});
     io.failReg=0;io.pagesOk=io.ptesOk=true;}
    // 5. Verdicts for each hypothesis.
    auto verdict=[&](){BdwHangCaptureDevice(io,c);return std::string(BdwHangVerdict(c));};
    const uint32_t nonce=0x42526000;
    {Submitted(c,io,nonce);c.diag.portWrites=0;c.diag.initStage=4;c.submitSite=7;assert(verdict()=="execlist_init_failed_not_submitted");}
    {Submitted(c,io,nonce);io.word(25,0,nonce);io.word(25,1,0);assert(verdict()=="breadcrumb_late_after_deadline");io.word(25,0,0);}
    {Submitted(c,io,nonce);io.regs[0x2034]=0xe0;io.word(25,4,nonce);io.word(25,5,0);
     assert(verdict()=="ring_completed_breadcrumb_missing");}
    {Submitted(c,io,nonce);io.word(25,4,0);assert(verdict()=="elsp_lost_no_csb_event");}
    {Submitted(c,io,nonce);io.word(25,4,0);io.regs[0x2234]=1u<<3;assert(verdict()=="context_not_loaded_element_pending");}
    {Submitted(c,io,nonce);io.word(25,4,0);io.regs[0x23a0]=0x500;io.regs[0x2034]=0x40;assert(verdict()=="stalled_in_ring_prelude");}
    {Submitted(c,io,nonce);io.word(25,4,nonce);io.regs[0x23a0]=0x500;io.regs[0x2034]=0xa8;io.regs[0x2140]=0x8000;
     assert(verdict()=="hung_in_user_batch");}
    {Submitted(c,io,nonce);io.word(25,4,nonce);io.regs[0x2034]=0xa8;io.regs[0x2068]=0x0e000002;// MI_SEMAPHORE_WAIT
     assert(verdict()=="waiting_semaphore_or_event_in_batch");}
    {Submitted(c,io,nonce);io.regs[0x4094]=0x1;assert(verdict()=="gpu_fault");io.regs[0x4094]=0;}
    {Submitted(c,io,nonce);c.runFailSite=3;assert(verdict()=="csb_unexpected_event_after_breadcrumb");}
    {Submitted(c,io,nonce);c.kind=2;io.word(25,0,nonce);io.word(25,1,0);assert(verdict()=="completed");io.word(25,0,0);}
    // 6. JSON is balanced and carries the key fields.
    {Submitted(c,io,nonce);io.regs[0x2034]=0xe0;io.word(25,4,nonce);BdwHangCaptureDevice(io,c);
     c.diag.yields=5;c.diag.grace=1;c.diag.longest.valid=true;c.diag.longest.totalTicks=2000000ull*2194;
     c.diag.longest.waitTicks=2000000ull*2190;c.diag.longest.switches=1;c.diag.longest.fromPid=432;
     c.pid=30;c.ctx=1;c.crumbCount=2;c.crumbs[0].job=7;c.crumbs[1].job=8;c.crumbs[1].outcome=BdwHangRecovered;
     g_out.clear();Writer w;BdwHangCaptureJson(w,c);w.send();
     assert(Balanced(g_out));
     for(const char* key:{"\"verdict\":\"ring_completed_breadcrumb_missing\"","\"RING_ACTHD\":\"0x","\"EXECLIST_STATUS_LO\":\"0x1\"",
                          "\"GEN8_RING_FAULT_REG\":","\"csb_regs\":[","\"lrc_regstate\":[","\"ring\":[","\"ggtt\":[[",
                          "\"crumbs\":[{\"job\":7","\"outcome\":\"failed_recovered\"","\"pid\":30","\"elsp\":50","\"grace\":1","\"total_us\":2194000","\"wait_us\":2190000","\"from_pid\":432"})
         if(g_out.find(key)==std::string::npos){std::printf("missing %s in %s\n",key,g_out.c_str());return 1;}
     std::printf("capture json %zu bytes\n",g_out.size());
     g_out.clear();Writer v;BdwHangCrumbJson(v,c.crumbs[1]);v.send();assert(Balanced(g_out) && g_out.find("\"job\":8")!=std::string::npos);}
    // 7. Started marker: only ring dwords 33..36 change, to a post-sync write of
    // the nonce at completion+16 with the breadcrumb's own PIPE_CONTROL flags.
    {uint32_t plain[56]{},marked[56]{};
     assert(BdwBuildRenderRing(0x123456780008ull,0x200000,0x4252,true,false,plain));
     assert(BdwBuildRenderRing(0x123456780008ull,0x200000,0x4252,true,false,marked,true));
     for(unsigned i=0;i<56;++i)if(i<33 || i>36)assert(plain[i]==marked[i]);
     assert(plain[32]==0x7a000004 && plain[33]==(1u<<20) && !plain[34] && !plain[36]);
     assert(marked[33]==plain[49] && marked[34]==0x200010 && marked[35]==0 && marked[36]==0x4252 && marked[37]==0);
     assert(plain[50]==0x200000 && plain[52]==0x4252);
     BdwHangSetStartedMarker(false);assert(!BdwHangStartedMarkerEnabled());BdwHangSetStartedMarker(true);assert(BdwHangStartedMarkerEnabled());}
    // 8. Execlist init diagnostics name the failed step and its value.
    {struct InitIo {uint32_t mode=0x8000,miMode=0,hws=0;bool syncOk=true;
         bool owned(){return true;} bool quiescent(){return true;} bool csbPrepared(uint64_t){return true;}
         bool writeReg(uint32_t r,uint32_t v){if(r==0x2080)hws=v;return true;}
         bool readReg(uint32_t r,uint32_t& v){v=r==0x229c?mode:r==0x209c?miMode:hws;return true;}
         bool synchronizeCsb(uint64_t){return syncOk;}} init;
     BdwExeclistInitDiag d{};
     assert(BdwInitializeEngineExeclists<0x2000>(init,0x10018000,&d) && d.stage==0);
     init.mode=0;assert(!BdwInitializeEngineExeclists<0x2000>(init,0x10018000,&d) && d.stage==2 && d.value==0);
     init.mode=0x8000;init.miMode=0x100;assert(!BdwInitializeEngineExeclists<0x2000>(init,0x10018000,&d) && d.stage==3 && d.value==0x100);
     init.miMode=0;assert(!BdwInitializeEngineExeclists<0x2000>(init,0x10018001,&d) && d.stage==1);
     assert(BdwInitializeEngineExeclists<0x2000>(init,0x10018000));}
    std::printf("test_intel_bdw_hang_diag ok\n");
    return 0;
}
