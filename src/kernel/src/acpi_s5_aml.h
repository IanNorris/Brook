#pragma once
#include <stdint.h>

namespace brook {

enum class AcpiS5Result { Absent, Found, Unsupported };

namespace acpi_s5 {

class AmlReader {
public:
    AmlReader(const uint8_t* bytes,uint32_t length) : cursor_(bytes),left_(length) {}
    uint32_t Remaining() const { return left_; }
    uint8_t Peek() const { return left_?*cursor_:0; }
    bool Byte(uint8_t& value)
    {
        if (!left_) return false;
        value=*cursor_++;
        --left_;
        return true;
    }
    bool Skip(uint32_t count)
    {
        if (count>left_) return false;
        cursor_+=count;
        left_-=count;
        return true;
    }
    bool PackageLength(uint32_t& length)
    {
        uint8_t lead=0;
        if (!Byte(lead)) return false;
        const unsigned more=lead>>6;
        if (more && (lead&0x30)) return false;
        length=lead&(more?0x0f:0x3f);
        for (unsigned i=0;i<more;++i) {
            uint8_t part=0;
            if (!Byte(part)) return false;
            length|=uint32_t(part)<<(4+8*i);
        }
        return true;
    }
    bool Package(AmlReader& contents)
    {
        const auto before=left_;
        uint32_t length=0;
        if (!PackageLength(length)) return false;
        const auto prefix=before-left_;
        if (length<prefix || length>before) return false;
        const uint32_t payload=length-prefix;
        contents=AmlReader(cursor_,payload);
        return Skip(payload);
    }
    bool Integer(uint64_t& value)
    {
        uint8_t opcode=0;
        if (!Byte(opcode)) return false;
        if (opcode==0 || opcode==1 || opcode==0xff) {
            value=opcode==0xff?~uint64_t{0}:opcode;
            return true;
        }
        unsigned width=0;
        switch (opcode) {
        case 0x0a: width=1; break;
        case 0x0b: width=2; break;
        case 0x0c: width=4; break;
        case 0x0e: width=8; break;
        default: return false;
        }
        value=0;
        for (unsigned i=0;i<width;++i) {
            uint8_t part=0;
            if (!Byte(part)) return false;
            value|=uint64_t(part)<<(8*i);
        }
        return true;
    }
private:
    const uint8_t* cursor_;
    uint32_t left_;
};

struct AmlPath {
    uint32_t minDepth=0,maxDepth=0;
    bool rootS5=false;
};

inline bool Segment(AmlReader& aml,bool& s5)
{
    constexpr uint8_t expected[]={'_','S','5','_'};
    s5=true;
    for (unsigned i=0;i<4;++i) {
        uint8_t part=0;
        if (!aml.Byte(part) ||
            !(part=='_' || (part>='A' && part<='Z') || (i && part>='0' && part<='9'))) return false;
        s5&=part==expected[i];
    }
    return true;
}

inline bool NamePath(AmlReader& aml,AmlPath scope,AmlPath& path,bool scopeLookup=false)
{
    bool anchored=false;
    if (aml.Peek()==0x5c) {
        if (!aml.Skip(1)) return false;
        scope={};
        anchored=true;
    } else {
        while (aml.Peek()==0x5e) {
            if (!scope.minDepth || !aml.Skip(1)) return false;
            --scope.minDepth;
            --scope.maxDepth;
            scope.rootS5=false;
            anchored=true;
        }
    }
    uint8_t segments=1;
    switch (aml.Peek()) {
    case 0:
        if (!scopeLookup || !anchored || !aml.Skip(1)) return false;
        path=scope;
        return true;
    case 0x2e:
        if (!aml.Skip(1)) return false;
        segments=2;
        break;
    case 0x2f:
        if (!aml.Skip(1) || !aml.Byte(segments) || !segments) return false;
        break;
    default: break;
    }
    bool s5=false;
    for (unsigned i=0;i<segments;++i)
        if (!Segment(aml,s5)) return false;
    // Unanchored Scope may resolve in an ancestor. Conservatively retain
    // that range, so parent prefixes cannot hide another root definition.
    const auto least=scopeLookup && !anchored?0:scope.minDepth;
    path={least+segments,scope.maxDepth+segments,!least && segments==1 && s5};
    return true;
}

inline bool DataObject(AmlReader& aml)
{
    const auto opcode=aml.Peek();
    if (opcode==0x0d) { // String
        if (!aml.Skip(1)) return false;
        uint8_t ch=0;
        while (aml.Byte(ch)) if (!ch) return true;
        return false;
    }
    if (opcode==0x11 || opcode==0x12 || opcode==0x13) {
        AmlReader body(nullptr,0);
        if (!aml.Skip(1) || !aml.Package(body)) return false;
        // Skip payload at its structural boundary. Require a static buffer
        // size or element count so this loader never follows an invocation.
        uint64_t size=0;
        uint8_t count=0;
        return opcode==0x12?body.Byte(count):body.Integer(size);
    }
    uint64_t unused=0;
    return aml.Integer(unused);
}

inline bool S5Package(AmlReader& aml,uint8_t& typeA,uint8_t& typeB)
{
    uint8_t op=0,count=0;
    AmlReader body(nullptr,0);
    uint64_t a=0,b=0;
    if (!aml.Byte(op) || op!=0x12 || !aml.Package(body) || !body.Byte(count) || count<2 ||
        !body.Integer(a) || !body.Integer(b) || a>7 || b>7) return false;

    // Historical firmware emits reserved trailing integer slots. Trailing
    // uninitialized elements are permitted by AML; surplus bytes are not.
    for (unsigned i=2;body.Remaining();++i) {
        uint64_t unused=0;
        if (i>=count || !body.Integer(unused)) return false;
    }
    typeA=static_cast<uint8_t>(a);
    typeB=static_cast<uint8_t>(b);
    return true;
}

inline bool FieldList(AmlReader& field,AmlPath scope)
{
    while (field.Remaining()) {
        uint32_t width=0;
        switch (field.Peek()) {
        case 0: // ReservedField
            if (!field.Skip(1) || !field.PackageLength(width)) return false;
            break;
        case 1: // AccessField
            if (!field.Skip(3)) return false;
            break;
        case 2: { // ConnectField
            if (!field.Skip(1)) return false;
            AmlPath resource{};
            if (!(field.Peek()==0x11?DataObject(field):NamePath(field,scope,resource))) return false;
            break;
        }
        case 3: // ExtendedAccessField
            if (!field.Skip(4)) return false;
            break;
        default: {
            bool s5=false;
            if (!Segment(field,s5) || (!scope.minDepth && s5) || !field.PackageLength(width)) return false;
            break;
        }
        }
    }
    return true;
}

struct S5State {
    bool found=false;
    uint8_t typeA=0,typeB=0;
};

inline bool NamespaceTerms(AmlReader aml,AmlPath scope,unsigned level,S5State& s5)
{
    if (level>24) return false;
    uint8_t op=0;
    while (aml.Byte(op)) {
        AmlPath name{},source{};
        AmlReader body(nullptr,0);
        uint64_t value=0;
        switch (op) {
        case 0x06: // Alias
            if (!NamePath(aml,scope,source) || !NamePath(aml,scope,name) || name.rootS5) return false;
            break;
        case 0x08: // Name
            if (!NamePath(aml,scope,name)) return false;
            if (name.rootS5) {
                if (name.minDepth!=name.maxDepth || s5.found ||
                    !S5Package(aml,s5.typeA,s5.typeB)) return false;
                s5.found=true;
            } else if (!DataObject(aml)) return false;
            break;
        case 0x10: // Scope
            if (!aml.Package(body) || !NamePath(body,scope,name,true) || name.rootS5 ||
                !NamespaceTerms(body,name,level+1,s5)) return false;
            break;
        case 0x14: // Method. Its bytecode is never loaded as a TermList.
            if (!aml.Package(body) || !NamePath(body,scope,name) || name.rootS5 || !body.Skip(1)) return false;
            break;
        case 0x15: // External
            if (!NamePath(aml,scope,name) || !aml.Skip(2)) return false;
            break;
        case 0x5b: { // Extended named objects
            uint8_t ext=0;
            if (!aml.Byte(ext)) return false;
            switch (ext) {
            case 0x01: // Mutex
            case 0x02: // Event
                if (!NamePath(aml,scope,name) || name.rootS5 || (ext==1 && !aml.Skip(1))) return false;
                break;
            case 0x80: // OperationRegion with literal base and length
                if (!NamePath(aml,scope,name) || name.rootS5 || !aml.Skip(1) ||
                    !aml.Integer(value) || !aml.Integer(value)) return false;
                break;
            case 0x81: // Field
            case 0x86: // IndexField
            case 0x87: // BankField
                if (!aml.Package(body) || !NamePath(body,scope,source) ||
                    (ext!=0x81 && !NamePath(body,scope,source)) ||
                    (ext==0x87 && !body.Integer(value)) ||
                    !body.Skip(1) || !FieldList(body,scope)) return false;
                break;
            case 0x82: // Device
            case 0x83: // Processor
            case 0x84: // PowerResource
            case 0x85: // ThermalZone
                if (!aml.Package(body) || !NamePath(body,scope,name) || name.rootS5 ||
                    !body.Skip(ext==0x83?6:ext==0x84?3:0) ||
                    !NamespaceTerms(body,name,level+1,s5)) return false;
                break;
            default: return false;
            }
            break;
        }
        case 0xa3: // Noop
            break;
        default:
            // Unknown load-time terms can create or change namespace objects.
            return false;
        }
    }
    return true;
}

} // namespace acpi_s5

// Resolve only a literal root _S5 package. The caller first validates the SDT
// checksum and supplies only its AML body. Executable definitions, unsupported
// load-time syntax, malformed namespace terms and duplicate definitions fail closed.
inline AcpiS5Result AcpiReadStaticS5(const uint8_t* aml,uint32_t size,uint8_t& typeA,uint8_t& typeB)
{
    if (!aml || size>1024*1024) return AcpiS5Result::Unsupported;
    acpi_s5::S5State s5{};
    if (!acpi_s5::NamespaceTerms({aml,size},{},0,s5)) return AcpiS5Result::Unsupported;
    if (!s5.found) return AcpiS5Result::Absent;
    typeA=s5.typeA;
    typeB=s5.typeB;
    return AcpiS5Result::Found;
}

} // namespace brook
