#pragma once
#include "intel_bdw_ggtt_extent_bind.h"

namespace brook {
// Unwired accessor over caller-audited UC mappings, one pointer per 4 KiB PTE
// page. The owner holds mapping/device serialization and pinned pages until
// retirement and audits each page against its expected physical MMIO address.
// It must also supply a completed GPU/display-appropriate invalidation; the
// flush-control posting read is not by itself an invalidation proof.
template<class Owner> class BdwGgttExtentMmio {
    Owner& owner_;BdwGgttExtentLease lease_;
    volatile uint64_t* const* pages_;uint32_t pageCount_;volatile uint32_t* flush_;uint64_t scratch_;
    bool geometry()const {
        const auto& r=lease_.reservation;
        return r.token && r.count && r.count<=65536 && r.first>=65536 && r.first<1048576 &&
            r.count<=1048576-r.first && lease_.dma && pages_ && flush_ &&
            !(reinterpret_cast<uintptr_t>(flush_)&3) &&
            pageCount_==((r.first&511u)+r.count+511u)/512u;
    }
    volatile uint64_t* page(uint32_t index)const {
        if(!geometry() || index<lease_.reservation.first || index-lease_.reservation.first>=lease_.reservation.count)return nullptr;
        const uint32_t n=(index>>9)-(lease_.reservation.first>>9);
        if(n>=pageCount_)return nullptr;
        auto* p=pages_[n];
        if(!p || (reinterpret_cast<uintptr_t>(p)&4095) || !owner_.auditPtePage((lease_.reservation.first>>9)+n,p))return nullptr;
        return p;
    }
public:
    BdwGgttExtentMmio(Owner& owner,const BdwGgttExtentLease& lease,volatile uint64_t* const* pages,
                      uint32_t pageCount,volatile uint32_t* flush,uint64_t scratch=0)
        :owner_(owner),lease_(lease),pages_(pages),pageCount_(pageCount),flush_(flush),scratch_(scratch){}
    BdwGgttExtentMmio(const BdwGgttExtentMmio&)=delete;
    BdwGgttExtentMmio& operator=(const BdwGgttExtentMmio&)=delete;
    bool validateLease(const BdwGgttExtentLease& l){
        if(!geometry() || l.dma!=lease_.dma || l.reservation.token!=lease_.reservation.token ||
           l.reservation.first!=lease_.reservation.first || l.reservation.count!=lease_.reservation.count ||
           !owner_.owned() || !owner_.validateLease(l))return false;
        for(uint32_t i=0;i<pageCount_;++i){
            const uint32_t index=i?((lease_.reservation.first>>9)+i)*512:lease_.reservation.first;
            if(!page(index))return false;
        }
        return owner_.owned();
    }
    bool quiescent(){return owner_.quiescent();}
    bool validateTakeover(const BdwGgttExtentLease& l,uint64_t scratch){
        return scratch && scratch==scratch_ && validateLease(l) && owner_.validateTakeover(l,scratch);
    }
    bool read(uint32_t index,uint64_t& value){
        if(!owner_.readable())return false;
        auto* p=page(index);if(!p)return false;
        __asm__ volatile("lfence":::"memory");value=p[index&511];
        __asm__ volatile("lfence":::"memory");return value!=~uint64_t{0} && owner_.readable();
    }
    bool write(uint32_t index,uint64_t value){
        if(!owner_.owned())return false;
        auto* p=page(index);if(!p)return false;
        const uint64_t dma=lease_.dma[index-lease_.reservation.first];
        if(!dma || (dma&4095) || dma>=(uint64_t{1}<<39) || (dma>=0xdd000000ull && dma<0xdf800000ull))return false;
        const uint64_t allowed=dma|1ull;
        if(value && value!=allowed && (!scratch_ || value!=(scratch_|1ull) || !validateTakeover(lease_,scratch_)))return false;
        p[index&511]=value;return owner_.owned();
    }
    bool invalidate(){
        if(!geometry() || !owner_.owned())return false;
        __asm__ volatile("mfence":::"memory");*flush_=1;
        __asm__ volatile("mfence":::"memory");const uint32_t posted=*flush_;
        __asm__ volatile("lfence":::"memory");
        return posted!=0xffffffffu && owner_.owned() && owner_.invalidateGpuTranslations();
    }
};
}
