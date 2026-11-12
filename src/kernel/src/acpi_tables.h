#pragma once
#include <stdint.h>

namespace brook {
#pragma pack(push,1)
struct AcpiSdtHeader {
    char signature[4];uint32_t length;uint8_t revision,checksum;
    char oemId[6],oemTableId[8];uint32_t oemRevision,creatorId,creatorRevision;
};
#pragma pack(pop)
static_assert(sizeof(AcpiSdtHeader)==36);
struct AcpiResetInfo {
    bool available=false;uint16_t port=0;uint8_t value=0;
    const char* reason="No validated FADT reset register";
};
struct AcpiPmTimerInfo { bool available=false;uint16_t port=0;bool wide=false; };
struct AcpiLegacyPowerInfo {
    uint64_t facs=0;
    uint16_t sci=0,pm1aControl=0,pm1bControl=0;
    uint32_t smiCommand=0;
    uint8_t acpiEnable=0;
};
struct AcpiSciRoute {
    bool available=false,activeLow=true;
    uint32_t gsi=0;
};

// The callback must reject unreadable physical spans before accessing memory.
// Checksums are streamed through a small stack buffer; firmware lengths never
// become heap allocations or unchecked reads. Limits fail closed with no I/O.
class AcpiTableReader {
public:
    using Read=bool(*)(void*,uint64_t,void*,uint32_t);
    static constexpr uint32_t MaximumTableBytes=1024*1024;
    static constexpr uint32_t MaximumRootBytes=64*1024;
    bool Init(Read read,void* context,uint64_t rsdp) {
        read_=read;context_=context;root_=0;count_=0;entryBytes_=0;
        uint8_t prefix[36]{};
        if(!Copy(rsdp,prefix,20) || !Equal(reinterpret_cast<char*>(prefix),"RSD PTR ",8) || !Checksum(rsdp,20))return false;
        uint32_t rsdt=0;__builtin_memcpy(&rsdt,prefix+16,4);
        uint64_t xsdt=0;
        if(prefix[15]>=2) {
            if(!Copy(rsdp,prefix,36))return false;
            uint32_t length=0;__builtin_memcpy(&length,prefix+20,4);
            if(length<36 || length>4096 || !Checksum(rsdp,length))return false;
            __builtin_memcpy(&xsdt,prefix+24,8);
        }
        uint64_t selected=xsdt?xsdt:rsdt;uint32_t width=xsdt?8:4;
        AcpiSdtHeader header{};
        if(!selected || !Header(selected,header) || !Equal(header.signature,xsdt?"XSDT":"RSDT",4) ||
           header.length>MaximumRootBytes || (header.length-sizeof(header))%width || !Checksum(selected,header.length))return false;
        root_=selected;entryBytes_=width;count_=(header.length-sizeof(header))/width;return true;
    }
    bool Copy(uint64_t at,void* out,uint32_t bytes) const {
        return read_ && out && at && bytes && uint64_t(bytes)<=~uint64_t{0}-at && read_(context_,at,out,bytes);
    }
    bool Header(uint64_t at,AcpiSdtHeader& out) const {
        return Copy(at,&out,sizeof(out)) && out.length>=sizeof(out) && out.length<=MaximumTableBytes;
    }
    bool ValidTable(uint64_t at,AcpiSdtHeader& out) const {
        return Header(at,out) && Checksum(at,out.length);
    }
    bool Initialized() const {return root_!=0;}
    uint32_t RootEntryCount() const {return root_?count_:0;}
    // Indexed access keeps duplicate signatures (notably SSDTs) distinct.
    uint64_t RootEntry(uint32_t index,AcpiSdtHeader* out=nullptr) const {
        if(!root_ || index>=count_)return 0;
        uint64_t address=0;AcpiSdtHeader header{};
        if(!Copy(root_+sizeof(AcpiSdtHeader)+uint64_t(index)*entryBytes_,&address,entryBytes_) ||
           !ValidTable(address,header))return 0;
        if(out)*out=header;return address;
    }
    // DSDT is referenced by the validated FADT, not normally an XSDT entry.
    uint64_t Dsdt(AcpiSdtHeader* out=nullptr) const {
        AcpiSdtHeader fadt{},dsdt{};const uint64_t at=Find("FACP",&fadt);
        if(!at || fadt.length<44)return 0;
        uint32_t legacy=0;uint64_t extended=0;
        if(!Copy(at+40,&legacy,sizeof(legacy)))return 0;
        if(fadt.length>=148 && !Copy(at+140,&extended,sizeof(extended)))return 0;
        const uint64_t address=extended?extended:legacy;
        if(!ValidTable(address,dsdt) || !Equal(dsdt.signature,"DSDT",4))return 0;
        if(out)*out=dsdt;return address;
    }
    // Only copy bytes from a table that is valid at the time of the request.
    // This does not provide arbitrary physical-memory access.
    bool CopyTable(uint64_t address,uint32_t offset,void* output,uint32_t bytes) const {
        AcpiSdtHeader h{};
        return output && bytes && ValidTable(address,h) && offset<=h.length &&
            bytes<=h.length-offset && Copy(address+offset,output,bytes);
    }
    uint64_t Find(const char* signature,AcpiSdtHeader* out=nullptr) const {
        if(!signature || !root_)return 0;
        for(uint32_t i=0;i<count_;++i) {
            uint64_t address=0;
            if(!Copy(root_+sizeof(AcpiSdtHeader)+uint64_t(i)*entryBytes_,&address,entryBytes_))return 0;
            AcpiSdtHeader header{};
            if(!Header(address,header) || !Equal(header.signature,signature,4) || !Checksum(address,header.length))continue;
            if(out)*out=header;return address;
        }
        return 0;
    }
    AcpiResetInfo Reset() const {
        AcpiResetInfo result{};AcpiSdtHeader header{};
        const uint64_t address=Find("FACP",&header);
        if(!address)return result;
        if(header.revision<2 || header.length<129){result.reason="FADT reset fields unavailable";return result;}
        uint8_t bytes[17]{};
        if(!Copy(address+112,bytes,sizeof(bytes))){result.reason="FADT reset fields unreadable";return result;}
        uint32_t flags=0;__builtin_memcpy(&flags,bytes,4);
        if(!(flags&(1u<<10))){result.reason="Firmware does not advertise reset support";return result;}
        uint64_t port=0;__builtin_memcpy(&port,bytes+8,8);
        // Narrow initial support: naturally aligned byte I/O only. GAS access
        // size 0 is unspecified; 1 explicitly requests a byte transaction.
        if(bytes[4]!=1 || bytes[5]!=8 || bytes[6]!=0 || bytes[7]>1 || !port || port>65535) {
            result.reason="Firmware reset GAS is unsupported (requires byte System I/O)";return result;
        }
        result.available=true;result.port=static_cast<uint16_t>(port);result.value=bytes[16];result.reason="FADT System I/O reset";return result;
    }
    // ACPI 6.6 FADT: PM_TMR_BLK (76), PM_TMR_LEN (91), flags (112),
    // and X_PM_TMR_BLK GAS (208). Only 32-bit System I/O is supported.
    AcpiPmTimerInfo PmTimer() const {
        AcpiPmTimerInfo result{};AcpiSdtHeader header{};
        const uint64_t address=Find("FACP",&header);
        if(!address || header.length<116)return result;
        uint32_t legacy=0,flags=0;uint8_t length=0;
        if(!Copy(address+76,&legacy,4) || !Copy(address+91,&length,1) ||
           !Copy(address+112,&flags,4) || length!=4)return result;
        uint64_t port=legacy;
        if(header.length>=220) {
            uint8_t gas[12]{};if(!Copy(address+208,gas,sizeof(gas)))return result;
            uint64_t extended=0;__builtin_memcpy(&extended,gas+4,8);
            if(extended && gas[0]==1 && (gas[1]==24 || gas[1]==32) && !gas[2] &&
               (gas[3]==0 || gas[3]==3) && extended<=65532)port=extended;
        }
        if(!port || port>65532)return result;
        result.available=true;result.port=uint16_t(port);result.wide=(flags&(1u<<8))!=0;return result;
    }
    // Discover the fixed-register subset used by a legacy FACS Global Lock
    // owner. Hardware-reduced, MMIO and non-word control blocks are excluded.
    AcpiLegacyPowerInfo LegacyPower() const {
        AcpiSdtHeader fadt{};
        const uint64_t address=Find("FACP",&fadt);
        if(!address || fadt.length<116)return {};
        uint32_t flags=0,legacyFacs=0,legacyA=0,legacyB=0,smiCommand=0;
        uint64_t extendedFacs=0;
        uint16_t sci=0;
        uint8_t controlLength=0,acpiEnable=0;
        if(!Copy(address+112,&flags,4) || (flags&(1u<<20)) ||
           !Copy(address+36,&legacyFacs,4) ||
           !Copy(address+46,&sci,2) || !sci ||
           !Copy(address+48,&smiCommand,4) || !Copy(address+52,&acpiEnable,1) ||
           !Copy(address+64,&legacyA,4) || !Copy(address+68,&legacyB,4) ||
           !Copy(address+89,&controlLength,1) || controlLength!=2)return {};
        if(fadt.length>=140 && !Copy(address+132,&extendedFacs,8))return {};
        if(extendedFacs && legacyFacs && extendedFacs!=legacyFacs)return {};
        const uint64_t facs=extendedFacs?extendedFacs:legacyFacs;
        if(!facs || (facs&63))return {};
        uint16_t a=0,b=0;
        if(!ControlPort(address,fadt.length,172,legacyA,a) || !a ||
           !ControlPort(address,fadt.length,184,legacyB,b) || a==b)return {};
        return {facs,sci,a,b,smiCommand,acpiEnable};
    }
private:
    bool ControlPort(uint64_t fadt,uint32_t length,uint32_t offset,uint32_t legacy,uint16_t& out) const {
        uint64_t port=legacy;
        if(length>=offset+12) {
            uint8_t gas[12]{};
            if(!Copy(fadt+offset,gas,sizeof(gas)))return false;
            uint64_t address=0;
            __builtin_memcpy(&address,gas+4,8);
            if(address) {
                if(gas[0]!=1 || gas[1]!=16 || gas[2] || (gas[3]!=0 && gas[3]!=2))return false;
                if(legacy && legacy!=address)return false;
                port=address;
            }
        }
        if(port>65534 || (port&1))return false;
        out=uint16_t(port);
        return true;
    }
    static bool Equal(const char* a,const char* b,uint32_t n){for(uint32_t i=0;i<n;++i)if(a[i]!=b[i])return false;return true;}
    bool Checksum(uint64_t at,uint32_t length) const {
        uint8_t buffer[256];uint8_t sum=0;
        while(length) {
            const uint32_t bytes=length>sizeof(buffer)?sizeof(buffer):length;
            if(!Copy(at,buffer,bytes))return false;
            for(uint32_t i=0;i<bytes;++i)sum+=buffer[i];at+=bytes;length-=bytes;
        }
        return sum==0;
    }
    Read read_=nullptr;void* context_=nullptr;uint64_t root_=0;uint32_t count_=0,entryBytes_=0;
};
}
