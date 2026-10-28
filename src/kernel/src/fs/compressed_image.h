#pragma once
#include "compressed_image_format.h"
namespace brook {
uint64_t CompressedImageChecksum(const void* bytes,uint64_t length);
bool CompressedImageHeaderValid(const CompressedImageHeader& header,uint64_t deviceBytes);
struct CompressedImageNodeState { uint32_t links,parent,directoryEntry,pathBytes; };
struct CompressedImageIo {
    void* context;
    int (*read)(void*,uint64_t,void*,uint64_t);
    int (*decode)(const void*,int,void*,int);
};
struct CompressedImageReadStats { uint64_t deviceBytes,blockLoads,decodedBlocks,cacheHits,ioErrors,dataErrors; };
// Index and node state are caller-owned resident storage; index/device must
// remain immutable. Two independent 64 KiB scratch buffers and serialization
// are provided by the mount. No operation allocates or changes an inode.
class CompressedImage {
public:
    CompressedImage()=default;
    CompressedImage(const CompressedImage&)=delete;
    CompressedImage& operator=(const CompressedImage&)=delete;
    bool Initialize(const CompressedImageHeader& header,uint64_t deviceBytes,
                    const void* index,uint64_t indexBytes,CompressedImageNodeState* nodes,uint64_t nodeCount,
                    CompressedImageIo io,void* encodedScratch,void* decodedScratch);
    const CompressedImageInode* Inode(uint32_t id) const;
    const CompressedImageDirent* Entry(uint32_t id) const;
    const char* Name(const CompressedImageDirent& entry) const;
    const char* Target(uint32_t inode) const;
    const CompressedImageNodeState* State(uint32_t id) const;
    int64_t FindChild(uint32_t parent,const char* name,uint32_t length) const; // dirent ID, or errno
    int64_t FindPath(const char* path,uint32_t* entry=nullptr) const; // normalized path, never follows links
    int Read(uint32_t inode,uint64_t offset,void* output,uint32_t length);
    CompressedImageReadStats Stats() const{return stats_;}
    const CompressedImageHeader& Header() const{return header_;}
private:
    int LoadBlock(uint64_t index);
    const CompressedImageBlock* Block(uint64_t index) const;
    CompressedImageHeader header_{};
    const uint8_t* index_=nullptr;
    CompressedImageNodeState* nodes_=nullptr;
    CompressedImageIo io_{};
    uint8_t* encoded_=nullptr;uint8_t* decoded_=nullptr;
    uint64_t cachedBlock_=~uint64_t{0};
    CompressedImageReadStats stats_{};
    bool ready_=false;
};
}
