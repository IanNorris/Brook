#pragma once

// display.h — Abstract display device interface
//
// The kernel starts with the UEFI GOP framebuffer. A PCI display driver
// (e.g. bochs_display) can register itself to take over, enabling mode
// switching and proper hardware control.
//
// The compositor and TTY query the active display for the framebuffer
// address and dimensions.

#include <stdint.h>

namespace brook {

struct DisplayMode {
    uint32_t width;
    uint32_t height;
    uint32_t stride;        // bytes per row
    uint32_t bpp;           // bits per pixel (always 32 for now)
};

struct DisplayOps {
    const char* name;

    // Set display mode (resolution). Returns true on success.
    bool (*SetMode)(uint32_t width, uint32_t height);

    // Get current mode info.
    void (*GetMode)(DisplayMode* mode);

    // Get the virtual address of the framebuffer (mapped into kernel space).
    volatile uint32_t* (*GetFramebuffer)();

    // Get the physical base address of the framebuffer.
    uint64_t (*GetFramebufferPhys)();

    // Flush/sync a dirty scanline span [minY, maxY) from the framebuffer to the
    // display. No-op for direct-mapped linear FBs (GOP, bochs); transfer-based
    // devices (virtio-gpu) use the rect to bound the host transfer + flush.
    void (*Flush)(uint32_t minY, uint32_t maxY);

    // Emergency full-screen present from the panic handler. Called once, after
    // the panic screen has been rendered into the framebuffer backing, with all
    // other CPUs halted (IF=0). Must be best-effort and self-contained: it may
    // NOT take any lock that a now-halted CPU could be holding. Direct-mapped
    // linear FBs (GOP, bochs) are scanned out as-is, so this is nullptr/no-op
    // for them. virtio-gpu reverts the scanout to the 2D framebuffer resource
    // (undoing GPU-composite's render-target binding) and transfers + flushes
    // the whole framebuffer, since the normal compositor present path is halted.
    // May be nullptr.
    void (*PanicPresent)();
};

// Register a display driver. Replaces the current active display.
// Called by display driver modules during init.
extern "C" void DisplayRegister(const DisplayOps* ops);

// Serialize hardware changes that also replace the TTY/compositor backing.
// A managed linear driver can take over the initial GOP and then keeps the
// display until shutdown. It may retire backing once its readers have drained.
extern "C" bool DisplayBeginFramebufferChange(bool managed, const DisplayOps* expected);
extern "C" void DisplayEndFramebufferChange(const DisplayOps* claim);
bool DisplayPublishManagedFramebuffer(uint64_t physical, volatile uint32_t* mmio,
                                      uint32_t* backbuffer, uint32_t width,
                                      uint32_t height, uint32_t stridePixels);
class DisplayFramebufferChange {
public:
    explicit DisplayFramebufferChange(bool managed, const DisplayOps* expected = nullptr)
        : admitted_(DisplayBeginFramebufferChange(managed, expected)) {}
    ~DisplayFramebufferChange() { if (admitted_) DisplayEndFramebufferChange(claim_); }
    explicit operator bool() const { return admitted_; }
    void Claim(const DisplayOps* owner) { claim_ = owner; }
    DisplayFramebufferChange(const DisplayFramebufferChange&) = delete;
    DisplayFramebufferChange& operator=(const DisplayFramebufferChange&) = delete;

private:
    bool admitted_;
    const DisplayOps* claim_ = nullptr;
};

struct CursorImage;

// Optional cursor provider, kept separate from the DisplayOps module ABI.
// Prepare returns whether the provider owns this frame's cursor. It may stage
// a new image, but a newly visible cursor appears only after Commit.
struct DisplayCursorOps {
    const DisplayOps* display;
    bool (*Prepare)(const CursorImage* image, int32_t x, int32_t y);
    void (*Commit)();
};
extern "C" void DisplayRegisterCursor(const DisplayCursorOps* ops);
bool DisplayPrepareCursor(const CursorImage* image, int32_t x, int32_t y);
void DisplayCommitCursor();

struct DisplayModePollOps {
    const DisplayOps* display;
    void (*Poll)();
};
extern "C" void DisplayRegisterModePoll(const DisplayModePollOps* ops);
void DisplayPollModeHints();

// Get the active display. Returns the GOP fallback if no driver registered.
const DisplayOps* DisplayGetActive();
bool DisplayUsesFirmwareFramebuffer();

// Set display mode via the active driver.
bool DisplaySetMode(uint32_t width, uint32_t height);

// Query current display info.
void DisplayGetMode(DisplayMode* mode);

// Get framebuffer pointer from active display.
volatile uint32_t* DisplayGetFramebuffer();

// Get physical framebuffer address from active display.
uint64_t DisplayGetFramebufferPhys();

// Flush a dirty scanline span [minY, maxY) to the active display. No-op for
// linear FBs; virtio-gpu uses it to transfer + flush the damaged region.
void DisplayFlush(uint32_t minY, uint32_t maxY);

// Emergency full-screen present from the panic handler (best-effort). No-op if
// the active display has no PanicPresent op (direct-mapped linear FBs).
void DisplayPanicPresent();

// 3D-acceleration status. A display driver that has proven a live host 3D
// (virgl/Venus) command path — e.g. the virtio-gpu driver's GPU-clear
// self-test passing — calls DisplaySet3DActive(true). The taskbar reads
// DisplayIs3DActive() to surface an at-a-glance "GPU 3D live" indicator.
// extern "C" so loadable display modules can import the setter.
extern "C" void DisplaySet3DActive(bool active);
bool DisplayIs3DActive();

} // namespace brook
