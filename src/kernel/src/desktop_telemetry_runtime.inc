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
static ChromeTelemetryModel WmTelemetryModel(const ChromeTaskbarLayout& l) {
    ChromeTelemetryModel t{};
    t.stats=g_desktopStats;
    t.compositor=&g_desktopCadence;
    static DesktopIntervals<> appCopy;
    if(l.metrics[5].w){SpinLockAcquire(&g_appCadenceLock);appCopy=g_appCadence;SpinLockRelease(&g_appCadenceLock);}
    t.app=&appCopy;
    if(l.metrics[6].w){
        static ThermalSample points[ThermalGraphHistory];
        ThermalSnapshot snap{};
        t.thermal=ThermalReadSnapshot(snap) && snap.latest.pkgC>=0;
        t.rapl=snap.rapl;t.tjmax=snap.tjmax;t.latest=snap.latest;
        if(t.thermal){t.pointCount=ThermalReadGraph(points,ThermalGraphHistory);t.points=points;}
    }
    return t;
}
// "Brook <build>" plus, on hardware payloads, a drawn middle dot and the
// payload name (the desktop font is ASCII-only).
struct WmBuildIdText { char main[48]; const char* suffix; int mainWidth, dotGap, dot; };
static bool WmBuildIdLayout(uint32_t sw,uint32_t sh,WmBuildIdText& text,WmRect& rect) {
    if(!WmIsActive())return false;
    WmStrCopy(text.main,"Brook ",sizeof(text.main));WmStrCopy(text.main+6,BuildGitHash(),sizeof(text.main)-6);
    text.suffix=g_buildPayloadId[0]?g_buildPayloadId:nullptr;
    text.mainWidth=WmTextWidth(text.main);text.dotGap=WmMetric(6);text.dot=WmMetric(2)>1?WmMetric(2):1;
    int width=text.mainWidth+WmMetric(14);
    if(text.suffix)width+=2*text.dotGap+text.dot+WmTextWidth(text.suffix);
    const int height=WmDesktopFont().lineHeight+WmMetric(6);
    if(sw<static_cast<uint32_t>(width)+WmMetric(10)||sh<WmMetric(WM_TASKBAR_HEIGHT)+height+WmMetric(6))return false;
    rect={int(sw)-width-int(WmMetric(8)),int(sh)-int(WmMetric(WM_TASKBAR_HEIGHT))-height-int(WmMetric(6)),width,height};
    return true;
}
// Draws the label text with its top-left glyph origin at (x,y) in a buffer
// of sw x sh pixels (a full frame, or one row for the span path).
static void WmDrawBuildIdText(uint32_t* pixels,uint32_t stride,uint32_t sw,uint32_t sh,int x,int y,
                          const WmBuildIdText& text,const WmRect* clip) {
    const ChromeTarget target{pixels,stride,sw,sh};
    ChromeRenderString(target,WmDesktopFont(),x,y,text.main,WmCurrentPalette().badgeText,0,clip);
    if(!text.suffix)return;
    const int dotX=x+text.mainWidth+text.dotGap,dotY=y+(WmDesktopFont().lineHeight-text.dot)/2;
    int l=dotX,t=dotY,r=dotX+text.dot,b=dotY+text.dot;
    if(clip){if(l<clip->x)l=clip->x;if(t<clip->y)t=clip->y;
        if(r>clip->x+clip->w)r=clip->x+clip->w;if(b>clip->y+clip->h)b=clip->y+clip->h;}
    if(r>l && b>t)ChromeFillRect(target,l,t,r-l,b-t,WmCurrentPalette().badgeText);
    ChromeRenderString(target,WmDesktopFont(),dotX+text.dot+text.dotGap,y,text.suffix,WmCurrentPalette().badgeText,0,clip);
}
void WmDrawBuildId(uint32_t* pixels,uint32_t stride,uint32_t sw,uint32_t sh,const WmRect* clip) {
    WmBuildIdText text{};WmRect rect{};if(!WmBuildIdLayout(sw,sh,text,rect))return;
    const int x=rect.x,y=rect.y,width=rect.w,height=rect.h;
    const int left=clip && clip->x>x?clip->x:x,top=clip && clip->y>y?clip->y:y;
    const int right=clip && clip->x+clip->w<x+width?clip->x+clip->w:x+width;
    const int bottom=clip && clip->y+clip->h<y+height?clip->y+clip->h:y+height;
    if(right<=left||bottom<=top)return;
    ChromeFillRect({pixels,stride,sw,sh},left,top,right-left,bottom-top,WmCurrentPalette().badgeFill);
    WmDrawBuildIdText(pixels,stride,sw,sh,x+WmMetric(7),y+WmMetric(3),text,clip);
}
// Draw into one already-populated screen span, using the same glyph renderer
// and coordinates as the normal full-frame path.
void WmDrawBuildIdSpan(uint32_t* pixels,uint32_t left,uint32_t count,uint32_t row,uint32_t sw,uint32_t sh) {
    if(!pixels || !count || left>=sw || count>sw-left || row>=sh)return;
    WmBuildIdText text{};WmRect rect{};if(!WmBuildIdLayout(sw,sh,text,rect) ||
        int64_t(row)<rect.y || int64_t(row)>=int64_t(rect.y)+rect.h)return;
    ChromeFillRect({pixels,count,count,1},rect.x-int(left),0,rect.w,1,WmCurrentPalette().badgeFill);
    WmDrawBuildIdText(pixels,count,count,1,rect.x+int(WmMetric(7))-int(left),
                      rect.y+int(WmMetric(3))-int(row),text,nullptr);
}
bool WmBootSummaryRect(uint32_t sw,uint32_t sh,int32_t* x,int32_t* y,uint32_t* width,uint32_t* height) {
    const uint64_t ready=CompositorDesktopReadyTick(),now=ApicTickCount();
    if(!ready||now<ready||now-ready>7000)return false;
    const uint32_t margin=WmMetric(16),w=WmMetric(480),h=WmDesktopFont().lineHeight*5+WmMetric(28);
    if(sw<margin*2+WmMetric(100)||sh<h+margin+WmMetric(WM_TASKBAR_HEIGHT))return false;
    *width=w<sw-margin*2?w:sw-margin*2;*height=h;*x=sw-*width-margin;*y=margin;return true;
}
void WmRenderBootSummary(uint32_t* pixels,uint32_t stride,uint32_t sw,uint32_t sh) {
    int32_t x,y;uint32_t w,h;if(!WmBootSummaryRect(sw,sh,&x,&y,&w,&h))return;
    char number[16],headline[96],timing[96],machine[96],build[96];
    if(const BootTimingSummary* timing0=BootTimingRead()) {
        // Firmware | boot menu | loader | kernel; fold the loader into the
        // kernel bucket when the full row would be clipped.
        BootTimingHeadline(headline,sizeof(headline),*timing0);
        BootTimingRow(timing,sizeof(timing),*timing0,false);
        if(WmTextWidth(timing)>int(w)-int(WmMetric(24)))BootTimingRow(timing,sizeof(timing),*timing0,true);
    } else {
        const uint64_t duration=CompositorBootDurationMs();
        char* row=headline;
        WmStrCopy(row,"Desktop ready in ",sizeof(headline));DesktopNumber(number,duration/1000);
        size_t n=strlen(row);WmStrCopy(row+n,number,sizeof(headline)-n);n=strlen(row);row[n++]='.';row[n++]='0'+(duration/100)%10;row[n++]='0'+(duration/10)%10;row[n]=0;
        WmStrCopy(row+n,KvmClockStableAvailable()?" s (VM start)":" s (kernel clock)",sizeof(headline)-n);
        WmStrCopy(timing,"Boot timing n/a",sizeof(timing));
    }
    size_t n=0;char* row=machine;
    DesktopNumber(row,SmpGetCpuCount());n=strlen(row);WmStrCopy(row+n," cores | ",sizeof(machine)-n);n=strlen(row);DesktopNumber(number,PmmGetFreePageCount()*4096/1048576);WmStrCopy(row+n,number,sizeof(machine)-n);n=strlen(row);WmStrCopy(row+n," MiB available",sizeof(machine)-n);
    WmStrCopy(build,"Build ",sizeof(build));WmStrCopy(build+6,BuildGitHash(),sizeof(build)-6);
    const ChromeBootSummaryModel model{{x,y,int(w),int(h)},{headline,timing,machine,
        CompositorGlassActive()?"GPU composition | glass":CompositorGpuActive()?"GPU composition | flat":"Software composition | flat",build}};
    ChromeDisplayList list=ChromeListOn(g_shellListStorage);
    ChromeBuildBootSummary(model,WmChromeStyle(),list);
    ChromeExecute(list,{pixels,stride,sw,sh},WmDesktopFont());
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
