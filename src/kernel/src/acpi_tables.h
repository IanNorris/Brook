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
private:
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
