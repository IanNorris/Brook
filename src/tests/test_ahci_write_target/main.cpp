#include "../../kernel/src/ahci_write_target.h"
#include <cassert>
#include <string>
using namespace brook;
int main() {
    const std::string valid="BRKAHCI1\n32902,10530,0,31,2,0,131072,512,512\nQEMU HARDDISK\nBROOK-AHCI-TEST-01\n";
    AhciWriteTarget t{};assert(AhciWriteTargetParse(valid.data(),valid.size(),t));
    AtaIdentifyInfo info{};info.sectors=131072;info.logicalSectorBytes=512;info.physicalSectorBytes=512;
    strcpy(info.model,"QEMU HARDDISK");strcpy(info.serial,"BROOK-AHCI-TEST-01");
    auto matches=[&](){return AhciWriteTargetMatches(t,32902,10530,0,31,2,0,info,true);};
    assert(matches());assert(!AhciWriteTargetMatches(t,32902,10530,0,31,2,0,info,false));
    ++t.vendor;assert(!matches());--t.vendor;++t.device;assert(!matches());--t.device;
    ++t.bus;assert(!matches());--t.bus;--t.slot;assert(!matches());++t.slot;
    ++t.function;assert(!matches());--t.function;++t.port;assert(!matches());--t.port;
    ++t.sectors;assert(!matches());--t.sectors;++t.logical;assert(!matches());--t.logical;
    ++t.physical;assert(!matches());--t.physical;info.model[0]='?';assert(!matches());info.model[0]='Q';
    info.serial[0]='?';assert(!matches());info.serial[0]='B';assert(matches());
    const std::string padded="BRKAHCI1\n32902,10530,0,31,2,0,131072,512,512\nQEMU HARDDISK\n      TEST-SERIAL\n";
    AhciWriteTarget paddedTarget{};assert(AhciWriteTargetParse(padded.data(),padded.size(),paddedTarget));
    strcpy(info.serial,"      TEST-SERIAL");assert(AhciWriteTargetMatches(paddedTarget,32902,10530,0,31,2,0,info,true));
    strcpy(info.serial,"TEST-SERIAL");assert(!AhciWriteTargetMatches(paddedTarget,32902,10530,0,31,2,0,info,true));
    strcpy(info.serial,"     TEST-SERIAL");assert(!AhciWriteTargetMatches(paddedTarget,32902,10530,0,31,2,0,info,true));
    auto bad=[&](const std::string& s){AhciWriteTarget x=t;assert(!AhciWriteTargetParse(s.data(),s.size(),x));assert(!x.sectors && !x.model[0] && !x.serial[0]);};
    bad("");bad(valid.substr(0,valid.size()-1));bad(valid+"extra\n");
    bad("BRKAHCI2\n32902,10530,0,31,2,0,131072,512,512\nQEMU HARDDISK\nBROOK-AHCI-TEST-01\n");
    bad("BRKAHCI1\n32902,10530,00,31,2,0,131072,512,512\nQEMU HARDDISK\nBROOK-AHCI-TEST-01\n");
    bad("BRKAHCI1\n18446744073709551616,10530,0,31,2,0,131072,512,512\nQEMU HARDDISK\nBROOK-AHCI-TEST-01\n");
    bad("BRKAHCI1\n32902,10530,0,32,2,0,131072,512,512\nQEMU HARDDISK\nBROOK-AHCI-TEST-01\n");
    bad("BRKAHCI1\n32902,10530,0,31,2,0,131072,512,512\nQEMU HARDDISK\n\n");
    bad("BRKAHCI1\n32902,10530,0,31,2,0,131072,512,512\nQEMU?HARDDISK\nSERIAL\n");
    bad("BRKAHCI1\n32902,10530,0,31,2,0,131072,512,512\n QEMU\nSERIAL\n");
    bad("BRKAHCI1\n32902,10530,0,31,2,0,131072,512,512\nQEMU \nSERIAL\n");
    bad("BRKAHCI1\n32902,10530,0,31,2,0,131072,512,512\nQEMU\r\nSERIAL\n");
    auto nul=valid;nul[10]='\0';bad(nul);
    const std::string prefix="BRKAHCI1\n65535,65535,255,31,7,31,281474976710656,512,16777216\n";
    auto max=prefix+std::string(40,'M')+"\n"+std::string(20,'S')+"\n";
    AhciWriteTarget maxTarget{};assert(AhciWriteTargetParse(max.data(),max.size(),maxTarget));
    bad(prefix+std::string(41,'M')+"\nS\n");bad(prefix+"M\n"+std::string(21,'S')+"\n");
    bad(std::string(193,'a'));
}
