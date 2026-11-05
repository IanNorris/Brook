#pragma once
#include <stdint.h>

namespace brook {
// CF8 is shared across CPUs. Every address/data pair, including a partial
// write, must run under one IRQ-safe lock. Io must provide out32/in32/out16.
template<class Lock,class Io>
class PciConfigAccess {
    Lock& lock_;Io& io_;
    static uint32_t Address(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t offset) {
        return (1u<<31)|(uint32_t(bus)<<16)|(uint32_t(dev&31)<<11)|(uint32_t(fn&7)<<8)|(offset&0xfcu);
    }
public:
    PciConfigAccess(Lock& lock,Io& io):lock_(lock),io_(io){}
    uint32_t Read32(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t offset) {
        auto flags=lock_.Acquire();io_.out32(0xcf8,Address(bus,dev,fn,offset));
        auto value=io_.in32(0xcfc);lock_.Release(flags);return value;
    }
    void Write32(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t offset,uint32_t value) {
        auto flags=lock_.Acquire();io_.out32(0xcf8,Address(bus,dev,fn,offset));
        io_.out32(0xcfc,value);lock_.Release(flags);
    }
    void Write16(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t offset,uint16_t value) {
        auto flags=lock_.Acquire();io_.out32(0xcf8,Address(bus,dev,fn,offset));
        // Do not rewrite the other half: status bits can be write-one-to-clear.
        io_.out16(uint16_t(0xcfc+(offset&2)),value);lock_.Release(flags);
    }
};
}
