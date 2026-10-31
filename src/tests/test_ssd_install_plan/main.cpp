#include "../../kernel/src/ssd_install_plan.h"
#include "../../kernel/src/strict_gpt_inventory.h"
#include <cassert>
#include <fstream>
#include <vector>
using namespace brook;
static int fixture(const char* path){
    std::ifstream f(path,std::ios::binary);if(!f)return 1;f.seekg(0,std::ios::end);
    const auto bytes=f.tellg();if(bytes<0 || uint64_t(bytes)%512)return 1;
    const uint64_t sectors=uint64_t(bytes)/512;uint8_t primary[512],backup[512],entry[128];
    f.seekg(512);f.read(reinterpret_cast<char*>(primary),512);
    f.seekg((sectors-1)*512);f.read(reinterpret_cast<char*>(backup),512);
    f.seekg(2*512);f.read(reinterpret_cast<char*>(entry),128);if(!f)return 1;
    StrictGptHeader a{},b{};
    if(StrictGptCheckHeader(primary,512,sectors,false,a)!=StrictGptResult::Ok ||
       StrictGptCheckHeader(backup,512,sectors,true,b)!=StrictGptResult::Ok ||
       a.count!=128 || a.entrySize!=128 || a.entriesLba!=2 || b.entriesLba!=sectors-33)return 1;
    const uint64_t first=StrictLe64(entry+32),last=StrictLe64(entry+40);
    if(last<first)return 1;SsdInstallPlan plan({sectors,first,last-first+1});
    while(plan.Stage()!=SsdInstallStage::Backup){const auto op=plan.Next();if(op.action==SsdInstallAction::Failed || !plan.Finish(op,true))return 1;}
    const auto backupOp=plan.Next();return backupOp.action==SsdInstallAction::Copy && backupOp.lba==b.entriesLba && backupOp.sectors==33?0:1;
}
int main(int argc,char** argv){
    if(argc==2)return fixture(argv[1]);
    assert(argc==1);
    assert(!SsdInstallGeometryValid({0,2048,4096}));
    assert(!SsdInstallGeometryValid({10000,33,4096}));
    assert(!SsdInstallGeometryValid({10000,2048,0}));
    assert(!SsdInstallGeometryValid({10000,2048,7920}));
    assert(!SsdInstallGeometryValid({(1ull<<48)+1,2048,4096}));
    assert(SsdInstallGeometryValid({68,34,1}));
    assert(SsdInstallGeometryValid({10000,2048,7919}));
    SsdInstallPlan shortTail({10000,2048,129});
    while(shortTail.Stage()!=SsdInstallStage::Payload)assert(shortTail.Finish(shortTail.Next(),true));
    auto chunk=shortTail.Next();assert(chunk.sectors==128);assert(shortTail.Finish(chunk,true));
    chunk=shortTail.Next();assert(chunk.sectors==1 && chunk.lba==2176);
    const SsdInstallGeometry geometry{10000,2048,4096};assert(SsdInstallGeometryValid(geometry));
    SsdInstallPlan p(geometry);std::vector<SsdInstallOperation> actions;
    while(p.Next().action!=SsdInstallAction::Done){const auto op=p.Next();assert(op.action!=SsdInstallAction::Failed);
        actions.push_back(op);assert(p.Finish(op,true));assert(actions.size()<200);}
    assert(actions[0].action==SsdInstallAction::Zero && actions[0].lba==9999);
    assert(actions[1].action==SsdInstallAction::Flush);
    assert(actions[2].action==SsdInstallAction::Zero && actions[2].lba==0 && actions[2].sectors==34);
    bool payloadVerified=false,backupVerified=false,primaryVerified=false,activation=false;
    std::vector<uint8_t> disk(geometry.diskSectors,0xa5),source(geometry.diskSectors),touched(geometry.diskSectors);
    for(size_t i=0;i<source.size();++i)source[i]=uint8_t((i*37+9)&255);
    for(const auto& op:actions){
        if(op.action==SsdInstallAction::Verify && op.lba+op.sectors==geometry.payloadFirst+geometry.payloadSectors)payloadVerified=true;
        if(op.action==SsdInstallAction::Verify && op.lba==geometry.diskSectors-33)backupVerified=true;
        if(op.action==SsdInstallAction::Verify && op.lba==1)primaryVerified=true;
        if(op.action==SsdInstallAction::Copy && op.lba==geometry.diskSectors-33)assert(payloadVerified);
        if(op.action==SsdInstallAction::Copy && op.lba==0){assert(payloadVerified&&backupVerified&&primaryVerified);activation=true;}
        if(op.action==SsdInstallAction::Copy || op.action==SsdInstallAction::Verify || op.action==SsdInstallAction::Zero)
            assert(op.sectors && op.sectors<=128 && op.lba<geometry.diskSectors && op.sectors<=geometry.diskSectors-op.lba);
        for(uint32_t i=0;i<op.sectors;++i){const auto sector=op.lba+i;
            const bool allowed=sector<34 || (sector>=geometry.payloadFirst && sector<geometry.payloadFirst+geometry.payloadSectors) ||
                sector>=geometry.diskSectors-33;
            assert(allowed);
            if(op.action==SsdInstallAction::Zero){disk[sector]=0;touched[sector]=1;}
            if(op.action==SsdInstallAction::Copy){disk[sector]=source[sector];touched[sector]=1;}
            if(op.action==SsdInstallAction::Verify)assert(disk[sector]==source[sector]);}
    }
    for(size_t i=0;i<disk.size();++i){const bool allowed=i<34 || (i>=geometry.payloadFirst && i<geometry.payloadFirst+geometry.payloadSectors) ||
        i>=geometry.diskSectors-33;if(!allowed){assert(!touched[i]);assert(disk[i]==0xa5);}}
    assert(activation && !p.Finish({SsdInstallAction::Done,0,0},true));
    for(size_t fail=0;fail<actions.size();++fail){SsdInstallPlan failing(geometry);
        for(size_t i=0;i<fail;++i)assert(failing.Finish(failing.Next(),true));
        assert(!failing.Finish(failing.Next(),false) && failing.Next().action==SsdInstallAction::Failed);
        assert(!failing.Finish(actions[fail],true));}
    SsdInstallPlan outOfOrder(geometry);auto wrong=outOfOrder.Next();++wrong.lba;
    assert(!outOfOrder.Finish(wrong,true) && outOfOrder.Next().action==SsdInstallAction::Failed);
}
