#pragma once
#include <stdint.h>

namespace brook {
struct AddressSpaceState;
struct FileObject;
struct MappingBacking;
// Embedded in an authoritative range node. Its existing backing reference pins
// the inode. The file index holds only a weak AS pointer, never an AS reference.
struct FileMappingRegistration {
    AddressSpaceState* space = nullptr;
    FileObject* file = nullptr;
    FileMappingRegistration* next = nullptr;
    uint64_t spaceIdentity = 0;
    bool retiring = false;
};
void FileMappingRegister(FileMappingRegistration* registration,AddressSpaceState* space,MappingBacking* backing);
// Retired nodes remain discoverable until their transaction's TLB ACK/Finish.
void FileMappingRetire(FileMappingRegistration* registration);
void FileMappingUnregister(FileMappingRegistration* registration);
// Caller retains the file object and holds its gate while enumerating. Each
// result retains its AS; increasing identity cursors deduplicate split ranges.
AddressSpaceState* FileMappingNextSpace(FileObject* file,uint64_t afterIdentity);
uint64_t FileMappingRegisteredRanges(FileObject* file);
// A truncator must await these while holding the file gate before enumerating
// active spaces. Ref-zero cleanup and ACK completion can unlink without it.
uint64_t FileMappingRetiringRanges(FileObject* file);

// Snapshot a space's files, retain descriptors, then lock files by stable order
// before reacquiring AS mutation and validating its generation. No sleeping
// file gate is ever acquired under AS mutation. OOM leaves metadata unchanged.
class FileMappingGateSet {
public:
    FileMappingGateSet() = default;
    ~FileMappingGateSet();
    FileMappingGateSet(const FileMappingGateSet&) = delete;
    FileMappingGateSet& operator=(const FileMappingGateSet&) = delete;
    bool Lock(AddressSpaceState* space,MappingBacking* incoming,MappingBacking* onlyBacking);
    void Unlock();
    void Finish();
private:
    struct Pin { MappingBacking* backing;FileObject* file; };
    void ReleasePins();
    bool Add(MappingBacking* backing);
    void SortPins();
    Pin local_[8]{};
    Pin* pins_=local_;
    uint64_t capacity_=8,count_=0;
    uint64_t* index_=nullptr;
    uint64_t indexCapacity_=0;
    AddressSpaceState* space_=nullptr;
};
}
