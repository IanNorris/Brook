#include "boot_variable_inventory.h"
#include "Protocol/LoadedImage.h"
#include "Protocol/SimpleFileSystem.h"
#include "Guid/GlobalVariable.h"

namespace brook { namespace bootloader {
namespace {
constexpr UINTN Capacity=128*1024,MaxValue=4096,MaxVariables=4096;
char report[Capacity];UINTN used=0;
CHAR16 variableName[512];UINT8 variableValue[MaxValue];
bool Append(char c) {if(used>=Capacity)return false;report[used++]=c;return true;}
bool Literal(const char* s) {while(*s)if(!Append(*s++))return false;return true;}
bool EqualGuid(const EFI_GUID& a,const EFI_GUID& b) {
    const auto* x=reinterpret_cast<const UINT8*>(&a);const auto* y=reinterpret_cast<const UINT8*>(&b);
    for(UINTN i=0;i<sizeof(EFI_GUID);++i)if(x[i]!=y[i])return false;
    return true;
}
bool Equal(const CHAR16* a,const char16_t* b) {
    while(*a && *a==*b){++a;++b;}return *a==0 && *b==0;
}
bool Selected(const CHAR16* name) {
    if(Equal(name,u"BootOrder")||Equal(name,u"BootCurrent")||Equal(name,u"BootNext"))return true;
    if(name[0]!='B'||name[1]!='o'||name[2]!='o'||name[3]!='t')return false;
    for(unsigned i=4;i<8;++i)if(!((name[i]>='0'&&name[i]<='9')||(name[i]>='A'&&name[i]<='F')))return false;
    return name[8]==0;
}
bool Collect(EFI_RUNTIME_SERVICES* rt) {
    if(!rt || !rt->GetNextVariableName || !rt->GetVariable)return false;
    used=0;if(!Literal("BRKBOOTV1\n"))return false;
    auto* name=variableName;name[0]=0;EFI_GUID vendor{};EFI_GUID global=EFI_GLOBAL_VARIABLE;
    bool finished=false;
    for(UINTN count=0;count<MaxVariables;++count) {
        UINTN nameSize=sizeof(variableName);
        EFI_STATUS status=rt->GetNextVariableName(&nameSize,name,&vendor);
        if(status==EFI_NOT_FOUND){finished=true;break;}
        if(EFI_ERROR(status) || nameSize<sizeof(CHAR16) || nameSize>sizeof(variableName) ||
           (nameSize%sizeof(CHAR16)) || name[nameSize/sizeof(CHAR16)-1]!=0)return false;
        if(!EqualGuid(vendor,global)||!Selected(name))continue;
        UINTN size=sizeof(variableValue);UINT32 attributes=0;
        status=rt->GetVariable(name,&vendor,&attributes,&size,variableValue);
        if(EFI_ERROR(status) || size>sizeof(variableValue))return false;
        for(UINTN i=0;name[i];++i)if(name[i]>127 || !Append(char(name[i])))return false;
        if(!Append('='))return false;
        constexpr char digits[]="0123456789abcdef";
        for(UINTN i=0;i<size;++i)if(!Append(digits[variableValue[i]>>4])||!Append(digits[variableValue[i]&15]))return false;
        if(!Append('\n'))return false;
    }
    return finished && Literal("END\n");
}
}
void SaveBootVariableInventory(EFI_HANDLE image,EFI_SYSTEM_TABLE* system) {
    if(!system || !system->BootServices || !Collect(system->RuntimeServices))return;
    auto* bs=system->BootServices;EFI_GUID loadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fsGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;EFI_LOADED_IMAGE_PROTOCOL* loaded=nullptr;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs=nullptr;EFI_FILE_PROTOCOL *root=nullptr,*directory=nullptr,*file=nullptr;
    if(EFI_ERROR(bs->HandleProtocol(image,&loadedGuid,reinterpret_cast<void**>(&loaded))) || !loaded ||
       EFI_ERROR(bs->HandleProtocol(loaded->DeviceHandle,&fsGuid,reinterpret_cast<void**>(&fs))) || !fs ||
       EFI_ERROR(fs->OpenVolume(fs,&root)) || !root)return;
    EFI_STATUS status=root->Open(root,&directory,(CHAR16*)u"RECOVERY",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0);
    if(!EFI_ERROR(status) && directory) {
        status=directory->Open(directory,&file,(CHAR16*)u"BOOTVARS.TXT",EFI_FILE_MODE_READ,0);
        if(!EFI_ERROR(status) && file) {file->Close(file);file=nullptr;}
        else if(status==EFI_NOT_FOUND) {
            status=directory->Open(directory,&file,(CHAR16*)u"BOOTVARS.TXT",
                                   EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
            if(!EFI_ERROR(status) && file) {
                UINTN size=used;
                if(!EFI_ERROR(file->Write(file,&size,report)) && size==used) (void)file->Flush(file);
                file->Close(file);
            }
        }
        directory->Close(directory);
    }
    root->Close(root);
}
}}
