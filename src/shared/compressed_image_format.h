#pragma once
#include <stdint.h>
#include <stddef.h>
namespace brook {
// BRKPACK1: immutable, little-endian x86-64 image. Runtime swap is a different
// format. Checksums detect accidental corruption, not malicious replacement.
constexpr uint64_t COMPRESSED_IMAGE_MAGIC=0x314b4341504b5242ULL;
constexpr uint32_t COMPRESSED_IMAGE_BLOCK=65536;
constexpr uint64_t COMPRESSED_IMAGE_INDEX_LIMIT=16*1024*1024;
struct CompressedImageHeader {
    uint64_t magic;
    uint32_t version,headerBytes;
    uint64_t requiredFeatures,imageBytes;
    uint32_t blockBytes,rootInode;
    uint64_t inodeOffset,inodeCount,direntOffset,direntCount;
    uint64_t blockOffset,blockCount,stringOffset,stringBytes,dataOffset;
    uint8_t identity[16];
    uint64_t metadataChecksum,headerChecksum;
    uint8_t reserved[16];
};
enum class CompressedImageKind:uint32_t { Directory=1,File=2,Symlink=3 };
struct CompressedImageInode {
    CompressedImageKind kind;
    uint32_t mode,uid,gid;
    uint64_t size,firstBlock,blocks,targetOffset;
    uint32_t targetBytes,reserved;
    int64_t mtime;
};
struct CompressedImageDirent { uint32_t parent,inode,nameOffset;uint16_t nameBytes,reserved; };
struct CompressedImageBlock { uint64_t offset;uint32_t storedBytes,plainBytes,codec,reserved;uint64_t checksum; };
static_assert(sizeof(CompressedImageHeader)==160);
static_assert(offsetof(CompressedImageHeader,metadataChecksum)==128);
static_assert(offsetof(CompressedImageHeader,headerChecksum)==136);
static_assert(sizeof(CompressedImageInode)==64);
static_assert(sizeof(CompressedImageDirent)==16);
static_assert(sizeof(CompressedImageBlock)==32);
}
