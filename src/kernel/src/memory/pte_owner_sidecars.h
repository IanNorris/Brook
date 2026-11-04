#pragma once
#include <stdint.h>

namespace brook {
// Software owner storage for leaf page tables. This is not connected to the
// live VMM yet. All operations require the caller's page-table domain lock;
// no pointer returned from Find may outlive that lock or table retirement.
// Pages are supplied by the caller, so allocation and failure handling remain
// explicit and no allocator is called from this directory.
class PteOwnerSidecars {
public:
    static constexpr uint16_t Escape=1023, Empty=0xffff;
    static constexpr unsigned Slots=512, Buckets=256;
    struct Page {
        Page* next;
        uint64_t tablePhysical;
        uint16_t owner[Slots];
    };
    static_assert(sizeof(Page)<=4096,"owner sidecar must fit one page");
private:
    Page* buckets_[Buckets]{};
    static unsigned Hash(uint64_t table) {
        auto page=table>>12;return unsigned((page^(page>>8)^(page>>16))&(Buckets-1));
    }
    static bool ValidTable(uint64_t table) {return table && !(table&4095);}
public:
    PteOwnerSidecars()=default;
    PteOwnerSidecars(const PteOwnerSidecars&)=delete;
    PteOwnerSidecars& operator=(const PteOwnerSidecars&)=delete;
    Page* Find(uint64_t table) const {
        if(!ValidTable(table))return nullptr;
        for(auto* p=buckets_[Hash(table)];p;p=p->next)if(p->tablePhysical==table)return p;
        return nullptr;
    }
    // Spare is an exclusively owned page. Returns false on invalid input or a
    // duplicate key without consuming/changing the supplied page.
    bool Install(uint64_t table,Page* spare) {
        if(!spare || !ValidTable(table) || Find(table))return false;
        const auto bucket=Hash(table);
        spare->tablePhysical=table;
        for(auto& owner:spare->owner)owner=Empty;
        spare->next=buckets_[bucket];buckets_[bucket]=spare;return true;
    }
    // A high-owner publication is allowed only after successful preflight.
    // Set rejects invalid input without changing existing owner metadata.
    bool Set(uint64_t table,unsigned slot,uint16_t owner) {
        if(slot>=Slots || owner<Escape || owner==Empty)return false;
        auto* p=Find(table);if(!p)return false;
        p->owner[slot]=owner;return true;
    }
    bool Read(uint64_t table,unsigned slot,uint16_t& owner) const {
        if(slot>=Slots)return false;
        const auto* p=Find(table);if(!p || p->owner[slot]==Empty)return false;
        owner=p->owner[slot];return true;
    }
    void Clear(uint64_t table,unsigned slot) {
        if(slot>=Slots)return;
        auto* p=Find(table);if(p)p->owner[slot]=Empty;
    }
    // Detaching a leaf does not remove its table's sidecar. Only unlink after
    // the table's last user is gone and its TLB retirement has completed.
    Page* Remove(uint64_t table) {
        if(!ValidTable(table))return nullptr;
        auto** link=&buckets_[Hash(table)];
        while(*link && (*link)->tablePhysical!=table)link=&(*link)->next;
        if(!*link)return nullptr;
        auto* p=*link;*link=p->next;p->next=nullptr;p->tablePhysical=0;return p;
    }
};
}
