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
static ChromeTelemetryModel WmTelemetryModel(bool app, bool temp) {
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
        if(t.thermal){t.pointCount=ThermalReadGraph(points,ThermalGraphHistory);t.points=points;}
    }
    return t;
}
bool WmBootSummaryRect(uint32_t sw,uint32_t sh,int32_t* x,int32_t* y,uint32_t* width,uint32_t* height) {
    const uint64_t ready=CompositorDesktopReadyTick(),now=ApicTickCount();
    if(!ready||now<ready||now-ready>7000)return false;
    const uint32_t margin=WmMetric(16),w=WmMetric(360),h=WmDesktopFont().lineHeight*6+WmMetric(41);
    if(sw<margin*2+WmMetric(100)||sh<h+margin+WmMetric(WM_TASKBAR_HEIGHT))return false;
    *width=w<sw-margin*2?w:sw-margin*2;*height=h;*x=sw-*width-margin;*y=margin;return true;
}
static void WmBootSummarySurface(uint32_t* pixels,uint32_t stride,uint32_t sw,uint32_t sh,uint64_t keySeed,uint64_t* key) {
    int32_t x,y;uint32_t w,h;if((!pixels && !key) || !WmBootSummaryRect(sw,sh,&x,&y,&w,&h))return;
    char number[16],headline[96],machine[96],build[96],phaseLabels[4][32];
    const BootTimingSummary* timing0 = BootTimingRead();
    if (timing0) {
        const BootTimingValue elapsed = timing0->total.valid ? timing0->total : timing0->kernel;
        BootText title(headline, sizeof(headline));
        title.Add("Desktop ready");
        if (elapsed.valid) {
            title.Add(" in "); title.Seconds(elapsed.ms, 2); title.Add(" s");
            if (!timing0->total.valid) title.Add(" (kernel)");
        }
    } else {
        const uint64_t duration=CompositorBootDurationMs();
        char* row=headline;
        WmStrCopy(row,"Desktop ready in ",sizeof(headline));DesktopNumber(number,duration/1000);
        size_t n=strlen(row);WmStrCopy(row+n,number,sizeof(headline)-n);n=strlen(row);row[n++]='.';row[n++]='0'+(duration/100)%10;row[n++]='0'+(duration/10)%10;row[n]=0;
        WmStrCopy(row+n,KvmClockStableAvailable()?" s (VM start)":" s (kernel clock)",sizeof(headline)-n);
    }
    static bool sampledMemory = false;
    static uint64_t freeAtReadyMiB;
    if (!sampledMemory) {
        freeAtReadyMiB = PmmGetFreePageCount() * 4096 / 1048576;
        sampledMemory = true;
    }
    size_t n=0;char* row=machine;
    DesktopNumber(row,SmpGetCpuCount());n=strlen(row);WmStrCopy(row+n," cores | ",sizeof(machine)-n);n=strlen(row);DesktopNumber(number,freeAtReadyMiB);WmStrCopy(row+n,number,sizeof(machine)-n);n=strlen(row);WmStrCopy(row+n," MiB available",sizeof(machine)-n);
    WmStrCopy(build,"Build ",sizeof(build));WmStrCopy(build+6,BuildGitHash(),sizeof(build)-6);
    ChromeBootSummaryModel model{{x,y,int(w),int(h)},{headline,timing0 ? nullptr : "Boot timing unavailable",machine,
        CompositorGlassActive()?"GPU composition | glass":CompositorGpuActive()?"GPU composition | flat":"Software composition | flat",build}};
    if (timing0) {
        const BootTimingValue phases[] = {timing0->firmware, timing0->wait, timing0->loader, timing0->kernel};
        const char* names[] = {"Firmware", "Menu", "Loader", "Kernel"};
        for (unsigned i = 0; i < 4; ++i) {
            if (phases[i].valid) model.phaseMs[i] = phases[i].ms;
            BootText label(phaseLabels[i], sizeof(phaseLabels[i]));
            label.Value(names[i], phases[i]);
            model.phaseLabels[i] = phaseLabels[i];
        }
    }
    ChromeDisplayList list=ChromeListOn(g_shellListStorage);
    ChromeBuildBootSummary(model,WmChromeStyle(),list);
    if(key)*key=ChromeListKey(list,keySeed);
    else ChromeExecute(list,{pixels,stride,sw,sh},WmDesktopFont());
}
void WmRenderBootSummary(uint32_t* pixels,uint32_t stride,uint32_t sw,uint32_t sh) {
    WmBootSummarySurface(pixels,stride,sw,sh,0,nullptr);
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
