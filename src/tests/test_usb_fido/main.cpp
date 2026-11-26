#include <assert.h>
#include <stdint.h>
#include <initializer_list>
#include <string.h>

#include "usb_fido.h"

int main()
{
    using namespace brook;
    constexpr uint8_t report[] = {
        0x06, 0xd0, 0xf1, 0x09, 0x01, 0xa1, 0x01,
        0x09, 0x20, 0x15, 0, 0x26, 0xff, 0, 0x75, 8, 0x95, 64, 0x81, 2,
        0x09, 0x21, 0x75, 8, 0x95, 64, 0x91, 2, 0xc0
    };
    assert(UsbFidoReportDescriptor(report, sizeof(report)));
    for (size_t length = 0; length < sizeof(report); ++length)
        assert(!UsbFidoReportDescriptor(report, length));
    uint8_t invalid[sizeof(report)];
    for (const size_t index : {size_t(1), size_t(4), size_t(8), size_t(17), size_t(27)}) {
        memcpy(invalid, report, sizeof(report));
        invalid[index] ^= 1;
        assert(!UsbFidoReportDescriptor(invalid, sizeof(invalid)));
    }

    constexpr uint8_t composite[] = {
        9, 4, 0, 0, 1, 3, 1, 1, 0,
        7, 5, 0x81, 3, 8, 0, 10,
        9, 4, 1, 0, 2, 3, 0, 0, 0,
        9, 0x21, 0x11, 1, 0, 1, 0x22, sizeof(report), 0,
        7, 5, 0x82, 3, 64, 0, 5,
        7, 5, 0x03, 3, 64, 0, 5,
        9, 4, 2, 0, 0, 0x0b, 0, 0, 0,
    };
    UsbFidoInterface found{};
    assert(UsbFidoFindInterface(composite, sizeof(composite), 0, found));
    assert(found.number == 1 && found.in == 0x82 && found.out == 0x03);
    assert(found.descriptorBytes == sizeof(report));
    assert(!UsbFidoFindInterface(composite, sizeof(composite), 2, found));
    assert(!UsbFidoFindInterface(composite, sizeof(composite) - 10, 0, found));
}
