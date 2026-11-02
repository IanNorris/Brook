#pragma once
#include <stdint.h>
#include <stddef.h>

// Bounded byte ownership for one TCP sender. The caller serializes every
// operation with the socket lock. Producers pass kernel-owned staged bytes;
// no user pointers or allocation are retained here. The caller also owns the
// storage and must keep it alive until all worker and producer references end.
//
// Reserved bytes have been copied to flight-owned retransmission storage but
// remain in this ring until a validated cumulative ACK retires them. Queue
// reservation is not itself a TCP sequence-number reservation: the caller
// must publish both changes together under the same socket lock.
// Copy buffers must not overlap the ring storage or this object's metadata.
struct TcpSendQueue {
    uint8_t* bytes=nullptr;
    uint32_t capacity=0,head=0,used=0,reserved=0;

    TcpSendQueue()=default;
    TcpSendQueue(const TcpSendQueue&)=delete;
    TcpSendQueue& operator=(const TcpSendQueue&)=delete;
    TcpSendQueue(TcpSendQueue&&)=delete;
    TcpSendQueue& operator=(TcpSendQueue&&)=delete;

    bool Init(uint8_t* storage,uint32_t size) {
        if(bytes || !storage || !size)return false;
        bytes=storage;capacity=size;return true;
    }
    uint32_t Free() const { return capacity-used; }
    uint32_t Unreserved() const { return used-reserved; }

    // Caller copied and validated the source already. Returns an accepted
    // prefix; never changes the ring for a null input or zero length.
    uint32_t Append(const uint8_t* source,uint32_t length) {
        if(!bytes || !source || !length)return 0;
        if(length>Free())length=Free();
        uint32_t tail=static_cast<uint32_t>((static_cast<uint64_t>(head)+used)%capacity);
        const uint32_t first=length<capacity-tail ? length : capacity-tail;
        __builtin_memcpy(bytes+tail,source,first);
        if(length>first)__builtin_memcpy(bytes,source+first,length-first);
        used+=length;return length;
    }
    // Copy a prefix of not-yet-reserved data to stable flight storage. The
    // caller must reserve the same byte count in the TCP stream under its
    // socket lock before making this operation visible to any other thread.
    uint32_t ReserveCopy(uint8_t* destination,uint32_t length) {
        if(!bytes || !destination || !length)return 0;
        if(length>Unreserved())length=Unreserved();
        uint32_t cursor=static_cast<uint32_t>((static_cast<uint64_t>(head)+reserved)%capacity);
        const uint32_t first=length<capacity-cursor ? length : capacity-cursor;
        __builtin_memcpy(destination,bytes+cursor,first);
        if(length>first)__builtin_memcpy(destination+first,bytes,length-first);
        reserved+=length;return length;
    }
    // The argument is a newly acknowledged byte count, not a TCP ACK number.
    // The caller must validate it against actually submitted bytes; reserved
    // alone does not prove a packet reached the NIC.
    bool Retire(uint32_t acknowledged) {
        if(acknowledged>reserved)return false;
        if(acknowledged){head=static_cast<uint32_t>((static_cast<uint64_t>(head)+acknowledged)%capacity);used-=acknowledged;reserved-=acknowledged;}
        return true;
    }
};
