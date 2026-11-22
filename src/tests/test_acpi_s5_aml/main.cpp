#include "acpi_s5_aml.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using brook::AcpiReadStaticS5;
using brook::AcpiS5Result;
using Bytes = std::vector<uint8_t>;

// AML package lengths include the length encoding itself.
static Bytes WithPackage(Bytes prefix, const Bytes& body)
{
    const auto size = body.size();
    assert(size <= 0xffd);
    if (size < 63) {
        prefix.push_back(uint8_t(size + 1));
    } else {
        const auto length = size + 2;
        prefix.push_back(uint8_t(0x40 | (length & 0xf)));
        prefix.push_back(uint8_t(length >> 4));
    }
    prefix.insert(prefix.end(), body.begin(), body.end());
    return prefix;
}

static Bytes Append(Bytes prefix, const Bytes& suffix)
{
    prefix.insert(prefix.end(), suffix.begin(), suffix.end());
    return prefix;
}

static void Expect(const Bytes& aml, AcpiS5Result expected,
                   uint8_t expectedA = 0, uint8_t expectedB = 0)
{
    uint8_t a = 0xee, b = 0xff;
    const uint8_t empty = 0;
    const auto result = AcpiReadStaticS5(aml.empty() ? &empty : aml.data(), aml.size(), a, b);
    assert(result == expected);
    if (result == AcpiS5Result::Found) {
        assert(a == expectedA && b == expectedB);
    } else {
        assert(a == 0xee && b == 0xff);
    }
}

int main()
{
    const Bytes s5 = WithPackage({0x08, '_', 'S', '5', '_', 0x12},
                                 {4, 0x0a, 5, 0x0b, 7, 0, 0, 0});
    const Bytes absolute = Append({0x08, 0x5c}, Bytes(s5.begin() + 1, s5.end()));
    Expect({}, AcpiS5Result::Absent);
    Expect(s5, AcpiS5Result::Found, 5, 7);
    Expect(absolute, AcpiS5Result::Found, 5, 7);
    Expect(WithPackage({0x10}, Append({0x5c, 0}, s5)), AcpiS5Result::Found, 5, 7);
    Expect(WithPackage({0x5b, 0x82}, Append({'D', 'E', 'V', '0'}, s5)),
           AcpiS5Result::Absent);
    Expect(WithPackage({0x5b, 0x82}, Append({'D', 'E', 'V', '0'}, absolute)),
           AcpiS5Result::Found, 5, 7);
    Expect(WithPackage({0x5b, 0x82}, Append({'D', 'E', 'V', '0', 0x08, 0x5e},
                                          Bytes(s5.begin() + 1, s5.end()))),
           AcpiS5Result::Found, 5, 7);

    const Bytes method = WithPackage({0x14}, Append({'M', 'T', 'H', 'D', 0}, absolute));
    const Bytes buffer = WithPackage({0x08, 'B', 'U', 'F', 'R', 0x11},
                                     Append({0x0a, uint8_t(absolute.size())}, absolute));
    const Bytes unrelated = Append(method, buffer);
    Expect(unrelated, AcpiS5Result::Absent);
    Expect(Append(unrelated, s5), AcpiS5Result::Found, 5, 7);
    Expect(WithPackage({0x14}, {'_', 'S', '5', '_', 0, 0xa4, 0}),
           AcpiS5Result::Unsupported);
    Expect({0x06, 'A', 'B', 'C', 'D', '_', 'S', '5', '_'}, AcpiS5Result::Unsupported);
    const Bytes field = WithPackage({0x5b, 0x81}, {'R', 'E', 'G', '0', 0, '_', 'S', '5', '_', 8});
    Expect(field, AcpiS5Result::Unsupported);
    Expect(WithPackage({0x5b, 0x82}, Append({'D', 'E', 'V', '0'}, field)),
           AcpiS5Result::Absent);

    // An unqualified Scope lookup inside DEV0 might open DEV0.DEV1 or
    // its ancestor's DEV1. A parent-qualified _S5_ could then be root.
    const Bytes parentS5 = Append({0x08, 0x5e}, Bytes(s5.begin() + 1, s5.end()));
    const Bytes relativeScope = WithPackage({0x10}, Append({'D', 'E', 'V', '1'}, parentS5));
    Expect(WithPackage({0x5b, 0x82}, Append({'D', 'E', 'V', '0'}, relativeScope)),
           AcpiS5Result::Unsupported);
    Expect(Append(s5, s5), AcpiS5Result::Unsupported);
    Expect(Append(s5, WithPackage({0x10}, Append({0x5c, 0}, s5))),
           AcpiS5Result::Unsupported);

    const Bytes s5Package = {0x08, '_', 'S', '5', '_', 0x12};
    Expect(WithPackage(s5Package, {2, 0, 1}), AcpiS5Result::Found, 0, 1);
    Expect(WithPackage(s5Package, {4, 0, 1}), AcpiS5Result::Found, 0, 1);
    Expect(WithPackage(s5Package, {2, 0x0c, 3, 0, 0, 0, 0x0e, 6, 0, 0, 0, 0, 0, 0, 0}),
           AcpiS5Result::Found, 3, 6);
    Expect(WithPackage(s5Package, {2, 0, 0, 0}), AcpiS5Result::Unsupported);
    Expect(WithPackage(s5Package, {1, 0, 0}), AcpiS5Result::Unsupported);
    Expect(WithPackage(s5Package, {2, 0x0a, 8, 1}), AcpiS5Result::Unsupported);
    Expect(WithPackage(s5Package, {2, 0, 0xff}), AcpiS5Result::Unsupported);
    Expect(WithPackage(s5Package, {2, 'N', 'A', 'M', 'E', 1}), AcpiS5Result::Unsupported);
    Expect(Append(s5, {0xa0, 2, 1}), AcpiS5Result::Unsupported);
    const Bytes unsupported = WithPackage({0x10}, Append({0x5c, 0}, Append(s5, {0xa0, 2, 1})));
    uint8_t typeA=0,typeB=0;
    uint32_t offset=0;
    assert(AcpiReadStaticS5(unsupported.data(),unsupported.size(),typeA,typeB,&offset)==AcpiS5Result::Unsupported);
    assert(offset==unsupported.size()-3 && unsupported[offset]==0xa0);
    Expect(Append(s5Package, {0x53, 0, 2, 0, 0}), AcpiS5Result::Unsupported);
    Expect(Append(s5Package, {0x40, 0}), AcpiS5Result::Unsupported);
    Expect({0x08, '_', 'S', '5', 'a', 0}, AcpiS5Result::Unsupported);
    for (size_t n = 1; n < s5.size(); ++n)
        Expect(Bytes(s5.begin(), s5.begin() + n), AcpiS5Result::Unsupported);

    Bytes manyElements = {70, 1, 0};
    manyElements.resize(71);
    Expect(WithPackage(s5Package, manyElements), AcpiS5Result::Found, 1, 0);
    Bytes nested = s5;
    for (int i = 0; i < 25; ++i)
        nested = WithPackage({0x10}, Append({0x5c, 0}, nested));
    Expect(nested, AcpiS5Result::Unsupported);

    // VMware declares an ECFG field and uses it as a region base, including
    // in Add(ECFG, literal, NullName). The static resolver never reads it.
    const Bytes ecfg = {'E', 'C', 'F', 'G'};
    const Bytes fieldList = WithPackage({0x5b,0x81},
        Append({'O','E','M','D',0}, Append(ecfg,{32})));
    const Bytes region = Append({0x5b,0x80,'E','I','C','H',0,0x72},
                                 Append(ecfg,{0x0b,0,0x40,0,0x0b,0,0x40}));
    const Bytes vmS5 = WithPackage(s5Package,{2,0,0});
    const Bytes vm = Append(Append(fieldList,region),vmS5);
    bool references=false;
    assert(AcpiReadStaticS5(vm.data(),vm.size(),typeA,typeB,&offset,&references)==AcpiS5Result::Found);
    assert(references && typeA==0 && typeB==0);
    Expect(vm,AcpiS5Result::Unsupported);
    const Bytes unsafe[] = {
        Append(region,vmS5),
        Append(vm,WithPackage({0x14},Append(ecfg,{1}))),
        Append(Append({0x06,'O','E','M','D'},ecfg),vm),
        Append(Append({0x15},Append(ecfg,{8,1})),vm),
    };
    for (const Bytes& aml : unsafe)
        assert(AcpiReadStaticS5(aml.data(),aml.size(),typeA,typeB,&offset,&references)
               ==AcpiS5Result::Unsupported);

    std::puts("ACPI static _S5 AML tests passed");
}
