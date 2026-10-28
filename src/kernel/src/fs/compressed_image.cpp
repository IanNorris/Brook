#include "compressed_image.h"
namespace brook {
static_assert(__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__,"BRKPACK reader needs little endian");
namespace {
constexpr uint32_t None=~uint32_t{0};
uint64_t Align(uint64_t n){return (n+4095)&~uint64_t{4095};}
bool Section(uint64_t offset,uint64_t count,uint64_t size,uint64_t limit,uint64_t next) {
    return offset<=limit && count<=(limit-offset)/size && next==offset+count*size;
}
int Compare(const char* a,uint32_t an,const char* b,uint32_t bn) {
    for(uint32_t i=0;i<an && i<bn;++i)if(uint8_t(a[i])!=uint8_t(b[i]))return uint8_t(a[i])<uint8_t(b[i])?-1:1;
    return an<bn?-1:an>bn?1:0;
}
bool StringValid(const char* bytes,uint32_t length,bool name) {
    if(!length || (name && length>255) || (!name && length>4095))return false;
    if(name && ((length==1 && bytes[0]=='.') || (length==2 && bytes[0]=='.' && bytes[1]=='.')))return false;
    for(uint32_t i=0;i<length;++i)if(!bytes[i] || (name && bytes[i]=='/'))return false;
    return true;
}
}
uint64_t CompressedImageChecksum(const void* bytes,uint64_t length) {
    uint64_t hash=14695981039346656037ULL;auto* p=static_cast<const uint8_t*>(bytes);
    for(uint64_t i=0;i<length;++i)hash=(hash^p[i])*1099511628211ULL;
    return hash;
}
bool CompressedImageHeaderValid(const CompressedImageHeader& h,uint64_t deviceBytes) {
    if(h.magic!=COMPRESSED_IMAGE_MAGIC || h.version!=1 || h.headerBytes!=sizeof(h) || h.requiredFeatures ||
       h.blockBytes!=COMPRESSED_IMAGE_BLOCK || h.rootInode || !h.inodeCount || h.inodeCount>0xffffffffu ||
       h.direntCount>0xffffffffu || h.imageBytes>deviceBytes || h.imageBytes>0x7fffffffffffffffULL ||
       (h.imageBytes&4095) || h.dataOffset<4096 || h.dataOffset>COMPRESSED_IMAGE_INDEX_LIMIT ||
       (h.dataOffset&4095) || h.imageBytes<h.dataOffset || h.inodeOffset!=sizeof(h) ||
       h.headerChecksum!=CompressedImageChecksum(&h,136))return false;
    bool identity=false;for(auto byte:h.identity)identity|=byte!=0;
    for(auto byte:h.reserved)if(byte)return false;
    return identity && Section(h.inodeOffset,h.inodeCount,sizeof(CompressedImageInode),h.dataOffset,h.direntOffset) &&
        Section(h.direntOffset,h.direntCount,sizeof(CompressedImageDirent),h.dataOffset,h.blockOffset) &&
        Section(h.blockOffset,h.blockCount,sizeof(CompressedImageBlock),h.dataOffset,h.stringOffset) &&
        h.stringOffset<=h.dataOffset && h.stringBytes<=h.dataOffset-h.stringOffset &&
        h.dataOffset==Align(h.stringOffset+h.stringBytes);
}
const CompressedImageInode* CompressedImage::Inode(uint32_t id) const {
    return index_ && id<header_.inodeCount?reinterpret_cast<const CompressedImageInode*>(index_+header_.inodeOffset-sizeof(header_))+id:nullptr;
}
const CompressedImageDirent* CompressedImage::Entry(uint32_t id) const {
    return index_ && id<header_.direntCount?reinterpret_cast<const CompressedImageDirent*>(index_+header_.direntOffset-sizeof(header_))+id:nullptr;
}
const CompressedImageBlock* CompressedImage::Block(uint64_t id) const {
    return index_ && id<header_.blockCount?reinterpret_cast<const CompressedImageBlock*>(index_+header_.blockOffset-sizeof(header_))+id:nullptr;
}
const char* CompressedImage::Name(const CompressedImageDirent& e) const {
    return reinterpret_cast<const char*>(index_+header_.stringOffset-sizeof(header_)+e.nameOffset);
}
const char* CompressedImage::Target(uint32_t id) const {
    auto* i=Inode(id);return i && i->kind==CompressedImageKind::Symlink?
        reinterpret_cast<const char*>(index_+header_.stringOffset-sizeof(header_)+i->targetOffset):nullptr;
}
const CompressedImageNodeState* CompressedImage::State(uint32_t id) const{return nodes_ && id<header_.inodeCount?nodes_+id:nullptr;}
bool CompressedImage::Initialize(const CompressedImageHeader& h,uint64_t deviceBytes,
                                const void* index,uint64_t bytes,CompressedImageNodeState* nodes,uint64_t count,
                                CompressedImageIo io,void* encoded,void* decoded) {
    if(ready_ || !CompressedImageHeaderValid(h,deviceBytes) || !index || bytes!=h.dataOffset-sizeof(h) ||
       (reinterpret_cast<uint64_t>(index)&7) || !nodes || (reinterpret_cast<uint64_t>(nodes)&3) || count<h.inodeCount || !io.read || !io.decode || !encoded || !decoded || encoded==decoded ||
       CompressedImageChecksum(index,bytes)!=h.metadataChecksum)return false;
    cachedBlock_=~uint64_t{0};stats_={};
    header_=h;index_=static_cast<const uint8_t*>(index);nodes_=nodes;io_=io;
    encoded_=static_cast<uint8_t*>(encoded);decoded_=static_cast<uint8_t*>(decoded);
    uint64_t used=h.stringOffset+h.stringBytes-sizeof(h);
    for(uint64_t i=used;i<bytes;++i)if(index_[i])return false;
    uint64_t nextBlock=0;
    for(uint32_t id=0;id<h.inodeCount;++id) {
        auto& n=nodes_[id];n={0,None,None,0};const auto& i=*Inode(id);
        uint32_t type=i.mode&0170000;
        if(i.reserved || (i.mode&~uint32_t{0177777}) ||
           (i.kind==CompressedImageKind::Directory && type!=0040000) ||
           (i.kind==CompressedImageKind::File && type!=0100000) ||
           (i.kind==CompressedImageKind::Symlink && type!=0120000))return false;
        if(i.kind==CompressedImageKind::File) {
            uint64_t expected=i.size/COMPRESSED_IMAGE_BLOCK+(i.size%COMPRESSED_IMAGE_BLOCK!=0);
            if(i.firstBlock!=nextBlock || i.blocks!=expected || nextBlock>h.blockCount ||
               i.blocks>h.blockCount-nextBlock || i.targetOffset || i.targetBytes)return false;
            for(uint64_t b=0;b<i.blocks;++b) {
                uint32_t plain=b+1==i.blocks && i.size%COMPRESSED_IMAGE_BLOCK?i.size%COMPRESSED_IMAGE_BLOCK:COMPRESSED_IMAGE_BLOCK;
                if(Block(nextBlock+b)->plainBytes!=plain)return false;
            }
            nextBlock+=i.blocks;
        } else if(i.kind==CompressedImageKind::Symlink) {
            if(i.firstBlock || i.blocks || i.size!=i.targetBytes || i.targetOffset>h.stringBytes ||
               i.targetBytes>h.stringBytes-i.targetOffset || !StringValid(Target(id),i.targetBytes,false))return false;
        } else if(i.kind!=CompressedImageKind::Directory || i.size || i.firstBlock || i.blocks || i.targetOffset || i.targetBytes)return false;
    }
    if(Inode(0)->kind!=CompressedImageKind::Directory || nextBlock!=h.blockCount)return false;
    uint64_t nextData=h.dataOffset;
    for(uint64_t b=0;b<h.blockCount;++b) {
        auto& r=*Block(b);
        if(r.offset!=nextData || !r.storedBytes || r.storedBytes>COMPRESSED_IMAGE_BLOCK ||
           !r.plainBytes || r.plainBytes>COMPRESSED_IMAGE_BLOCK || r.codec>1 || r.reserved ||
           (r.codec==0 && r.storedBytes!=r.plainBytes) || (r.codec==1 && r.storedBytes>=r.plainBytes) ||
           nextData>h.imageBytes || r.storedBytes>h.imageBytes-nextData)return false;
        nextData+=r.storedBytes;
    }
    if(h.imageBytes!=Align(nextData))return false;
    for(uint32_t d=0;d<h.direntCount;++d) {
        auto& e=*Entry(d);
        if(e.parent>=h.inodeCount || !e.inode || e.inode>=h.inodeCount || e.reserved ||
           Inode(e.parent)->kind!=CompressedImageKind::Directory || e.nameOffset>h.stringBytes ||
           e.nameBytes>h.stringBytes-e.nameOffset || !StringValid(Name(e),e.nameBytes,true))return false;
        if(d) {auto& p=*Entry(d-1);if(p.parent>e.parent || (p.parent==e.parent && Compare(Name(p),p.nameBytes,Name(e),e.nameBytes)>=0))return false;}
        auto& n=nodes_[e.inode];++n.links;
        if(Inode(e.inode)->kind==CompressedImageKind::Directory) {
            if(n.links!=1)return false;
            n.parent=e.parent;n.directoryEntry=d;
        }
    }
    for(uint32_t id=1;id<h.inodeCount;++id) {
        if(!nodes_[id].links)return false;
        if(Inode(id)->kind!=CompressedImageKind::Directory)continue;
        uint32_t at=id,length=0;
        for(unsigned depth=0;at;++depth) {
            if(depth>=128 || nodes_[at].parent==None || nodes_[at].directoryEntry==None)return false;
            length+=Entry(nodes_[at].directoryEntry)->nameBytes+1;
            if(length>4095)return false;
            at=nodes_[at].parent;
        }
        nodes_[id].pathBytes=length;
    }
    for(uint32_t d=0;d<h.direntCount;++d) {auto& e=*Entry(d);if(nodes_[e.parent].pathBytes+e.nameBytes+1>4095)return false;}
    ready_=true;return true;
}
int64_t CompressedImage::FindChild(uint32_t parent,const char* name,uint32_t length) const {
    if(!ready_ || !name || !Inode(parent))return -22;
    if(Inode(parent)->kind!=CompressedImageKind::Directory)return -20;
    uint64_t low=0,high=header_.direntCount;
    while(low<high) {
        uint64_t mid=low+(high-low)/2;auto& e=*Entry(mid);
        int compare=e.parent<parent?-1:e.parent>parent?1:Compare(Name(e),e.nameBytes,name,length);
        if(compare<0)low=mid+1;else high=mid;
    }
    if(low==header_.direntCount)return -2;auto& e=*Entry(low);
    return e.parent==parent && !Compare(Name(e),e.nameBytes,name,length)?int64_t(low):-2;
}
int64_t CompressedImage::FindPath(const char* path,uint32_t* last) const {
    if(!ready_ || !path)return -22;uint32_t inode=0;if(last)*last=None;
    while(*path=='/')++path;
    while(*path) {
        auto* begin=path;while(*path && *path!='/')++path;uint32_t length=path-begin;
        if(!StringValid(begin,length,true))return -22;
        auto result=FindChild(inode,begin,length);if(result<0)return result;
        inode=Entry(result)->inode;if(last)*last=result;
        if(*path=='/' && Inode(inode)->kind!=CompressedImageKind::Directory)return -20;
        while(*path=='/')++path;
    }
    return inode;
}
int CompressedImage::LoadBlock(uint64_t id) {
    if(cachedBlock_==id){++stats_.cacheHits;return 0;}
    cachedBlock_=~uint64_t{0};auto& b=*Block(id);
    int result=io_.read(io_.context,b.offset,encoded_,b.storedBytes);
    if(result!=int(b.storedBytes)){++stats_.ioErrors;return result<0?result:-5;}
    stats_.deviceBytes+=b.storedBytes;
    if(!b.codec)for(uint32_t i=0;i<b.plainBytes;++i)decoded_[i]=encoded_[i];
    else if(io_.decode(encoded_,b.storedBytes,decoded_,b.plainBytes)!=int(b.plainBytes)){++stats_.dataErrors;return -5;}
    if(CompressedImageChecksum(decoded_,b.plainBytes)!=b.checksum){++stats_.dataErrors;return -5;}
    ++stats_.blockLoads;if(b.codec)++stats_.decodedBlocks;cachedBlock_=id;return 0;
}
int CompressedImage::Read(uint32_t id,uint64_t offset,void* output,uint32_t length) {
    if(!ready_ || !Inode(id) || (!output && length) || length>0x7fffffffu)return -22;
    auto& i=*Inode(id);if(i.kind!=CompressedImageKind::File)return -21;
    if(offset>=i.size)return 0;
    if(length>i.size-offset)length=i.size-offset;
    uint32_t done=0;auto* out=static_cast<uint8_t*>(output);
    while(done<length) {
        uint64_t at=offset+done;int result=LoadBlock(i.firstBlock+at/COMPRESSED_IMAGE_BLOCK);
        if(result)return done?int(done):result;
        uint32_t within=at%COMPRESSED_IMAGE_BLOCK,n=COMPRESSED_IMAGE_BLOCK-within;
        if(n>length-done)n=length-done;
        for(uint32_t j=0;j<n;++j)out[done+j]=decoded_[within+j];done+=n;
    }
    return done;
}
}
