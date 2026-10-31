#pragma once
#include "ssd_install_plan.h"
#include "update/payload.h"
#include <stdint.h>

namespace brook {
// 40-byte canonical header + 32-byte SHA-256 over header followed by all
// packed source bytes. This is an integrity record, not a signature or an
// authorization decision. A production caller must obtain an expected digest
// from a trusted source and hold immutable leases for the entire installation.
static constexpr unsigned SsdDigestHeaderBytes=40,SsdDigestRecordBytes=72;
inline bool SsdDigestRecordMatches(const uint8_t* record,uint32_t bytes,const SsdInstallGeometry& geometry,uint64_t packedBytes) {
    static constexpr uint8_t magic[8]={'B','R','K','S','S','D','1',0};
    return record && bytes==SsdDigestRecordBytes && SsdInstallGeometryValid(geometry) &&
        BrookUpdateEqual(record,magic,8) && BrookUpdateGet64(record+8)==geometry.diskSectors &&
        BrookUpdateGet64(record+16)==geometry.payloadFirst && BrookUpdateGet64(record+24)==geometry.payloadSectors &&
        BrookUpdateGet64(record+32)==packedBytes && packedBytes==(34+geometry.payloadSectors+33)*512;
}
template<class Source> bool SsdDigestVerifyPinned(Source& source,const uint8_t* record,uint32_t bytes,
                                                   const SsdInstallGeometry& geometry,uint64_t packedBytes,
                                                   uint8_t* buffer,uint32_t capacity) {
    if(!buffer || !capacity || capacity>65536 || !SsdDigestRecordMatches(record,bytes,geometry,packedBytes))return false;
    BrookSha256 hash;BrookShaInit(&hash);BrookShaAdd(&hash,record,SsdDigestHeaderBytes);
    for(uint64_t pos=0;pos<packedBytes;) {
        const uint32_t n=uint32_t(packedBytes-pos<capacity?packedBytes-pos:capacity);
        if(source.readPacked(pos,buffer,n)!=int(n))return false;
        BrookShaAdd(&hash,buffer,n);pos+=n;
    }
    uint8_t digest[32];BrookShaFinish(&hash,digest);
    return BrookUpdateEqual(digest,record+SsdDigestHeaderBytes,32);
}
// A source-supplied checksum alone can be replaced with the source. A future
// installer must provide the expected record from its boot-pinned manifest,
// and must reject any mismatch before reading the payload or writing a target.
template<class Source> bool SsdDigestVerifyExpectedPinned(Source& source,const uint8_t* record,uint32_t bytes,
                                                           const uint8_t* expected,uint32_t expectedBytes,
                                                           const SsdInstallGeometry& geometry,uint64_t packedBytes,
                                                           uint8_t* buffer,uint32_t capacity) {
    if(!expected || expectedBytes!=SsdDigestRecordBytes || !record || bytes!=SsdDigestRecordBytes ||
       !BrookUpdateEqual(record,expected,SsdDigestRecordBytes))return false;
    return SsdDigestVerifyPinned(source,record,bytes,geometry,packedBytes,buffer,capacity);
}
}
