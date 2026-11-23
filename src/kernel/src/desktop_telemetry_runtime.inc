// Compositor-owned bounded histories; no timers, profiler capture or heap work.
static DesktopIntervals<> g_desktopCadence,g_appCadence;
static DesktopTelemetry g_desktopStats{};
static uint64_t g_metricsBusy=0,g_metricsIdle=0,g_metricsRead=0,g_metricsWrite=0,g_metricsDown=0,g_metricsUp=0;
static uint64_t g_metricsAppIdentity=0,g_metricsAppGeneration=0;
// App cadence is fed by the compositor tick (CPU-path publications) and by
// the renderer's GPU-source frame reports, on different threads.
static SpinLock g_appCadenceLock;
static DesktopClientFrameFilter g_appClientFrames;
static void SampleDesktopMetrics(uint64_t now) {
    if(g_desktopStats.tick && now-g_desktopStats.tick<500)return;
    uint64_t busy=0,idle=0;
    for(uint32_t cpu=0;cpu<SmpGetCpuCount();++cpu){uint64_t b=0,i=0;SchedulerGetCpuTicks(cpu,b,i);busy+=b;idle+=i;}
    uint64_t disk[2]{};
    DeviceIterate(DeviceType::Block,[](Device* d,void* context){
        bool base=d->name && strncmp(d->name,"virtio",6)==0 && d->name[6];
        if(base)for(const char* c=d->name+6;*c;++c)if(*c<'0'||*c>'9')base=false;
        if(base){uint64_t ro,wo,r,w;VirtioBlkGetStats(d,ro,wo,r,w);auto* v=static_cast<uint64_t*>(context);v[0]+=r;v[1]+=w;}return true;
    },disk);
    // AHCI and USB mass storage: completed device transfers, not cache hits.
    const auto storage=StorageIoReadAll();disk[0]+=storage.readBytes;disk[1]+=storage.writeBytes;
    uint64_t down=0,up=0;
    for(uint32_t i=0;i<NetIfCount();++i){auto* n=NetIfAt(i);if(n){down+=__atomic_load_n(&n->rxBytes,__ATOMIC_RELAXED);up+=__atomic_load_n(&n->txBytes,__ATOMIC_RELAXED);}}
    const uint64_t elapsed=g_desktopStats.tick && now>g_desktopStats.tick?now-g_desktopStats.tick:0;
    auto rate=[&](uint64_t current,uint64_t prior){return elapsed && current>=prior?(current-prior)*1000/elapsed:0;};
    const uint64_t db=busy>=g_metricsBusy?busy-g_metricsBusy:0,di=idle>=g_metricsIdle?idle-g_metricsIdle:0;
    const uint64_t total=PmmGetTotalPageCount(),free=PmmGetFreePageCount();
    g_desktopStats={now,rate(disk[0],g_metricsRead),rate(disk[1],g_metricsWrite),rate(down,g_metricsDown),rate(up,g_metricsUp),
        static_cast<uint32_t>(elapsed && db+di?db*100/(db+di):0),static_cast<uint32_t>(total && free<=total?(total-free)*100/total:0)};
    g_metricsBusy=busy;g_metricsIdle=idle;g_metricsRead=disk[0];g_metricsWrite=disk[1];g_metricsDown=down;g_metricsUp=up;
}
DesktopTelemetry WmDesktopTelemetryRead() {
    DesktopTelemetry copy;
    copy.tick=__atomic_load_n(&g_desktopStats.tick,__ATOMIC_RELAXED);
    copy.readBytesPerSecond=__atomic_load_n(&g_desktopStats.readBytesPerSecond,__ATOMIC_RELAXED);
    copy.writeBytesPerSecond=__atomic_load_n(&g_desktopStats.writeBytesPerSecond,__ATOMIC_RELAXED);
    copy.downBytesPerSecond=__atomic_load_n(&g_desktopStats.downBytesPerSecond,__ATOMIC_RELAXED);
    copy.upBytesPerSecond=__atomic_load_n(&g_desktopStats.upBytesPerSecond,__ATOMIC_RELAXED);
    copy.cpuPercent=__atomic_load_n(&g_desktopStats.cpuPercent,__ATOMIC_RELAXED);
    copy.memoryPercent=__atomic_load_n(&g_desktopStats.memoryPercent,__ATOMIC_RELAXED);
    return copy;
}
void WmObserveDesktopTick(uint64_t now) {
    g_desktopCadence.Record(now);SampleDesktopMetrics(now);
    Window* w=WmGetWindow(WmGetFocusedWindow());
    const uint64_t identity=w && w->visible && !w->minimized?w->creationIdentity:0;
    SpinLockAcquire(&g_appCadenceLock);
    if(identity!=__atomic_load_n(&g_metricsAppIdentity,__ATOMIC_RELAXED)){
        __atomic_store_n(&g_metricsAppIdentity,identity,__ATOMIC_RELAXED);
        g_appCadence.Reset();g_metricsAppGeneration=0;g_appClientFrames.Reset(identity);
    }
    if(identity){
        const bool dirty=!w->vfb && !w->externalCaptured && w->proc && w->proc->fbDirty;
        if(dirty || w->publishedGeneration!=g_metricsAppGeneration) {
            // Observed client publication cadence; not the app's internal CPU/GPU cost.
            g_appCadence.Record(now);g_metricsAppGeneration=w->publishedGeneration;
        }
    }
    SpinLockRelease(&g_appCadenceLock);
}
// Renderer report: a GPU-source frame (window identity, client commit) was
// composited. Counted once per new commit for the focused window, so GPU
// clients (yquake, Chromium/ANGLE, any dmabuf producer) show APP cadence.
void WmNoteClientFrame(uint64_t identity,uint64_t commit) {
    const uint64_t now=ApicTickCount();
    SpinLockAcquire(&g_appCadenceLock);
    if(g_appClientFrames.Accept(__atomic_load_n(&g_metricsAppIdentity,__ATOMIC_RELAXED),identity,commit))g_appCadence.Record(now);
    SpinLockRelease(&g_appCadenceLock);
}
// Telemetry for the taskbar builder: the sampled rates and loads, both
// cadence histories and, when the TEMP metric is shown, the thermal graph.
// Compositor thread only (the copies are static).
static ChromeTelemetryModel WmTelemetryModel(bool app, bool temp, bool graph = true) {
    ChromeTelemetryModel t{};
    t.stats=g_desktopStats;
    t.compositor=&g_desktopCadence;
    static DesktopIntervals<> appCopy;
    if(app){SpinLockAcquire(&g_appCadenceLock);appCopy=g_appCadence;SpinLockRelease(&g_appCadenceLock);}
    t.app=&appCopy;
    if(temp){
        static ThermalSample points[ThermalGraphHistory];
        ThermalSnapshot snap{};
        t.thermal=ThermalReadSnapshot(snap) && snap.latest.pkgC>=0;
        t.rapl=snap.rapl;t.tjmax=snap.tjmax;t.latest=snap.latest;
        if(t.thermal && graph){t.pointCount=ThermalReadGraph(points,ThermalGraphHistory);t.points=points;}
    }
    return t;
}
// Rebase graph slot ages and raw-stat expiry onto a fixed render clock so
// unchanged chrome stays cacheable. Leave room for all eight launch animation
// steps without changing the graph clock or the live/expired boundaries.
static constexpr uint64_t ShellCadenceTick = 10400;
static constexpr uint64_t ShellRecentSample = 1200;

static void WmCaptureCadence(DesktopIntervals<>& out, const DesktopIntervals<>& source,
                            uint64_t observed, uint64_t displayTick = ShellCadenceTick,
                            bool taskbarApp = false) {
    out.next = source.next;
    out.count = source.count;
    out.bins.Capture(source.bins, observed, displayTick);
    if (taskbarApp && source.last && observed >= source.last && observed - source.last <= 2000)
        out.last = displayTick;
    for (unsigned i = 0; i < source.count; ++i) {
        const auto& sample = source.samples[i];
        if (observed < sample.tick || observed - sample.tick > 10000) continue;
        out.samples[i].tick = ShellRecentSample;
        out.samples[i].ms = sample.ms;
    }
}

static uint64_t WmShellRate(uint64_t bytes) {
    const uint64_t tenths = bytes / 100000;
    return (tenths > 99999 ? 99999 : tenths) * 100000;
}

static void WmCaptureThermal(ChromeShellTelemetry& out, const ChromeTelemetryModel& source, bool graph) {
    if (!source.thermal || source.latest.pkgC < 0) return;
    out.thermal = true;
    out.tjmax = source.tjmax;
    out.latest.pkgC = source.latest.pkgC;
    if (!graph) return;
    out.rapl = source.rapl;
    if (source.points && source.pointCount) {
        out.latest.pkgC = 0; // the readout uses the newest graph point
        out.pointCount = source.pointCount;
        for (unsigned i = 0; i < out.pointCount; ++i)
            out.points[i].pkgC = source.points[i].pkgC;
        if (out.rapl)
            out.points[out.pointCount - 1].pkgMw = source.points[out.pointCount - 1].pkgMw / 100 * 100;
    } else if (out.rapl) out.latest.pkgMw = source.latest.pkgMw / 100 * 100;
}

// Temperature flyout power rows: RAPL limits and the balance controller's
// current caps. Copies owned values only (the shell draws the snapshot).
static ChromePowerModel WmPowerModel() {
    ChromePowerModel out{};
    ThermalSnapshot snap{};
    const bool sampled = ThermalReadSnapshot(snap);
    if (sampled && snap.rapl) { out.pl1Mw = snap.pl1Mw; out.pl2Mw = snap.pl2Mw; }
    // Max non-turbo clock (MSR 0xCE) is shown with or without a controller.
    if (sampled) out.cpuMaxMhz = static_cast<uint16_t>(snap.baseMhz > 0xffff ? 0xffff : snap.baseMhz);
    const auto b = ThermalBalanceRead();
    if (!b.available) return out;
    using M = ChromeBalanceMode;
    const auto& st = b.state;
    out.mode = b.enabled ? (!b.ownerActive ? M::Starting : st.mode == BalanceMode::Heavy ? M::Heavy :
                            st.idle ? M::Idle : M::Light)
                         : b.autoState.optedOut ? M::OptedOut : M::Off;
    out.gpuMhz = snap.latest.gpuMhz;
    if (!b.enabled) return out;
    out.targetMw = b.config.pkgLimitMw;
    out.capRatio = static_cast<uint8_t>(b.appliedCap > 0xff ? 0xff : b.appliedCap);
    if (b.ownerActive && b.lastSampleAt) {
        const auto& l = b.limits;
        uint32_t top = l.gpuBaseMhz;
        if (out.mode == M::Heavy)
            top = st.gpuCeilingMhz && st.gpuCeilingMhz < l.gpuMaxMhz
                ? (st.gpuCeilingMhz > l.gpuBaseMhz ? st.gpuCeilingMhz : l.gpuBaseMhz) : l.gpuMaxMhz;
        else if (out.mode == M::Idle && l.gpuIdleMhz) top = l.gpuIdleMhz;
        out.gpuTopMhz = static_cast<uint16_t>(top > 0xffff ? 0xffff : top);
    }
    return out;
}

static void WmCaptureHudCadence(ChromeShellTelemetry& out, const ChromePerformanceHudModel& hud) {
    out.hasCompositor = out.hasApp = true;
    WmCaptureCadence(out.compositor, *hud.telemetry.compositor, hud.tick);
    WmCaptureCadence(out.app, *hud.telemetry.app, hud.tick);
}

// The boot toast: 360 wide, 16 px from the top-right corner, as tall as its
// content (ChromeBootSummaryHeight). Shown for 7 s after the desktop is ready.
static unsigned WmBootSummaryMetaLines() { return 3; }
bool WmBootSummaryRect(uint32_t sw,uint32_t sh,int32_t* x,int32_t* y,uint32_t* width,uint32_t* height) {
    const uint64_t ready=CompositorDesktopReadyTick(),now=ApicTickCount();
    if(!ready||now<ready||now-ready>7000)return false;
    const BootTimingSummary* timing=BootTimingRead();
    const uint32_t margin=WmMetric(16),w=WmMetric(360);
    const uint32_t h=static_cast<uint32_t>(ChromeBootSummaryHeight(WmChromeStyle(),true,timing!=nullptr,WmBootSummaryMetaLines()));
    if(sw<margin*2+WmMetric(100)||sh<h+margin+WmMetric(WM_TASKBAR_HEIGHT))return false;
    *width=w<sw-margin*2?w:sw-margin*2;*height=h;*x=sw-*width-margin;*y=margin;return true;
}
static void WmCaptureBootSummary(uint32_t sw, uint32_t sh, ChromeShellModel& snapshot) {
    int32_t x, y;
    uint32_t w, h;
    if (!WmBootSummaryRect(sw, sh, &x, &y, &w, &h)) return;
    snapshot.kind = ChromeShellKind::BootSummary;
    snapshot.rect = {x, y, int(w), int(h)};
    auto& boot = snapshot.boot;
    const BootTimingSummary* timing = BootTimingRead();
    BootText total(boot.total, sizeof(boot.total));
    const char* note;
    if (timing && (timing->total.valid || timing->kernel.valid)) {
        total.Seconds(timing->total.valid ? timing->total.ms : timing->kernel.ms, 2);
        note = timing->total.valid ? "from power-on" : "in the kernel";
    } else {
        total.Seconds(CompositorBootDurationMs(), 2);
        note = KvmClockStableAvailable() ? "from VM start" : "on the kernel clock";
    }
    total.Add(" s");
    WmStrCopy(boot.totalNote, note, sizeof(boot.totalNote));
    static bool sampledMemory = false;
    static uint64_t freeAtReadyMiB;
    if (!sampledMemory) {
        freeAtReadyMiB = PmmGetFreePageCount() * 4096 / 1048576;
        sampledMemory = true;
    }
    BootText machine(boot.meta[0], sizeof(boot.meta[0]));
    machine.Number(SmpGetCpuCount());
    machine.Add(" cores " BROOK_CHROME_DOT " ");
    machine.Number(freeAtReadyMiB);
    machine.Add(" MiB available");
    WmStrCopy(boot.meta[1], CompositorGlassActive() ? "GPU composition " BROOK_CHROME_DOT " Glass" :
        CompositorGpuActive() ? "GPU composition " BROOK_CHROME_DOT " Opaque" :
                                "Software composition " BROOK_CHROME_DOT " Opaque", sizeof(boot.meta[1]));
    BootText build(boot.meta[2], sizeof(boot.meta[2]));
    build.Add("Brook ");
    build.Add(BuildGitHash());
    if (!timing) return;
    const BootTimingValue phases[] = {timing->firmware, timing->wait, timing->loader, timing->kernel};
    static const char* const names[] = {"Firmware", "Menu", "Loader", "Kernel"};
    for (unsigned i = 0; i < 4; ++i) {
        WmStrCopy(boot.phaseNames[i], names[i], sizeof(boot.phaseNames[i]));
        BootText value(boot.phaseValues[i], sizeof(boot.phaseValues[i]));
        if (phases[i].valid) {
            boot.phaseMs[i] = phases[i].ms;
            value.Seconds(phases[i].ms, 2);
            value.Add(" s");
        } else value.Add("n/a");
    }
}

// Tray tooltip for the TEMP metric (fits the 48-byte tooltip buffer).
static void DesktopThermalTooltip(char (&text)[48]) {
    ThermalSnapshot snap{};
    if(!ThermalReadSnapshot(snap) || snap.latest.pkgC<0){WmStrCopy(text,"Temperature: unavailable",sizeof(text));return;}
    const auto& v=snap.latest;unsigned used=0;text[0]=0;
    auto append=[&](const char* s){while(*s && used+1<sizeof(text))text[used++]=*s++;text[used]=0;};
    char n[12];
    append("Pkg ");DesktopNumber(n,unsigned(v.pkgC));append(n);append("C max ");DesktopNumber(n,unsigned(snap.maxima.pkgC<0?0:snap.maxima.pkgC));append(n);append("C");
    if(snap.rapl){append(" ");DesktopNumber(n,v.pkgMw/1000);append(n);append(".");DesktopNumber(n,(v.pkgMw/100)%10);append(n);append("W");}
    append(" CPU ");DesktopNumber(n,v.cpuMhz);append(n);
    append(" GPU ");DesktopNumber(n,v.gpuMhz);append(n);append("MHz");
}
