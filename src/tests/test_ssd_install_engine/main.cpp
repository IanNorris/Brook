#include "../../kernel/src/ssd_install_engine.h"
#include "../../kernel/src/ssd_install_preflight.h"
#include "../../kernel/src/ssd_install_compact_source.h"
#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
using namespace brook;
struct Io {
    std::vector<uint8_t> source,target;unsigned calls=0,failAt=0,writes=0,flushes=0;
    bool corruptRead=false,shortRead=false,shortWrite=false,ignoreZero=false;
    int readSource(uint64_t lba,void* out,uint32_t n){if(++calls==failAt)return -5;std::memcpy(out,source.data()+lba*512,n);return int(shortRead?n/2:n);}
    int readTarget(uint64_t lba,void* out,uint32_t n){if(++calls==failAt)return -5;std::memcpy(out,target.data()+lba*512,n);if(corruptRead)static_cast<uint8_t*>(out)[0]^=1;return int(shortRead?n/2:n);}
    int writeTarget(uint64_t lba,const void* data,uint32_t n){++writes;if(++calls==failAt)return -5;
        if(shortWrite){std::memcpy(target.data()+lba*512,data,n/2);return int(n/2);}
        if(!ignoreZero)std::memcpy(target.data()+lba*512,data,n);return int(n);}
    int flushTarget(){++flushes;return ++calls==failAt?-5:0;}
};
struct CompactIo {
    SsdInstallCompactSource map;
    std::vector<uint8_t> packed, target;
    unsigned reads=0;
    int readPacked(uint64_t offset,void* out,uint32_t n) {
        ++reads;if(offset>packed.size() || n>packed.size()-offset)return -1;
        std::memcpy(out,packed.data()+offset,n);return int(n);
    }
    int readSource(uint64_t lba,void* out,uint32_t n){return map.Read(*this,lba*512,out,n);}
    int readTarget(uint64_t lba,void* out,uint32_t n){std::memcpy(out,target.data()+lba*512,n);return int(n);}
    int writeTarget(uint64_t lba,const void* data,uint32_t n){std::memcpy(target.data()+lba*512,data,n);return int(n);}
    int flushTarget(){return 0;}
};
struct MockPackedReader {
    unsigned calls=0; uint64_t lastOffset=0; bool shortRead=false;
    int readPacked(uint64_t offset,void*,uint32_t n) {++calls;lastOffset=offset;return shortRead?int(n/2):int(n);}
};
static Io makeIo(){Io io;io.source.resize(4096*512);io.target.assign(4096*512,0xa5);
    for(size_t i=0;i<io.source.size();++i)io.source[i]=uint8_t((i*31+17)&255);return io;}
struct FileIo {
    int source,target;
    int readSource(uint64_t lba,void* data,uint32_t n){return int(pread(source,data,n,off_t(lba*512)));}
    int readTarget(uint64_t lba,void* data,uint32_t n){return int(pread(target,data,n,off_t(lba*512)));}
    int writeTarget(uint64_t lba,const void* data,uint32_t n){return int(pwrite(target,data,n,off_t(lba*512)));}
    int flushTarget(){return fsync(target);}
};
static int fixture(const char* sourcePath,const char* targetPath){
    int source=open(sourcePath,O_RDONLY|O_NOFOLLOW);if(source<0)return 1;struct stat st{};
    if(fstat(source,&st) || !S_ISREG(st.st_mode) || st.st_size<68*512 || st.st_size%512){close(source);return 1;}
    const uint64_t sectors=uint64_t(st.st_size)/512;uint8_t mbr[512],primary[512],backup[512],entries[16384],copy[16384];
    if(pread(source,mbr,512,0)!=512 || pread(source,primary,512,512)!=512 ||
       pread(source,entries,sizeof(entries),1024)!=sizeof(entries) ||
       pread(source,copy,sizeof(copy),off_t((sectors-33)*512))!=sizeof(copy) ||
       pread(source,backup,512,off_t((sectors-1)*512))!=512){close(source);return 1;}
    const auto first=StrictLe64(entries+32),last=StrictLe64(entries+40);
    if(last<first){close(source);return 1;}const SsdInstallGeometry g{sectors,first,last-first+1};
    if(!SsdInstallPreflight(g,sectors,sectors,512,mbr,primary,backup,entries,copy,sizeof(entries))){close(source);return 1;}
    int target=open(targetPath,O_CREAT|O_EXCL|O_RDWR|O_NOFOLLOW,0600);if(target<0){close(source);return 1;}
    if(ftruncate(target,st.st_size)){close(target);close(source);return 1;}
    const uint8_t sentinel=0xa5;if(pwrite(target,&sentinel,1,100*512)!=1){close(target);close(source);return 1;}
    std::vector<uint8_t> a(65536),b(65536);FileIo io{source,target};SsdInstallEngine engine(g,true);
    SsdInstallStepResult result;
    do{result=engine.Step(io,a.data(),b.data());}while(result==SsdInstallStepResult::Progress);
    uint8_t gap=0;const bool ok=result==SsdInstallStepResult::Complete && pread(target,&gap,1,100*512)==1 && gap==sentinel;
    close(target);close(source);return ok?0:1;
}
int main(int argc,char** argv){
    if(argc==3)return fixture(argv[1],argv[2]);
    assert(argc==1);
    const SsdInstallGeometry g{4096,2048,129};std::vector<uint8_t> a(65536),b(65536);
    SsdInstallEngine denied(g,false);auto none=makeIo();assert(denied.Step(none,a.data(),b.data())==SsdInstallStepResult::Failed && !none.calls);
    SsdInstallEngine alias(g,true);assert(alias.Step(none,a.data(),a.data()+1)==SsdInstallStepResult::Failed && !none.calls);
    auto io=makeIo();SsdInstallEngine engine(g,true);unsigned steps=0;
    while(engine.Step(io,a.data(),b.data())==SsdInstallStepResult::Progress)assert(++steps<100);
    assert(engine.Stage()==SsdInstallStage::Complete && io.flushes==6);
    for(uint64_t sector=0;sector<g.diskSectors;++sector){
        const bool written=sector==0 || (sector>=1 && sector<34) ||
            (sector>=g.payloadFirst && sector<g.payloadFirst+g.payloadSectors) || sector>=g.diskSectors-33;
        for(unsigned j=0;j<512;++j){const auto at=sector*512+j;assert(io.target[at]==(written?io.source[at]:0xa5));}
    }
    CompactIo compact{SsdInstallCompactSource(g,(34+g.payloadSectors+33)*512),{},std::vector<uint8_t>(g.diskSectors*512,0xa5)};
    compact.packed.insert(compact.packed.end(),io.source.begin(),io.source.begin()+34*512);
    compact.packed.insert(compact.packed.end(),io.source.begin()+g.payloadFirst*512,io.source.begin()+(g.payloadFirst+g.payloadSectors)*512);
    compact.packed.insert(compact.packed.end(),io.source.end()-33*512,io.source.end());
    assert(compact.map.Valid() && compact.map.ExpectedSize()==compact.packed.size());
    SsdInstallEngine compactEngine(g,true);
    while(compactEngine.Step(compact,a.data(),b.data())==SsdInstallStepResult::Progress){}
    assert(compactEngine.Stage()==SsdInstallStage::Complete && compact.target==io.target);
    const auto beforeReads=compact.reads;
    assert(compact.map.Read(compact,33*512,a.data(),1024)==-1 && compact.reads==beforeReads);
    assert(compact.map.Read(compact,(g.payloadFirst+g.payloadSectors)*512-1,a.data(),2)==-1 && compact.reads==beforeReads);
    assert(compact.map.Read(compact,(g.diskSectors-33)*512-1,a.data(),2)==-1 && compact.reads==beforeReads);
    assert(compact.map.Read(compact,g.diskSectors*512-1,a.data(),2)==-1 && compact.reads==beforeReads);
    assert(!SsdInstallCompactSource(g,compact.packed.size()-1).Valid());
    const SsdInstallGeometry physicalSize{500118192,2048,12582912};
    const SsdInstallCompactSource physicalMap(physicalSize,(34+physicalSize.payloadSectors+33)*512);
    assert(physicalMap.Valid() && physicalMap.ExpectedSize()<physicalSize.diskSectors*512);
    const SsdInstallGeometry adjacent{100,34,33};
    CompactIo contiguous{SsdInstallCompactSource(adjacent,100*512),std::vector<uint8_t>(100*512),{}};
    for(size_t i=0;i<contiguous.packed.size();++i)contiguous.packed[i]=uint8_t(i/512);
    assert(contiguous.map.Read(contiguous,33*512,a.data(),35*512)==35*512);
    for(unsigned sector=0;sector<35;++sector)assert(a[sector*512]==sector+33);
    assert(contiguous.map.Read(contiguous,0,nullptr,1)==-1);
    assert(contiguous.map.Read(contiguous,UINT64_MAX,a.data(),1)==-1);
    assert(contiguous.map.Read(contiguous,100*512,nullptr,0)==0);
    assert(!SsdInstallCompactSource({67,34,1},0).Valid());
    const SsdInstallCompactSource minimum({68,34,1},68*512);
    MockPackedReader mock;assert(minimum.Valid());
    assert(minimum.Read(mock,33*512,a.data(),3*512)==3*512 && mock.calls==3);
    mock.shortRead=true;assert(minimum.Read(mock,0,a.data(),512)==-1);
    const SsdInstallGeometry maximum{1ull<<48,34,1};
    const SsdInstallCompactSource maximumMap(maximum,68*512);
    mock.shortRead=false;assert(maximumMap.Valid());
    assert(maximumMap.Read(mock,(maximum.diskSectors-1)*512,a.data(),512)==512 && mock.lastOffset==67*512);
    const unsigned total=io.calls;
    for(unsigned fail=1;fail<=total;++fail){auto broken=makeIo();broken.failAt=fail;SsdInstallEngine attempt(g,true);
        while(attempt.Step(broken,a.data(),b.data())==SsdInstallStepResult::Progress){}
        assert(attempt.Stage()==SsdInstallStage::Failed);const auto calls=broken.calls;
        assert(attempt.Step(broken,a.data(),b.data())==SsdInstallStepResult::Failed && broken.calls==calls);}
    auto shortIo=makeIo();shortIo.shortWrite=true;SsdInstallEngine partial(g,true);
    assert(partial.Step(shortIo,a.data(),b.data())==SsdInstallStepResult::Failed && partial.Stage()==SsdInstallStage::Failed);
    auto ignored=makeIo();ignored.ignoreZero=true;SsdInstallEngine noClear(g,true);
    assert(noClear.Step(ignored,a.data(),b.data())==SsdInstallStepResult::Progress);
    assert(noClear.Step(ignored,a.data(),b.data())==SsdInstallStepResult::Progress);
    assert(noClear.Step(ignored,a.data(),b.data())==SsdInstallStepResult::Failed);
    auto corrupt=makeIo();corrupt.corruptRead=true;SsdInstallEngine mismatch(g,true);
    while(mismatch.Step(corrupt,a.data(),b.data())==SsdInstallStepResult::Progress){}
    assert(mismatch.Stage()==SsdInstallStage::Failed);
    for(const auto phase:{SsdInstallStage::VerifyPayload,SsdInstallStage::VerifyPrimary}){
        auto broken=makeIo();SsdInstallEngine attempt(g,true);
        while(attempt.Stage()!=phase)assert(attempt.Step(broken,a.data(),b.data())==SsdInstallStepResult::Progress);
        broken.corruptRead=true;assert(attempt.Step(broken,a.data(),b.data())==SsdInstallStepResult::Failed);
        assert(attempt.Stage()==SsdInstallStage::Failed);
    }
    auto shortSource=makeIo();SsdInstallEngine shortAttempt(g,true);
    while(shortAttempt.Stage()!=SsdInstallStage::Payload)assert(shortAttempt.Step(shortSource,a.data(),b.data())==SsdInstallStepResult::Progress);
    shortSource.shortRead=true;const auto priorWrites=shortSource.writes;
    assert(shortAttempt.Step(shortSource,a.data(),b.data())==SsdInstallStepResult::Failed && shortSource.writes==priorWrites);
}
