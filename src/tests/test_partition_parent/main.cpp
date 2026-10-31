#include "../../kernel/src/gpt.h"
#include "../../kernel/src/ssd_install_source_identity.h"
#include <cassert>
#include <cstring>
#include <vector>
using namespace brook;
static std::vector<Device*> devices;
static uint8_t mbr[512];
extern "C" bool brook::DeviceRegister(Device* d){devices.push_back(d);return true;}
extern "C" bool brook::DeviceIsRegistered(Device* d){for(auto* v:devices)if(v==d)return true;return false;}
extern "C" Device* brook::DeviceFind(const char* name){for(auto* v:devices)if(!strcmp(v->name,name))return v;return nullptr;}
extern "C" int brook::DeviceWrite(Device*,uint64_t,const void*,uint64_t){return -1;}
int brook::DeviceFlush(Device*){return 0;}
extern "C" void SerialPrintf(const char*,...) {}
static int read(Device*,uint64_t off,void* out,uint64_t len){if(off||len!=512)return -1;memcpy(out,mbr,512);return 512;}
static uint64_t blocks(Device*){return 100000;}
static uint32_t blockSize(Device*){return 512;}
int main(){
 BlockDeviceOps ops{read,nullptr,nullptr,nullptr,blocks,blockSize};
 Device usb{reinterpret_cast<DeviceOps*>(&ops),"usb0",DeviceType::Block,nullptr};
 Device forged{reinterpret_cast<DeviceOps*>(&ops),"usb0p1",DeviceType::Block,nullptr};
 Device target{reinterpret_cast<DeviceOps*>(&ops),"ahci0",DeviceType::Block,nullptr};
 DeviceRegister(&usb);DeviceRegister(&forged);DeviceRegister(&target);
 mbr[510]=0x55;mbr[511]=0xaa;mbr[450]=0x0c;mbr[454]=0x00;mbr[455]=0x08;mbr[458]=0x00;mbr[459]=0x10;
 assert(!PartitionParentDevice(nullptr));assert(!PartitionParentDevice(&usb));assert(!PartitionParentDevice(&forged));
 assert(!SsdInstallUsbSource(&forged,&target));devices.erase(devices.begin()+1);
 assert(MbrProbeDevice(&usb)==1);Device* partition=devices.back();assert(PartitionParentDevice(partition)==&usb);
 assert(SsdInstallUsbSource(partition,&target));assert(SsdInstallUsbSource(&usb,&target));
 assert(!SsdInstallUsbSource(partition,&usb));
 assert(!SsdInstallUsbSource(&target,&target));assert(!SsdInstallUsbSource(nullptr,&target));
 auto* saved=partition->priv;partition->priv=nullptr;assert(!PartitionParentDevice(partition));partition->priv=saved;
 devices.erase(devices.begin());assert(!PartitionParentDevice(partition));assert(!SsdInstallUsbSource(partition,&target));
}
