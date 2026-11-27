#pragma once
#include <stdint.h>

namespace brook {
// Caller serializes writes/drains and protects storage lifetime. Reads may run
// concurrently with other reads. All storage is supplied before use; no allocator
// or filesystem callbacks are used here. Backend accesses the device directly.
struct WriteBehindQueue {
    enum class Kind : uint8_t { Other, Data, Inode };
    struct Record {
        uint64_t offset;
        uint32_t size, payload, file;
        Kind kind;
    };
    static constexpr uint32_t MaximumDataRun=1024*1024;
    enum class Result { Ok, WriteError, FlushError };
    Record* records=nullptr;
    uint8_t* bytes=nullptr;
    uint32_t recordCapacity=0, byteCapacity=0, count=0, used=0;
    uint64_t oldestNs=0, inputs=0, merges=0, backendWrites=0, backendBytes=0;
    uint64_t drains=0, capacityDrains=0, syncDrains=0, backgroundDrains=0, errors=0;
    bool retry=false, poisoned=false;

    void Init(Record* rows,uint32_t rowsCount,uint8_t* data,uint32_t capacity) {
        *this={};records=rows;recordCapacity=rowsCount;bytes=data;byteCapacity=capacity;
    }
    template<class Backend> Result Drain(Backend& io) {
        if(poisoned)return Result::WriteError;
        ++drains;
        for(uint32_t i=0;i<count;++i) {
            const auto& r=records[i];++backendWrites;backendBytes+=r.size;
            if(!io.Write(r.offset,bytes+r.payload,r.size)) {
                retry=true;++errors;return Result::WriteError;
            }
        }
        if(!io.Flush()) {retry=true;++errors;return Result::FlushError;}
        count=used=0;oldestNs=0;retry=false;return Result::Ok;
    }
    template<class Backend> bool Write(Backend& io,uint64_t offset,const void* source,
                                       uint64_t length,uint64_t now,Kind kind=Kind::Other,uint32_t file=0) {
        if(poisoned)return false;
        if(!length)return true;
        if(length>~uint64_t{0}-offset)return false;
        ++inputs;
        // Never merge or admit a newer write across an uncompleted batch.
        if(retry && Drain(io)!=Result::Ok)return false;
        if(length>byteCapacity || !recordCapacity) {
            ++capacityDrains;
            if(Drain(io)!=Result::Ok)return false;
            ++backendWrites;backendBytes+=length;
            const bool ok=io.Write(offset,source,length);
            if(!ok){poisoned=true;++errors;}return ok;
        }
        if(count) {
            auto& last=records[count-1];
            // The only movable record is this same file's pending inode.
            // Keeping it after newer data preserves the publication ordering.
            // Indirect blocks, bitmaps, directories and other inodes are
            // barriers and stay in arrival order.
            const bool beforeInode=count>=2 && kind==Kind::Data && file &&
                last.kind==Kind::Inode && last.file==file;
            auto& tail=records[count-1-uint32_t(beforeInode)];
            const uint64_t end=offset+length;
            const uint64_t tailEnd=tail.offset+tail.size;
            if(kind==tail.kind && tail.file==file && offset<=tailEnd && end>=tail.offset) {
                const uint64_t start=offset<tail.offset?offset:tail.offset;
                const uint64_t stop=end>tailEnd?end:tailEnd;
                const uint64_t extent=stop-start;
                const uint32_t retained=beforeInode?last.size:0;
                // Reject aliases even though ext2 only tags allocated file
                // data, because metadata must never overwrite a merged run.
                const bool disjoint=!beforeInode || stop<=last.offset ||
                    start>=last.offset+last.size;
                if(disjoint && extent+retained<=byteCapacity-tail.payload &&
                   (kind!=Kind::Data || extent<=MaximumDataRun)) {
                    if(beforeInode) {
                        const uint32_t next=tail.payload+static_cast<uint32_t>(extent);
                        __builtin_memmove(bytes+next,bytes+last.payload,last.size);
                        last.payload=next;
                    }
                    if(start!=tail.offset)
                        __builtin_memmove(bytes+tail.payload+(tail.offset-start),
                                          bytes+tail.payload,tail.size);
                    __builtin_memcpy(bytes+tail.payload+(offset-start),source,length);
                    tail.offset=start;tail.size=static_cast<uint32_t>(extent);
                    used=tail.payload+tail.size+retained;++merges;return true;
                }
            }
        }
        if(count==recordCapacity || length>byteCapacity-used) {
            ++capacityDrains;if(Drain(io)!=Result::Ok)return false;
        }
        if(!count)oldestNs=now;
        records[count++]={offset,static_cast<uint32_t>(length),used,file,kind};
        __builtin_memcpy(bytes+used,source,length);used+=static_cast<uint32_t>(length);
        return true;
    }
    template<class Backend> bool Read(Backend& io,uint64_t offset,void* destination,
                                      uint64_t length) const {
        if(!length)return true;
        if(length>~uint64_t{0}-offset)return false;
        // A containing record supplies the base without touching the device;
        // later records still overlay it. Otherwise retain bulk device reads.
        uint32_t first=0;bool covered=false;
        for(uint32_t i=count;i>0;--i) {
            const auto& r=records[i-1];
            if(r.offset<=offset && offset+length<=r.offset+r.size) {
                __builtin_memcpy(destination,bytes+r.payload+(offset-r.offset),length);
                first=i;covered=true;break;
            }
        }
        if(!covered && !io.Read(offset,destination,length))return false;
        for(uint32_t i=first;i<count;++i) {
            const auto& r=records[i];const uint64_t end=offset+length;
            const uint64_t start=offset>r.offset?offset:r.offset;
            const uint64_t stop=end<r.offset+r.size?end:r.offset+r.size;
            if(stop>start)__builtin_memcpy(static_cast<uint8_t*>(destination)+(start-offset),
                                          bytes+r.payload+(start-r.offset),stop-start);
        }
        return true;
    }
};
}
