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
    info.sectors=12587008;strcpy(info.serial,"BROOK-AHCI-INSTALL06");
    assert(AhciDisposableDesktopInstallAdmission(true,true,0,info,true));
    assert(!AhciDisposableDesktopInstallAdmission(false,true,0,info,true));
    assert(!AhciDisposableDesktopInstallAdmission(true,false,0,info,true));
    assert(!AhciDisposableDesktopInstallAdmission(true,true,1,info,true));
    assert(!AhciDisposableDesktopInstallAdmission(true,true,0,info,false));
    info.sectors=131072;assert(!AhciDisposableDesktopInstallAdmission(true,true,0,info,true));info.sectors=12587008;
    --info.sectors;assert(!AhciDisposableDesktopInstallAdmission(true,true,0,info,true));++info.sectors;
    info.serial[0]='X';assert(!AhciDisposableDesktopInstallAdmission(true,true,0,info,true));
    info.sectors=500118192;strcpy(info.serial,"BROOK-AHCI-DELL256");
    assert(AhciDisposableDesktopInstallAdmission(true,true,0,info,true));
    assert(!AhciDisposableDesktopInstallAdmission(false,true,0,info,true));
    assert(!AhciDisposableDesktopInstallAdmission(true,false,0,info,true));
    assert(!AhciDisposableDesktopInstallAdmission(true,true,1,info,true));
    assert(!AhciDisposableDesktopInstallAdmission(true,true,0,info,false));
    --info.sectors;assert(!AhciDisposableDesktopInstallAdmission(true,true,0,info,true));++info.sectors;
    strcpy(info.serial,"BROOK-AHCI-INSTALL06");assert(!AhciDisposableDesktopInstallAdmission(true,true,0,info,true));
    info.sectors=12587008;strcpy(info.serial,"BROOK-AHCI-DELL256");assert(!AhciDisposableDesktopInstallAdmission(true,true,0,info,true));
    info.sectors=500118192;strcpy(info.model,"SAMSUNG SSD PM851");assert(!AhciDisposableDesktopInstallAdmission(true,true,0,info,true));
    strcpy(info.model,"QEMU HARDDISK");
    info.serial[0]='X';assert(!AhciDisposableDesktopInstallAdmission(true,true,0,info,true));
    strcpy(info.serial,"BROOK-DATA-TEST01");info.logicalSectorBytes=512;info.physicalSectorBytes=512;
    assert(AhciDisposableLargeDataAdmission(true,false,true,0,info,true));
    assert(!AhciDisposableLargeDataAdmission(false,false,true,0,info,true));
    assert(!AhciDisposableLargeDataAdmission(true,true,true,0,info,true));
    assert(!AhciDisposableLargeDataAdmission(true,false,false,0,info,true));
    assert(!AhciDisposableLargeDataAdmission(true,false,true,1,info,true));
    assert(!AhciDisposableLargeDataAdmission(true,false,true,0,info,false));
    --info.sectors;assert(!AhciDisposableLargeDataAdmission(true,false,true,0,info,true));++info.sectors;
    info.logicalSectorBytes=4096;assert(!AhciDisposableLargeDataAdmission(true,false,true,0,info,true));info.logicalSectorBytes=512;
    info.physicalSectorBytes=4096;assert(!AhciDisposableLargeDataAdmission(true,false,true,0,info,true));info.physicalSectorBytes=512;
    strcpy(info.model,"SAMSUNG SSD PM851");assert(!AhciDisposableLargeDataAdmission(true,false,true,0,info,true));
    strcpy(info.model,"QEMU HARDDISK");info.serial[0]='X';assert(!AhciDisposableLargeDataAdmission(true,false,true,0,info,true));
}
