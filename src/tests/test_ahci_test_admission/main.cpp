#include "../../kernel/src/ahci_test_admission.h"
#include <cassert>
using namespace brook;
int main() {
    AtaIdentifyInfo info{};info.sectors=131072;
    strcpy(info.model,"QEMU HARDDISK");strcpy(info.serial,"BROOK-AHCI-TEST-01");
    assert(AhciDisposableTestAdmission(true,true,0,info,true));
    assert(!AhciDisposableTestAdmission(false,true,0,info,true));
    assert(!AhciDisposableTestAdmission(true,false,0,info,true));
    assert(!AhciDisposableTestAdmission(true,true,1,info,true));
    assert(!AhciDisposableTestAdmission(true,true,0,info,false));
    info.sectors--;assert(!AhciDisposableTestAdmission(true,true,0,info,true));info.sectors++;
    info.model[0]='X';assert(!AhciDisposableTestAdmission(true,true,0,info,true));info.model[0]='Q';
    info.serial[0]='X';assert(!AhciDisposableTestAdmission(true,true,0,info,true));
}
