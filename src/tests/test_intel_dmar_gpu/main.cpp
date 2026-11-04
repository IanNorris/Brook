#include <cassert>
#include <cstring>
#include "intel_dmar_gpu.h"
#include "dell_fixture.h"
using namespace brook;
int main() {
    uint8_t table[sizeof(dellDmarFixture)];std::memcpy(table,dellDmarFixture,sizeof(table));
    auto fixedChecksum=[&] { table[9]=0;uint8_t sum=0;for(auto v:table)sum=uint8_t(sum+v);table[9]=uint8_t(0-sum); };
    auto found=DmarAuditedDellGpuUnit(table,sizeof(table));
    assert(found.found && found.registerBase==0xfed90000ull);
    assert(!DmarAuditedDellGpuUnit(table,sizeof(table)-1).found);
    table[10]^=1;fixedChecksum(); // valid topology, but unreviewed firmware
    assert(DmarExplicitDellGpuUnit(table,sizeof(table)).found);
    assert(!DmarAuditedDellGpuUnit(table,sizeof(table)).found);
    std::memcpy(table,dellDmarFixture,sizeof(table));
    table[50]=0xff;assert(!DmarAuditedDellGpuUnit(table,sizeof(table)).found);
    fixedChecksum();assert(!DmarExplicitDellGpuUnit(table,sizeof(table)).found);
    std::memcpy(table,dellDmarFixture,sizeof(table));
    table[48+16+6]=3;fixedChecksum();
    assert(!DmarExplicitDellGpuUnit(table,sizeof(table)).found);
    assert(!DmarAuditedDellGpuUnit(table,sizeof(table)).found);
    std::memcpy(table,dellDmarFixture,sizeof(table));
    table[48+24+2]=0xff;fixedChecksum(); // malformed record after a valid GPU match
    assert(!DmarExplicitDellGpuUnit(table,sizeof(table)).found);
    assert(!DmarAuditedDellGpuUnit(table,sizeof(table)).found);
    std::memcpy(table,dellDmarFixture,sizeof(table));
    table[48+16+1]=7;fixedChecksum();
    assert(!DmarExplicitDellGpuUnit(table,sizeof(table)).found);
    assert(!DmarAuditedDellGpuUnit(table,sizeof(table)).found);
}
