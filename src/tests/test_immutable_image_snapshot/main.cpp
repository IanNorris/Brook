#include "../../kernel/src/fs/immutable_image_snapshot.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace brook;
struct Io {
    std::vector<uint8_t> bytes=std::vector<uint8_t>(131072,0x27);
    unsigned allocated=0,released=0,reads=0;bool oom=false,fail=false;unsigned shortBytes=0;
    void* allocate(uint64_t n){if(oom)return nullptr;allocated++;return malloc(n);}
    void release(void* p,uint64_t){released++;free(p);}
    int read(uint64_t offset,void* p,uint32_t n){reads++;if(fail)return -5;if(offset>=bytes.size())return 0;if(n>bytes.size()-offset)n=bytes.size()-offset;if(shortBytes&&n>shortBytes)n=shortBytes;memcpy(p,bytes.data()+offset,n);return n;}
};
int main() {
    AppSnapshotPlan plan{};std::string hash(64,'0');
    auto parse=[&](std::string text){return AppSnapshotParse(text.data(),text.size(),plan);};
    assert(parse("/media/usb/APPS.BRK /nix 131072 "+hash+"\n"));
    assert(!parse("/media/usb/../APPS.BRK /nix 131072 "+hash));
    assert(!parse("/media/usb/APPS.BRK /other 131072 "+hash));
    assert(!parse("/media/usb/APPS.BRK /nix 536875008 "+hash));
    assert(!parse("/media/usb/APPS.BRK /nix 18446744073709551616 "+hash));
    assert(!parse("/media/usb/APPS.BRK /nix 131071 "+hash));
    assert(!parse("/media/usb/APPS.BRK /nix 131072 "+hash+"\n\n"));
    assert(!parse("/media/usb/APPS.BRK /nix 131072 "+hash.substr(1)));
    assert(parse("/media/usb/UPDATES/B/APPS.BRK /nix 131072 "+hash));
    Io io;BrookUpdateHash(io.bytes.data(),io.bytes.size(),plan.hash);AppSnapshotError error;
    auto* snapshot=AppSnapshotCopy(io,plan,AppSnapshotHeadroom+plan.bytes,error);assert(snapshot&&error==AppSnapshotError::None);
    io.bytes[12]^=1;assert(static_cast<uint8_t*>(snapshot)[12]==0x27);io.release(snapshot,plan.bytes);assert(io.released==1);
    assert(!AppSnapshotCopy(io,plan,AppSnapshotHeadroom+plan.bytes,error)&&error==AppSnapshotError::Hash&&io.released==2);
    io.bytes[12]^=1;io.shortBytes=37;snapshot=AppSnapshotCopy(io,plan,AppSnapshotHeadroom+plan.bytes,error);assert(snapshot);io.release(snapshot,plan.bytes);
    io.fail=true;assert(!AppSnapshotCopy(io,plan,AppSnapshotHeadroom+plan.bytes,error)&&error==AppSnapshotError::Read);io.fail=false;
    io.bytes.resize(65536);assert(!AppSnapshotCopy(io,plan,AppSnapshotHeadroom+plan.bytes,error)&&error==AppSnapshotError::Read);
    unsigned before=io.allocated;assert(!AppSnapshotCopy(io,plan,AppSnapshotHeadroom+plan.bytes-1,error)&&io.allocated==before);
    io.oom=true;assert(!AppSnapshotCopy(io,plan,AppSnapshotHeadroom+plan.bytes,error)&&error==AppSnapshotError::Memory);
    assert(io.allocated==io.released);
}
