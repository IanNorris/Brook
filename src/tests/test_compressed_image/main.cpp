#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <vector>
#include <sys/stat.h>
#include "../../kernel/src/fs/compressed_image.h"
#include "../../third_party/lz4/lz4.h"
using namespace brook;
struct Disk {
    std::vector<uint8_t> bytes;bool shortIo=false;int error=0;unsigned reads=0;
    static int Read(void* p,uint64_t at,void* out,uint64_t n) {
        auto& d=*static_cast<Disk*>(p);++d.reads;if(d.error)return d.error;
        assert(at<=d.bytes.size() && n<=d.bytes.size()-at);
        if(d.shortIo)n/=2;memcpy(out,d.bytes.data()+at,n);return n;
    }
};
static int Decode(const void* in,int n,void* out,int capacity){return LZ4_decompress_safe(static_cast<const char*>(in),static_cast<char*>(out),n,capacity);}
struct Reader {
    CompressedImage image;std::vector<CompressedImageNodeState> states;
    alignas(8) std::array<uint8_t,65536> encoded{},decoded{};
    bool Init(Disk& disk) {
        if(disk.bytes.size()<sizeof(CompressedImageHeader))return false;
        auto h=*reinterpret_cast<const CompressedImageHeader*>(disk.bytes.data());
        if(!CompressedImageHeaderValid(h,disk.bytes.size()))return false;
        states.resize(h.inodeCount);
        return image.Initialize(h,disk.bytes.size(),disk.bytes.data()+sizeof(h),h.dataOffset-sizeof(h),states.data(),states.size(),{&disk,Disk::Read,Decode},encoded.data(),decoded.data());
    }
};
static uint64_t align(uint64_t n){return (n+4095)&~uint64_t{4095};}
static CompressedImageHeader& header(Disk& d){return *reinterpret_cast<CompressedImageHeader*>(d.bytes.data());}
static CompressedImageInode* nodes(Disk& d){return reinterpret_cast<CompressedImageInode*>(d.bytes.data()+header(d).inodeOffset);}
static CompressedImageDirent* entries(Disk& d){return reinterpret_cast<CompressedImageDirent*>(d.bytes.data()+header(d).direntOffset);}
static CompressedImageBlock* blocks(Disk& d){return reinterpret_cast<CompressedImageBlock*>(d.bytes.data()+header(d).blockOffset);}
static void rehash(Disk& disk) {
    auto& h=header(disk);h.metadataChecksum=CompressedImageChecksum(disk.bytes.data()+sizeof(h),h.dataOffset-sizeof(h));
    h.headerChecksum=CompressedImageChecksum(&h,136);
}
static Disk fixture() {
    Disk d;d.bytes.resize(4096);auto& h=header(d);
    h={};h.magic=COMPRESSED_IMAGE_MAGIC;h.version=1;h.headerBytes=sizeof(h);h.blockBytes=65536;h.identity[0]=1;
    h.inodeOffset=sizeof(h);h.inodeCount=5;h.direntOffset=h.inodeOffset+5*sizeof(CompressedImageInode);h.direntCount=4;
    h.blockOffset=h.direntOffset+4*sizeof(CompressedImageDirent);h.blockCount=2;
    h.stringOffset=h.blockOffset+2*sizeof(CompressedImageBlock);h.stringBytes=25;h.dataOffset=4096;
    std::memcpy(d.bytes.data()+h.stringOffset,"emptylinksubdata/sub/data",25);
    auto* n=nodes(d);n[0]={CompressedImageKind::Directory,0040755,0,0,0,0,0,0,0,0,0};n[1]=n[0];
    n[2]={CompressedImageKind::File,0100755,1000,1000,65553,0,2,0,0,0,0};
    n[3]={CompressedImageKind::Symlink,0120777,1000,1000,9,0,0,16,9,0,0};
    n[4]={CompressedImageKind::File,0100644,1000,1000,0,2,0,0,0,0,0};
    auto* e=entries(d);e[0]={0,4,0,5,0};e[1]={0,3,5,4,0};e[2]={0,1,9,3,0};e[3]={1,2,12,4,0};
    std::array<uint8_t,65536> plain{};for(unsigned i=0;i<plain.size();++i)plain[i]=i*73+(i>>8);
    std::array<char,66000> encoded{};int size=LZ4_compress_default(reinterpret_cast<char*>(plain.data()),encoded.data(),plain.size(),encoded.size());assert(size>0 && size<65536);
    auto* b=blocks(d);b[0]={4096,unsigned(size),65536,1,0,CompressedImageChecksum(plain.data(),plain.size())};
    b[1]={4096+uint64_t(size),17,17,0,0,CompressedImageChecksum(plain.data(),17)};
    uint64_t total=align(4096+size+17);header(d).imageBytes=total;d.bytes.resize(total);
    std::memcpy(d.bytes.data()+4096,encoded.data(),size);std::memcpy(d.bytes.data()+4096+size,plain.data(),17);rehash(d);return d;
}
static void invalid(const Disk& seed,const std::function<void(Disk&)>& mutate,bool checksum=true) {
    auto d=seed;mutate(d);if(checksum)rehash(d);Reader r;assert(!r.Init(d));
}
static void ScalarCodecBoundaries() {
    // Repetitions exercise match offsets 1..32, including LZ4's short-distance
    // overlap paths. Exact capacities and sentinels catch copy widening errors.
    std::array<char,65536> plain{};
    std::array<char,66000> encoded{};
    std::array<char,65568> output{};
    const int lengths[]={0,1,2,3,4,7,8,15,16,31,32,63,64,255,256,4095,4096,65536};
    for(unsigned period=1;period<=32;++period)for(int size:lengths) {
        for(int i=0;i<size;++i)plain[i]=char((i%period)*73);
        int bytes=LZ4_compress_default(plain.data(),encoded.data(),size,encoded.size());assert(bytes>0);
        auto guards=[&](int capacity) {
            for(int i=0;i<16;++i)assert(output[i]==char(0xc7));
            for(unsigned i=16+capacity;i<output.size();++i)assert(output[i]==char(0xc7));
        };
        output.fill(char(0xc7));
        assert(LZ4_decompress_safe(encoded.data(),output.data()+16,bytes,size)==size);
        assert(!std::memcmp(plain.data(),output.data()+16,size));guards(size);
        if(size) {
            output.fill(char(0xc7));
            assert(LZ4_decompress_safe(encoded.data(),output.data()+16,bytes,size-1)<0);guards(size-1);
        }
        const int cuts[]={0,bytes/2,bytes-1};
        for(int cut:cuts) {
            output.fill(char(0xc7));
            assert(LZ4_decompress_safe(encoded.data(),output.data()+16,cut,size)!=size);guards(size);
        }
    }
}
// File-backed verification for multi-GiB images: resident storage is the
// bounded index, node state and two blocks, never the full image or file.
struct StreamDisk {
    std::FILE* file=nullptr;uint64_t bytes=0;
    ~StreamDisk(){if(file)std::fclose(file);}
    static int Read(void* p,uint64_t at,void* out,uint64_t n) {
        auto& d=*static_cast<StreamDisk*>(p);
        if(at>d.bytes || n>d.bytes-at || n>0x7fffffff || fseeko(d.file,static_cast<off_t>(at),SEEK_SET))return -5;
        return static_cast<int>(std::fread(out,1,n,d.file));
    }
};
static int streamVerify(const char* path) {
    StreamDisk disk;disk.file=std::fopen(path,"rb");if(!disk.file)return 2;
    struct stat info{};
    if(fstat(fileno(disk.file),&info) || !S_ISREG(info.st_mode) || info.st_size<0)return 2;
    disk.bytes=info.st_size;CompressedImageHeader h{};
    if(StreamDisk::Read(&disk,0,&h,sizeof(h))!=sizeof(h) || !CompressedImageHeaderValid(h,disk.bytes))return 2;
    std::vector<uint8_t> index(h.dataOffset-sizeof(h));
    std::vector<CompressedImageNodeState> states(h.inodeCount);
    std::array<uint8_t,65536> encoded{},decoded{},output{};CompressedImage image;
    if(StreamDisk::Read(&disk,sizeof(h),index.data(),index.size())!=static_cast<int>(index.size()) ||
       !image.Initialize(h,disk.bytes,index.data(),index.size(),states.data(),states.size(),{&disk,StreamDisk::Read,Decode},encoded.data(),decoded.data()))return 2;
    std::puts("[");bool first=true;
    for(uint32_t id=0;id<h.inodeCount;++id) {
        const auto* node=image.Inode(id);if(node->kind!=CompressedImageKind::File)continue;
        uint64_t at=0,hash=14695981039346656037ULL;
        while(at<node->size) {
            uint32_t amount=std::min<uint64_t>(output.size(),node->size-at);
            if(image.Read(id,at,output.data(),amount)!=static_cast<int>(amount))return 3;
            for(uint32_t i=0;i<amount;++i)hash=(hash^output[i])*1099511628211ULL;
            at+=amount;
        }
        std::printf("%s{\"inode\":%u,\"size\":%llu,\"checksum\":%llu}",first?"":",\n",id,(unsigned long long)node->size,(unsigned long long)hash);first=false;
    }
    std::puts("\n]");return 0;
}
static void indexBudgetBoundaries() {
    auto d=fixture();auto h=header(d);
    h.dataOffset=COMPRESSED_IMAGE_INDEX_LIMIT;h.stringBytes=h.dataOffset-h.stringOffset;
    h.imageBytes=h.dataOffset+4096;h.headerChecksum=CompressedImageChecksum(&h,136);
    assert(CompressedImageHeaderValid(h,h.imageBytes));
    h.dataOffset+=4096;h.stringBytes+=4096;h.imageBytes+=4096;
    h.headerChecksum=CompressedImageChecksum(&h,136);
    assert(!CompressedImageHeaderValid(h,h.imageBytes));
}
int main(int argc,char** argv) {
    if(argc==3 && !std::strcmp(argv[1],"--streaming"))return streamVerify(argv[2]);
    indexBudgetBoundaries();
    if(argc==2) {
        Disk d;std::ifstream stream(argv[1],std::ios::binary);d.bytes.assign(std::istreambuf_iterator<char>(stream),{});
        Reader r;if(!r.Init(d)){std::fputs("invalid compressed image\n",stderr);return 2;}
        std::puts("[");bool first=true;
        for(uint32_t id=0;id<r.image.Header().inodeCount;++id) {
            const auto* n=r.image.Inode(id);if(n->kind!=CompressedImageKind::File)continue;
            std::vector<uint8_t> bytes(n->size);uint64_t at=0;
            while(at<n->size){uint32_t amount=std::min<uint64_t>(65536,n->size-at);int got=r.image.Read(id,at,bytes.data()+at,amount);if(got!=int(amount))return 3;at+=got;}
            std::printf("%s{\"inode\":%u,\"size\":%llu,\"checksum\":%llu}",first?"":",\n",id,(unsigned long long)n->size,(unsigned long long)CompressedImageChecksum(bytes.data(),bytes.size()));first=false;
        }
        std::puts("\n]");return 0;
    }
    ScalarCodecBoundaries();
    auto d=fixture();Reader r;assert(r.Init(d));
    assert(r.image.FindPath("/")==0 && r.image.FindPath("sub/data")==2 && r.image.FindPath("sub/data/")==-20);
    assert(r.image.FindPath("link")==3 && r.image.FindPath("missing")==-2 && r.image.FindPath("sub/../data")==-22);
    assert(r.image.State(2)->links==1 && std::memcmp(r.image.Target(3),"/sub/data",9)==0);
    std::array<uint8_t,65553> out{};assert(r.image.Read(2,0,out.data(),4096)==4096);
    unsigned reads=d.reads;assert(r.image.Read(2,4096,out.data()+4096,4096)==4096 && d.reads==reads);
    assert(r.image.Read(2,65530,out.data(),32)==23);for(unsigned i=0;i<6;++i)assert(out[i]==uint8_t((65530+i)*73+((65530+i)>>8)));
    for(unsigned i=6;i<23;++i)assert(out[i]==uint8_t((i-6)*73));
    assert(r.image.Read(4,0,out.data(),1)==0 && r.image.Read(2,65553,out.data(),1)==0 && r.image.Read(0,0,out.data(),1)==-21);
    invalid(d,[](Disk& a){header(a).magic^=1;});invalid(d,[](Disk& a){header(a).requiredFeatures=1;});
    invalid(d,[](Disk& a){header(a).inodeCount=~uint64_t{0};});invalid(d,[](Disk& a){header(a).stringOffset=~uint64_t{0};});
    invalid(d,[](Disk& a){header(a).imageBytes=~uint64_t{0};});invalid(d,[](Disk& a){header(a).reserved[0]=1;});
    invalid(d,[](Disk& a){header(a).metadataChecksum^=1;header(a).headerChecksum=CompressedImageChecksum(&header(a),136);},false);
    invalid(d,[](Disk& a){entries(a)[1].nameOffset=0;entries(a)[1].nameBytes=5;});
    invalid(d,[](Disk& a){entries(a)[0].parent=2;});invalid(d,[](Disk& a){entries(a)[0].inode=0;});
    invalid(d,[](Disk& a){entries(a)[0].inode=1;});
    invalid(d,[](Disk& a){entries(a)[2].parent=1;std::swap(entries(a)[2],entries(a)[3]);}); // sorted self-cycle
    invalid(d,[](Disk& a){a.bytes[header(a).stringOffset]='.';entries(a)[0].nameBytes=1;});
    invalid(d,[](Disk& a){nodes(a)[2].mode=0040755;});invalid(d,[](Disk& a){nodes(a)[4].firstBlock=0;});
    invalid(d,[](Disk& a){nodes(a)[3].targetOffset=~uint64_t{0};});
    invalid(d,[](Disk& a){blocks(a)[1].plainBytes=18;});invalid(d,[](Disk& a){blocks(a)[0].codec=2;});
    invalid(d,[](Disk& a){blocks(a)[0].offset=~uint64_t{0};});invalid(d,[](Disk& a){a.bytes[4095]=1;});
    auto truncated=d;truncated.bytes.resize(d.bytes.size()-1);Reader shortImage;assert(!shortImage.Init(truncated));
    for(int kind=0;kind<3;++kind) {
        auto a=d;Reader reader;assert(reader.Init(a));
        if(kind==0)a.error=-5;else if(kind==1)a.shortIo=true;else a.bytes[blocks(a)[1].offset]^=1;
        out.fill(0xcc);assert(reader.image.Read(2,65536,out.data(),17)==-5);for(auto v:out)assert(v==0xcc);
        if(kind==2)a.bytes[blocks(a)[1].offset]^=1;a.error=0;a.shortIo=false;
        assert(reader.image.Read(2,65536,out.data(),17)==17);
    }
    auto corrupt=d;corrupt.bytes[blocks(corrupt)[1].offset]^=1;Reader partial;assert(partial.Init(corrupt));
    assert(partial.image.Read(2,0,out.data(),out.size())==65536);assert(partial.image.Read(2,65536,out.data(),17)==-5);
    auto malformed=d;malformed.bytes[4096]^=0xff;Reader badBlock;assert(badBlock.Init(malformed));assert(badBlock.image.Read(2,0,out.data(),1)==-5);
    std::puts("compressed image: canonical index/graph/bounds/cache/raw/LZ4/corruption/short-I/O/verified-prefix pass");
}
