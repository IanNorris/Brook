// Compositor-owned bounded histories; no timers, profiler capture or heap work.
static DesktopIntervals<> g_desktopCadence,g_appCadence;
static DesktopTelemetry g_desktopStats{};
static uint64_t g_metricsBusy=0,g_metricsIdle=0,g_metricsRead=0,g_metricsWrite=0,g_metricsDown=0,g_metricsUp=0;
static uint64_t g_metricsAppIdentity=0,g_metricsAppGeneration=0;
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
void WmObserveDesktopTick(uint64_t now) {
    g_desktopCadence.Record(now);SampleDesktopMetrics(now);
    Window* w=WmGetWindow(WmGetFocusedWindow());
    const uint64_t identity=w && w->visible && !w->minimized?w->creationIdentity:0;
    if(identity!=g_metricsAppIdentity){g_metricsAppIdentity=identity;g_appCadence.Reset();g_metricsAppGeneration=0;}
    if(!identity)return;
    const bool dirty=!w->vfb && !w->externalCaptured && w->proc && w->proc->fbDirty;
    if(dirty || w->publishedGeneration!=g_metricsAppGeneration) {
        // Observed client publication cadence; not the app's internal CPU/GPU cost.
        g_appCadence.Record(now);g_metricsAppGeneration=w->publishedGeneration;
    }
}
static void DesktopNumber(char* out,unsigned value) {
    char rev[12];unsigned n=0;do{rev[n++]='0'+value%10;value/=10;}while(value && n<sizeof(rev));
    while(n)*out++=rev[--n];*out=0;
}
static void DesktopRate(char* out,uint64_t bytes) {
    // Decimal MB/s, one fractional digit. Never call the potentially locking printf.
    uint64_t tenths=bytes/100000;if(tenths>99999)tenths=99999;
    DesktopNumber(out,static_cast<unsigned>(tenths/10));while(*out)++out;*out++='.';*out++='0'+tenths%10;*out=0;
}
static void DrawMetricGraph(uint32_t* pixels,uint32_t stride,uint32_t sw,uint32_t sh,
                            int x,int y,int width,int height,const DesktopIntervals<>& history,uint64_t now) {
    if(width<=0||height<=0)return;
    const unsigned n=history.count,shown=n<static_cast<unsigned>(width)?n:width;
    for(unsigned i=0;i<shown;++i){const auto& v=history.samples[(history.next+128-shown+i)%128];
        if(now<v.tick || now-v.tick>10000)continue;
        unsigned value=v.ms>100?100:v.ms;int bar=static_cast<uint64_t>(value)*height/100;if(!bar)bar=1;
        WmFillRect(pixels,stride,sw,sh,x+width-shown+i,y+height-bar,1,bar,0xff68acff);
    }
}
static void DrawDesktopMetric(uint32_t* pixels,uint32_t stride,uint32_t sw,uint32_t sh,
                              const WmRect& r,unsigned kind,uint64_t now) {
    if(r.w<=0)return;
    const auto& p=WmCurrentPalette();const int line=WmDesktopFont().lineHeight;
    const int top=r.y,bottom=r.y+r.h-line;
    auto text=[&](int x,int y,const char* v,uint32_t color){WmRenderString(pixels,stride,sw,sh,x,y,v,color,p.taskbar,r.x+r.w-x);};
    if(kind<2){
        unsigned percent=kind?g_desktopStats.memoryPercent:g_desktopStats.cpuPercent;if(percent>100)percent=100;
        const uint32_t color=percent>=90?0xe06c75:percent>=70?0xe5bf69:0x63b782;
        text(r.x,top,kind?"MEM":"CPU",p.secondaryText);
        WmFillRect(pixels,stride,sw,sh,r.x,bottom,r.w,line,WmMaterialColor(p.taskButton));
        WmFillRect(pixels,stride,sw,sh,r.x,bottom,r.w*percent/100,line,color|0xff000000u);
        char value[12];DesktopNumber(value,percent);size_t n=strlen(value);value[n]='%';value[n+1]=0;
        text(r.x+WmMetric(3),bottom,value,p.taskText);return;
    }
    if(kind<4){
        char first[20],second[20],number[12];const bool net=kind==3;
        WmStrCopy(first,net?"D ":"R ",sizeof(first));DesktopRate(number,net?g_desktopStats.downBytesPerSecond:g_desktopStats.readBytesPerSecond);
        WmStrCopy(first+2,number,sizeof(first)-2);size_t n=strlen(first);WmStrCopy(first+n," MB/s",sizeof(first)-n);
        WmStrCopy(second,net?"U ":"W ",sizeof(second));DesktopRate(number,net?g_desktopStats.upBytesPerSecond:g_desktopStats.writeBytesPerSecond);
        WmStrCopy(second+2,number,sizeof(second)-2);n=strlen(second);WmStrCopy(second+n," MB/s",sizeof(second)-n);
        text(r.x,top,first,p.secondaryText);text(r.x,bottom,second,p.secondaryText);return;
    }
    const auto& history=kind==4?g_desktopCadence:g_appCadence;
    const auto summary=history.Read(now);
    text(r.x,top,kind==4?"COMP":"APP",p.secondaryText);
    DrawMetricGraph(pixels,stride,sw,sh,r.x+WmMetric(48),top,r.w-WmMetric(48),line-2,history,now);
    if(!summary.count || (kind==5 && (!history.last || now-history.last>2000))){text(r.x,bottom,"idle / no frames",p.mutedText);return;}
    char value[64]{};unsigned used=0;
    auto append=[&](const char* v){while(*v && used+1<sizeof(value))value[used++]=*v++;value[used]=0;};
    char n[12];append("80:");DesktopNumber(n,summary.p80);append(n);append(" 95:");DesktopNumber(n,summary.p95);append(n);append(" max:");DesktopNumber(n,summary.max);append(n);append("ms");
    text(r.x,bottom,value,p.secondaryText);
}
void WmDrawBuildId(uint32_t* pixels,uint32_t stride,uint32_t sw,uint32_t sh,const WmRect* clip) {
    if(!WmIsActive())return;
    char text[100]="Brook ";WmStrCopy(text+6,BuildGitHash(),sizeof(text)-6);
    const int width=WmTextWidth(text)+WmMetric(14),height=WmDesktopFont().lineHeight+WmMetric(6);
    if(sw<static_cast<uint32_t>(width)+WmMetric(10)||sh<WmMetric(WM_TASKBAR_HEIGHT)+height+WmMetric(6))return;
    const int x=sw-width-WmMetric(8),y=sh-WmMetric(WM_TASKBAR_HEIGHT)-height-WmMetric(6);
    const int left=clip && clip->x>x?clip->x:x,top=clip && clip->y>y?clip->y:y;
    const int right=clip && clip->x+clip->w<x+width?clip->x+clip->w:x+width;
    const int bottom=clip && clip->y+clip->h<y+height?clip->y+clip->h:y+height;
    if(right<=left||bottom<=top)return;
    WmFillRect(pixels,stride,sw,sh,left,top,right-left,bottom-top,0x172335);
    WmRenderString(pixels,stride,sw,sh,x+WmMetric(7),y+WmMetric(3),text,0xa4b2c7,0x172335,0,clip);
}
bool WmBootSummaryRect(uint32_t sw,uint32_t sh,int32_t* x,int32_t* y,uint32_t* width,uint32_t* height) {
    const uint64_t ready=CompositorDesktopReadyTick(),now=ApicTickCount();
    if(!ready||now<ready||now-ready>7000)return false;
    const uint32_t margin=WmMetric(16),w=WmMetric(480),h=WmDesktopFont().lineHeight*4+WmMetric(28);
    if(sw<margin*2+WmMetric(100)||sh<h+margin+WmMetric(WM_TASKBAR_HEIGHT))return false;
    *width=w<sw-margin*2?w:sw-margin*2;*height=h;*x=sw-*width-margin;*y=margin;return true;
}
void WmRenderBootSummary(uint32_t* pixels,uint32_t stride,uint32_t sw,uint32_t sh) {
    int32_t x,y;uint32_t w,h;if(!WmBootSummaryRect(sw,sh,&x,&y,&w,&h))return;
    const auto& p=WmCurrentPalette();WmFillRect(pixels,stride,sw,sh,x,y,w,h,p.taskbar);
    WmFillRect(pixels,stride,sw,sh,x,y,w,WmMetric(3),0x68acff);
    int py=y+WmMetric(11);const int line=WmDesktopFont().lineHeight;
    auto text=[&](const char* s,uint32_t color){WmRenderString(pixels,stride,sw,sh,x+WmMetric(12),py,s,color,p.taskbar,w-WmMetric(24));py+=line;};
    char number[16],row[96];const uint64_t duration=CompositorBootDurationMs();
    WmStrCopy(row,"Desktop ready in ",sizeof(row));DesktopNumber(number,duration/1000);
    size_t n=strlen(row);WmStrCopy(row+n,number,sizeof(row)-n);n=strlen(row);row[n++]='.';row[n++]='0'+(duration/100)%10;row[n++]='0'+(duration/10)%10;row[n]=0;
    WmStrCopy(row+n,KvmClockStableAvailable()?" s (VM start)":" s (kernel clock)",sizeof(row)-n);text(row,p.text);
    DesktopNumber(row,SmpGetCpuCount());n=strlen(row);WmStrCopy(row+n," cores | ",sizeof(row)-n);n=strlen(row);DesktopNumber(number,PmmGetFreePageCount()*4096/1048576);WmStrCopy(row+n,number,sizeof(row)-n);n=strlen(row);WmStrCopy(row+n," MiB available",sizeof(row)-n);text(row,p.secondaryText);
    text(CompositorGlassActive()?"GPU composition | glass":CompositorGpuActive()?"GPU composition | flat":"Software composition | flat",p.secondaryText);
    WmStrCopy(row,"Build ",sizeof(row));WmStrCopy(row+6,BuildGitHash(),sizeof(row)-6);text(row,p.mutedText);
}
