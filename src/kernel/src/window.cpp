#include "boot_timing.h"
#include "boot_timing_runtime.h"
#include "chrome_material.h"
#include "fw_cfg.h"
#include "audio.h"
// Brook window manager — z-ordered windows with chrome.
//
// Each process gets a Window that wraps its VFB with a title bar, border,
// and close/maximise buttons. The compositor calls WmRenderChrome() after
// blitting client areas to draw the window decorations.

#include "window.h"
#include "../../shared/brook_pointer.h"
#include "mouse.h"
#include "wm_chrome_key.h"
#include "desktop_telemetry.h"
#include "taskbar_volume.h"
#include "volume_feedback.h"
#include "debug_hardware.h"
#include "device.h"
#include "virtio_blk.h"
#include "storage_io_stats.h"
#include "net.h"
#include "debug_presence.h"
#include "smp.h"
#include "build_info.h"
#include "kvmclock.h"
#include "shutdown.h"
#include "launcher_command.h"
#include "launch_feedback.h"
#include "launcher_power.h"
#include "shell.h"
#include "process.h"
#include "scheduler.h"
#include "wayland_provider.h"
#include "apic.h"
#include "spinlock.h"
#include "compositor.h"
#include "font_atlas.h"
#include "chrome_raster.h"
#include "chrome_alpha.h"
#include "chrome_layout.h"
#include "chrome_frame.h"
#include "chrome_shell.h"
#include "chrome_surface.h"
#include "desktop_shell_surfaces.h"
#include "input.h"
#include "mouse.h"
#include "serial.h"
#include "panic.h"
#include "rtc.h"
#include "terminal.h"
#include "display.h"
#include "vfs.h"
#include "vfs_path.h"
#include "thermal_runtime.h"
#include "memory/virtual_memory.h"
#include "memory/user_mapping.h"
#include "memory/physical_memory.h"
#include "memory/address.h"
#include "memory/heap.h"
#include "mem_tag.h"
#include "string.h"

namespace brook {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// Serializes VFB source publication, source pinning, resize and destruction.
// Rendering continues to use the compositor epoch to retire kernel readers.
static KMutex g_vfbLock{};
struct VfbGuard {
    VfbGuard() { KMutexLock(&g_vfbLock); }
    ~VfbGuard() { KMutexUnlock(&g_vfbLock); }
};
static uint64_t NextPublication(Window& window) {
    if (window.publicationGeneration!=~uint64_t{0}) ++window.publicationGeneration;
    return window.publicationGeneration;
}
static const DesktopMetrics* g_desktopMetrics = &DesktopMetric100;
static uint32_t g_requestedScale = 100;
const DesktopMetrics& WmDesktopMetrics() { return *__atomic_load_n(&g_desktopMetrics,__ATOMIC_ACQUIRE); }
uint32_t WmMetric(uint32_t base) { return static_cast<uint32_t>(WmDesktopMetrics().pixels(static_cast<int>(base))); }
uint32_t WmGetRequestedScale() { return __atomic_load_n(&g_requestedScale,__ATOMIC_ACQUIRE); }
uint32_t WmGetAppliedScale() { return WmDesktopMetrics().percent; }
const FontAtlas& WmDesktopFont() {
    uint32_t scale=WmGetAppliedScale();
    return scale==200 ? g_desktopFont200 : scale==150 ? g_desktopFont150 : g_desktopFont100;
}
bool WmSetScale(uint32_t percent) {
    if (!DesktopMetricsFor(percent)) return false;
    __atomic_store_n(&g_requestedScale,percent,__ATOMIC_RELEASE);
    CompositorWake();return true;
}
static bool ResizeWindowLocked(int idx,uint16_t newClientW,uint16_t newClientH);
static void FocusWindowLocked(int idx);

// Clearwater (refactor stages 2.2 and 2.3): the saved selection (theme,
// mode, hue). Writers hold g_vfbLock and bump the chrome epoch.
static uint32_t g_themeSelection = BROOK_THEME_SELECTION_DEFAULT;
// Protected by g_vfbLock. Never reuse an epoch after counter exhaustion.
static uint64_t g_chromeSettingsEpoch=1;
// Writers hold g_vfbLock; lock-free readers use this as a sequence counter.
static uint64_t g_backdropStyleSequence=2;
uint64_t WmBackdropStyleVersion(){return __atomic_load_n(&g_backdropStyleSequence,__ATOMIC_ACQUIRE);}
static void BeginBackdropStyleChange(){
    const uint64_t value=WmBackdropStyleVersion();
    if(value && value<~uint64_t{0}-1)
        __atomic_add_fetch(&g_backdropStyleSequence,1,__ATOMIC_ACQ_REL);
    else __atomic_store_n(&g_backdropStyleSequence,0,__ATOMIC_RELEASE);
}
static void EndBackdropStyleChange(){
    if(WmBackdropStyleVersion())__atomic_add_fetch(&g_backdropStyleSequence,1,__ATOMIC_RELEASE);
}
static bool g_themeSettingsLoaded = false;
static uint32_t g_appearance = BROOK_APPEARANCE_DEFAULT;
static uint32_t g_taskbarMetrics = BROOK_TASKBAR_DEFAULT;
uint32_t WmGetTaskbarMetrics(){return __atomic_load_n(&g_taskbarMetrics,__ATOMIC_ACQUIRE);}
bool WmSetTaskbarMetrics(uint32_t value){
    if(!brook_taskbar_metrics_valid(value))return false;
    __atomic_store_n(&g_taskbarMetrics,value,__ATOMIC_RELEASE);
    CompositorMarkDirty();CompositorWake();return true;
}
static bool g_backendBlur=false;
uint32_t WmGetThemeSelection(){return __atomic_load_n(&g_themeSelection,__ATOMIC_ACQUIRE);}
bool WmBackendBlur(){return __atomic_load_n(&g_backendBlur,__ATOMIC_ACQUIRE);}
// The theme in use: the selection resolved against the backend's backdrop
// blur (Glass, or its Opaque stand-in).
uint32_t WmResolvedTheme(){
    const uint32_t s=WmGetThemeSelection();
    return brook_theme_resolve(brook_selection_theme(s),brook_selection_mode(s),WmBackendBlur());
}
// The appearance word keeps opacity and blur; its dark and performance bits
// are views of the selection, for older Settings and GTK apps: dark = the app
// content in use is dark (Mixed is light), performance = an Opaque theme is
// selected.
static uint32_t AppearanceViewBits(uint32_t selection,bool blur){
    const unsigned theme=brook_selection_theme(selection),mode=brook_selection_mode(selection);
    const unsigned resolved=brook_theme_resolve(theme,mode,blur);
    return (brook_theme_content_dark(resolved,mode)?BROOK_APPEARANCE_DARK:0u)|
           (theme!=BROOK_THEME_GLASS?BROOK_APPEARANCE_PERFORMANCE:0u);
}
uint32_t WmGetAppearance(){
    const uint32_t stored=__atomic_load_n(&g_appearance,__ATOMIC_ACQUIRE)&~uint32_t(BROOK_APPEARANCE_DARK|BROOK_APPEARANCE_PERFORMANCE);
    return stored|AppearanceViewBits(WmGetThemeSelection(),WmBackendBlur());
}
static void PublishThemeSelectionLocked(uint32_t selection){
    if(g_chromeSettingsEpoch)++g_chromeSettingsEpoch;
    __atomic_store_n(&g_themeSelection,selection,__ATOMIC_RELEASE);
}
bool WmSetAppearance(uint32_t value){
    if(!brook_appearance_valid(value))return false;
    {
        VfbGuard guard;
        if(g_chromeSettingsEpoch)++g_chromeSettingsEpoch;
        // Dark and performance bits that disagree with the selection are an
        // older Settings speaking: read them as aliases, as settings.ini's.
        const uint32_t selection=WmGetThemeSelection();
        const uint32_t view=AppearanceViewBits(selection,WmBackendBlur());
        const uint32_t asked=value&(BROOK_APPEARANCE_DARK|BROOK_APPEARANCE_PERFORMANCE);
        if(asked!=view){
            const bool dark=value&BROOK_APPEARANCE_DARK;
            unsigned theme=BROOK_THEME_GLASS,mode=brook_selection_mode(selection);
            if(value&BROOK_APPEARANCE_PERFORMANCE)theme=dark?BROOK_THEME_OPAQUE_DARK:BROOK_THEME_OPAQUE_LIGHT;
            if(theme!=BROOK_THEME_GLASS || brook_theme_content_dark(brook_theme_resolve(theme,mode,WmBackendBlur()),mode)!=dark)
                mode=dark?BROOK_MODE_DARK:BROOK_MODE_LIGHT;
            PublishThemeSelectionLocked(BROOK_THEME_SELECTION(theme,mode,brook_selection_hue(selection)));
        }
        __atomic_store_n(&g_appearance,value&~uint32_t(BROOK_APPEARANCE_DARK|BROOK_APPEARANCE_PERFORMANCE),__ATOMIC_RELEASE);
    }
    CompositorMarkDirty();CompositorWake();return true;
}
bool WmSetThemeSelection(uint32_t selection){
    if(!brook_selection_valid(selection))return false;
    {VfbGuard guard;PublishThemeSelectionLocked(selection);}
    CompositorMarkDirty();CompositorWake();return true;
}
// Debug "gpu glass on|off": Glass, or the Opaque theme for the mode.
void WmSetPerformanceMode(bool enabled){
    const uint32_t s=WmGetThemeSelection();
    const unsigned mode=brook_selection_mode(s);
    const unsigned theme=!enabled?BROOK_THEME_GLASS:mode==BROOK_MODE_LIGHT?BROOK_THEME_OPAQUE_LIGHT:BROOK_THEME_OPAQUE_DARK;
    WmSetThemeSelection(BROOK_THEME_SELECTION(theme,mode,brook_selection_hue(s)));
}
// Stage 1.5: the backend's backdrop-blur capability, set by the compositor at
// each frame boundary. A change re-resolves the theme (Glass or its Opaque
// stand-in): every chrome key changes and everything is repainted once.
void WmSetBackendBlur(bool blur){
    if(WmBackendBlur()==blur)return;
    {
        VfbGuard guard;
        if(g_chromeSettingsEpoch)++g_chromeSettingsEpoch;
        __atomic_store_n(&g_backendBlur,blur,__ATOMIC_RELEASE);
    }
    SerialPrintf("WM: backend backdrop blur %s\n",blur?"available":"unavailable");
    CompositorMarkDirty();
}
// The appearance as resolved against the backend: the opaque material
// (BROOK_APPEARANCE_PERFORMANCE) unless the theme in use is Glass. The saved
// selection is never rewritten.
uint32_t WmResolvedAppearance(){
    const uint32_t value=WmGetAppearance();
    return WmResolvedTheme()==BROOK_THEME_GLASS?value&~uint32_t(BROOK_APPEARANCE_PERFORMANCE):
                                                value|BROOK_APPEARANCE_PERFORMANCE;
}
// Every hue's token set and painter palettes, derived once (about thirty
// tokens of floating-point maths per hue) and immutable afterwards, so the
// painters read them lock-free on any thread.
static BrookHueTokens g_hueTokens[BROOK_HUE_COUNT];
static BrookThemePalette g_themePalettes[BROOK_HUE_COUNT][2][2]; // [hue][dark chrome][glass]
static bool g_themeTablesReady=false;
static SpinLock g_themeTablesLock;
static void EnsureThemeTables(){
    if(__atomic_load_n(&g_themeTablesReady,__ATOMIC_ACQUIRE))return;
    SpinLockAcquire(&g_themeTablesLock);
    if(!g_themeTablesReady){
        for(unsigned hue=0;hue<BROOK_HUE_COUNT;++hue){
            brook_hue_derive(hue,&g_hueTokens[hue]);
            for(unsigned dark=0;dark<2;++dark)for(unsigned glass=0;glass<2;++glass)
                brook_theme_build_palette(&g_hueTokens[hue],glass?BROOK_THEME_GLASS:
                                          dark?BROOK_THEME_OPAQUE_DARK:BROOK_THEME_OPAQUE_LIGHT,
                                          int(dark),&g_themePalettes[hue][dark][glass]);
        }
        __atomic_store_n(&g_themeTablesReady,true,__ATOMIC_RELEASE);
    }
    SpinLockRelease(&g_themeTablesLock);
}
const BrookHueTokens& WmHueTokens(){
    EnsureThemeTables();
    const uint32_t hue=brook_selection_hue(WmGetThemeSelection());
    return g_hueTokens[hue<BROOK_HUE_COUNT?hue:0];
}
// Changes with the selection and with what it resolves to (chrome keys).
uint32_t WmGetTheme() { return WmGetThemeSelection() | WmResolvedTheme() << 16; }
const WmPalette& WmCurrentPalette() {
    EnsureThemeTables();
    const uint32_t s=WmGetThemeSelection();
    const unsigned resolved=WmResolvedTheme(),hue=brook_selection_hue(s);
    const unsigned dark=brook_theme_chrome_dark(resolved,brook_selection_mode(s))?1:0;
    return g_themePalettes[hue<BROOK_HUE_COUNT?hue:0][dark][resolved==BROOK_THEME_GLASS?1:0];
}
uint32_t WmContentColour(unsigned index){
    const uint32_t s=WmGetThemeSelection();
    const unsigned mode=brook_selection_mode(s);
    const BrookContentTokens& c=WmHueTokens().content[brook_theme_content_dark(WmResolvedTheme(),mode)?1:0];
    switch(index){
    case BROOK_CONTENT_BACKGROUND: return c.background;
    case BROOK_CONTENT_INK: return c.ink;
    case BROOK_CONTENT_DIM: return c.dim;
    case BROOK_CONTENT_LINE: return c.line;
    case BROOK_CONTENT_RAISED: return c.raised;
    case BROOK_CONTENT_SELECTION: return brook_tint_over(c.selection,c.background);
    case BROOK_CONTENT_SUNKEN: return c.sunken;
    case BROOK_CONTENT_ACCENT: return c.accent;
    case BROOK_CONTENT_ACCENT_HOVER: return c.accentHover;
    case BROOK_CONTENT_ON_ACCENT: return c.onAccent;
    default: return 0;
    }
}
// 0xB02 op 1 (older Settings): a legacy palette id chooses its hue.
bool WmSetTheme(uint32_t legacyPalette)
{
    if (legacyPalette >= BROOK_LEGACY_PALETTE_COUNT) return false;
    const uint32_t s=WmGetThemeSelection();
    return WmSetThemeSelection(BROOK_THEME_SELECTION(brook_selection_theme(s),brook_selection_mode(s),
                                                     brook_legacy_palette_hue(legacyPalette)));
}

// Hardware payload name for the desktop build label, read once at activation
// (empty on QEMU and other images without /etc/hardware-payload-id).
static char g_buildPayloadId[64];

// Startup I/O runs in the shell's activation path, never during rendering.
// Accept only a complete bounded file and an exact supported numeric ID.
static void WmRestoreTheme()
{
    if (g_themeSettingsLoaded) return;
    g_themeSettingsLoaded = true;
    if (HardwarePayloadIdRead(g_buildPayloadId, sizeof(g_buildPayloadId)))
        SerialPrintf("WM: build label payload %s\n", g_buildPayloadId);
    else g_buildPayloadId[0] = 0;
    BrookThemeSettings themeSettings=brook_theme_settings_init();
    uint32_t selectedScale = 100;
    uint32_t appearance=WmGetAppearance();
    uint32_t taskbarMetrics=BROOK_TASKBAR_DEFAULT;
    uint32_t pointerSpeed=BROOK_POINTER_SPEED_DEFAULT;
    uint32_t touchpadTap=BROOK_TOUCHPAD_TAP_DEFAULT;
    uint32_t audioControl=AudioOutputControl();
    const bool forceMuted=(audioControl&BROOK_AUDIO_MUTED)!=0;
    VfsPath root{};Vnode* vn=nullptr;
    if(!VfsPathRoot(&root))vn=VfsOpenAt(root,root,"/data/home/.config/brook/settings.ini",0,0,0,nullptr);
    VfsPathRelease(root);
    if (vn)
    {
        char buf[2049]; uint32_t done = 0; uint64_t offset = 0;
        bool failed = false;
        while (done < sizeof(buf))
        {
            int rd = VfsRead(vn, buf + done, sizeof(buf) - done, &offset);
            if (rd < 0) { failed = true; break; }
            if (!rd) break;
            done += static_cast<uint32_t>(rd);
        }
        VfsClose(vn);
        if (!failed && done < sizeof(buf))
        {
            buf[done] = 0;
            bool desktopSection = false;
            bool audioSection = false;
            for (char* line = buf; *line;)
            {
                char* end = line; while (*end && *end != '\n') ++end;
                bool more = *end != 0; *end = 0;
                char* p = line; while (*p == ' ' || *p == '\t') ++p;
                if (*p == '[')
                {
                    audioSection=strncmp(p,"[Audio]",7)==0;
                    if(audioSection){const char* tail=p+7;while(*tail==' '||*tail=='\t'||*tail=='\r')++tail;audioSection=!*tail;}
                    desktopSection = strncmp(p, "[Desktop]", 9) == 0;
                    if (desktopSection) {
                        p += 9;
                        while (*p == ' ' || *p == '\t' || *p == '\r') ++p;
                        desktopSection = !*p;
                    }
                }
                else if(audioSection) audioControl=brook_audio_control_setting(audioControl,p);
                else if (desktopSection && brook_theme_settings_line(&themeSettings, p) &&
                         strncmp(p,"dark=",5)!=0 && strncmp(p,"performance=",12)!=0) {}
                else if (desktopSection && strncmp(p,"scale=",6)==0) {
                    p+=6;uint32_t value=0,digits=0;
                    while (*p>='0' && *p<='9' && digits<4) {value=value*10+(*p++-'0');++digits;}
                    while (*p==' ' || *p=='\t' || *p=='\r') ++p;
                    selectedScale=!*p && digits==3 && DesktopMetrics::Supported(value) ? value : 100;
                }
                else if(desktopSection){
                    appearance=brook_appearance_setting(appearance,p);
                    taskbarMetrics=brook_taskbar_metrics_setting(taskbarMetrics,p);
                    pointerSpeed=brook_pointer_speed_setting(pointerSpeed,p);
                    touchpadTap=brook_touchpad_tap_setting(touchpadTap,p);
                }
                if (!more) break;
                line = end + 1;
            }
        }
    }
    uint32_t selection=brook_theme_settings_selection(&themeSettings);
    char glass[8]{};
    if(FwCfgReadFile("opt/glass",glass,sizeof(glass)-1)) {
        // QEMU's glass switch: Glass, or the Opaque theme for the mode.
        const unsigned mode=brook_selection_mode(selection),hue=brook_selection_hue(selection);
        if(glass[0]=='0'||(glass[0]=='o'&&glass[1]=='f'))
            selection=BROOK_THEME_SELECTION(mode==BROOK_MODE_LIGHT?BROOK_THEME_OPAQUE_LIGHT:BROOK_THEME_OPAQUE_DARK,mode,hue);
        else if(glass[0]=='1'||(glass[0]=='o'&&glass[1]=='n'))
            selection=BROOK_THEME_SELECTION(BROOK_THEME_GLASS,mode,hue);
    }
    WmSetThemeSelection(selection);
    SerialPrintf("WM: theme %s mode %s accent %s\n",brook_theme_key(brook_selection_theme(selection)),
                 brook_mode_key(brook_selection_mode(selection)),brook_hue_key(brook_selection_hue(selection)));
    // Opacity and blur; the dark and performance bits are views of the
    // selection just set (their saved values were read as aliases above).
    WmSetAppearance((appearance&~uint32_t(BROOK_APPEARANCE_DARK|BROOK_APPEARANCE_PERFORMANCE))|
                    (WmGetAppearance()&(BROOK_APPEARANCE_DARK|BROOK_APPEARANCE_PERFORMANCE)));
    WmSetTaskbarMetrics(taskbarMetrics);
    MouseTouchpadSetSpeed(pointerSpeed);
    MouseTouchpadSetTap(touchpadTap!=0);
    WmSetScale(selectedScale);
    AudioSetOutputControl(audioControl|(forceMuted?BROOK_AUDIO_MUTED:0));
}

static Window   g_windows[WM_MAX_WINDOWS] = {};
static bool     g_wmActive = false;
static int      g_focusedIdx = -1;
static uint64_t g_windowIdentity = 0;
static int g_taskbarPageTarget = -1;
static uint32_t g_nextZOrder = 1;  // 0 = backmost, higher = front

// Protected by g_vfbLock; only the published active bit is read lock-free by
// rendering. The lease is bound to the window creation identity, not its slot.
static int g_pointerCaptureIndex=-1;
static uint64_t g_pointerCaptureIdentity=0,g_pointerCaptureSequence=0,g_pointerCaptureDrops=0;
static uint64_t g_pointerReleaseSequence=0,g_pointerCaptureRevision=0;
static uint64_t g_pointerInteractionSerial=0;
// Creation identity of the window whose client area receives pointer input;
// zero over shell surfaces, chrome and desktop. Written by WM routing.
static uint64_t g_pointerHoverIdentity=0;
// The window whose provider was last told the pointer is over it (slot and
// creation identity), so it can be told when the pointer leaves.
static int g_pointerEnteredIdx=-1;
static uint64_t g_pointerEnteredIdentity=0;
// Deliberate keyboard/taskbar activation (Alt+Tab, taskbar button): like
// Windows granting ClipCursor on activation, the activated window may acquire
// once without the pointer over its content. Cancelled by any focus change,
// Super, the launcher/volume popup, minimising, or the pointer moving to a
// different delivery target than it had at activation.
static uint64_t g_pointerActivationIdentity=0,g_pointerActivationHover=0;
static void PointerActivationCancel() { __atomic_store_n(&g_pointerActivationIdentity,0,__ATOMIC_RELEASE); }
static uint32_t g_pointerQuarantinedButtons=0;
// Physical-up recovery must retain ownership of a delayed or queued release.
static uint32_t g_pointerOrphanedReleases=0;
static uint32_t g_pointerCaptureToken=0,g_nextPointerCaptureToken=0,g_pointerCapturePublished=0;
static int32_t g_pointerAnchorX=0,g_pointerAnchorY=0;
static int16_t g_pointerLocalX=0,g_pointerLocalY=0;
static bool g_pointerEscapeHeld=false;

static void PointerCaptureReleaseLocked(bool voluntary=false) {
    if(g_pointerCaptureIndex<0)return;
    // Anything but the provider's own release is a user/WM revoke. The window
    // must not reacquire until it is explicitly clicked again, so a persistent
    // lock or a recreated oneshot cannot grab the pointer back mid-taskbar.
    Window& owner=g_windows[g_pointerCaptureIndex];
    if(!voluntary && owner.proc && owner.creationIdentity==g_pointerCaptureIdentity)
        owner.pointerDisengaged=true;
    g_pointerReleaseSequence=InputLatestSequence();
    __atomic_fetch_or(&g_pointerQuarantinedButtons,static_cast<uint32_t>(MouseGetButtons()),__ATOMIC_ACQ_REL);
    g_pointerCaptureIndex=-1;g_pointerCaptureIdentity=0;g_pointerCaptureToken=0;
    __atomic_store_n(&g_pointerCapturePublished,0u,__ATOMIC_RELEASE);
    __atomic_add_fetch(&g_pointerCaptureRevision,1,__ATOMIC_RELEASE);
    MouseSetPosition(g_pointerAnchorX,g_pointerAnchorY);
    CompositorMarkDirty();CompositorWake();
}
static void PointerCaptureValidateLocked() {
    if(g_pointerCaptureIndex<0)return;
    const auto& w=g_windows[g_pointerCaptureIndex];
    if(!g_wmActive || !w.proc || !w.vfb || !w.visible || w.minimized ||
       !w.focused || g_focusedIdx!=g_pointerCaptureIndex ||
       w.creationIdentity!=g_pointerCaptureIdentity ||
       InputDroppedEvents()!=g_pointerCaptureDrops || WmLauncherVisible() || WmVolumePopupVisible())
        PointerCaptureReleaseLocked();
}
bool WmPointerCaptureActive() {
    return __atomic_load_n(&g_pointerCapturePublished,__ATOMIC_ACQUIRE)!=0;
}
uint64_t WmPointerCaptureRevision() { return __atomic_load_n(&g_pointerCaptureRevision,__ATOMIC_ACQUIRE); }
uint32_t WmPointerSuppressedButtons() { return __atomic_load_n(&g_pointerQuarantinedButtons,__ATOMIC_ACQUIRE); }
void WmPointerCaptureRefresh() {
    VfbGuard guard;PointerCaptureValidateLocked();
    // Recover a dropped Up without forwarding an already-queued (or not yet
    // published) Up to the next client. Fence published packets; retain a
    // release tombstone until Up or a new Down establishes a new button pair.
    const uint32_t recovered=WmPointerSuppressedButtons()&~static_cast<uint32_t>(MouseGetButtons());
    if(recovered) {
        g_pointerReleaseSequence=InputLatestSequence();
        g_pointerOrphanedReleases|=recovered;
        __atomic_fetch_and(&g_pointerQuarantinedButtons,~recovered,__ATOMIC_ACQ_REL);
    }
}
uint64_t WmPointerHoverIdentity() {
    return __atomic_load_n(&g_pointerHoverIdentity,__ATOMIC_ACQUIRE);
}
void WmPointerSetHover(int idx) {
    const uint64_t identity=idx>=0 && idx<static_cast<int>(WM_MAX_WINDOWS) && g_windows[idx].proc
        ?g_windows[idx].creationIdentity:0;
    __atomic_store_n(&g_pointerHoverIdentity,identity,__ATOMIC_RELEASE);
    if(identity!=__atomic_load_n(&g_pointerActivationHover,__ATOMIC_ACQUIRE))PointerActivationCancel();
    // Called from WM input routing only, which also delivers this window's
    // pointer events, so LEFT is ordered after the last of them.
    if(identity!=g_pointerEnteredIdentity && (identity || !MouseGetButtons())) {
        const int left=g_pointerEnteredIdx;
        if(g_pointerEnteredIdentity && left>=0 && left<static_cast<int>(WM_MAX_WINDOWS) &&
           g_windows[left].proc && g_windows[left].creationIdentity==g_pointerEnteredIdentity && g_windows[left].vfb)
            WmPushWmEvent(&g_windows[left],WM_EVT_POINTER_LEFT);
        g_pointerEnteredIdx=identity?idx:-1;g_pointerEnteredIdentity=identity;
    }
}
void WmPointerActivate(int idx) {
    if(idx<0 || idx>=static_cast<int>(WM_MAX_WINDOWS))return;
    VfbGuard guard;
    Window& w=g_windows[idx];
    if(!w.proc || !w.focused || g_focusedIdx!=idx || w.minimized)return;
    if(g_pointerInteractionSerial<0x7fffffffffffffffull)++g_pointerInteractionSerial;
    w.pointerEngageSerial=g_pointerInteractionSerial;w.pointerDisengaged=false;
    __atomic_store_n(&g_pointerActivationHover,__atomic_load_n(&g_pointerHoverIdentity,__ATOMIC_ACQUIRE),__ATOMIC_RELEASE);
    __atomic_store_n(&g_pointerActivationIdentity,w.creationIdentity,__ATOMIC_RELEASE);
}
void WmPointerEngage(int idx) {
    if(idx<0 || idx>=static_cast<int>(WM_MAX_WINDOWS))return;
    VfbGuard guard;
    Window& w=g_windows[idx];
    if(!w.proc)return;
    if(g_pointerInteractionSerial<0x7fffffffffffffffull)++g_pointerInteractionSerial;
    w.pointerEngageSerial=g_pointerInteractionSerial;w.pointerDisengaged=false;
}
bool WmPointerCaptureReleaseByUser() {
    VfbGuard guard;
    PointerActivationCancel(); // Super means the shell, not the activated game.
    if(g_pointerCaptureIndex<0)return false;
    PointerCaptureReleaseLocked();return true;
}
int64_t WmPointerCapture(Process* provider,uint32_t wmId,uint32_t operation,uint32_t token) {
    if(!provider || !wmId || wmId>WM_MAX_WINDOWS)return -22;
    VfbGuard guard;PointerCaptureValidateLocked();
    Window* w=WmFindWindowById(provider,wmId);
    if(!w)return -2;
    // Physical interaction proof for providers: queued WM events and new
    // protocol resources cannot manufacture a fresh post-release click, and
    // clicks on the taskbar, launcher or another window do not count.
    if(operation==3 && !token)return static_cast<int64_t>(w->pointerEngageSerial);
    const bool owned=g_pointerCaptureIndex==static_cast<int>(wmId)-1 &&
                     g_pointerCaptureIdentity==w->creationIdentity;
    if(operation==0)return owned && (!token || token==g_pointerCaptureToken)?g_pointerCaptureToken:0;
    const bool activated=__atomic_load_n(&g_pointerActivationIdentity,__ATOMIC_ACQUIRE)==w->creationIdentity;
    if(operation==4)return token?-22:activated?1:0;
    if(operation==2) {
        if(!owned || !token || token!=g_pointerCaptureToken)return -116;
        PointerCaptureReleaseLocked(true);return 0;
    }
    if(operation!=1 || token)return -22;
    if(owned)return g_pointerCaptureToken;
    if(g_pointerCaptureIndex>=0)return -16;
    if(WmLauncherVisible() || WmVolumePopupVisible())PointerActivationCancel();
    const bool hovered=__atomic_load_n(&g_pointerHoverIdentity,__ATOMIC_ACQUIRE)==w->creationIdentity;
    // An activation waiver never inherits the activating click: the taskbar
    // press/release pair must finish in the shell before the game is granted.
    const bool waived=!hovered && activated && !MouseGetButtons();
    if(!g_wmActive || !w->vfb || !w->visible || w->minimized || !w->focused ||
       g_focusedIdx!=static_cast<int>(wmId)-1 || WmLauncherVisible() || WmVolumePopupVisible() ||
       WmPointerSuppressedButtons() || w->pointerDisengaged || (!hovered && !waived))return -11;
    if(g_nextPointerCaptureToken==0xffffffffu)return -75;
    PointerActivationCancel();
    if(waived) {
        // Anchor inside the game, as Windows clips an activated cursor into
        // the window: release then returns the pointer over its content.
        MouseSetPosition(w->clientX()+w->clientW/2,w->clientY()+w->clientH/2);
        __atomic_store_n(&g_pointerHoverIdentity,w->creationIdentity,__ATOMIC_RELEASE);
    }
    MouseGetPosition(&g_pointerAnchorX,&g_pointerAnchorY);
    g_pointerLocalX=static_cast<int16_t>(g_pointerAnchorX-w->clientX());
    g_pointerLocalY=static_cast<int16_t>(g_pointerAnchorY-w->clientY());
    g_pointerCaptureIndex=static_cast<int>(wmId)-1;
    g_pointerCaptureIdentity=w->creationIdentity;
    g_pointerCaptureSequence=InputLatestSequence();g_pointerCaptureDrops=InputDroppedEvents();
    g_pointerCaptureToken=++g_nextPointerCaptureToken;
    __atomic_store_n(&g_pointerCapturePublished,1u,__ATOMIC_RELEASE);
    __atomic_add_fetch(&g_pointerCaptureRevision,1,__ATOMIC_RELEASE);
    CompositorMarkDirty();CompositorWake();return g_pointerCaptureToken;
}

static bool PushRelativeLocked(Window* w,const InputEvent& event,uint32_t token) {
    int64_t dx=event.deltaX,dy=event.deltaY;
    while(dx || dy) {
        const int16_t x=static_cast<int16_t>(dx>32767?32767:dx<(-32768)?(-32768):dx);
        const int16_t y=static_cast<int16_t>(dy>32767?32767:dy<(-32768)?(-32768):dy);
        if(!WmInputPush(w,event,x,y,token))return false;
        dx-=x;dy-=y;
    }
    return true;
}
bool WmInputPushRelative(Window* window,const InputEvent& event) {
    VfbGuard guard;
    if(!window || !window->proc || g_pointerCaptureIndex>=0)return false;
    return PushRelativeLocked(window,event,0);
}
bool WmRouteCapturedPointer(const InputEvent& event) {
    VfbGuard guard;PointerCaptureValidateLocked();
    const bool pointer=event.type==InputEventType::MouseMove || event.type==InputEventType::MouseRelativeMove ||
        event.type==InputEventType::MouseButtonDown || event.type==InputEventType::MouseButtonUp ||
        event.type==InputEventType::MouseScroll;
    if(pointer && event.sequence<=g_pointerReleaseSequence)return true;
    if((event.type==InputEventType::MouseButtonDown || event.type==InputEventType::MouseButtonUp) && event.scanCode<32) {
        const uint32_t bit=uint32_t(1)<<event.scanCode;
        if(g_pointerOrphanedReleases&bit) {
            g_pointerOrphanedReleases&=~bit;
            if(event.type==InputEventType::MouseButtonUp)return true;
            // A new press after the recovery fence starts an ordinary pair.
        }
        if(WmPointerSuppressedButtons()&bit) {
            if(event.type==InputEventType::MouseButtonUp)
                __atomic_fetch_and(&g_pointerQuarantinedButtons,~bit,__ATOMIC_ACQ_REL);
            return true;
        }
    }
    if(g_pointerEscapeHeld && event.scanCode==1 &&
       (event.type==InputEventType::KeyPress || event.type==InputEventType::KeyRelease)) {
        if(event.type==InputEventType::KeyRelease)g_pointerEscapeHeld=false;
        return true;
    }
    if(g_pointerCaptureIndex<0)return false;
    if(event.type==InputEventType::KeyPress && event.scanCode==1 &&
       (event.modifiers&(INPUT_MOD_CTRL|INPUT_MOD_ALT))==(INPUT_MOD_CTRL|INPUT_MOD_ALT)) {
        g_pointerEscapeHeld=true;PointerCaptureReleaseLocked();return true;
    }
    if(!pointer)return false;
    // Never assign already-queued input to a newly granted lease.
    if(event.sequence<=g_pointerCaptureSequence)return true;
    Window* w=&g_windows[g_pointerCaptureIndex];
    if(event.type==InputEventType::MouseMove)return true; // absolute tablet has no raw motion
    const bool queued=event.type==InputEventType::MouseRelativeMove
        ?PushRelativeLocked(w,event,g_pointerCaptureToken)
        :WmInputPush(w,event,g_pointerLocalX,g_pointerLocalY,g_pointerCaptureToken);
    if(!queued)PointerCaptureReleaseLocked(); // loss must not silently corrupt a locked stream
    return true;
}

// Immutable launcher payloads. Only refs change after publication. The single
// maintenance worker owns building/pending/retired lifetimes; no renderer does
// VFS I/O or frees icon memory. The compositor alone swaps an active snapshot.
struct LauncherSnapshot {
    LauncherItem items[WM_LAUNCHER_MAX_ITEMS];
    uint32_t count;
    uint32_t generation;
    uint64_t builtTick;
    uint32_t refs;
    uint32_t* applicationIcon;
    uint32_t* terminalIcon;
    uint32_t* appsIcon;
};
static LauncherSnapshot g_emptyLauncher = {};
static LauncherSnapshot* g_launcherActive = &g_emptyLauncher;
static LauncherSnapshot* g_launcherPending = nullptr;
static LauncherSnapshot* g_launcherRetired = nullptr;
static SpinLock g_launcherLock;
// Zero means exhausted; it is never reused for a later icon catalog.
static uint64_t g_launcherSnapshotEpoch=1;
static uint32_t g_launcherRequested = 1;
static bool g_launcherWorkerStarted = false;
static bool g_launcherLoaded = false;
static bool g_launcherOpen = false;
static int g_launcherError = 0;
static uint32_t g_launcherScroll = 0;
// Power buttons arm a confirmation; only the separate confirm button acts.
static LauncherPowerGuard g_launcherPower;
static void LauncherStartWorker();

// Requests own all metadata/icon pixels, never launcher snapshot pointers.
struct LauncherRequest {
    char command[128];
    bool desktop, hasIcon;
    uint32_t icon[LAUNCHER_ICON_PX * LAUNCHER_ICON_PX];
};
static SpinLock g_launchLock;
static LaunchFeedbackTable g_launches;
static LaunchHistory g_launchHistory;
static LauncherRequest g_launchRequests[LaunchFeedbackTable::Capacity];
static bool g_launchBusy = false;

bool WmLaunchBusy() { return __atomic_load_n(&g_launchBusy, __ATOMIC_ACQUIRE); }

void WmLaunchFeedbackPoll()
{
    static uint64_t lastPoll = 0;
    const uint64_t now = ApicTickCount();
    if (now - lastPoll < 100) return;
    lastPoll = now;
    // Window/process lookups may take sleeping/scheduler locks. Never hold the
    // feedback spinlock while acquiring them. Completion is versioned by token.
    bool hadEntries = false,needsObservation = false;
    SpinLockAcquire(&g_launchLock);
    for (const auto& e : g_launches.entries) hadEntries |= e.phase != LaunchPhase::Empty;
    for (const auto& r : g_launchHistory.records) needsObservation |= LaunchHistory::NeedsObservation(r,now);
    SpinLockRelease(&g_launchLock);
    if (!hadEntries && !needsObservation) {
        if (__atomic_exchange_n(&g_launchBusy, false, __ATOMIC_ACQ_REL)) CompositorMarkDirty();
        return;
    }
    {
        VfbGuard windows;
        for (const auto& w : g_windows) {
            if (!w.proc || !w.visible || !w.focusable) continue;
            SpinLockAcquire(&g_launchLock);
            g_launchHistory.Observe(w.clientLaunchId,w.clientOrigin==WmClientOrigin::RegisteredUnixPeer,
                w.creationIdentity,w.appId,w.wmId,w.creationIdentity,now);
            uint64_t token=w.clientLaunchId;
            if(w.clientOrigin==WmClientOrigin::RegisteredUnixPeer&&g_launches.Find(token)<0)token=0;
            g_launches.Window(token, w.creationIdentity, w.appId,nullptr,w.wmId,w.creationIdentity,now);
            SpinLockRelease(&g_launchLock);
        }
    }
    for (unsigned i = 0; i < LaunchFeedbackTable::Capacity; ++i) {
        LaunchFeedback entry;
        SpinLockAcquire(&g_launchLock);
        entry = g_launches.entries[i];
        SpinLockRelease(&g_launchLock);
        if (entry.phase == LaunchPhase::Empty) continue;
        // Scheduler lock only, outside the feedback spinlock. Absence of
        // tagged processes ends the spinner as Exited, never as Failed.
        const bool live = entry.phase != LaunchPhase::Waiting || SchedulerLaunchHasLiveProcesses(entry.token);
        SpinLockAcquire(&g_launchLock);
        g_launches.Advance(entry.token, now, live);
        SpinLockRelease(&g_launchLock);
    }
    bool busy = false;
    SpinLockAcquire(&g_launchLock);
    for (const auto& e : g_launches.entries) busy |= e.busy()&&!e.windowObserved;
    SpinLockRelease(&g_launchLock);
    const bool busyChanged=__atomic_exchange_n(&g_launchBusy,busy,__ATOMIC_ACQ_REL)!=busy;
    if(hadEntries||busyChanged)CompositorMarkDirty(); // Also erase the final expired notice.
}

void WmTaskbarPendingClick(int target)
{
    const bool navigation = target <= -100;
    if (navigation) target += 90;
    if (target > -10 || target <= -10 - static_cast<int>(LaunchFeedbackTable::Capacity)) return;
    const unsigned slot = static_cast<unsigned>(-10 - target);
    if (!navigation) {
        SpinLockAcquire(&g_launchLock);
        g_launches.Dismiss(slot); // Active launch is not cancellation.
        SpinLockRelease(&g_launchLock);
    }
    g_taskbarPageTarget = target;
    CompositorMarkDirty();
}

class LauncherReadGuard {
    LauncherSnapshot* snapshot;
public:
    LauncherReadGuard() {
        SpinLockAcquire(&g_launcherLock);
        snapshot = g_launcherActive;
        __atomic_fetch_add(&snapshot->refs, 1u, __ATOMIC_RELAXED);
        SpinLockRelease(&g_launcherLock);
    }
    ~LauncherReadGuard() { __atomic_fetch_sub(&snapshot->refs, 1u, __ATOMIC_RELEASE); }
    const LauncherSnapshot& get() const { return *snapshot; }
    LauncherReadGuard(const LauncherReadGuard&) = delete;
    LauncherReadGuard& operator=(const LauncherReadGuard&) = delete;
};

uint32_t WmSnapshotDebugApps(WmDebugApp* out,uint32_t capacity,
                             uint32_t* generation,uint64_t* builtTick)
{
    LauncherReadGuard guard;const auto& catalog=guard.get();
    *generation=catalog.generation;*builtTick=catalog.builtTick;
    uint32_t count=0;
    for(uint32_t i=0;i<catalog.count&&count<capacity;++i) {
        const auto& item=catalog.items[i];auto& app=out[count++];app={};
        // Fixed arrays have identical extents; no live catalog pointers escape.
        memcpy(app.id,item.catalogId,sizeof(app.id));memcpy(app.title,item.title,sizeof(app.title));
        memcpy(app.desktopAppId,item.appId,sizeof(app.desktopAppId));
        memcpy(app.executable,item.executable,sizeof(app.executable));
        app.desktop=item.isDesktopEntry;app.eligible=item.valid;
    }
    return count;
}

void WmRequestLauncherRefresh()
{
    __atomic_fetch_add(&g_launcherRequested, 1u, __ATOMIC_RELEASE);
}

bool WmLauncherApplyPending()
{
    VfbGuard windows;
    SpinLockAcquire(&g_launcherLock);
    if (!g_launcherPending || g_launcherRetired) {
        SpinLockRelease(&g_launcherLock);
        return false;
    }
    LauncherSnapshot* previous = g_launcherActive;
    g_launcherActive = g_launcherPending;
    if(g_launcherSnapshotEpoch)++g_launcherSnapshotEpoch;
    g_launcherPending = nullptr;
    if (previous != &g_emptyLauncher) g_launcherRetired = previous;
    const uint32_t count = g_launcherActive->count;
    const uint32_t generation = g_launcherActive->generation;
    SpinLockRelease(&g_launcherLock);
    g_launcherScroll = 0;
    SerialPrintf("WM: launcher published generation=%u items=%u\n", generation, count);
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------



// Caption cells: index 0 is close, 1 maximize/restore, 2 minimize. They are
// drawn in these cells; input uses the part table's hit rects, which extend
// them to the outer edges (chrome_layout.h).
static DesktopWindowGeometry CaptionGeometry(const Window& w)
{
    return {w.x,w.y,w.clientW,w.clientH,w.noChrome,w.noChrome && w.captionOverlay,
            w.captionInsetRight,w.captionInsetTop,w.captionHeight};
}
// The window's part table (chrome_layout.h). screenW/H: the output, whose
// edges remove resize zones; 0 when only the visual parts matter.
static ChromeLayout WindowChromeLayout(const Window& w, uint32_t screenW = 0, uint32_t screenH = 0)
{
    return ChromeFrameLayout({CaptionGeometry(w), w.state == WindowState::Maximized,
                              {0, 0, static_cast<int>(screenW), static_cast<int>(screenH)}},
                             w.desktopMetrics());
}
WmRect WmCaptionOverlayRect(const Window& w)
{
    return DesktopCaptionOverlayRect(CaptionGeometry(w),w.desktopMetrics());
}

static void WmStrCopy(char* dst, const char* src, uint32_t maxLen)
{
    uint32_t i = 0;
    while (src[i] && i < maxLen - 1) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static bool WmEqualI(const char* a, const char* b)
{
    if (!a || !b) return false;
    while (*a && *b)
    {
        char ac = *a++, bc = *b++;
        if (ac >= 'A' && ac <= 'Z') ac += 'a' - 'A';
        if (bc >= 'A' && bc <= 'Z') bc += 'a' - 'A';
        if (ac != bc) return false;
    }
    return !*a && !*b;
}

static const char* WmBaseName(const char* path)
{
    const char* name = path;
    for (; *path; ++path) if (*path == '/') name = path + 1;
    return name;
}

static bool WmTitleHasAppName(const char* title, const char* name)
{
    if (!name[0]) return false;
    for (const char* start = title; *start; ++start)
    {
        if (start != title && start[-1] != ' ' && start[-1] != '-') continue;
        const char *a = start, *b = name;
        while (*a && *b)
        {
            char ac = *a, bc = *b;
            if (ac >= 'A' && ac <= 'Z') ac += 'a' - 'A';
            if (bc >= 'A' && bc <= 'Z') bc += 'a' - 'A';
            if (ac != bc) break;
            ++a; ++b;
        }
        if (!*b && (!*a || *a == ' ' || *a == '-')) return true;
    }
    return false;
}

static const uint32_t* WindowIcon(const Window& w, const LauncherSnapshot& launcher)
{
    if (WmEqualI(w.appId, "terminal")) return launcher.terminalIcon ? launcher.terminalIcon : launcher.applicationIcon;
    // Explicit desktop identity takes precedence over executable/name guesses.
    for (uint32_t i = 0; i < launcher.count; ++i)
    {
        const LauncherItem& item = launcher.items[i];
        if (item.valid && item.iconPixels && w.appId[0] && item.appId[0] && WmEqualI(w.appId, item.appId))
            return item.iconPixels;
    }
    const char* executable = w.proc ? WmBaseName(w.proc->exePath) : "";
    for (uint32_t i = 0; i < launcher.count; ++i)
    {
        const LauncherItem& item = launcher.items[i];
        if (!item.valid || !item.iconPixels || !item.executable[0]) continue;
        if ((w.appId[0] && WmEqualI(w.appId, item.executable)) ||
            (executable[0] && WmEqualI(executable, item.executable))) return item.iconPixels;
    }
    // Legacy native windows have no app_id. Their title is only a last resort;
    // an unknown explicit ID must not acquire another app's icon by title.
    if (!w.appId[0])
    {
        if (w.proc && TerminalFindByProcess(w.proc)) return launcher.terminalIcon ? launcher.terminalIcon : launcher.applicationIcon;
        for (uint32_t i = 0; i < launcher.count; ++i)
            if (launcher.items[i].valid && launcher.items[i].iconPixels &&
                WmTitleHasAppName(w.title, launcher.items[i].title)) return launcher.items[i].iconPixels;
    }
    return launcher.applicationIcon;
}

// ---------------------------------------------------------------------------
// API implementation
// ---------------------------------------------------------------------------

void WmInit()
{
    if (g_wmActive)
    {
        // Idempotent: launcher shortcuts that include `set wm` for first-run
        // boot scripts must not nuke the existing window table when invoked
        // a second time. Without this guard, every subsequent app launch
        // would wipe every existing window's `proc` pointer (leaving the
        // owning processes orphaned with no chrome and never reaped).
        SerialPuts("WM: WmInit() called again — already initialised, skipping\n");
        return;
    }
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
        g_windows[i].proc = nullptr;
    g_focusedIdx = -1;
    g_nextZOrder = 1;
    SerialPuts("WM: initialised\n");
}

bool WmIsActive()
{
    return __atomic_load_n(&g_wmActive,__ATOMIC_ACQUIRE);
}

static void SetWmActiveLocked(bool active) {
    if(g_wmActive!=active){BeginBackdropStyleChange();__atomic_store_n(&g_wmActive,active,__ATOMIC_RELEASE);EndBackdropStyleChange();}
    if(!active)PointerCaptureReleaseLocked();
}
void WmSetActive(bool active)
{
    if (active) WmRestoreTheme();
    if (active && !g_launcherLoaded) WmLauncherLoad();
    if (active) LauncherStartWorker();
    { VfbGuard guard;SetWmActiveLocked(active); }
    if (active)
        SerialPuts("WM: window manager mode enabled\n");
}

static int CreateWindowLocked(Process* proc, int16_t x, int16_t y,
                   uint16_t clientW, uint16_t clientH,
                   const char* title, uint8_t upscale,
                   bool focusable, bool focusOnCreate = true)
{
    // Find free slot
    int idx = -1;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        if (g_windows[i].proc == nullptr)
        {
            idx = static_cast<int>(i);
            break;
        }
    }
    if (idx < 0) return -1;

    Window& w = g_windows[idx];
    w.proc = proc;
    w.metrics = &WmDesktopMetrics();
    w.creationIdentity = __atomic_add_fetch(&g_windowIdentity, 1, __ATOMIC_ACQ_REL);
    w.pointerEngageSerial = 0;
    w.pointerDisengaged = false;
    w.publicationGeneration=w.publishedGeneration=1;
    w.clientPid=proc?(proc->tgid?proc->tgid:proc->pid):0;
    w.clientIncarnation=proc?(proc->isThread?proc->leaderIncarnation:proc->incarnation):0;
    w.clientLaunchId=proc?proc->launchId:0;
    w.clientOrigin=proc?WmClientOrigin::NativeProcess:WmClientOrigin::Unknown;
    w.appId[0] = 0;
    w.x = x;
    w.y = y;
    w.clientW = clientW;
    w.clientH = clientH;
    w.zOrder = g_nextZOrder++;
    w.upscale = (upscale >= 1) ? upscale : 1;
    w.opacity = 255;
    w.blurRadius = 0;
    w.state = WindowState::Normal;
    w.focused = false;
    w.visible = true;
    w.minimized = false;
    w.noChrome = false;
    w.fullscreen = false;
    w.captionOverlay = false;
    w.captionInsetRight = w.captionInsetTop = w.captionHeight = 0;
    w.captionStyle = 0;
    w.focusable = focusable;
    w.savedX = x;
    w.savedY = y;
    w.savedW = clientW;
    w.savedH = clientH;
    w.vfb       = nullptr;
    w.vfbStride = 0;
    w.vfbWidth=w.vfbHeight=0;
    w.vfbBytes  = 0;
    w.vfbUser   = nullptr;
    w.vfbAddressSpace = nullptr;
    w.vfbBacking = nullptr;
    w.vfbRetirement = nullptr;
    w.vfbDirty  = 0;
    // A reused slot must not keep the previous client's dmabuf. Otherwise a
    // new SHM window keeps displaying the dead GL client's last frame.
    w.externalCaptured = false;
    w.externalW = w.externalH = 0;
    w.wmId      = static_cast<uint16_t>(idx + 1);
    if (proc) {proc->fbDestX=w.clientX();proc->fbDestY=w.clientY();}
    w.inputHead = 0;
    w.inputTail = 0;
    w.inputDropCount = 0;
    WmStrCopy(w.title, title ? title : "Window", sizeof(w.title));

    SerialPrintf("WM: created window %d '%s' at (%d,%d) %ux%u scale=%u for pid %u\n",
                 idx, w.title, x, y, clientW, clientH, upscale, proc ? proc->pid : 0);

    // Auto-focus normal windows. Popup/tooltips are raised by z-order but keep
    // keyboard focus on the invoking toplevel. Non-focusable windows (popups,
    // menus) already received the highest z-order above, so they'll render
    // on top of existing windows without stealing keyboard focus.
    if (w.focusable && focusOnCreate)
        FocusWindowLocked(idx);

    return idx;
}

int WmCreateWindow(Process* proc,int16_t x,int16_t y,uint16_t clientW,uint16_t clientH,
                   const char* title,uint8_t upscale,bool focusable,bool focusOnCreate) {
    VfbGuard guard;
    return CreateWindowLocked(proc,x,y,clientW,clientH,title,upscale,focusable,focusOnCreate);
}

static void DestroyWindowLocked(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;

    if(g_pointerCaptureIndex==idx)PointerCaptureReleaseLocked();
    SerialPrintf("WM: destroyed window %d '%s'\n", idx, w.title);

    // Detach the per-window VFB (if any).  The compositor may already have
    // snapshotted w.vfb for the current frame, so kernel pages are freed only
    // after a later compositor epoch.
    if (w.vfb)
    {
        auto* retirement=w.vfbRetirement;
        w.vfbRetirement=nullptr;
        AddressSpaceState* space = w.vfbAddressSpace;
        auto* backing=w.vfbBacking;
        w.vfbBacking = nullptr;
        w.vfbAddressSpace = nullptr;
        w.vfb       = nullptr;
        w.vfbStride = 0;
        w.vfbWidth=w.vfbHeight=0;
        w.vfbBytes  = 0;
        w.vfbUser   = nullptr;
        w.vfbDirty  = 0;
        // Buffer-specific removal uses preallocated retirement counters and
        // cannot fail allocation. Complete AS cleanup before renderer retirement.
        if (space && backing && UserMappingRemoveBacking(ProcessCurrent(),space,backing)<0)
            KernelPanic("WM: invalid framebuffer mappings during destruction");
        MappingBackingUnref(backing);AddressSpaceUnref(space);
        CompositorRetireVfb(retirement);
    }
    w.externalCaptured = false;
    w.externalW = w.externalH = 0;
    w.wmId = 0;
    w.proc = nullptr;
    w.visible = false;

    if (g_focusedIdx == idx)
    {
        g_focusedIdx = -1;
        // Focus the next highest z-order window
        int bestIdx = -1;
        uint8_t bestZ = 0;
        for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
        {
            if (g_windows[i].proc && g_windows[i].visible && g_windows[i].focusable
                && g_windows[i].zOrder >= bestZ)
            {
                bestZ = g_windows[i].zOrder;
                bestIdx = static_cast<int>(i);
            }
        }
        if (bestIdx >= 0) FocusWindowLocked(bestIdx);
    }
    extern void CompositorMarkDirty();
    CompositorMarkDirty();
}

void WmDestroyWindow(int idx) {
    VfbGuard guard;DestroyWindowLocked(idx);
}

void WmDestroyWindowForProcess(Process* proc)
{
    if (!proc) return;
    VfbGuard guard;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        if (g_windows[i].proc == proc)
        {
            DestroyWindowLocked(static_cast<int>(i));
            // Don't return — a single process may own multiple windows
            // (e.g. waylandd hosting several xdg_toplevels).
        }
    }
}

int WmFindWindowForProcess(Process* proc)
{
    if (!proc) return -1;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        if (g_windows[i].proc == proc)
            return static_cast<int>(i);
    }
    return -1;
}

WmHitResult WmHitTest(int32_t mx, int32_t my)
{
    WmHitResult result = { -1, WmHitZone::None };

    // Collect sorted indices
    int sorted[WM_MAX_WINDOWS];
    uint32_t count = WmGetZOrder(sorted, WM_MAX_WINDOWS);
    uint32_t screenW = 0, screenH = 0;
    CompositorGetPhysDims(&screenW, &screenH);

    // Walk front to back (last in z-order array = frontmost). The first
    // window whose part table claims the point wins, including its outer
    // resize margin.
    for (int i = static_cast<int>(count) - 1; i >= 0; --i)
    {
        int idx = sorted[i];
        const Window& w = g_windows[idx];
        if (!w.proc || !w.visible || w.minimized) continue;
        const ChromeLayout layout = WindowChromeLayout(w, screenW, screenH);
        const ChromePart* part = layout.Hit(mx, my);
        if (!part) continue;
        result.windowIndex = idx;
        switch (part->role) {
        case ChromePartRole::Resize: result.zone = WmHitZoneForResizeEdges(part->edges); break;
        case ChromePartRole::Close: result.zone = WmHitZone::CloseButton; break;
        case ChromePartRole::Maximise: result.zone = WmHitZone::MaximizeButton; break;
        case ChromePartRole::Minimise: result.zone = WmHitZone::MinimizeButton; break;
        case ChromePartRole::Drag: result.zone = WmHitZone::TitleBar; break;
        case ChromePartRole::Client: result.zone = WmHitZone::ClientArea; break;
        case ChromePartRole::Border: result.zone = WmHitZone::Border; break;
        }
        return result;
    }

    return result;
}

static void FocusWindowLocked(int idx)
{
    g_taskbarPageTarget = -1;
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    if (!g_windows[idx].proc) return;
    if (!g_windows[idx].focusable) return;
    if (g_focusedIdx == idx) return;

    PointerCaptureReleaseLocked();
    PointerActivationCancel(); // Only WmPointerActivate after this focus re-arms it.
    // Unfocus old
    if (g_focusedIdx >= 0 && g_focusedIdx < static_cast<int>(WM_MAX_WINDOWS))
    {
        Window& old = g_windows[g_focusedIdx];
        old.focused = false;
        if (old.proc)
            WmPushWmEvent(&old, WM_EVT_FOCUS_LOST, 0, 0);
    }

    // Focus and raise new.  Push FOCUS_GAINED unconditionally, even if
    // the VFB isn't attached yet -- a freshly-created Wayland window
    // attaches its buffer some time after WM_CREATE_WINDOW returns, and
    // waylandd needs the focus event to drive wl_keyboard.enter so the
    // client believes it has the keyboard.  Without this, no keystroke
    // ever reaches a newly-opened window.
    g_windows[idx].focused = true;
    g_windows[idx].zOrder = g_nextZOrder++;
    g_focusedIdx = idx;

    // Re-raise any non-focusable windows (popups, menus) belonging to the
    // same process so they stay above the newly-raised parent.
    Process* focusProc = g_windows[idx].proc;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
    {
        if (static_cast<int>(i) == idx) continue;
        Window& sib = g_windows[i];
        if (sib.proc == focusProc && sib.visible && !sib.focusable)
            sib.zOrder = g_nextZOrder++;
    }

    WmPushWmEvent(&g_windows[idx], WM_EVT_FOCUS_GAINED, 0, 0);
    CompositorMarkDirty();
    CompositorWake();
}

void WmSetFocus(int idx) { VfbGuard guard;FocusWindowLocked(idx); }

void WmSendToBack(int idx)
{
    VfbGuard guard;
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    if (!g_windows[idx].proc) return;

    // Renumber all z-orders to avoid collision: get sorted order, then
    // assign sequential values starting from 1, placing idx at 0.
    int sorted[WM_MAX_WINDOWS];
    uint32_t count = 0;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i) {
        if (g_windows[i].proc && (int)i != idx)
            sorted[count++] = (int)i;
    }
    // Sort by current z-order (ascending)
    for (uint32_t i = 1; i < count; ++i) {
        int key = sorted[i];
        uint8_t keyZ = g_windows[key].zOrder;
        int j = (int)i - 1;
        while (j >= 0 && g_windows[sorted[j]].zOrder > keyZ) {
            sorted[j + 1] = sorted[j];
            --j;
        }
        sorted[j + 1] = key;
    }
    // Assign: target gets 0, others get 1..count
    g_windows[idx].zOrder = 0;
    for (uint32_t i = 0; i < count; ++i)
        g_windows[sorted[i]].zOrder = (uint8_t)(i + 1);
    g_nextZOrder = (uint8_t)(count + 1);
    CompositorMarkDirty();
    CompositorWake();
}

int WmGetFocusedWindow()
{
    return g_focusedIdx;
}

Window* WmGetWindow(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return nullptr;
    if (!g_windows[idx].proc) return nullptr;
    return &g_windows[idx];
}

void WmSetClientSideDecoration(int idx, bool enable, bool captionOverlay,
                               uint16_t insetRight, uint16_t insetTop, uint16_t captionHeight,
                               uint32_t captionStyle)
{
    VfbGuard guard;
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;
    captionOverlay = enable && captionOverlay;
    if (!captionOverlay) insetRight = insetTop = captionHeight = 0;
    if (!captionOverlay || !(captionStyle & DesktopCaptionStyleValid)) captionStyle = 0;
    if (w.fullscreen) {
        w.fsNoChrome = enable; w.fsCaptionOverlay = captionOverlay;
        w.fsCaptionInsetRight = insetRight; w.fsCaptionInsetTop = insetTop;
        w.fsCaptionHeight = captionHeight; w.fsCaptionStyle = captionStyle;
        return;
    }
    if (w.noChrome == enable && w.captionOverlay == captionOverlay &&
        w.captionInsetRight == insetRight && w.captionInsetTop == insetTop &&
        w.captionHeight == captionHeight && w.captionStyle == captionStyle) return;
    const bool modeChanged = w.noChrome != enable || w.captionOverlay != captionOverlay;
    w.noChrome = enable;
    w.captionOverlay = captionOverlay;
    w.captionInsetRight = insetRight;
    w.captionInsetTop = insetTop;
    w.captionHeight = captionHeight;
    w.captionStyle = captionStyle;
    if (!modeChanged) { extern void CompositorMarkDirty(); CompositorMarkDirty(); return; }
    // Mark compositor dirty so the chrome region repaints/clears next frame.
    extern void CompositorMarkDirty();
    CompositorMarkDirty();
    SerialPrintf("WM: window %d CSD=%d caption_overlay=%d inset=%u,%u height=%u\n", idx, (int)enable,
                 (int)captionOverlay, insetRight, insetTop, captionHeight);
}

// Set per-window display properties (opacity and/or backdrop blur radius). `mask`
// selects which fields to apply: bit 0 = opacity, bit 1 = blurRadius. Marks the
// compositor dirty so the change is composited next frame. The GPU DRAW path
// reads w.opacity (and, for frosted glass, w.blurRadius).
static void SetWindowPropertiesLocked(int idx, uint32_t mask, uint8_t opacity, uint8_t blurRadius)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;
    bool changed = false;
    if ((mask & WM_PROP_OPACITY) && w.opacity != opacity)    { w.opacity = opacity; changed = true; }
    if ((mask & WM_PROP_BLUR)    && w.blurRadius != blurRadius) { w.blurRadius = blurRadius; changed = true; }
    if (!changed) return;
    extern void CompositorMarkDirty();
    CompositorMarkDirty();
}

void WmToggleFocusedWindowGlass()
{
    VfbGuard guard;
    const int idx=g_focusedIdx;
    if(idx<0 || idx>=static_cast<int>(WM_MAX_WINDOWS) || !g_windows[idx].proc || !g_windows[idx].focused)return;
    const bool glassOn=g_windows[idx].blurRadius==0;
    SetWindowPropertiesLocked(idx,WM_PROP_OPACITY|WM_PROP_BLUR,
                              glassOn?WM_GLASS_OPACITY:255,glassOn?WM_GLASS_BLUR:0);
}

bool WmSetOwnedWindowProperties(Process* proc, uint32_t wmId, uint32_t mask,
                                uint8_t opacity, uint8_t blurRadius)
{
    VfbGuard guard;
    if (!WmFindWindowById(proc, wmId)) return false;
    SetWindowPropertiesLocked(static_cast<int>(wmId) - 1, mask, opacity, blurRadius);
    return true;
}

uint32_t WmWindowCount()
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; ++i)
        if (g_windows[i].proc) ++n;
    return n;
}

static void SetMaximizedLocked(int idx, bool enable)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;
    // Fullscreen owns the geometry; leaving it restores the earlier state.
    if (w.fullscreen) return;
    if ((w.state == WindowState::Maximized) == enable) return;

    uint32_t screenW, screenH;
    CompositorGetPhysDims(&screenW, &screenH);

    uint16_t newW, newH;
    const auto oldState=w.state;
    const auto oldX=w.x,oldY=w.y;
    const auto oldW=w.clientW,oldH=w.clientH;
    const auto savedX=w.savedX,savedY=w.savedY;
    const auto savedW=w.savedW,savedH=w.savedH;

    if (w.state == WindowState::Normal)
    {
        const int64_t width = static_cast<int64_t>(screenW) - (w.noChrome ? 0 : 2 * w.metric(WM_BORDER_WIDTH));
        const int64_t height = static_cast<int64_t>(WmDesktopHeight(screenH)) -
            (w.noChrome ? 0 : w.metric(WM_TITLE_BAR_HEIGHT) + 2 * w.metric(WM_BORDER_WIDTH));
        if (width < WM_MIN_WIDTH || height < WM_MIN_HEIGHT || width > 65535 || height > 65535) return;
        // Save current geometry
        w.savedX = w.x;
        w.savedY = w.y;
        w.savedW = w.clientW;
        w.savedH = w.clientH;

        // Maximize: fill desktop area (screen minus taskbar) minus chrome.
        // CSD windows have no chrome, so the client area takes the full space.
        w.x = 0;
        w.y = 0;
        newW = static_cast<uint16_t>(width);
        newH = static_cast<uint16_t>(height);
        w.state = WindowState::Maximized;
    }
    else
    {
        // Restore
        const auto restored = ReframeDesktopWindow(
            {w.savedX,w.savedY,w.savedW,w.savedH,w.noChrome},
            w.desktopMetrics(),w.desktopMetrics(),screenW,WmDesktopHeight(screenH),
            !w.noChrome && w.focusable);
        w.x = static_cast<int16_t>(restored.x);
        w.y = static_cast<int16_t>(restored.y);
        newW = w.savedW;
        newH = w.savedH;
        w.state = WindowState::Normal;
    }

    // Route through WmResizeWindow so terminal VFBs / SIGWINCH paths fire.
    // Without this, chrome grows but the inner VFB stays the old size.
    if (!ResizeWindowLocked(idx,newW,newH)) {
        w.x=oldX;w.y=oldY;w.clientW=oldW;w.clientH=oldH;w.state=oldState;
        w.savedX=savedX;w.savedY=savedY;w.savedW=savedW;w.savedH=savedH;
    }
    w.proc->fbDestX=w.clientX();w.proc->fbDestY=w.clientY();
}

void WmSetMaximized(int idx,bool enable) { VfbGuard guard;SetMaximizedLocked(idx,enable); }

void WmToggleMaximize(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;
    WmSetMaximized(idx, w.state != WindowState::Maximized);
}

static bool SetFullscreenLocked(int idx, bool enable)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return false;
    Window& w = g_windows[idx];
    if (!w.proc) return false;
    if (w.fullscreen == enable) return true;
    if (enable) {
        uint32_t screenW = 0, screenH = 0;
        CompositorGetPhysDims(&screenW, &screenH);
        if (screenW < WM_MIN_WIDTH || screenH < WM_MIN_HEIGHT || screenW > 32767 || screenH > 32767) return false;
        w.fsState = w.state; w.fsX = w.x; w.fsY = w.y; w.fsW = w.clientW; w.fsH = w.clientH;
        w.fsNoChrome = w.noChrome; w.fsCaptionOverlay = w.captionOverlay;
        w.fsCaptionInsetRight = w.captionInsetRight; w.fsCaptionInsetTop = w.captionInsetTop;
        w.fsCaptionHeight = w.captionHeight; w.fsCaptionStyle = w.captionStyle;
        w.noChrome = true; w.captionOverlay = false;
        w.captionInsetRight = w.captionInsetTop = w.captionHeight = 0; w.captionStyle = 0;
        w.x = 0; w.y = 0; w.fullscreen = true; w.state = WindowState::Maximized;
        if (!ResizeWindowLocked(idx, static_cast<uint16_t>(screenW), static_cast<uint16_t>(screenH))) {
            w.fullscreen = false; w.state = w.fsState; w.x = w.fsX; w.y = w.fsY;
            w.noChrome = w.fsNoChrome; w.captionOverlay = w.fsCaptionOverlay;
            w.captionInsetRight = w.fsCaptionInsetRight; w.captionInsetTop = w.fsCaptionInsetTop;
            w.captionHeight = w.fsCaptionHeight; w.captionStyle = w.fsCaptionStyle;
            return false;
        }
    } else {
        const auto x = w.x, y = w.y; const auto width = w.clientW, height = w.clientH;
        w.fullscreen = false; w.state = w.fsState;
        w.noChrome = w.fsNoChrome; w.captionOverlay = w.fsCaptionOverlay;
        w.captionInsetRight = w.fsCaptionInsetRight; w.captionInsetTop = w.fsCaptionInsetTop;
        w.captionHeight = w.fsCaptionHeight; w.captionStyle = w.fsCaptionStyle;
        w.x = w.fsX; w.y = w.fsY;
        if (!ResizeWindowLocked(idx, w.fsW, w.fsH)) {
            // Stay fullscreen rather than half-restored; the caller may retry.
            w.fsState = w.state; w.fsNoChrome = w.noChrome; w.fsCaptionOverlay = w.captionOverlay;
            w.fsCaptionInsetRight = w.captionInsetRight; w.fsCaptionInsetTop = w.captionInsetTop;
            w.fsCaptionHeight = w.captionHeight; w.fsCaptionStyle = w.captionStyle;
            w.noChrome = true; w.captionOverlay = false;
            w.captionInsetRight = w.captionInsetTop = w.captionHeight = 0; w.captionStyle = 0;
            w.x = x; w.y = y; w.clientW = width; w.clientH = height;
            w.fullscreen = true; w.state = WindowState::Maximized;
            return false;
        }
    }
    w.proc->fbDestX = w.clientX(); w.proc->fbDestY = w.clientY();
    SerialPrintf("WM: window %d '%s' fullscreen=%d %ux%u at %d,%d\n", idx, w.title, (int)enable,
                 w.clientW, w.clientH, w.x, w.y);
    extern void CompositorMarkDirty();
    CompositorMarkDirty();
    return true;
}

bool WmSetFullscreen(int idx, bool enable) { VfbGuard guard; return SetFullscreenLocked(idx, enable); }

bool WmTaskbarHidden()
{
    if (!g_wmActive || WmLauncherVisible() || WmVolumePopupVisible()) return false;
    const int idx = __atomic_load_n(&g_focusedIdx, __ATOMIC_ACQUIRE);
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return false;
    const Window& w = g_windows[idx];
    if (!w.proc || !w.visible || w.minimized) return false;
    if (w.fullscreen) return true;
    // A chrome-less window placed over the whole output is fullscreen too:
    // the Xwayland WM shipped before WM_SET_FULLSCREEN sizes EWMH fullscreen
    // windows this way. Maximized windows stop at the taskbar, so never match.
    uint32_t screenW = 0, screenH = 0;
    CompositorGetPhysDims(&screenW, &screenH);
    return w.noChrome && screenW && screenH && w.x <= 0 && w.y <= 0 &&
           int64_t(w.x) + w.clientW >= int64_t(screenW) && int64_t(w.y) + w.clientH >= int64_t(screenH);
}

uint32_t WmTaskbarTop(uint32_t screenH) { return WmTaskbarHidden() ? screenH : WmDesktopHeight(screenH); }

static void MoveWindowLocked(int idx, int16_t newX, int16_t newY)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc) return;
    // CSD can maximize during a title drag; its old offset must not move the
    // new desktop-sized surface away from the origin.
    if (w.state == WindowState::Maximized) return;
    w.x = newX;
    w.y = newY;
    w.proc->fbDestX=w.clientX();w.proc->fbDestY=w.clientY();
    extern void CompositorMarkDirty();
    CompositorMarkDirty();
}

void WmMoveWindow(int idx, int16_t newX, int16_t newY)
{
    VfbGuard guard;
    MoveWindowLocked(idx,newX,newY);
}

bool WmMoveWindowRelativeToParent(Process* proc, uint32_t wmId,
                                  uint32_t parentWmId,
                                  int32_t relX, int32_t relY)
{
    VfbGuard guard;
    Window* child = WmFindWindowById(proc, wmId);
    Window* parent = WmFindWindowById(proc, parentWmId);
    if (!child || !parent) return false;

    int32_t clientX = parent->clientX();
    int32_t clientY = parent->clientY();
    int32_t x = clientX + relX;
    int32_t y = clientY + relY;
    static constexpr int32_t kMinI16 = -32768;
    static constexpr int32_t kMaxI16 = 32767;
    if (x < kMinI16) x = kMinI16;
    if (x > kMaxI16) x = kMaxI16;
    if (y < kMinI16) y = kMinI16;
    if (y > kMaxI16) y = kMaxI16;

    MoveWindowLocked(static_cast<int>(wmId) - 1, static_cast<int16_t>(x), static_cast<int16_t>(y));
    return true;
}

int WmPlaceOwnedWindow(Process* proc, uint32_t wmId, uint64_t incarnation,
                       int32_t clientX, int32_t clientY)
{
    if (!proc || !incarnation) return -22;
    VfbGuard guard;
    Window* w = WmFindWindowById(proc, wmId);
    if (!w) return -2;
    if (w->creationIdentity != incarnation) return -116;
    const int64_t x = static_cast<int64_t>(clientX) - (w->noChrome ? 0 : w->metric(WM_BORDER_WIDTH));
    const int64_t y = static_cast<int64_t>(clientY) - (w->noChrome ? 0 : w->metric(WM_TITLE_BAR_HEIGHT)+w->metric(WM_BORDER_WIDTH));
    if (x < -32768 || x > 32767 || y < -32768 || y > 32767) return -22;
    if (w->x != x || w->y != y) {
        MoveWindowLocked(static_cast<int>(wmId)-1, static_cast<int16_t>(x), static_cast<int16_t>(y));
        CompositorWake();
    }
    return 0;
}

static bool ResizeWindowLocked(int idx, uint16_t newClientW, uint16_t newClientH)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return false;
    Window& w = g_windows[idx];
    if (!w.proc) return false;
    if (w.vfb) {
        const uint32_t head=__atomic_load_n(&w.inputHead,__ATOMIC_ACQUIRE);
        const uint32_t tail=__atomic_load_n(&w.inputTail,__ATOMIC_ACQUIRE);
        if ((head+1)%Window::WM_INPUT_QUEUE==tail) return false;
    }

    // Enforce minimum size
    if (newClientW < WM_MIN_WIDTH)  newClientW = WM_MIN_WIDTH;
    if (newClientH < WM_MIN_HEIGHT) newClientH = WM_MIN_HEIGHT;

    // Keep exact ownership and geometry stable across terminal preparation.
    // Both locks sleep; terminal state never acquires the window guard.
    auto terminal = TerminalResizeForProcess(w.proc->pid,w.proc->incarnation,newClientW,newClientH);
    if (terminal == TerminalResizeResult::Failed) return false;
    NextPublication(w);
    w.clientW = newClientW;
    w.clientH = newClientH;
    if (terminal == TerminalResizeResult::Resized)
    {
        // Backing pixels and SIGWINCH were committed by the exact owner.
    }
    else if (w.vfb)
    {
        // WM-API window (waylandd-hosted toplevel etc): notify the client
        // so it can reallocate a buffer at the new size and re-commit.
        // The compositor blit clamps to the existing VFB until the new
        // buffer arrives, so the visible chrome grows immediately and
        // content "fills in" once the client acknowledges the configure.
        SerialPrintf("WM: resize wm_id=%u '%s' -> %ux%u (vfb %ux%u stride=%u)"
                     " — pushing WM_EVT_RESIZED\n",
                     w.wmId, w.title, newClientW, newClientH,
                     w.clientW, w.clientH, w.vfbStride);
        WmPushWmEvent(&w,
                      WM_EVT_RESIZED,
                      static_cast<int16_t>(newClientW),
                      static_cast<int16_t>(newClientH));
    }
    else
    {
        // Legacy per-process VFB (not WM-API): stays the same size,
        // upscale handles it.
    }
    extern void CompositorMarkDirty();
    CompositorMarkDirty();
    return true;
}

bool WmResizeWindow(int idx,uint16_t newClientW,uint16_t newClientH)
{
    VfbGuard guard;
    return ResizeWindowLocked(idx,newClientW,newClientH);
}

bool WmApplyScalePending()
{
    const auto* next=DesktopMetricsFor(WmGetRequestedScale());
    if (!next || next==&WmDesktopMetrics()) return false;
    uint32_t sw,sh;CompositorGetPhysDims(&sw,&sh);
    VfbGuard guard;
    const int desktopH=sh>static_cast<uint32_t>(next->taskbar()) ? sh-next->taskbar() : 0;
    for (auto& w:g_windows) {
        if (!w.proc) continue;
        const auto& before=w.desktopMetrics();
        if (w.fullscreen) {
            // Still covers the output; only the geometry it returns to changes.
            auto saved=ReframeDesktopWindow({w.fsX,w.fsY,w.fsW,w.fsH,w.fsNoChrome},before,*next,sw,desktopH,false);
            w.fsX=static_cast<int16_t>(saved.x);w.fsY=static_cast<int16_t>(saved.y);
            w.metrics=next;continue;
        }
        auto rect=ReframeDesktopWindow({w.x,w.y,w.clientW,w.clientH,w.noChrome},before,*next,sw,desktopH,!w.noChrome && w.focusable);
        auto saved=ReframeDesktopWindow({w.savedX,w.savedY,w.savedW,w.savedH,w.noChrome},before,*next,sw,desktopH,false);
        w.metrics=next;w.x=static_cast<int16_t>(rect.x);w.y=static_cast<int16_t>(rect.y);
        w.savedX=static_cast<int16_t>(saved.x);w.savedY=static_cast<int16_t>(saved.y);
        if (w.state==WindowState::Maximized) {
            const int border=w.noChrome ? 0 : next->border();
            const int title=w.noChrome ? 0 : next->title();
            const int width=static_cast<int>(sw)-2*border,height=desktopH-title-2*border;
            if (width>=static_cast<int>(WM_MIN_WIDTH) && height>=static_cast<int>(WM_MIN_HEIGHT) && width<=65535 && height<=65535 && ResizeWindowLocked(static_cast<int>(&w-g_windows),width,height)) {
                w.x=w.y=0;
            } else {
                // Keep the existing owned pixels and make the window movable
                // if memory cannot satisfy a new maximized backing allocation.
                w.state=WindowState::Normal;
                SerialPrintf("WM: scale resize failed for window %u; preserving client buffer\n",w.wmId);
            }
        }
        w.proc->fbDestX=w.clientX();w.proc->fbDestY=w.clientY();
    }
    BeginBackdropStyleChange();
    __atomic_store_n(&g_desktopMetrics,next,__ATOMIC_RELEASE);
    EndBackdropStyleChange();
    if(g_chromeSettingsEpoch)++g_chromeSettingsEpoch;
    g_launcherScroll=0;
    CompositorMarkDirty();
    SerialPrintf("WM: desktop scale applied %u%%\n",next->percent);
    return true;
}

static int NextFocusCandidateLocked(int excluded)
{
    int best=-1;uint8_t bestZ=0;
    for(uint32_t i=0;i<WM_MAX_WINDOWS;++i) {
        const auto& w=g_windows[i];
        if(static_cast<int>(i)!=excluded && w.proc && w.visible && !w.minimized && w.focusable && w.zOrder>=bestZ) {
            bestZ=w.zOrder;best=static_cast<int>(i);
        }
    }
    return best;
}

static void MinimizeWindowLocked(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc || w.minimized) return;

    if(g_pointerCaptureIndex==idx)PointerCaptureReleaseLocked();
    PointerActivationCancel();
    w.minimized = true;
    SerialPrintf("WM: minimized window %d '%s'\n", idx, w.title);

    // If this was focused, focus the next visible window
    if (g_focusedIdx == idx)
    {
        g_focusedIdx = -1;
        w.focused = false;
        WmPushWmEvent(&w,WM_EVT_FOCUS_LOST,0,0);
        const int bestIdx=NextFocusCandidateLocked(idx);
        if (bestIdx >= 0) FocusWindowLocked(bestIdx);
    }
    extern void CompositorMarkDirty();
    CompositorMarkDirty();
}

static void RestoreWindowLocked(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    Window& w = g_windows[idx];
    if (!w.proc || !w.minimized) return;

    w.minimized = false;
    SerialPrintf("WM: restored window %d '%s'\n", idx, w.title);
    FocusWindowLocked(idx);
    extern void CompositorMarkDirty();
    CompositorMarkDirty();
}

void WmMinimizeWindow(int idx) { VfbGuard guard;MinimizeWindowLocked(idx); }
void WmRestoreWindow(int idx) { VfbGuard guard;RestoreWindowLocked(idx); }

static bool RequestCloseLocked(int idx,uint32_t* method)
{
    if (idx<0 || idx>=static_cast<int>(WM_MAX_WINDOWS)) return false;
    auto& w=g_windows[idx];
    if (!w.proc || !w.focusable) return false;
    if (w.vfb) {
        *method=1;
        return WmPushWmEvent(&w,WM_EVT_CLOSE_REQUESTED,0,0);
    }
    if(w.proc->state==ProcessState::Terminated)return false;
    TerminalGeometry terminal{};
    const bool isTerminal=TerminalSnapshotGeometry(w.proc,&terminal);
    *method=isTerminal?2:3;
    // Terminal hangup is cooperative too: no hidden SIGKILL on close.
    ProcessSendSignal(w.proc,isTerminal?1:15);
    return true;
}

bool WmRequestCloseWindow(int idx) {
    VfbGuard guard;uint32_t method=0;return RequestCloseLocked(idx,&method);
}

static void SnapshotDebugWindowLocked(const Window& w,WmDebugWindow* out)
{
    *out={};out->id=w.wmId;out->incarnation=w.creationIdentity;
    out->providerPid=w.proc->tgid?w.proc->tgid:w.proc->pid;
    out->providerIncarnation=w.proc->isThread?w.proc->leaderIncarnation:w.proc->incarnation;
    out->clientPid=w.clientPid;out->clientIncarnation=w.clientIncarnation;
    out->clientOrigin=w.clientOrigin;out->launchId=w.clientLaunchId;
    out->x=w.x;out->y=w.y;out->width=w.clientW;out->height=w.clientH;
    out->outerWidth=w.outerWidth();out->outerHeight=w.outerHeight();
    out->bufferWidth=w.vfbWidth;out->bufferHeight=w.vfbHeight;
    out->visible=w.visible;out->minimized=w.minimized;out->maximized=w.state==WindowState::Maximized && !w.fullscreen;out->fullscreen=w.fullscreen;
    out->focused=w.focused;out->focusable=w.focusable;out->apiWindow=w.vfb!=nullptr;
    out->providerKernel=w.proc->isKernelThread;
    WmStrCopy(out->title,w.title,sizeof(out->title));WmStrCopy(out->appId,w.appId,sizeof(out->appId));
}

Process* WmReferenceTerminatedOwner()
{
    VfbGuard guard;
    for(const auto& w:g_windows) {
        if(!w.proc || w.proc->state!=ProcessState::Terminated)continue;
        Process* pinned=SchedulerFindProcessReference(w.proc->pid);
        if(pinned && pinned==w.proc && pinned->incarnation==w.proc->incarnation)return pinned;
        if(pinned)ProcessUnref(pinned);
    }
    return nullptr;
}

uint32_t WmSnapshotDebugWindows(WmDebugWindow* out,uint32_t capacity,uint64_t* tick)
{
    VfbGuard guard;uint32_t count=0;
    for (const auto& w:g_windows) if(w.proc && count<capacity) SnapshotDebugWindowLocked(w,&out[count++]);
    *tick=ApicTickCount();return count;
}

int WmApplyDebugAction(uint32_t id,uint64_t incarnation,WmDebugAction action,
                      uint32_t restoreTarget,WmDebugWindow* result,uint32_t* closeMethod)
{
    VfbGuard guard;
    if (!id || id>WM_MAX_WINDOWS) return -2;
    const int idx=static_cast<int>(id)-1;auto& w=g_windows[idx];
    if (!w.proc || w.creationIdentity!=incarnation) return -2;
    if (!w.focusable) return -95;
    const bool needsResize=(action==WmDebugAction::Maximize && w.state!=WindowState::Maximized) ||
        (action==WmDebugAction::Restore && restoreTarget &&
        (w.state==WindowState::Maximized)!=(restoreTarget==2));
    uint32_t notifications[WM_MAX_WINDOWS]{};
    if(needsResize)++notifications[idx];
    int focusTarget=-1;
    if(action==WmDebugAction::Focus || (action==WmDebugAction::Restore && w.minimized))focusTarget=idx;
    if(action==WmDebugAction::Minimize && !w.minimized && g_focusedIdx==idx) {
        ++notifications[idx];focusTarget=NextFocusCandidateLocked(idx);
        if(focusTarget>=0)++notifications[focusTarget];
    }else if(focusTarget>=0 && focusTarget!=g_focusedIdx) {
        ++notifications[focusTarget];
        if(g_focusedIdx>=0)++notifications[g_focusedIdx];
    }
    // Check every notification before applying any part of the transition.
    // Consumers can only free slots while this WM lock excludes producers.
    for(uint32_t i=0;i<WM_MAX_WINDOWS;++i)if(notifications[i] && g_windows[i].vfb) {
        const auto& target=g_windows[i];
        const uint32_t head=__atomic_load_n(&target.inputHead,__ATOMIC_ACQUIRE);
        const uint32_t tail=__atomic_load_n(&target.inputTail,__ATOMIC_ACQUIRE);
        const uint32_t free=(tail+Window::WM_INPUT_QUEUE-head-1)%Window::WM_INPUT_QUEUE;
        if(free<notifications[i])return -11;
    }
    switch(action) {
    case WmDebugAction::Focus:RestoreWindowLocked(idx);FocusWindowLocked(idx);break;
    case WmDebugAction::Minimize:MinimizeWindowLocked(idx);break;
    case WmDebugAction::Maximize:SetMaximizedLocked(idx,true);if(w.state!=WindowState::Maximized)return -12;break;
    case WmDebugAction::Restore:
        if(restoreTarget>2)return -22;
        if(restoreTarget) SetMaximizedLocked(idx,restoreTarget==2);
        if(restoreTarget && (w.state==WindowState::Maximized)!=(restoreTarget==2))return -12;
        RestoreWindowLocked(idx);break;
    case WmDebugAction::Close:
        // An API window receives a cooperative close event through its own
        // queue, even when a compositor cannot identify its client.  Never
        // use the signal fallback for an unknown client: w.proc is then the
        // provider and may own many unrelated windows.
        if(w.proc->isKernelThread || (w.clientOrigin==WmClientOrigin::Unknown && !w.vfb))return -95;
        if(!RequestCloseLocked(idx,closeMethod))return -11;
        break;
    }
    SnapshotDebugWindowLocked(w,result);
    CompositorMarkDirty();CompositorWake();return 0;
}

uint32_t WmGetZOrder(int* outIndices, uint32_t maxOut)
{
    // Collect visible, non-minimized windows
    uint32_t count = 0;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS && count < maxOut; ++i)
    {
        if (g_windows[i].proc && g_windows[i].visible && !g_windows[i].minimized)
            outIndices[count++] = static_cast<int>(i);
    }

    // Simple insertion sort by z-order (ascending = back to front)
    for (uint32_t i = 1; i < count; ++i)
    {
        int key = outIndices[i];
        uint32_t keyZ = g_windows[key].zOrder;
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && g_windows[outIndices[j]].zOrder > keyZ)
        {
            outIndices[j + 1] = outIndices[j];
            --j;
        }
        outIndices[j + 1] = key;
    }

    return count;
}

// ---------------------------------------------------------------------------
// Chrome rendering
// ---------------------------------------------------------------------------

static ChromeMaterialCache g_chromeMaterial;
static uint64_t g_materialRebuilds=0;
// The compositor thread and, under glass, chrome captures for a renderer
// both use it: rebuilds are serialised. A rebuild follows a theme, opacity or
// scale change, which also changes every chrome key, so a capture that read
// it mid-rebuild is discarded as stale.
static SpinLock g_chromeMaterialLock;
const ChromeMaterialCache& WmChromeMaterial(){
    const auto& p=WmCurrentPalette();
    SpinLockAcquire(&g_chromeMaterialLock);
    if(g_chromeMaterial.Ensure(p.titleActive,p.titleInactive,CompositorMaterialOpacity(),
                              WmMetric(WM_TITLE_BAR_HEIGHT),WmMetric(7),WmMetric(WM_BORDER_WIDTH)))
        __atomic_add_fetch(&g_materialRebuilds,1,__ATOMIC_RELAXED);
    SpinLockRelease(&g_chromeMaterialLock);
    return g_chromeMaterial;
}
uint64_t WmMaterialCacheRebuilds(){return __atomic_load_n(&g_materialRebuilds,__ATOMIC_RELAXED);}

// The builders' view of the WM's theme, scale, font and material state.
ChromeStyle WmChromeStyle()
{
    const bool glass = CompositorGlassActive();
    return {&WmCurrentPalette(), &WmDesktopMetrics(), &WmDesktopFont(), glass, CompositorMaterialOpacity(),
            glass ? &WmChromeMaterial() : nullptr};
}
static int WmTextWidth(const char* text) { return ChromeTextWidth(WmDesktopFont(), text); }
// Shell surfaces are built and executed on the compositor thread only; they
// share this list storage (too large for the stack).
static ChromeShellStorage g_shellListStorage;

// The frame is the window's part table drawn through its display list
// (chrome_frame.cpp); the executor writes the same pixels the painter did.
static void RenderWindowChrome(uint32_t* buf, uint32_t stride,
                                uint32_t screenW, uint32_t screenH,
                                const Window& w,
                                int32_t mouseX = -1, int32_t mouseY = -1)
{
    if (!buf || !stride || !screenW || !screenH || !w.proc || !w.visible) return;
    if (w.noChrome && !w.captionOverlay) return;
    LauncherReadGuard icons;
    const ChromeFrameModel model{WindowChromeLayout(w), w.clientW, w.focused, w.state == WindowState::Maximized,
                                 w.noChrome, w.captionStyle, w.title, WindowIcon(w, icons.get()), mouseX, mouseY};
    ChromeFrameStorage storage;
    ChromeDisplayList list = ChromeListOn(storage);
    ChromeBuildFrame(model, WmChromeStyle(), list);
    ChromeExecute(list, {buf, stride, screenW, screenH}, WmDesktopFont());
}

void WmRenderChrome(uint32_t* backBuffer, uint32_t stride,
                    uint32_t screenW, uint32_t screenH)
{
    if (!g_wmActive) return;

    int sorted[WM_MAX_WINDOWS];
    uint32_t count = WmGetZOrder(sorted, WM_MAX_WINDOWS);

    for (uint32_t i = 0; i < count; ++i)
    {
        const Window& w = g_windows[sorted[i]];
        if (!w.proc || !w.visible) continue;
        RenderWindowChrome(backBuffer, stride, screenW, screenH, w);
    }
}

void WmRenderChromeForWindow(uint32_t* backBuffer, uint32_t stride,
                              uint32_t screenW, uint32_t screenH, int idx,
                              int32_t mouseX, int32_t mouseY)
{
    if (!g_wmActive || idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return;
    const Window& w = g_windows[idx];
    if (!w.proc || !w.visible) return;
    RenderWindowChrome(backBuffer, stride, screenW, screenH, w, mouseX, mouseY);
}

#include "wm_shell_taskbar.inc"

uint32_t WmDesktopHeight(uint32_t screenH)
{
    return screenH > WmMetric(WM_TASKBAR_HEIGHT) ? screenH - WmMetric(WM_TASKBAR_HEIGHT) : 0;
}

void WmSpawnTerminal()
{
    if (!g_wmActive || !ShutdownLaunchAllowed()) return;

    uint32_t clientW = 800;
    uint32_t clientH = 600;

    int termIdx = TerminalCreate(clientW, clientH);
    if (termIdx < 0)
    {
        SerialPuts("WM: failed to spawn new terminal\n");
        return;
    }

    Terminal* t = TerminalGet(termIdx);
    if (t && t->child)
    {
        // Stagger position based on terminal index
        int16_t winX = static_cast<int16_t>(60 + (termIdx % 6) * 40);
        int16_t winY = static_cast<int16_t>(60 + (termIdx % 6) * 40);

        t->child->fbDestX = winX + static_cast<int16_t>(WmMetric(WM_BORDER_WIDTH));
        t->child->fbDestY = winY + static_cast<int16_t>(WmMetric(WM_TITLE_BAR_HEIGHT) + WmMetric(WM_BORDER_WIDTH));
        t->child->fbScale = 1;
        ProcessFramebufferMarkDirty(t->child);

        WmCreateWindow(t->child, winX, winY,
                       static_cast<uint16_t>(clientW),
                       static_cast<uint16_t>(clientH), "Terminal");

        SerialPrintf("WM: spawned new terminal (bash pid %u)\n", t->child->pid);
    }
}

// ---------------------------------------------------------------------------
// App Launcher — popup panel with shortcut items
// ---------------------------------------------------------------------------

// Launcher and taskbar sizes are the shell builders' (chrome_shell.h).
static_assert(LAUNCHER_ICON_PX == ChromeShellMetrics::LauncherIconSize, "launcher icon size");
static_assert(WM_TASKBAR_BTN_WIDTH == ChromeShellMetrics::TaskbarButtonWidth &&
              WM_TASKBAR_BTN_HEIGHT == ChromeShellMetrics::TaskbarButtonHeight &&
              WM_TASKBAR_PADDING == ChromeShellMetrics::TaskbarPadding, "taskbar metrics");
static_assert(WM_MAX_WINDOWS + LaunchFeedbackTable::Capacity <= ChromeTaskbarButtonLimit, "taskbar entries");

// Read title and explicit window identity from a shortcut's comment header.
// Identity metadata matches Wayland app_id/executable without guessing by title.
static bool ParseShortcutMetadata(const char* path, LauncherItem& item)
{
    Vnode* vn = VfsOpen(path, 0);
    if (!vn) return false;
    char buf[512]; uint64_t offset = 0;
    int rd = VfsRead(vn, buf, sizeof(buf) - 1, &offset);
    VfsClose(vn);
    if (rd <= 0) return false;
    buf[rd] = 0;
    struct Field { const char* key; char* output; uint32_t capacity; };
    const Field fields[] = {
        {"title:", item.title, sizeof(item.title)},
        {"app_id:", item.appId, sizeof(item.appId)},
        {"executable:", item.executable, sizeof(item.executable)},
    };
    for (char* line = buf; *line;)
    {
        char* end = line;
        while (*end && *end != '\n') ++end;
        const bool more = *end != 0; *end = 0;
        char* text = line;
        while (*text == ' ' || *text == '\t') ++text;
        if (!*text || *text == '\r') {
            if (!more) break;
            line = end + 1;
            continue;
        }
        if (*text != '#') break;
        ++text; while (*text == ' ' || *text == '\t') ++text;
        for (const auto& field : fields)
        {
            const uint32_t length = strlen(field.key);
            if (strncmp(text, field.key, length) != 0) continue;
            char* value = text + length;
            while (*value == ' ' || *value == '\t') ++value;
            char* tail = value + strlen(value);
            while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t' || tail[-1] == '\r')) --tail;
            *tail = 0;
            WmStrCopy(field.output, value, field.capacity);
            break;
        }
        if (!more) break;
        line = end + 1;
    }
    return item.title[0] != 0;
}

// Asset files are 24x24 straight-alpha pixels encoded as little-endian ARGB.
static uint32_t* LoadIconFile(const char* name)
{
    if (!name || !name[0]) return nullptr;
    for (const char* p = name; *p; ++p) if (*p == '/' || *p == '\\') return nullptr;
    char path[192];
    const char* prefix = "/nix/share/applications/icons/";
    uint32_t n = 0;
    while (*prefix) path[n++] = *prefix++;
    for (; *name && n + 1 < sizeof(path); ++name) path[n++] = *name;
    if (*name) return nullptr;
    path[n] = 0;
    Vnode* vn = VfsOpen(path, 0);
    if (!vn) return nullptr;
    auto* pixels = static_cast<uint32_t*>(kmalloc(LAUNCHER_ICON_BYTES));
    uint64_t offset = 0;
    if (pixels)
    {
        uint32_t done = 0;
        while (done < LAUNCHER_ICON_BYTES)
        {
            int rd = VfsRead(vn, reinterpret_cast<uint8_t*>(pixels) + done,
                             LAUNCHER_ICON_BYTES - done, &offset);
            if (rd <= 0) break;
            done += static_cast<uint32_t>(rd);
        }
        if (done != LAUNCHER_ICON_BYTES) { kfree(pixels); pixels = nullptr; }
    }
    VfsClose(vn);
    return pixels;
}

static void LauncherLoadIcon(LauncherItem* item)
{
    char name[64]; uint32_t n = 0;
    for (const char* p = item->title; *p && n + 6 < sizeof(name); ++p)
    {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c == ' ') c = '_';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') name[n++] = c;
    }
    WmStrCopy(name + n, ".rgba", sizeof(name) - n);
    item->iconPixels = LoadIconFile(name);
}

static bool LauncherFileExists(const char* path)
{
    VnodeStat st{};
    return path[0] == '/' && VfsStatPath(path, &st) == 0 && !st.isDir;
}

static bool LauncherExecutable(const char* command, char* executable, uint32_t capacity)
{
    command = LauncherExecutableStart(command);
    if (!command) return false;
    char path[256]; uint32_t n = 0;
    while (*command == ' ' || *command == '\t') ++command;
    char quote = (*command == '\"' || *command == '\'') ? *command++ : 0;
    while (*command && (quote ? *command != quote : *command != ' ' && *command != '\t'))
    {
        if (n + 1 >= sizeof(path)) return false;
        path[n++] = *command++;
    }
    if (!n || (quote && *command != quote)) return false;
    path[n] = 0;
    WmStrCopy(executable, WmBaseName(path), capacity);
    if (path[0] == '/') {
        if (strncmp(path, "/nix/store/", 11) != 0) return LauncherFileExists(path);
        // Store objects survive profile removal. A desktop entry is installed
        // only while the active profile exposes this same executable, not just
        // another package's executable with an identical basename. stat follows
        // profile symlinks; compare mount identity as well as the inode.
        const char* basename = WmBaseName(path);
        if (!*basename) return false;
        char profile[288];
        constexpr char prefix[] = "/nix/profile/bin/";
        constexpr uint32_t prefixLength = sizeof(prefix) - 1;
        WmStrCopy(profile, prefix, sizeof(profile));
        WmStrCopy(profile + prefixLength, basename, sizeof(profile) - prefixLength);
        VnodeStat target{}, installed{};
        return VfsStatPath(path, &target) == 0 && !target.isDir && target.ino &&
               VfsStatPath(profile, &installed) == 0 && !installed.isDir &&
               target.dev == installed.dev && target.ino == installed.ino;
    }
    // Only a bare executable name may be searched in installed package bins.
    for (uint32_t i = 0; i < n; ++i) if (path[i] == '/') return false;
    const char* roots[] = {"/nix/profile/bin/", "/nix/bin/"};
    for (const char* root : roots)
    {
        char resolved[288]; uint32_t p = 0;
        while (*root) resolved[p++] = *root++;
        WmStrCopy(resolved + p, path, sizeof(resolved) - p);
        if (LauncherFileExists(resolved)) return true;
    }
    return false;
}

static bool ShortcutRequirementsMet(const char* path)
{
    Vnode* vn = VfsOpen(path, 0);
    if (!vn) return false;
    char buf[1024]; uint64_t offset = 0;
    int rd = VfsRead(vn, buf, sizeof(buf) - 1, &offset);
    VfsClose(vn);
    if (rd <= 0) return false;
    buf[rd] = 0;
    bool declared = false;
    for (char* line = buf; *line;)
    {
        char* end = line;
        while (*end && *end != '\n') ++end;
        const bool more = *end != 0; *end = 0;
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '#') break;
        ++p; while (*p == ' ' || *p == '\t') ++p;
        if (strncmp(p, "Requires:", 9) == 0)
        {
            p += 9; while (*p == ' ' || *p == '\t') ++p;
            char dependency[256]; WmStrCopy(dependency, p, sizeof(dependency));
            uint32_t len = strlen(dependency);
            while (len && (dependency[len - 1] == ' ' || dependency[len - 1] == '\t' || dependency[len - 1] == '\r')) dependency[--len] = 0;
            if (!WmEqualI(dependency, "builtin") && !LauncherFileExists(dependency)) return false;
            declared = true;
        }
        if (!more) break;
        line = end + 1;
    }
    return declared;
}

// A candidate is all-or-nothing: oversized/short/erroring manifests or item
// overflow retain the published snapshot. Limits bound memory and scan work.
static constexpr uint32_t LAUNCHER_MANIFEST_LIMIT = 64 * 1024;
static constexpr uint32_t LAUNCHER_RECORD_LIMIT = 1024;

static bool LauncherLoadDesktopEntries(LauncherSnapshot& launcher)
{
    const char* manifest = "/nix/share/applications/applications.idx";
    VnodeStat st{};
    if (VfsStatPath(manifest, &st) != 0) {
        // ext2 currently conflates missing paths and I/O failures. A missing
        // manifest is valid at first boot, but cannot safely replace a live
        // menu. Removal notifications should publish an empty manifest.
        if (!g_launcherLoaded) return true;
        SerialPuts("WM: launcher manifest unavailable; retaining menu\n");
        return false;
    }
    if (st.isDir || st.size > LAUNCHER_MANIFEST_LIMIT) {
        SerialPuts("WM: launcher manifest exceeds 64KiB limit or is not a file; retaining menu\n");
        return false;
    }
    Vnode* vn = VfsOpen(manifest, 0);
    if (!vn) return false;
    char* buf = static_cast<char*>(kmalloc(LAUNCHER_MANIFEST_LIMIT + 2));
    if (!buf) { VfsClose(vn); return false; }
    uint64_t offset = 0; uint32_t done = 0; bool ok = true;
    while (done <= LAUNCHER_MANIFEST_LIMIT)
    {
        int rd = VfsRead(vn, buf + done, LAUNCHER_MANIFEST_LIMIT + 1 - done, &offset);
        if (rd < 0) { ok = false; break; }
        if (!rd) break;
        done += static_cast<uint32_t>(rd);
    }
    VfsClose(vn);
    if (done > LAUNCHER_MANIFEST_LIMIT || done != st.size) ok = false;
    // Embedded NUL would silently hide records if treated as a C string.
    for (uint32_t i = 0; i < done && ok; ++i) if (!buf[i]) ok = false;
    buf[done] = 0;
    for (char* line = buf; ok && *line;)
    {
        char* end = line; while (*end && *end != '\n') ++end;
        const bool more = *end != 0;
        if (end - line > LAUNCHER_RECORD_LIMIT) { ok = false; break; }
        *end = 0;
        if (end > line && end[-1] == '\r') end[-1] = 0;
        if (!*line) { if (!more) break; line = end + 1; continue; }
        char* fields[5] = {line, nullptr, nullptr, nullptr, nullptr};
        uint32_t count = 1;
        for (char* p = line; *p && count < 5; ++p)
            if (*p == '\t') { *p = 0; fields[count++] = p + 1; }
        if (count < 2 || !fields[0][0] || strlen(fields[1]) >= sizeof(LauncherItem{}.scriptPath) ||
            (count >= 5 && strlen(fields[4]) >= sizeof(LauncherItem{}.appId))) { ok = false; break; }
        char executable[64] = {};
        if (LauncherExecutable(fields[1], executable, sizeof(executable)))
        {
            LauncherItem* item = nullptr;
            for (uint32_t i = 0; i < launcher.count; ++i)
                if (!launcher.items[i].isDesktopEntry && WmEqualI(launcher.items[i].title, fields[0])) { item = &launcher.items[i]; break; }
            if (!item)
            {
                if (launcher.count == WM_LAUNCHER_MAX_ITEMS) { ok = false; break; }
                item = &launcher.items[launcher.count++];
            }
            // The installed executable has passed profile validation. A
            // same-title legacy shortcut must not retain its stale command.
            // This candidate is unpublished; its icon can be replaced here.
            item->valid = true; item->isDesktopEntry = true;
            WmStrCopy(item->title, fields[0], sizeof(item->title));
            WmStrCopy(item->scriptPath, fields[1], sizeof(item->scriptPath));
            WmStrCopy(item->executable, executable, sizeof(item->executable));
            WmStrCopy(item->appId, count >= 5 ? fields[4] : "", sizeof(item->appId));
            kfree(item->iconPixels);item->iconPixels = nullptr;
            if (count >= 3) item->iconPixels = LoadIconFile(fields[2]);
            if (!item->iconPixels) LauncherLoadIcon(item);
        }
        if (!more) break;
        line = end + 1;
    }
    kfree(buf);
    if (!ok) SerialPuts("WM: launcher manifest read/record/capacity error (64KiB/1KiB/128 items); retaining menu\n");
    return ok;
}

static void FreeLauncherSnapshot(LauncherSnapshot* snapshot)
{
    if (!snapshot || snapshot == &g_emptyLauncher) return;
    for (uint32_t i = 0; i < snapshot->count; ++i) kfree(snapshot->items[i].iconPixels);
    kfree(snapshot->applicationIcon); kfree(snapshot->terminalIcon); kfree(snapshot->appsIcon);
    kfree(snapshot);
}

static bool IdentifyLauncherItems(LauncherSnapshot& catalog)
{
    const char* hex="0123456789abcdef";char generation[9];
    for(unsigned i=0;i<8;++i)generation[i]=hex[(catalog.generation>>((7-i)*4))&15];
    generation[8]=0;
    for(uint32_t i=0;i<catalog.count;++i) {
        auto& item=catalog.items[i];LauncherIdentity identity;
        identity.field(item.isDesktopEntry?"desktop":"shortcut");
        // New manifests carry the exact desktop-file basename. For legacy
        // manifests without it, use the command and label as an explicit fallback.
        if(item.isDesktopEntry&&item.appId[0])identity.field(item.appId);
        else {identity.field(item.scriptPath);if(item.isDesktopEntry)identity.field(item.title);}
        identity.finish(item.catalogId);
        for(uint32_t j=0;j<i;++j)if(!strcmp(item.catalogId,catalog.items[j].catalogId)) {
            SerialPuts("WM: duplicate/colliding launcher identity; retaining previous catalog\n");return false;
        }
        LauncherIdentity spec;spec.field(item.catalogId);spec.field(item.scriptPath);
        spec.field(item.isDesktopEntry?"desktop":"shortcut");spec.field(generation);
        spec.finish(item.specification);
    }
    return true;
}

static LauncherSnapshot* BuildLauncherSnapshot(uint32_t generation)
{
    auto* snapshot = static_cast<LauncherSnapshot*>(kmalloc(sizeof(LauncherSnapshot)));
    if (!snapshot) return nullptr;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->generation = generation;
    snapshot->applicationIcon = LoadIconFile("application.rgba");
    snapshot->terminalIcon = LoadIconFile("terminal.rgba");
    snapshot->appsIcon = LoadIconFile("applications.rgba");
    bool ok = true;
    Vnode* dir = VfsOpen("/boot/SHORTCUTS", 0);
    if (dir)
    {
        DirEntry de; uint32_t cookie = 0; int rd;
        while ((rd = VfsReaddir(dir, &de, &cookie)) == 1)
        {
            if (de.isDir) continue;
            LauncherItem item{};
            const char* prefix = "/boot/SHORTCUTS/";
            const uint32_t prefixLen = strlen(prefix);
            if (prefixLen + strlen(de.name) >= sizeof(item.scriptPath)) { ok = false; break; }
            WmStrCopy(item.scriptPath, prefix, sizeof(item.scriptPath));
            WmStrCopy(item.scriptPath + prefixLen, de.name, sizeof(item.scriptPath) - prefixLen);
            if (!ShortcutRequirementsMet(item.scriptPath)) continue;
            if (snapshot->count == WM_LAUNCHER_MAX_ITEMS) { ok = false; break; }
            if (!ParseShortcutMetadata(item.scriptPath, item))
            {
                uint32_t ti = 0;
                for (uint32_t j = 0; de.name[j] && de.name[j] != '.' && ti + 1 < sizeof(item.title); ++j)
                    item.title[ti++] = de.name[j];
                item.title[ti] = 0;
            }
            item.valid = true;
            LauncherLoadIcon(&item);
            snapshot->items[snapshot->count++] = item;
        }
        if (rd < 0) ok = false;
        VfsClose(dir);
    }
    else if (g_launcherLoaded) {
        SerialPuts("WM: shortcut directory unavailable; retaining menu\n");
        ok = false;
    }
    if (ok) ok = LauncherLoadDesktopEntries(*snapshot);
    if (ok) ok = IdentifyLauncherItems(*snapshot);
    if (!ok) {
        SerialPuts("WM: launcher rebuild failed; previous snapshot retained\n");
        FreeLauncherSnapshot(snapshot);
        return nullptr;
    }
    snapshot->builtTick=ApicTickCount();
    return snapshot;
}

static bool g_launchInhibited=false;
static void LauncherWorker(void*)
{
    uint32_t attempted = 0;
    { LauncherReadGuard guard; attempted = guard.get().generation; }
    for (;;)
    {
        LauncherSnapshot* disposable = nullptr;
        SpinLockAcquire(&g_launcherLock);
        if (g_launcherRetired && !__atomic_load_n(&g_launcherRetired->refs, __ATOMIC_ACQUIRE)) {
            disposable = g_launcherRetired;
            g_launcherRetired = nullptr;
        }
        const bool occupied = g_launcherPending || g_launcherRetired;
        SpinLockRelease(&g_launcherLock);
        FreeLauncherSnapshot(disposable);
        const uint32_t requested = __atomic_load_n(&g_launcherRequested, __ATOMIC_ACQUIRE);
        if (!occupied && requested != attempted)
        {
            LauncherSnapshot* next = BuildLauncherSnapshot(requested);
            attempted = requested; // failure retries on the next notification/open
            if (next)
            {
                SpinLockAcquire(&g_launcherLock);
                g_launcherPending = next;
                SpinLockRelease(&g_launcherLock);
                CompositorWake();
            }
        }
        char command[128];
        bool desktop = false;
        uint64_t token = 0;
        SpinLockAcquire(&g_launchLock);
        int launch = g_launchInhibited?-1:g_launches.Take(ApicTickCount());
        if (launch >= 0) {
            token = g_launches.entries[launch].token;
            g_launchHistory.Start(token,ApicTickCount());
            WmStrCopy(command, g_launchRequests[launch].command, sizeof(command));
            desktop = g_launchRequests[launch].desktop;
        }
        SpinLockRelease(&g_launchLock);
        if (token) {
            Process* self = ProcessCurrent();
            const uint64_t previous = self->launchId;
            self->launchId = token;
            int result;
            char executable[64];
            const bool available=desktop?LauncherExecutable(command,executable,sizeof(executable)):ShortcutRequirementsMet(command);
            if(!available)result=-2;
            else if (desktop) {
                char script[512];
                // Helpers can fail independently of a successful app spawn.
                // Start the CPU waylandd only when no provider (including the
                // GPU desktop's INTLWL.ELF) already serves the socket; see
                // wayland_provider.h.
                if (WaylandProviderRunning([](const char* base) { return SchedulerFindProcessByBaseName(base) != nullptr; }))
                    ShellExecString("set wm\nset vfb none\n");
                else
                    ShellExecString("set wm\nset vfb none\nrun --service --once waylandd\n");
                const char* hdr = "run ";
                uint32_t si = 0;
                while (*hdr && si < sizeof(script) - 2) script[si++] = *hdr++;
                const char* exec = command;
                while (*exec && si < sizeof(script) - 2) script[si++] = *exec++;
                script[si] = 0;
                result = ShellLaunchCommand(script);
            } else result = ShellExecScript(command);
            self->launchId = previous;
            SpinLockAcquire(&g_launchLock);
            g_launches.Complete(token, result >= 0, ApicTickCount(),result,true);
            g_launchHistory.Complete(token,result,ApicTickCount());
            SpinLockRelease(&g_launchLock);
            SerialPrintf("WM: launch token %lu dispatch %s result=%d\n", token, result >= 0 ? "complete" : "failed",result);
            CompositorMarkDirty();
            CompositorWake();
        }
        // One worker, coalesced requests, no busy waiting or retained rebuild
        // list. A slow reader delays reclamation and subsequent builds.
        SchedulerSleepMs(250);
    }
}

static void LauncherStartWorker()
{
    bool expected = false;
    if (!__atomic_compare_exchange_n(&g_launcherWorkerStarted, &expected, true,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    Process* worker = KernelThreadCreate("launcher", LauncherWorker, nullptr, 3);
    if (!worker) {
        __atomic_store_n(&g_launcherWorkerStarted, false, __ATOMIC_RELEASE);
        SerialPuts("WM: launcher worker allocation failed\n");
        return;
    }
    SchedulerAddProcess(worker);
}

void WmLauncherLoad()
{
    if (g_launcherLoaded) return;
    const uint32_t generation = __atomic_load_n(&g_launcherRequested, __ATOMIC_ACQUIRE);
    LauncherSnapshot* snapshot = BuildLauncherSnapshot(generation);
    if (snapshot)
    {
        // Initial activation precedes rendering; later publication belongs
        // exclusively to the compositor's frame-boundary hook.
        { VfbGuard windows;
          SpinLockAcquire(&g_launcherLock);
          g_launcherActive = snapshot;
          if(g_launcherSnapshotEpoch)++g_launcherSnapshotEpoch;
          SpinLockRelease(&g_launcherLock);
        }
        SerialPrintf("WM: launcher initial generation=%u items=%u\n", generation, snapshot->count);
    }
    g_launcherLoaded = true;
}

void WmLauncherClose() { g_launcherOpen = false; g_launcherPower.Reset(); }

void WmLauncherToggle()
{
    // Loading belongs to WmSetActive (shell context), never the input/render thread.
    if (!g_launcherLoaded) return;
    g_launcherOpen = !g_launcherOpen;
    g_launcherPower.Reset();
    if (g_launcherOpen) {
        PointerActivationCancel();
        WmVolumePopupClose(); // the two popups share one overlay slot
        g_launcherError = 0;
        g_launcherScroll = 0;
        WmRequestLauncherRefresh(); // also catches installs/removals outside the installer
    }
}

bool WmLauncherVisible()
{
    return g_launcherOpen;
}

#include "wm_shell_launcher.inc"

// Content key of one shell surface as it would render now (stage 1.4): its
// display list hashed with what the list does not carry. Compositor thread
// only (shares the shell list storage). 0: the surface draws nothing.
uint64_t WmShellSurfaceKey(const WmShellSurface& s, uint32_t screenW, uint32_t screenH,
                           uint64_t uptimeMs, int32_t mx, int32_t my)
{
    const FontAtlas& font = WmDesktopFont();
    constexpr uint64_t prime = 0x100000001b3ull;
    uint64_t seed = 0x6b65792d7368656cull;
    auto word = [&](uint64_t v) { seed = (seed ^ v) * prime; };
    word(uint64_t(s.kind) << 32 | uint32_t(s.material));
    word(uint64_t(uint32_t(s.x)) << 32 | uint32_t(s.y));
    word(uint64_t(s.w) << 32 | s.h);
    word(uint64_t(screenW) << 32 | screenH);
    word(reinterpret_cast<uintptr_t>(&font));
    word(uint64_t(uint32_t(font.lineHeight)) << 32 | uint32_t(font.glyphCount));
    word(uint64_t(CompositorGlassActive()) << 32 | CompositorMaterialOpacity());
    uint64_t key = 0;
    switch (s.kind) {
    case WmShellSurfaceKind::Taskbar: WmTaskbarSurface(nullptr, 0, screenW, screenH, uptimeMs, mx, my, seed, &key); break;
    case WmShellSurfaceKind::Launcher: WmLauncherSurface(nullptr, 0, screenW, screenH, mx, my, seed, &key); break;
    case WmShellSurfaceKind::VolumePopup: WmVolumePopupSurface(nullptr, 0, screenW, screenH, mx, my, seed, &key); break;
    case WmShellSurfaceKind::TrayTooltip: WmTrayTooltipSurface(nullptr, 0, screenW, screenH, mx, my, seed, &key); break;
    case WmShellSurfaceKind::BootSummary: WmBootSummarySurface(nullptr, 0, screenW, screenH, seed, &key); break;
    }
    return key;
}

static int AdmitLauncherItem(const LauncherSnapshot& catalog,int itemIdx,bool tracked,
                             uint64_t* token,bool* coalesced)
{
    if(itemIdx<0||itemIdx>=static_cast<int>(catalog.count)||!catalog.items[itemIdx].valid)return -2;
    LauncherStartWorker();
    if(!__atomic_load_n(&g_launcherWorkerStarted,__ATOMIC_ACQUIRE))return -12;
    const auto& item=catalog.items[itemIdx];int slot;
    SpinLockAcquire(&g_launchLock);
    if(g_launchInhibited){SpinLockRelease(&g_launchLock);return -108;}
    uint64_t pending=0;
    for(const auto& e:g_launches.entries)if((e.busy()||e.inFlight())&&LaunchFeedbackTable::Exact(e.identity,item.catalogId)) {pending=e.token;break;}
    // Reserve durable tracking capacity before any launch admission side effect.
    if(tracked&&!g_launchHistory.CanTrack(pending)){SpinLockRelease(&g_launchLock);return -28;}
    bool created=false;
    slot=g_launches.Begin(ApicTickCount(),__atomic_load_n(&g_windowIdentity,__ATOMIC_ACQUIRE),
        item.title,item.appId,item.executable,item.catalogId,item.specification,&created,catalog.generation);
    if(slot>=0) {
        const auto& entry=g_launches.entries[slot];*token=entry.token;*coalesced=!created;
        if(created) {
            auto& request=g_launchRequests[slot];
            WmStrCopy(request.command,item.scriptPath,sizeof(request.command));
            request.desktop=item.isDesktopEntry;request.hasIcon=item.iconPixels!=nullptr;
            if(request.hasIcon)memcpy(request.icon,item.iconPixels,sizeof(request.icon));
        }
        if(tracked)g_launchHistory.Track(entry,created);
        else g_launchHistory.ShareWithUser(entry.token);
    }
    SpinLockRelease(&g_launchLock);
    g_launcherError=slot<0?slot:0;g_launcherOpen=slot<0;
    if(slot>=0) {
        g_taskbarPageTarget=-10-slot;__atomic_store_n(&g_launchBusy,true,__ATOMIC_RELEASE);
    }else SerialPuts(slot==-2?"WM: same app has pending launch from older catalog\n":"WM: launcher request queue full\n");
    CompositorMarkDirty();CompositorWake();
    return slot>=0?0:slot==-2?-16:-11;
}

void WmSetLaunchInhibited(bool inhibited) {
    SpinLockAcquire(&g_launchLock);g_launchInhibited=inhibited;
    if(inhibited)for(auto& entry:g_launches.entries)if(entry.phase==LaunchPhase::Queued) {
        const auto token=entry.token;entry={};
        const int index=g_launchHistory.Find(token);
        if(index>=0){g_launchHistory.records[index].cancelled=true;g_launchHistory.Complete(token,-125,ApicTickCount());}
    }
    SpinLockRelease(&g_launchLock);
}
bool WmLauncherDispatching() {
    bool dispatching=false;SpinLockAcquire(&g_launchLock);
    for(const auto& entry:g_launches.entries)if(entry.inFlight())dispatching=true;
    SpinLockRelease(&g_launchLock);return dispatching;
}
static void LauncherPowerRun(LauncherPowerCommand command)
{
    switch(command) {
    case LauncherPowerCommand::None:break;
    case LauncherPowerCommand::Restart:SerialPuts("WM: launcher restart confirmed\n");ShutdownDesktopRequest(true);break;
    case LauncherPowerCommand::Shutdown:SerialPuts("WM: launcher shutdown confirmed\n");ShutdownDesktopRequest(false);break;
    case LauncherPowerCommand::CancelShutdown:ShutdownDesktopCancel();break;
    }
    CompositorMarkDirty();CompositorWake();
}

bool WmLauncherPowerPrompt() { return g_launcherOpen && g_launcherPower.prompt(); }

void WmLauncherPowerKey(uint8_t scan)
{
    if(!WmLauncherPowerPrompt())return;
    const LauncherPowerKey key=scan==0x01?LauncherPowerKey::Escape:scan==0x1C?LauncherPowerKey::Enter:
        scan==SC_EXT_LEFT?LauncherPowerKey::Previous:LauncherPowerKey::Next;
    LauncherPowerRun(g_launcherPower.Key(key,ApicTickCount()));
}

void WmLauncherExec(int itemIdx)
{
    constexpr int footer=static_cast<int>(WM_LAUNCHER_MAX_ITEMS);
    if(itemIdx>=footer){
        LauncherPowerRun(g_launcherPower.Activate(static_cast<LauncherPowerTarget>(itemIdx-footer),ApicTickCount()));
        return;
    }
    g_launcherPower.Reset(); // any other launcher press dismisses the prompt
    LauncherReadGuard guard;uint64_t token=0;bool coalesced=false;
    AdmitLauncherItem(guard.get(),itemIdx,false,&token,&coalesced);
}

int WmLauncherActivate(const char* appId,uint32_t generation,uint64_t* token,bool* coalesced)
{
    LauncherReadGuard guard;const auto& catalog=guard.get();
    if(!generation||catalog.generation!=generation)return -116;
    for(uint32_t i=0;i<catalog.count;++i)if(!strcmp(catalog.items[i].catalogId,appId))
        return AdmitLauncherItem(catalog,static_cast<int>(i),true,token,coalesced);
    return -2;
}

bool WmSnapshotLaunch(uint64_t token,LaunchRecord* out)
{
    SpinLockAcquire(&g_launchLock);int index=g_launchHistory.Find(token);
    if(index>=0)*out=g_launchHistory.records[index];
    SpinLockRelease(&g_launchLock);return index>=0;
}

uint32_t WmSnapshotLaunches(LaunchRecord* out,uint32_t capacity)
{
    uint32_t count=0;SpinLockAcquire(&g_launchLock);
    for(const auto& r:g_launchHistory.records)if(r.token&&count<capacity)out[count++]=r;
    SpinLockRelease(&g_launchLock);return count;
}

void WmCancelBridgeQueuedLaunches(uint32_t* cancelled,uint32_t* startedOrShared)
{
    *cancelled=*startedOrShared=0;
    SpinLockAcquire(&g_launchLock);
    for(auto& record:g_launchHistory.records) {
        if(!record.token||record.cancelled||record.dispatchFinished)continue;
        const int slot=g_launches.Find(record.token);
        if(!record.started&&record.bridgeOnly&&slot>=0&&g_launches.entries[slot].phase==LaunchPhase::Queued) {
            record.cancelled=true;g_launches.entries[slot]={};++*cancelled;
        }else if(record.started||(slot>=0&&g_launches.entries[slot].phase==LaunchPhase::Queued))++*startedOrShared;
    }
    SpinLockRelease(&g_launchLock);CompositorMarkDirty();
}

// ---------------------------------------------------------------------------
// Per-window VFB API (Phase A of wayland↔WM unification).
// ---------------------------------------------------------------------------

Window* WmFindWindowById(Process* proc, uint32_t wmId)
{
    if (!proc || wmId == 0) return nullptr;
    int idx = static_cast<int>(wmId) - 1;
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return nullptr;
    Window& w = g_windows[idx];
    if (w.proc != proc || w.wmId != wmId) return nullptr;
    return &w;
}

uint64_t WmWindowCreationIdentity(Process* proc, uint32_t wmId)
{
    VfbGuard guard;
    const Window* w = WmFindWindowById(proc, wmId);
    return w ? w->creationIdentity : 0;
}

bool WmGpuSourceProviderMatches(uint32_t window,uint64_t incarnation,uint32_t pid,uint64_t providerIncarnation)
{
    if(!window || window>WM_MAX_WINDOWS || !incarnation || !pid || !providerIncarnation)return false;
    VfbGuard guard;const Window& w=g_windows[window-1];
    return w.proc && w.wmId==window && w.creationIdentity==incarnation && (w.proc->tgid?w.proc->tgid:w.proc->pid)==pid &&
        (w.proc->isThread?w.proc->leaderIncarnation:w.proc->incarnation)==providerIncarnation;
}

bool WmClientScanoutWindowMatches(uint32_t window,uint64_t incarnation,uint32_t width,uint32_t height)
{
    if(!window || window>WM_MAX_WINDOWS || !incarnation)return false;
    VfbGuard guard;
    const int idx=__atomic_load_n(&g_focusedIdx,__ATOMIC_ACQUIRE);
    if(idx!=static_cast<int>(window)-1)return false;
    const Window& w=g_windows[idx];
    return w.proc && w.wmId==window && w.creationIdentity==incarnation && w.visible && !w.minimized &&
        w.noChrome && !w.clientX() && !w.clientY() && w.clientW==width && w.clientH==height;
}

bool WmSnapshotFocus(uint32_t* window, uint64_t* incarnation, uint32_t* flags)
{
    if (!window || !incarnation || !flags) return false;
    *window = 0; *incarnation = 0; *flags = 0;
    VfbGuard guard;
    const int idx = __atomic_load_n(&g_focusedIdx, __ATOMIC_ACQUIRE);
    if (idx < 0 || idx >= static_cast<int>(WM_MAX_WINDOWS)) return false;
    const Window& w = g_windows[idx];
    if (!w.proc || !w.wmId) return false;
    *window = w.wmId; *incarnation = w.creationIdentity;
    *flags = BROOK_WINDOW_STATE_FOCUSED |
        (w.visible ? BROOK_WINDOW_STATE_VISIBLE : 0) |
        (w.minimized ? BROOK_WINDOW_STATE_MINIMIZED : 0) |
        (w.fullscreen ? BROOK_WINDOW_STATE_FULLSCREEN :
         w.state == WindowState::Maximized ? BROOK_WINDOW_STATE_MAXIMIZED : 0) |
        (w.noChrome ? BROOK_WINDOW_STATE_CLIENT_CHROME : 0);
    return true;
}

int WmSnapshotOwnedWindowState(Process* proc, uint32_t wmId,
                               uint64_t expectedIncarnation, BrookWindowState* out)
{
    if (!proc || !out) return -22; // EINVAL
    VfbGuard guard;
    const Window* w = WmFindWindowById(proc, wmId);
    if (!w) return -2; // ENOENT (also hides foreign windows)
    if (expectedIncarnation && expectedIncarnation != w->creationIdentity)
        return -116; // ESTALE
    BrookWindowState state{};
    state.version = BROOK_WINDOW_STATE_VERSION;
    state.window = w->wmId;
    state.incarnation = w->creationIdentity;
    state.client_x = w->clientX(); state.client_y = w->clientY();
    state.width = w->clientW; state.height = w->clientH;
    state.flags = (w->visible ? BROOK_WINDOW_STATE_VISIBLE : 0) |
        (w->focused ? BROOK_WINDOW_STATE_FOCUSED : 0) |
        (w->minimized ? BROOK_WINDOW_STATE_MINIMIZED : 0) |
        (w->fullscreen ? BROOK_WINDOW_STATE_FULLSCREEN :
         w->state == WindowState::Maximized ? BROOK_WINDOW_STATE_MAXIMIZED : 0) |
        (w->noChrome ? BROOK_WINDOW_STATE_CLIENT_CHROME : 0) |
        (w->focusable ? BROOK_WINDOW_STATE_FOCUSABLE : 0);
    *out = state;
    return 0;
}

WmCreateWindowResult WmCreateWindowForProcess(Process* proc,
                                               uint16_t clientW,
                                               uint16_t clientH,
                                               const char* title,
                                               bool focusable,
                                               bool nativeClient)
{
    WmCreateWindowResult res = {};
    VfbGuard guard;

    if (!proc || clientW == 0 || clientH == 0) return res;
    // Provider buffers define their exact client extent (including tiny menus
    // and X11 utility windows). Interactive chrome resize minima are separate.

    // Cascade placement: each new window offset 30px from the prior count.
    static int s_cascade = 0;
    int16_t winX = static_cast<int16_t>(40 + (s_cascade % 8) * 30);
    int16_t winY = static_cast<int16_t>(40 + (s_cascade % 8) * 30);
    s_cascade++;

    int idx = CreateWindowLocked(proc, winX, winY, clientW, clientH, title, 1,
                             focusable);
    if (idx < 0) return res;

    Window& w = g_windows[idx];

    // Allocate the VFB pages kernel-side.
    // Shared display-server windows must prove their client separately.
    // A native caller can explicitly identify only itself; retain the identity
    // captured by CreateWindowLocked, never a userspace-supplied PID.
    if (!nativeClient) {
        w.clientPid=0;w.clientIncarnation=0;w.clientLaunchId=0;
        w.clientOrigin=WmClientOrigin::Unknown;
    }
    uint32_t stride = clientW;
    uint64_t bytes  = static_cast<uint64_t>(stride) * clientH * 4;
    uint64_t pages  = (bytes + 4095) / 4096;

    VirtualAddress kAddr = VmmAllocPages(pages, VMM_WRITABLE,
                                          MemTag::Device, proc->pid);
    if (!kAddr)
    {
        SerialPrintf("WM: VFB alloc failed for window '%s' (%ux%u)\n",
                     w.title, clientW, clientH);
        DestroyWindowLocked(idx);
        return res;
    }
    auto* kVfb = reinterpret_cast<uint32_t*>(kAddr.raw());
    memset(kVfb, 0, bytes);

    auto* retirement=CompositorPrepareVfbRetirement(kAddr.raw(),pages);
    if (!retirement) { VmmFreePages(kAddr,pages);DestroyWindowLocked(idx);return res; }
    auto* backing=UserMappingBufferSnapshot(kAddr.raw(),pages*4096);
    int64_t mapped=backing?UserMappingBufferAlias(ProcessCurrent(),proc->addressSpace,backing,3):-12;
    if (mapped<0) {
        MappingBackingUnref(backing);CompositorCancelVfbRetirement(retirement);
        VmmFreePages(kAddr,pages);DestroyWindowLocked(idx);return res;
    }
    uint64_t userBase=static_cast<uint64_t>(mapped);
    w.vfbBacking=backing;
    w.vfbRetirement=retirement;

    w.vfb       = kVfb;
    w.vfbStride = stride;
    w.vfbWidth=clientW;w.vfbHeight=clientH;
    w.vfbBytes  = pages * 4096;
    w.vfbUser   = reinterpret_cast<void*>(userBase);
    AddressSpaceRef(proc->addressSpace);
    w.vfbAddressSpace = proc->addressSpace;
    w.vfbDirty  = 1;  // first frame

    SerialPrintf("WM: window %d '%s' VFB %ux%u kvfb=0x%lx user=0x%lx pid=%u\n",
                 idx, w.title, clientW, clientH,
                 reinterpret_cast<uint64_t>(kVfb), userBase, proc->pid);

    res.wmId      = w.wmId;
    res.vfbUser   = w.vfbUser;
    res.vfbStride = w.vfbStride;
    return res;
}

void WmDestroyWindowById(Process* proc, uint32_t wmId)
{
    VfbGuard guard;
    Window* w = WmFindWindowById(proc, wmId);
    if (!w) return;
    int idx = static_cast<int>(wmId) - 1;
    DestroyWindowLocked(idx);
}

int WmResizeVfbForProcess(Process* proc, uint32_t wmId,
                          uint16_t newW, uint16_t newH,
                          WmCreateWindowResult* outResult)
{
    if (!proc || !outResult) return -22; // -EINVAL
    if (!newW || !newH) return -22;

    VfbGuard guard;
    Window* w = WmFindWindowById(proc, wmId);
    if (!w) return -2; // -ENOENT
    if (!w->vfb) return -22; // -EINVAL — only for WM-API windows

    // Compute new size; bail if identical to current to avoid churn.
    uint32_t newStride = newW;
    uint64_t newBytes  = static_cast<uint64_t>(newStride) * newH * 4;
    uint64_t newPages  = (newBytes + 4095) / 4096;
    uint64_t roundedBytes = newPages * 4096;

    // Allocate new kernel-side VFB pages.
    VirtualAddress newKAddr = VmmAllocPages(newPages, VMM_WRITABLE,
                                             MemTag::Device, proc->pid);
    if (!newKAddr)
    {
        SerialPrintf("WM: resize_vfb wm=%u alloc %lu pages failed\n", wmId, newPages);
        return -12; // -ENOMEM
    }
    auto* newKVfb = reinterpret_cast<uint32_t*>(newKAddr.raw());
    memset(newKVfb, 0, roundedBytes);

    auto* retirement=CompositorPrepareVfbRetirement(newKAddr.raw(),newPages);
    if (!retirement) { VmmFreePages(newKAddr,newPages);return -12; }
    auto* newBacking=UserMappingBufferSnapshot(newKAddr.raw(),newPages*4096);
    int64_t mapped=newBacking?UserMappingBufferAlias(ProcessCurrent(),proc->addressSpace,newBacking,3):-12;
    if (mapped<0) {
        MappingBackingUnref(newBacking);CompositorCancelVfbRetirement(retirement);
        VmmFreePages(newKAddr,newPages);return -12;
    }
    uint64_t newUserBase=static_cast<uint64_t>(mapped);

    // Snapshot old mapping so the compositor can finish any in-flight
    // blit using w->vfb before we tear it down.
    auto* oldRetirement=w->vfbRetirement;
    MappingBacking* oldBacking = w->vfbBacking;
    AddressSpaceState* oldSpace = w->vfbAddressSpace;

    // Atomically swap the kernel VFB pointer + sizes so the compositor
    // sees a consistent view from this point on.  Mark dirty so the
    // first blit at the new size happens before any client commit.
    w->externalCaptured=false;w->externalW=w->externalH=0;
    w->publishedGeneration=NextPublication(*w);
    w->vfbRetirement = retirement;
    w->vfbBacking = newBacking;
    w->vfb       = newKVfb;
    w->vfbStride = newStride;
    w->vfbWidth=newW;w->vfbHeight=newH;
    w->vfbBytes  = roundedBytes;
    w->vfbUser   = reinterpret_cast<void*>(newUserBase);
    AddressSpaceRef(proc->addressSpace);
    w->vfbAddressSpace = proc->addressSpace;
    w->clientW   = newW;
    w->clientH   = newH;
    w->vfbDirty  = 1;

    if (oldSpace && oldBacking && UserMappingRemoveBacking(ProcessCurrent(),oldSpace,oldBacking)<0)
        KernelPanic("WM: invalid framebuffer mappings during resize");
    MappingBackingUnref(oldBacking);AddressSpaceUnref(oldSpace);
    // Only the independent kernel-buffer owner waits for renderer retirement.
    CompositorRetireVfb(oldRetirement);

    SerialPrintf("WM: resize_vfb wm=%u -> %ux%u stride=%u kvfb=0x%lx user=0x%lx\n",
                 wmId, newW, newH, newStride,
                 reinterpret_cast<uint64_t>(newKVfb), newUserBase);

    outResult->wmId      = wmId;
    outResult->vfbUser   = w->vfbUser;
    outResult->vfbStride = w->vfbStride;
    // Client-accepted sizes can differ from the WM offer. Invalidate cached
    // caption geometry along with publishing the replacement client buffer.
    CompositorMarkDirty();CompositorWake();
    return 0;
}

bool WmSnapshotVfb(const Window* window,uint32_t** pixels,uint32_t* stride,uint64_t* bytes) {
    VfbGuard guard;
    *pixels=window->vfb;*stride=window->vfbStride;*bytes=window->vfbBytes;
    return *pixels && *stride && *bytes;
}

// g_vfbLock covers the ordered metadata and pin acquisition. The renderer
// never receives w.vfb: its old kernel virtual address can be retired after
// resize even while the backing's physical pages remain retained.
static bool DesktopLayerLocked(const Window& w,WmDesktopLayer& out,bool retain) {
    if(!w.proc || !w.visible || w.minimized || w.externalCaptured ||
       w.publicationGeneration==~uint64_t{0} || !w.creationIdentity || !w.wmId || !w.upscale)return false;
    MappingBacking* backing=nullptr;uint64_t generation=0,bytes=0;
    uint32_t width=0,height=0,stride=0;bool legacy=!w.vfb;
    if(!legacy){
        backing=w.vfbBacking;generation=w.publishedGeneration;bytes=w.vfbBytes;
        width=w.vfbWidth;height=w.vfbHeight;stride=w.vfbStride;
        if(!backing || !width || !height || stride<width ||
           uint64_t(stride)>~uint64_t{0}/4/height || uint64_t(stride)*height*4>bytes)return false;
        if(retain)MappingBackingRef(backing);
    }else{
        auto* proc=w.proc;KMutexLock(&proc->framebufferLock);
        backing=proc->fbBacking;generation=__atomic_load_n(&proc->fbPublicationGeneration,__ATOMIC_ACQUIRE);
        bytes=proc->fbVirtualSize;width=proc->fbVfbWidth;height=proc->fbVfbHeight;stride=proc->fbVfbStride;
        const bool valid=backing && proc->fbVirtual && generation!=~uint64_t{0} && width && height && stride>=width &&
            uint64_t(stride)<=~uint64_t{0}/4/height && uint64_t(stride)*height*4<=bytes;
        if(valid && retain)MappingBackingRef(backing);
        KMutexUnlock(&proc->framebufferLock);if(!valid)return false;
    }
    out={backing,w.creationIdentity,generation,bytes,w.wmId,width,height,stride,w.clientW,w.clientH,
         w.clientX(),w.clientY(),w.upscale,uint8_t(legacy),0,0,0,0,0,{}, {},0,0,0,0,0,0};
    out.focused=w.focused;out.noChrome=w.noChrome?(w.captionOverlay?2:1):0;out.state=uint8_t(w.state);
    out.opacity=w.opacity;out.blurRadius=w.blurRadius;
    __builtin_memcpy(out.title,w.title,sizeof(out.title));__builtin_memcpy(out.appId,w.appId,sizeof(out.appId));
    out.ownerPid=w.proc->pid;out.ownerIncarnation=w.proc->incarnation;
    return true;
}
bool WmAcquireDesktopLayers(WmDesktopLayers* out) {
    if(!out)return false;
    *out={};bool success=true;
    { VfbGuard guard;int sorted[WM_MAX_WINDOWS];const uint32_t count=WmGetZOrder(sorted,WM_MAX_WINDOWS);
      out->theme=WmGetTheme();out->appearance=WmGetAppearance();out->scale=WmGetAppliedScale();out->settingsGeneration=g_chromeSettingsEpoch;
      SpinLockAcquire(&g_launcherLock);out->launcherGeneration=g_launcherSnapshotEpoch;SpinLockRelease(&g_launcherLock);
      if(!out->launcherGeneration || !out->settingsGeneration)success=false;
      for(uint32_t i=0;success && i<count;++i){
          if(!DesktopLayerLocked(g_windows[sorted[i]],out->layers[i],true)){success=false;break;}
          ++out->count;
      }
    }
    if(success && ((ApicTickCount()/500)&1)==0){
        for(uint32_t i=0;i<out->count;++i){auto& layer=out->layers[i];TerminalCaret caret{};
            if(!TerminalSnapshotCaret(layer.ownerPid,layer.ownerIncarnation,&caret))continue;
            // Inversion before alpha compositing is only equivalent for opaque legacy buffers.
            if(!layer.legacyOpaque){success=false;break;}
            layer.caretX=caret.x;layer.caretY=caret.y;layer.caretWidth=caret.width;layer.caretHeight=caret.height;
        }
    }
    if(!success)WmReleaseDesktopLayers(out);
    return success;
}
bool WmRetainDesktopLayers(const WmDesktopLayers& source,WmDesktopLayers* out) {
    if(!out || out==&source || source.count>WM_MAX_WINDOWS)return false;
    VfbGuard guard;
    for(uint32_t i=0;i<source.count;++i)if(!source.layers[i].backing)return false;
    *out=source;
    for(uint32_t i=0;i<source.count;++i)MappingBackingRef(source.layers[i].backing);
    return true;
}
static bool DesktopLayersCurrentLocked(const WmDesktopLayers& layers,bool visual=true) {
    if(layers.count>WM_MAX_WINDOWS)return false;
    if(!layers.settingsGeneration || layers.settingsGeneration!=g_chromeSettingsEpoch || layers.theme!=WmGetTheme() ||
       layers.appearance!=WmGetAppearance() || layers.scale!=WmGetAppliedScale())return false;
    SpinLockAcquire(&g_launcherLock);const auto launcherGeneration=g_launcherSnapshotEpoch;SpinLockRelease(&g_launcherLock);
    if(layers.launcherGeneration!=launcherGeneration)return false;
    int sorted[WM_MAX_WINDOWS];
    const uint32_t count=WmGetZOrder(sorted,WM_MAX_WINDOWS);
    if(count!=layers.count)return false;
    for(uint32_t i=0;i<count;++i){
        WmDesktopLayer now{};const auto& old=layers.layers[i];
        if(!DesktopLayerLocked(g_windows[sorted[i]],now,false) ||
           now.backing!=old.backing || now.incarnation!=old.incarnation || now.generation!=old.generation ||
           now.bytes!=old.bytes || now.window!=old.window || now.width!=old.width || now.height!=old.height ||
           now.stride!=old.stride || now.clientWidth!=old.clientWidth || now.clientHeight!=old.clientHeight ||
           now.clientX!=old.clientX || now.clientY!=old.clientY || now.upscale!=old.upscale ||
           now.legacyOpaque!=old.legacyOpaque || (visual && now.focused!=old.focused) || now.noChrome!=old.noChrome ||
           now.state!=old.state || now.opacity!=old.opacity || now.blurRadius!=old.blurRadius)return false;
        if(visual){
            for(uint32_t j=0;j<sizeof(now.title);++j)if(now.title[j]!=old.title[j])return false;
            for(uint32_t j=0;j<sizeof(now.appId);++j)if(now.appId[j]!=old.appId[j])return false;
        }
    }
    return true;
}
bool WmDesktopLayersCurrent(const WmDesktopLayers& layers) {
    VfbGuard guard;return DesktopLayersCurrentLocked(layers);
}
bool WmDesktopLayersCaptureCurrent(const WmDesktopLayers& layers) {
    VfbGuard guard;return DesktopLayersCurrentLocked(layers,false);
}
static bool DesktopLayersPresentableLocked(const WmDesktopLayers& layers) {
    if(layers.count>WM_MAX_WINDOWS)return false;
    int sorted[WM_MAX_WINDOWS];
    if(WmGetZOrder(sorted,WM_MAX_WINDOWS)!=layers.count)return false;
    // Compare the set of identities, not their order: raises and moves may be
    // reflected by the next frame. Do not pair a recycled ID with old pixels.
    for(uint32_t i=0;i<layers.count;++i){
        const auto& old=layers.layers[i];bool found=false;
        for(uint32_t j=0;j<layers.count;++j){
            WmDesktopLayer now{};
            if(!DesktopLayerLocked(g_windows[sorted[j]],now,false))return false;
            if(now.window!=old.window || now.incarnation!=old.incarnation)continue;
            if(now.backing!=old.backing || now.width!=old.width || now.height!=old.height ||
               now.stride!=old.stride || now.bytes!=old.bytes || now.legacyOpaque!=old.legacyOpaque)return false;
            found=true;break;
        }
        if(!found)return false;
    }
    return true;
}
int64_t WmDesktopLayersUseIfPresentable(const WmDesktopLayers& layers,void* context,int64_t (*use)(void*)) {
    if(!use || layers.count>WM_MAX_WINDOWS)return -22;
    VfbGuard guard;if(!DesktopLayersPresentableLocked(layers))return -116;
    return use(context);
}
int64_t WmDesktopLayersUseIfBothPresentable(const WmDesktopLayers& base,const WmDesktopLayers& next,
                                             void* context,int64_t (*use)(void*)) {
    if(!use || base.count>WM_MAX_WINDOWS || next.count>WM_MAX_WINDOWS)return -22;
    VfbGuard guard;
    if(!DesktopLayersPresentableLocked(base) || !DesktopLayersPresentableLocked(next))return -116;
    return use(context);
}
struct DesktopChromeEpochs {uint32_t theme,appearance,scale;uint64_t settings,launcher;};
static uint64_t DesktopChromeKeyLocked(const DesktopChromeEpochs& layers,const Window& origin0,int32_t mouseX,int32_t mouseY);
static uint64_t DesktopChromeKeyLocked(const WmDesktopLayers& layers,const Window& origin0,int32_t mouseX,int32_t mouseY) {
    return DesktopChromeKeyLocked(DesktopChromeEpochs{uint32_t(layers.theme),uint32_t(layers.appearance),uint32_t(layers.scale),
        layers.settingsGeneration,layers.launcherGeneration},origin0,mouseX,mouseY);
}
static uint64_t DesktopChromeKeyLocked(const DesktopChromeEpochs& layers,const Window& origin0,int32_t mouseX,int32_t mouseY) {
    // origin0 has x=y=0, exactly as RenderWindowChrome sees it in a capture.
    int32_t hover=-1;
    const ChromeLayout layout=WindowChromeLayout(origin0);
    for(int button=0;button<3 && hover<0;++button){
        const ChromePart* part=layout.Find(ChromeCaptionRole(button));
        if(part && part->hit.contains(mouseX,mouseY))hover=button;
    }
    WmChromeKeyInput in{};
    in.title=origin0.title;in.titleCapacity=sizeof(origin0.title);
    in.appId=origin0.appId;in.appIdCapacity=sizeof(origin0.appId);
    in.windowIdentity=origin0.creationIdentity;in.metricsIdentity=reinterpret_cast<uintptr_t>(origin0.metrics);
    in.outerWidth=origin0.outerWidth();in.outerHeight=origin0.outerHeight();
    in.clientWidth=origin0.clientW;in.clientHeight=origin0.clientH;
    in.clientOffsetX=origin0.clientX();in.clientOffsetY=origin0.clientY();
    in.state=static_cast<uint32_t>(origin0.state);
    in.focused=origin0.focused;in.visible=origin0.visible;in.noChrome=origin0.noChrome;
    in.captionOverlay=origin0.noChrome && origin0.captionOverlay ?
        (1ull<<63)|(uint64_t(origin0.captionHeight)<<32)|(uint64_t(origin0.captionInsetTop)<<16)|origin0.captionInsetRight : 0;
    in.captionStyle=origin0.noChrome && origin0.captionOverlay ? origin0.captionStyle : 0;
    in.hoverButton=hover;
    in.theme=layers.theme;in.appearance=layers.appearance;in.scale=layers.scale;
    in.settingsEpoch=layers.settings;in.launcherEpoch=layers.launcher;
    return WmChromeKey(in);
}
// Surface records (stage 1.3): what the active backend resolved for chrome.
ChromeSurfaceEffects WmChromeSurfaceEffects() {
    ChromeSurfaceEffects e{};
    e.glass=CompositorGlassActive();e.blur=CompositorMaterialBlur();
    e.shadowSpread=WmMetric(16);e.shadowCorner=WmMetric(24);e.shellRadius=e.glass?WmMetric(4):0;
    if(e.glass){const auto& material=WmChromeMaterial();e.topRadius=material.top.radius;e.bottomRadius=material.bottom.radius;}
    return e;
}
// One window's chrome record; the key is the keyed chrome capture's key for
// the pointer at (mouseX, mouseY). False: the window draws no chrome.
static bool WindowSurfaceLocked(const Window& w,const DesktopChromeEpochs& epochs,const ChromeSurfaceEffects& effects,
                                int32_t mouseX,int32_t mouseY,BrookDesktopSurface& out) {
    if(!w.proc || !w.visible || w.minimized || !w.wmId || !w.creationIdentity)return false;
    if(w.noChrome && !w.captionOverlay)return false;
    const WmRect block=w.noChrome?WmCaptionOverlayRect(w):WmRect{w.x,w.y,int(w.outerWidth()),int(w.outerHeight())};
    if(block.w<=0 || block.h<=0)return false;
    Window origin0=w;origin0.x=0;origin0.y=0;
    auto rel=[](int32_t value,int16_t origin){
        const int64_t n=int64_t(value)-origin;
        return n<(-2147483647ll-1)?int32_t(-2147483647-1):n>2147483647ll?int32_t(2147483647):int32_t(n);
    };
    ChromeFrameSurfaceModel m{};
    m.window=w.wmId;m.incarnation=w.creationIdentity;
    m.key=DesktopChromeKeyLocked(epochs,origin0,rel(mouseX,w.x),rel(mouseY,w.y));
    m.clientDecorated=w.noChrome;m.maximised=w.state==WindowState::Maximized;m.focused=w.focused;
    m.outer=block;m.client={w.clientX(),w.clientY(),int(w.clientW),int(w.clientH)};
    m.captionRadius=(w.captionStyle&DesktopCaptionStyleValid)?DesktopCaptionStyleRadius(w.captionStyle):0;
    m.opacity=w.opacity;
    out=ChromeFrameSurface(m,effects);return true;
}
uint32_t WmDesktopSurfaceRecords(BrookDesktopSurface* out,uint32_t max) {
    if(!out || !g_wmActive)return 0;
    const ChromeSurfaceEffects effects=WmChromeSurfaceEffects();
    int32_t mouseX=0,mouseY=0;MouseGetPosition(&mouseX,&mouseY);
    VfbGuard guard;
    DesktopChromeEpochs epochs{uint32_t(WmGetTheme()),uint32_t(WmGetAppearance()),uint32_t(WmGetAppliedScale()),g_chromeSettingsEpoch,0};
    SpinLockAcquire(&g_launcherLock);epochs.launcher=g_launcherSnapshotEpoch;SpinLockRelease(&g_launcherLock);
    int sorted[WM_MAX_WINDOWS];const uint32_t count=WmGetZOrder(sorted,WM_MAX_WINDOWS);
    uint32_t n=0;
    for(uint32_t i=0;i<count && n<max;++i)
        if(WindowSurfaceLocked(g_windows[sorted[i]],epochs,effects,mouseX,mouseY,out[n]))++n;
    return n;
}
bool WmWindowSurfaceRecord(const Window& w,BrookDesktopSurface* out) {
    if(!out)return false;
    const DesktopChromeEpochs epochs{uint32_t(WmGetTheme()),uint32_t(WmGetAppearance()),uint32_t(WmGetAppliedScale()),g_chromeSettingsEpoch,0};
    return WindowSurfaceLocked(w,epochs,WmChromeSurfaceEffects(),-1,-1,*out);
}
BrookDesktopSurface WmShellSurfaceRecord(const WmShellSurface& surface,uint64_t key) {
    uint32_t kind=BROOK_DESKTOP_SURFACE_TASKBAR;
    switch(surface.kind){
    case WmShellSurfaceKind::Taskbar: kind=BROOK_DESKTOP_SURFACE_TASKBAR;break;
    case WmShellSurfaceKind::Launcher: kind=BROOK_DESKTOP_SURFACE_LAUNCHER;break;
    case WmShellSurfaceKind::VolumePopup: kind=BROOK_DESKTOP_SURFACE_VOLUME_POPUP;break;
    case WmShellSurfaceKind::TrayTooltip: kind=BROOK_DESKTOP_SURFACE_TOOLTIP;break;
    case WmShellSurfaceKind::BootSummary: kind=BROOK_DESKTOP_SURFACE_BOOT_SUMMARY;break;
    }
    return ChromeShellSurfaceRecord(kind,{surface.x,surface.y,int(surface.w),int(surface.h)},surface.material,key,
                                    CompositorShellMaterialOpacity(),WmChromeSurfaceEffects());
}
int64_t WmDesktopChromeCapture(const WmDesktopLayers& layers,uint32_t index,
                               WmDesktopChrome* info,uint32_t* pixels,uint64_t capacity,
                               int32_t mouseX,int32_t mouseY,bool tolerateVisualChanges,bool spansOnly) {
    if(!info || index>=layers.count || layers.count>WM_MAX_WINDOWS)return -22;
    Window local{};uint32_t width=0,height=0;int16_t originX=0,originY=0;
    {
        VfbGuard guard;
        if(!DesktopLayersCurrentLocked(layers,!tolerateVisualChanges))return -116;
        if(!g_wmActive)return -95;
        int sorted[WM_MAX_WINDOWS];if(WmGetZOrder(sorted,WM_MAX_WINDOWS)!=layers.count)return -116;
        const Window& source=g_windows[sorted[index]];
        if(source.noChrome && !source.captionOverlay){*info={source.x,source.y,0,0,0,0,0,0,0};return 0;}
        // Overlaid captions: a surface covering only the button block, placed
        // over the client (renderers draw chrome after the client).
        const WmRect block=source.noChrome?WmCaptionOverlayRect(source):WmRect{source.x,source.y,0,0};
        if(source.noChrome && (block.w<=0 || block.h<=0)){*info={source.x,source.y,0,0,0,0,0,0,0};return 0;}
        width=source.noChrome?uint32_t(block.w):source.outerWidth();
        height=source.noChrome?uint32_t(block.h):source.outerHeight();
        if(!width || !height || uint64_t(width)*height>64ull*1024*1024/4)return -7;
        {
            Window origin0=source;origin0.x=0;origin0.y=0;
            const bool noMouseKey=mouseX==-1 && mouseY==-1;
            auto rel=[](int32_t value,int16_t origin){
                const int64_t n=int64_t(value)-origin;
                return n<(-2147483647ll-1)?int32_t(-2147483647-1):n>2147483647ll?int32_t(2147483647):int32_t(n);
            };
            *info={source.noChrome?block.x:source.x,source.noChrome?block.y:source.y,width,height,
                DesktopChromeKeyLocked(layers,origin0,noMouseKey?-1:rel(mouseX,source.x),noMouseKey?-1:rel(mouseY,source.y)),0,0,0,0};
            if(!source.noChrome){
                // Interior clipped to the surface (always inside it for a frame).
                const int64_t ix=int64_t(origin0.clientX()),iy=int64_t(origin0.clientY());
                const int64_t ir=ix+source.clientW,ib=iy+source.clientH;
                auto clip=[](int64_t v,uint32_t limit){return uint32_t(v<0?0:v>int64_t(limit)?int64_t(limit):v);};
                info->interiorX=clip(ix,width);info->interiorY=clip(iy,height);
                info->interiorW=clip(ir,width)-info->interiorX;info->interiorH=clip(ib,height)-info->interiorY;
            }
        }
        if(!pixels)return 0;
        if(capacity<uint64_t(width)*height*4)return -22;
        local=source;originX=source.noChrome?int16_t(block.x):source.x;originY=source.noChrome?int16_t(block.y):source.y;
        ProcessRef(local.proc);
    }
    const uint64_t bytes=uint64_t(width)*height*4;
    const uint32_t ix=info->interiorX,iy=info->interiorY,iw=info->interiorW,ih=info->interiorH;
    if(spansOnly && !local.noChrome && iw && ih){
        // Frame strips only: top and bottom rows whole, left and right columns
        // beside the interior. The interior is neither cleared nor drawn.
        __builtin_memset(pixels,0,uint64_t(width)*iy*4);
        __builtin_memset(pixels+uint64_t(iy+ih)*width,0,uint64_t(width)*(height-iy-ih)*4);
        for(uint32_t row=iy;row<iy+ih;++row){
            __builtin_memset(pixels+uint64_t(row)*width,0,uint64_t(ix)*4);
            __builtin_memset(pixels+uint64_t(row)*width+ix+iw,0,uint64_t(width-ix-iw)*4);
        }
    }else __builtin_memset(pixels,0,bytes);
    if(local.noChrome){
        // Render the window so its button block lands at the surface origin.
        local.x=int16_t(local.x-originX);local.y=int16_t(local.y-originY);
        RenderWindowChrome(pixels,width,width,height,local,mouseX==-1 && mouseY==-1?-1:int32_t(mouseX-originX),
                           mouseX==-1 && mouseY==-1?-1:int32_t(mouseY-originY));
        // A client-matched block follows the client's rounded top-right corner.
        const unsigned radius=(local.captionStyle&DesktopCaptionStyleValid) && local.state!=WindowState::Maximized ?
            DesktopCaptionStyleRadius(local.captionStyle) : 0;
        ChromeMarkCaptionBlockOpaque(pixels,width,width,height,{0,0,width,height},radius);
        ProcessUnref(local.proc);
        return (tolerateVisualChanges?WmDesktopLayersCaptureCurrent(layers):WmDesktopLayersCurrent(layers))?0:-116;
    }
    local.x=0;local.y=0;
    auto relative=[](int32_t value,int16_t origin){
        const int64_t n=int64_t(value)-origin;
        return n<(-2147483647ll-1)?int32_t(-2147483647-1):n>2147483647ll?int32_t(2147483647):int32_t(n);
    };
    const bool noMouse=mouseX==-1 && mouseY==-1;
    RenderWindowChrome(pixels,width,width,height,local,noMouse?-1:relative(mouseX,originX),noMouse?-1:relative(mouseY,originY));
    // The client interior remains transparent; only the frame is marked.
    // Glass resolves only for a renderer that reported backdrop blur (layers
    // op 18): it then gets the same glass alpha as virgl's chrome layer.
    if(CompositorGlassActive()){
        const bool rounded=local.state!=WindowState::Maximized && !local.blurRadius;
        ChromeMarkFrameGlass(pixels,width,width,height,{0,0,width,height},
                             {local.clientX(),local.clientY(),local.clientW,local.clientH},WmMetric(WM_BORDER_WIDTH),
                             CompositorMaterialOpacity(),local.focused?WmCurrentPalette().text:WmCurrentPalette().mutedText,
                             WmCurrentPalette().accent,rounded?&WmChromeMaterial():nullptr,local.focused);
    }else
    ChromeMarkFrameOpaque(pixels,width,width,height,{0,0,width,height},
                          {local.clientX(),local.clientY(),local.clientW,local.clientH});
    ProcessUnref(local.proc);
    return (tolerateVisualChanges?WmDesktopLayersCaptureCurrent(layers):WmDesktopLayersCurrent(layers))?0:-116;
}
int64_t WmDesktopLayersUseIfCurrent(const WmDesktopLayers& layers,void* context,int64_t (*use)(void*)) {
    if(!use)return -22;
    VfbGuard guard;
    if(!DesktopLayersCurrentLocked(layers))return -116;
    return use(context);
}
bool WmCopyDesktopLayers(const WmDesktopLayers& layers,const WmDesktopLayerCopy* copies,uint32_t count) {
    if(count!=layers.count || count>WM_MAX_WINDOWS || (count && !copies))return false;
    for(uint32_t i=0;i<count;++i){
        const auto& layer=layers.layers[i];
        if(!layer.height || uint64_t(layer.stride)>~uint64_t{0}/4/layer.height)return false;
        const uint64_t bytes=uint64_t(layer.stride)*layer.height*4;
        if(!layer.backing || !bytes || bytes>layer.bytes || !copies[i].pixels || copies[i].capacity<bytes)return false;
    }
    if(!WmDesktopLayersCurrent(layers))return false;
    for(uint32_t i=0;i<count;++i){
        const auto& layer=layers.layers[i];
        if(!UserMappingBufferRead(layer.backing,0,copies[i].pixels,uint64_t(layer.stride)*layer.height*4))return false;
    }
    return WmDesktopLayersCurrent(layers);
}
void WmReleaseDesktopLayers(WmDesktopLayers* layers) {
    if(!layers)return;
    const uint32_t count=layers->count<=WM_MAX_WINDOWS?layers->count:0;
    for(uint32_t i=0;i<count;++i)MappingBackingUnref(layers->layers[i].backing);
    *layers={};
}

uint64_t WmMapWindowVfb(Process* caller,uint32_t wmId) {
    if (!caller) return 0;
    VfbGuard guard;
    for (uint32_t i=0;i<WM_MAX_WINDOWS;++i) {
        const auto& window=g_windows[i];
        if (window.proc && window.wmId==wmId && window.vfbBacking) {
            int64_t mapped=UserMappingBufferAlias(ProcessCurrent(),caller->addressSpace,window.vfbBacking,1);
            return mapped<0?0:static_cast<uint64_t>(mapped);
        }
    }
    return 0;
}

void WmSignalDirtyById(Process* proc, uint32_t wmId)
{
    VfbGuard guard;
    Window* w = WmFindWindowById(proc, wmId);
    if (!w) return;
    w->sourceWriteActive=false;
    // This syscall publishes CPU-written VFB content, including a surface
    // switching from dmabuf to SHM. Stop sampling any previous GL buffer.
    w->externalCaptured = false;
    w->publishedGeneration=NextPublication(*w);
    w->externalW = w->externalH = 0;
    w->vfbDirty = 1;
    CompositorWake();
}

int64_t WmBeginSourceWrite(Process* proc,uint32_t wmId,uint64_t incarnation)
{
    if(!proc || !wmId || !incarnation)return -22;
    VfbGuard guard;Window* w=WmFindWindowById(proc,wmId);
    if(!w)return -2;
    if(w->creationIdentity!=incarnation)return -116;
    if(w->sourceWriteActive && w->sourceWriteEpoch==w->publicationGeneration)return -16;
    w->sourceCommitGeneration=0;w->sourceCommit=0;
    w->sourceWriteEpoch=w->publicationGeneration;w->sourceWriteActive=true;return 0;
}
int64_t WmPublishSourceCommit(Process* proc,uint32_t wmId,uint64_t incarnation,uint64_t commit)
{
    if(!proc || !wmId || !incarnation || !commit)return -22;
    VfbGuard guard;Window* w=WmFindWindowById(proc,wmId);
    if(!w)return -2;
    if(w->creationIdentity!=incarnation)return -116;
    if(!w->sourceWriteActive || w->sourceWriteEpoch!=w->publicationGeneration){w->sourceWriteActive=false;return -116;}
    if(!w->vfb || w->publicationGeneration>=~uint64_t{0}-1)return -75;
    w->externalCaptured=false;w->externalW=w->externalH=0;
    w->publishedGeneration=NextPublication(*w);
    w->sourceCommitGeneration=w->publishedGeneration;w->sourceCommit=commit;w->sourceWriteActive=false;
    w->vfbDirty=1;CompositorWake();return 0;
}
int64_t WmQuerySourceCommit(uint32_t wmId,uint64_t incarnation,uint64_t generation,uint64_t* commit)
{
    if(!wmId || wmId>WM_MAX_WINDOWS || !incarnation || !generation || !commit)return -22;
    VfbGuard guard;const Window& w=g_windows[wmId-1];
    if(!w.proc || w.wmId!=wmId || w.creationIdentity!=incarnation || w.externalCaptured ||
       w.publishedGeneration!=generation || w.sourceCommitGeneration!=generation || !w.sourceCommit)return -116;
    *commit=w.sourceCommit;return 0;
}

bool WmReserveCapture(Process* proc,uint32_t wmId,uint32_t width,uint32_t height,WmCaptureToken* out)
{
    VfbGuard guard;
    auto* win=WmFindWindowById(proc,wmId);
    if (!win || !width || !height || !out || win->publicationGeneration>=~uint64_t{0}-1) return false;
    *out={wmId,width,height,win->creationIdentity,++win->publicationGeneration};
    return true;
}
bool WmGetCaptureBinding(Process* proc,uint32_t wmId,WmCaptureToken* out)
{
    VfbGuard guard;auto* win=WmFindWindowById(proc,wmId);
    if (!win || !out) return false;
    *out={wmId,1,1,win->creationIdentity,win->publicationGeneration};return true;
}
static Window* CaptureWindowLocked(const WmCaptureToken& token)
{
    if (!token.wmId || token.wmId>WM_MAX_WINDOWS) return nullptr;
    auto& win=g_windows[token.wmId-1];
    return win.proc && win.creationIdentity==token.incarnation &&
           win.publicationGeneration==token.generation ? &win : nullptr;
}
bool WmReserveBoundCapture(const WmCaptureToken& binding,uint32_t width,uint32_t height,WmCaptureToken* out)
{
    VfbGuard guard;
    if (!out || !width || !height || !binding.wmId || binding.wmId>WM_MAX_WINDOWS) return false;
    auto& win=g_windows[binding.wmId-1];
    if (!win.proc || win.creationIdentity!=binding.incarnation || win.publicationGeneration>=~uint64_t{0}-1)
        return false;
    *out={binding.wmId,width,height,win.creationIdentity,++win.publicationGeneration};return true;
}
bool WmCaptureCurrent(const WmCaptureToken& token)
{
    VfbGuard guard;return CaptureWindowLocked(token)!=nullptr;
}
bool WmCompleteCapture(const WmCaptureToken& token)
{
    VfbGuard guard;
    auto* win=CaptureWindowLocked(token);
    if (!win) return false;
    win->externalCaptured=true;win->externalW=token.w;win->externalH=token.h;
    win->publishedGeneration=token.generation;win->vfbDirty=1;
    CompositorWake();return true;
}
WmContentSnapshot WmSnapshotContent(uint32_t slot)
{
    VfbGuard guard;
    if (slot>=WM_MAX_WINDOWS || !g_windows[slot].proc) return {};
    const auto& w=g_windows[slot];
    return {true,w.externalCaptured,w.creationIdentity,w.publishedGeneration,w.externalW,w.externalH};
}

void WmAcknowledgeContent(uint32_t slot,const WmContentSnapshot& uploaded)
{
    VfbGuard guard;
    if (slot>=WM_MAX_WINDOWS) return;
    auto& w=g_windows[slot];
    if (w.proc && w.creationIdentity==uploaded.incarnation &&
        w.publishedGeneration==uploaded.generation && !w.externalCaptured) w.vfbDirty=0;
}

bool WmSetAppIdById(Process* proc, uint32_t wmId, const char* appId)
{
    VfbGuard guard;
    Window* w = WmFindWindowById(proc, wmId);
    if (!w || !appId) return false;
    WmStrCopy(w->appId, appId, sizeof(w->appId));
    CompositorMarkDirty();
    CompositorWake();
    return true;
}

bool WmRegisterClientOrigin(Process* provider,uint32_t wmId,uint16_t clientPid,
                            uint64_t incarnation,uint64_t launchId)
{
    VfbGuard guard;
    Window* w=WmFindWindowById(provider,wmId);
    if (!w || !w->vfb || !clientPid || !incarnation) return false;
    if (w->clientOrigin!=WmClientOrigin::Unknown)
        return w->clientOrigin==WmClientOrigin::RegisteredUnixPeer &&
               w->clientPid==clientPid && w->clientIncarnation==incarnation && w->clientLaunchId==launchId;
    w->clientPid=clientPid;w->clientIncarnation=incarnation;w->clientLaunchId=launchId;
    w->clientOrigin=WmClientOrigin::RegisteredUnixPeer;
    CompositorWake();return true;
}

void WmSetTitleById(Process* proc, uint32_t wmId, const char* title)
{
    VfbGuard guard;
    Window* w = WmFindWindowById(proc, wmId);
    if (!w || !title) return;
    WmStrCopy(w->title, title, sizeof(w->title));
    CompositorMarkDirty();
    CompositorWake();
}

bool WmInputPush(Window* win, const InputEvent& ev, int16_t localX, int16_t localY, uint32_t captureToken)
{
    if (!win) return false;
    uint32_t head = __atomic_load_n(&win->inputHead, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&win->inputTail, __ATOMIC_ACQUIRE);
    uint32_t next = (head + 1) % Window::WM_INPUT_QUEUE;
    if (next == tail) {
        // Queue full — for mouse-move events, coalesce by overwriting the
        // newest entry if it's also a mouse-move (consumer only needs latest
        // position). For other event types, drop with a rate-limited warning.
        if (ev.type == InputEventType::MouseMove && head != tail)
        {
            uint32_t prev = (head + Window::WM_INPUT_QUEUE - 1) % Window::WM_INPUT_QUEUE;
            Window::WmInputEvent& slot = win->inputQueue[prev];
            if (slot.type == static_cast<uint8_t>(InputEventType::MouseMove))
            {
                slot.x = localX;
                slot.y = localY;
                slot.modifiers = ev.modifiers;
                return true; // coalesced
            }
        }
        // Rate-limit the warning: only print once per ~256 drops per window
        win->inputDropCount++;
        if ((win->inputDropCount & 0xFF) == 1)
            SerialPrintf("WmInputPush: queue FULL wm=%u type=%d (dropped %u)\n",
                         win->wmId, (int)ev.type, win->inputDropCount);
        return false;
    }
    Window::WmInputEvent& slot = win->inputQueue[head];
    slot.type      = static_cast<uint8_t>(ev.type);
    slot.scanCode  = ev.scanCode;
    slot.ascii     = static_cast<uint8_t>(ev.ascii);
    slot.modifiers = ev.modifiers;
    slot.x         = localX;
    slot.y         = localY;
    slot.reserved  = captureToken;
    __atomic_store_n(&win->inputHead, next, __ATOMIC_RELEASE);
    return true;
}

uint32_t WmInputPop(Window* win, Window::WmInputEvent* out, uint32_t max)
{
    if (!win || !out || !max) return 0;
    uint32_t produced = 0;
    while (produced < max)
    {
        uint32_t tail = __atomic_load_n(&win->inputTail, __ATOMIC_ACQUIRE);
        uint32_t head = __atomic_load_n(&win->inputHead, __ATOMIC_ACQUIRE);
        if (tail == head) break;
        out[produced++] = win->inputQueue[tail];
        __atomic_store_n(&win->inputTail,
                         (tail + 1) % Window::WM_INPUT_QUEUE,
                         __ATOMIC_RELEASE);
    }
    return produced;
}

int WmInputPopOwned(Process* proc, uint32_t wmId, uint64_t incarnation,
                    Window::WmInputEvent* out, uint32_t max)
{
    if (!proc || !out || !max || !incarnation) return -22;
    VfbGuard guard;
    Window* win=WmFindWindowById(proc,wmId);
    if (!win) return -2;
    if (win->creationIdentity!=incarnation) return -116;
    return static_cast<int>(WmInputPop(win,out,max));
}

int WmInputPopById(Process* proc, uint32_t wmId, Window::WmInputEvent* out, uint32_t max)
{
    if (!proc || !out || !max) return -22;
    VfbGuard guard;
    Window* win=WmFindWindowById(proc,wmId);
    if (!win) return -2;
    return static_cast<int>(WmInputPop(win,out,max));
}

bool WmPushWmEvent(Window* win, uint8_t type, int16_t x, int16_t y)
{
    if (!win) return false;
    uint32_t head = __atomic_load_n(&win->inputHead, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&win->inputTail, __ATOMIC_ACQUIRE);
    uint32_t next = (head + 1) % Window::WM_INPUT_QUEUE;
    if (next == tail) return false;
    Window::WmInputEvent& slot = win->inputQueue[head];
    slot.type      = type;
    slot.scanCode  = 0;
    slot.ascii     = 0;
    slot.modifiers = 0;
    slot.x         = x;
    slot.y         = y;
    slot.reserved  = type!=WM_EVT_RESIZED ? 0 : win->fullscreen ? WM_RESIZE_FULLSCREEN :
                     win->state==WindowState::Maximized ? WM_RESIZE_MAXIMIZED : WM_RESIZE_RESIZING;
    __atomic_store_n(&win->inputHead, next, __ATOMIC_RELEASE);
    return true;
}

} // namespace brook
