// EFI side of loader_breadcrumb.h: fixed-size records on the loader's own volume.
#include "loader_breadcrumb_efi.h"
#include "loader_breadcrumb.h"
#include "Protocol/LoadedImage.h"
#include "Protocol/SimpleFileSystem.h"

namespace brook {
namespace bootloader {
namespace {
EFI_HANDLE g_image = nullptr;
EFI_BOOT_SERVICES* g_bs = nullptr;
bool g_enabled = false;
breadcrumb::State g_state;
char g_halt[96];

EFI_FILE_PROTOCOL* OpenRoot() {
    EFI_GUID loadedGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID, fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL* loaded = nullptr;
    if (EFI_ERROR(g_bs->HandleProtocol(g_image, &loadedGuid, reinterpret_cast<void**>(&loaded))) || !loaded) return nullptr;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = nullptr;
    if (EFI_ERROR(g_bs->HandleProtocol(loaded->DeviceHandle, &fsGuid, reinterpret_cast<void**>(&fs))) || !fs) return nullptr;
    EFI_FILE_PROTOCOL* root = nullptr;
    if (EFI_ERROR(fs->OpenVolume(fs, &root))) return nullptr;
    return root;
}
// Overwrite (or create) `name` in the volume root with one record at offset 0.
void WriteRecord(const char16_t* name, const char* rec) {
    EFI_FILE_PROTOCOL* root = OpenRoot();
    if (!root) return;
    EFI_FILE_PROTOCOL* f = nullptr;
    if (!EFI_ERROR(root->Open(root, &f, (CHAR16*)name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0)) && f) {
        UINTN size = breadcrumb::kRecordBytes;
        (void)f->SetPosition(f, 0);
        (void)f->Write(f, &size, (void*)rec);
        (void)f->Flush(f);
        f->Close(f);
    }
    root->Close(root);
}
uint32_t ReadEntries() {
    EFI_FILE_PROTOCOL* root = OpenRoot();
    if (!root) return 0;
    EFI_FILE_PROTOCOL* f = nullptr;
    uint32_t n = 0;
    if (!EFI_ERROR(root->Open(root, &f, (CHAR16*)u"LOADER.TXT", EFI_FILE_MODE_READ, 0)) && f) {
        char buf[breadcrumb::kRecordBytes];
        UINTN size = sizeof(buf);
        if (!EFI_ERROR(f->Read(f, &size, buf))) n = breadcrumb::ParseEntries(buf, uint32_t(size));
        f->Close(f);
    }
    root->Close(root);
    return n;
}
void Commit() {
    char rec[breadcrumb::kRecordBytes];
    breadcrumb::Format(g_state, rec);
    WriteRecord(u"LOADER.TXT", rec);
}
}  // namespace

void BreadcrumbEntry(EFI_HANDLE image, EFI_BOOT_SERVICES* bs) {
    g_image = image; g_bs = bs; g_enabled = image && bs;
    if (!g_enabled) return;
    g_state = breadcrumb::State{};
    g_state.entries = ReadEntries() + 1;
    g_state.stage = "entry";
    Commit();
}
void BreadcrumbGop(bool gop, uint32_t w, uint32_t h, uint32_t modes, bool setmode_tried, bool setmode_failed, uint64_t status) {
    if (!g_enabled) return;
    g_state.stage = "gop"; g_state.gop = gop; g_state.gop_w = w; g_state.gop_h = h; g_state.gop_modes = modes;
    g_state.setmode_tried = setmode_tried; g_state.setmode_failed = setmode_failed; g_state.setmode_status = status;
    Commit();
}
void BreadcrumbKernel() {
    if (!g_enabled) return;
    g_state.stage = "kernel";
    Commit();
    g_enabled = false;   // nothing after this point: ExitBootServices follows
}
void BreadcrumbHalt(uint64_t status, const char16_t* message) {
    if (!g_enabled) return;
    breadcrumb::Ascii(message, g_halt, sizeof(g_halt));
    g_state.stage = "halt"; g_state.halt = g_halt; g_state.halt_status = status;
    char rec[breadcrumb::kRecordBytes];
    breadcrumb::Format(g_state, rec);
    WriteRecord(u"HALT.TXT", rec);
    WriteRecord(u"LOADER.TXT", rec);
    g_enabled = false;
}
}  // namespace bootloader
}  // namespace brook
