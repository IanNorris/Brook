#define _GNU_SOURCE
static long test_syscall(long number,...);
#define syscall test_syscall
static int failed_fence=-1;
#define DESKTOP_GPU_FENCE_FAILED(fd) ((fd)>=0 && (fd)==failed_fence)
static int failed_buffer=-1;
#define DESKTOP_GPU_BUFFER_FAILED(fd) ((fd)>=0 && (fd)==failed_buffer)
#include "desktop_gpu_channel.h"
#undef syscall
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
static int owner_valid=1,snapshot_tagged=0;static uint64_t snapshot_commit=0;static unsigned snapshot_queries=0;
static long test_syscall(long number,...){
    if(number==BROOK_SYS_WM_QUERY_SOURCE_COMMIT){
        va_list args;va_start(args,number);(void)va_arg(args,long);(void)va_arg(args,long);(void)va_arg(args,long);
        uint64_t *out=(uint64_t *)(uintptr_t)va_arg(args,long);va_end(args);++snapshot_queries;
        if(!snapshot_tagged){errno=ESTALE;return -1;}
        *out=snapshot_commit;return 0;
    }
    assert(number==BROOK_SYS_GPU_WINDOW_VALIDATE);return owner_valid?0:-1;}
static void expect_reply(int socket,uint32_t type,uint64_t commit){
    struct BrookGpuWindowPacket reply;int fds[2];
    assert(brook_gpu_window_receive(socket,&reply,fds)==1);
    assert(reply.type==type && reply.version==BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN && reply.identity.commit==commit);
}
static void expect_quiet(int socket){struct BrookGpuWindowPacket reply;int fds[2];assert(brook_gpu_window_receive(socket,&reply,fds)==0);}
/* Version 2: the newest presented offer stays selectable; SAFE follows a
 * newer presentation, supersession, a newer CPU snapshot or window loss. */
static void check_retained(void){
    struct DesktopGpuChannel c;desktop_gpu_channel_init(&c);
    int pair[2],fence[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair) && !pipe(fence));
    c.peer=pair[1];c.listener=dup(pair[1]);assert(c.listener>=0);
    int buffers[2];struct stat st[2];
    for(unsigned i=0;i<2;++i){char path[]="/tmp/brook-desktop-retain-XXXXXX";buffers[i]=mkstemp(path);assert(buffers[i]>=0);
        unlink(path);assert(!ftruncate(buffers[i],4096) && !fstat(buffers[i],&st[i]));}
    struct BrookGpuWindowPacket base={.magic=BROOK_GPU_WINDOW_WIRE_MAGIC,.version=BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN,.type=BROOK_GPU_WINDOW_OFFER,
        .words=1024,.stride=16,.width=16,.height=16,.fourcc=0x34325258};
#define OFFER(commit_,index_) do{struct BrookGpuWindowPacket p_=base;p_.identity=(struct BrookGpuWindowIdentity){4,0,400,(commit_),\
        (uint64_t)st[index_].st_dev,(uint64_t)st[index_].st_ino};assert(brook_gpu_window_send_offer(pair[0],&p_,buffers[index_],fence[0])==1);\
        desktop_gpu_channel_poll(&c);}while(0)
    struct BrookDesktopLayer layer={.window=4,.incarnation=400,.generation=9,.width=16,.height=16,.clientWidth=16,.clientHeight=16,.upscale=1};
    struct BrookVkSceneLayer scene;struct DesktopGpuOffer *o;
    OFFER(1,0);
    uint64_t f=desktop_gpu_channel_begin_frame(&c);o=desktop_gpu_channel_select(&c,&layer,100,100,&scene,f);
    assert(o && o->packet.identity.commit==1);
    desktop_gpu_channel_presented(&c,f);desktop_gpu_channel_finish(&c,f);
    expect_reply(pair[0],BROOK_GPU_WINDOW_PRESENTED,1);expect_quiet(pair[0]);
    /* Later frames keep sampling it without another notification. */
    f=desktop_gpu_channel_begin_frame(&c);o=desktop_gpu_channel_select(&c,&layer,100,100,&scene,f);
    assert(o && o->packet.identity.commit==1 && o->current);
    desktop_gpu_channel_presented(&c,f);desktop_gpu_channel_finish(&c,f);expect_quiet(pair[0]);
    /* An untagged CPU buffer never displaces the retained source. */
    assert(snapshot_queries>=1);
    /* A newer presentation releases the old source only after its frame. */
    OFFER(2,1);
    f=desktop_gpu_channel_begin_frame(&c);o=desktop_gpu_channel_select(&c,&layer,100,100,&scene,f);
    assert(o && o->packet.identity.commit==2);
    desktop_gpu_channel_presented(&c,f);desktop_gpu_channel_finish(&c,f);
    expect_reply(pair[0],BROOK_GPU_WINDOW_PRESENTED,2);expect_reply(pair[0],BROOK_GPU_WINDOW_SAFE,1);expect_quiet(pair[0]);
    /* Overlapping frames: the older frame still samples commit 2. */
    uint64_t older=desktop_gpu_channel_begin_frame(&c);o=desktop_gpu_channel_select(&c,&layer,100,100,&scene,older);
    assert(o && o->packet.identity.commit==2);
    OFFER(3,0);
    uint64_t later=desktop_gpu_channel_begin_frame(&c);o=desktop_gpu_channel_select(&c,&layer,100,100,&scene,later);
    assert(o && o->packet.identity.commit==3);
    desktop_gpu_channel_presented(&c,later);desktop_gpu_channel_finish(&c,later);
    expect_reply(pair[0],BROOK_GPU_WINDOW_PRESENTED,3);expect_quiet(pair[0]);
    desktop_gpu_channel_presented(&c,older);desktop_gpu_channel_finish(&c,older);
    expect_reply(pair[0],BROOK_GPU_WINDOW_SAFE,2);expect_quiet(pair[0]);
    /* While the window is not composed, an older unpresented offer is
     * superseded by a newer one; the current and newest offers are kept. */
    OFFER(4,1);OFFER(5,1);
    f=desktop_gpu_channel_begin_frame(&c);desktop_gpu_channel_finish(&c,f);
    expect_reply(pair[0],BROOK_GPU_WINDOW_SAFE,4);expect_quiet(pair[0]);
    /* A selection undone before submission leaves the offer available. */
    f=desktop_gpu_channel_begin_frame(&c);o=desktop_gpu_channel_select(&c,&layer,100,100,&scene,f);
    assert(o && o->packet.identity.commit==5);desktop_gpu_channel_unselect(&c,o,f);assert(o->state==1);
    desktop_gpu_channel_finish(&c,f);expect_quiet(pair[0]);
    f=desktop_gpu_channel_begin_frame(&c);o=desktop_gpu_channel_select(&c,&layer,100,100,&scene,f);
    assert(o && o->packet.identity.commit==5);desktop_gpu_channel_presented(&c,f);desktop_gpu_channel_finish(&c,f);
    expect_reply(pair[0],BROOK_GPU_WINDOW_PRESENTED,5);expect_reply(pair[0],BROOK_GPU_WINDOW_SAFE,3);expect_quiet(pair[0]);
    /* An older tagged CPU snapshot is ignored; one at least as new wins. */
    snapshot_tagged=1;snapshot_commit=4;
    f=desktop_gpu_channel_begin_frame(&c);assert(desktop_gpu_channel_select(&c,&layer,100,100,&scene,f));
    desktop_gpu_channel_presented(&c,f);desktop_gpu_channel_finish(&c,f);expect_quiet(pair[0]);
    snapshot_commit=5;
    f=desktop_gpu_channel_begin_frame(&c);assert(!desktop_gpu_channel_select(&c,&layer,100,100,&scene,f));
    desktop_gpu_channel_finish(&c,f);expect_reply(pair[0],BROOK_GPU_WINDOW_SAFE,5);expect_quiet(pair[0]);
    snapshot_tagged=0;
    /* A closed incarnation releases its current source without a frame. */
    OFFER(6,0);
    f=desktop_gpu_channel_begin_frame(&c);assert(desktop_gpu_channel_select(&c,&layer,100,100,&scene,f));
    desktop_gpu_channel_presented(&c,f);desktop_gpu_channel_finish(&c,f);expect_reply(pair[0],BROOK_GPU_WINDOW_PRESENTED,6);
    f=desktop_gpu_channel_begin_frame(&c);desktop_gpu_channel_finish(&c,f);expect_quiet(pair[0]);
    owner_valid=0;f=desktop_gpu_channel_begin_frame(&c);desktop_gpu_channel_finish(&c,f);
    expect_reply(pair[0],BROOK_GPU_WINDOW_SAFE,6);expect_quiet(pair[0]);owner_valid=1;
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i)assert(!c.offers[i].state);
#undef OFFER
    desktop_gpu_channel_close(&c);close(pair[0]);close(buffers[0]);close(buffers[1]);close(fence[0]);close(fence[1]);
}
/* App-driven cadence: PRESENTED for the dominant window's offer is sent at
 * submit, once; SAFE still waits for a newer presentation, and a frame that
 * then fails to present neither repeats the notification nor frees the
 * buffer. */
static void check_early_notify(void){
    struct DesktopGpuChannel c;desktop_gpu_channel_init(&c);
    int pair[2],fence[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair) && !pipe(fence));
    c.peer=pair[1];c.listener=dup(pair[1]);assert(c.listener>=0);
    int buffers[2];struct stat st[2];
    for(unsigned i=0;i<2;++i){char path[]="/tmp/brook-desktop-early-XXXXXX";buffers[i]=mkstemp(path);assert(buffers[i]>=0);
        unlink(path);assert(!ftruncate(buffers[i],4096) && !fstat(buffers[i],&st[i]));}
    struct BrookGpuWindowPacket base={.magic=BROOK_GPU_WINDOW_WIRE_MAGIC,.version=BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN,.type=BROOK_GPU_WINDOW_OFFER,
        .words=1024,.stride=16,.width=16,.height=16,.fourcc=0x34325258};
    struct BrookDesktopLayer layer={.window=6,.incarnation=600,.generation=9,.width=16,.height=16,.clientWidth=16,.clientHeight=16,.upscale=1};
    struct BrookVkSceneLayer scene;
    {struct BrookGpuWindowPacket p=base;p.identity=(struct BrookGpuWindowIdentity){6,0,600,1,(uint64_t)st[0].st_dev,(uint64_t)st[0].st_ino};
     assert(brook_gpu_window_send_offer(pair[0],&p,buffers[0],fence[0])==1);desktop_gpu_channel_poll(&c);}
    uint64_t f=desktop_gpu_channel_begin_frame(&c);assert(desktop_gpu_channel_select(&c,&layer,100,100,&scene,f));
    assert(!desktop_gpu_channel_notify_early(&c,f,7));expect_quiet(pair[0]); /* another window */
    assert(desktop_gpu_channel_notify_early(&c,f,6)==1);expect_reply(pair[0],BROOK_GPU_WINDOW_PRESENTED,1);
    assert(!desktop_gpu_channel_notify_early(&c,f,6));
    desktop_gpu_channel_presented(&c,f);desktop_gpu_channel_finish(&c,f);expect_quiet(pair[0]);
    /* Commit 2: notified early, then its present fails (finish without
     * presented). Commit 1 stays current and commit 2 is not freed. */
    {struct BrookGpuWindowPacket p=base;p.identity=(struct BrookGpuWindowIdentity){6,0,600,2,(uint64_t)st[1].st_dev,(uint64_t)st[1].st_ino};
     assert(brook_gpu_window_send_offer(pair[0],&p,buffers[1],fence[0])==1);desktop_gpu_channel_poll(&c);}
    f=desktop_gpu_channel_begin_frame(&c);struct DesktopGpuOffer *o=desktop_gpu_channel_select(&c,&layer,100,100,&scene,f);
    assert(o && o->packet.identity.commit==2 && desktop_gpu_channel_notify_early(&c,f,6)==1);
    expect_reply(pair[0],BROOK_GPU_WINDOW_PRESENTED,2);
    desktop_gpu_channel_finish(&c,f);expect_quiet(pair[0]);
    /* Retried and presented: no second PRESENTED; commit 1 is now SAFE. */
    f=desktop_gpu_channel_begin_frame(&c);o=desktop_gpu_channel_select(&c,&layer,100,100,&scene,f);
    assert(o && o->packet.identity.commit==2 && !desktop_gpu_channel_notify_early(&c,f,6));
    desktop_gpu_channel_presented(&c,f);desktop_gpu_channel_finish(&c,f);
    expect_reply(pair[0],BROOK_GPU_WINDOW_SAFE,1);expect_quiet(pair[0]);
    desktop_gpu_channel_close(&c);close(pair[0]);close(buffers[0]);close(buffers[1]);close(fence[0]);close(fence[1]);
}
/* gen 375: a producer frame whose fence failed is never sampled (a
 * compositor job waiting on it would fail and park the renderer); the
 * window falls back to its CPU source until a newer good offer arrives. */
static void check_failed_fence(void){
    struct DesktopGpuChannel c;desktop_gpu_channel_init(&c);
    int pair[2],bad[2],good[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair) && !pipe(bad) && !pipe(good));
    c.peer=pair[1];c.listener=dup(pair[1]);assert(c.listener>=0);
    int buffers[2];struct stat st[2];
    for(unsigned i=0;i<2;++i){char path[]="/tmp/brook-desktop-failed-XXXXXX";buffers[i]=mkstemp(path);assert(buffers[i]>=0);
        unlink(path);assert(!ftruncate(buffers[i],4096) && !fstat(buffers[i],&st[i]));}
    struct BrookGpuWindowPacket base={.magic=BROOK_GPU_WINDOW_WIRE_MAGIC,.version=BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN,.type=BROOK_GPU_WINDOW_OFFER,
        .words=1024,.stride=16,.width=16,.height=16,.fourcc=0x34325258};
    struct BrookDesktopLayer layer={.window=5,.incarnation=500,.generation=9,.width=16,.height=16,.clientWidth=16,.clientHeight=16,.upscale=1};
    struct BrookVkSceneLayer scene;
    struct BrookGpuWindowPacket p=base;p.identity=(struct BrookGpuWindowIdentity){5,0,500,1,(uint64_t)st[0].st_dev,(uint64_t)st[0].st_ino};
    assert(brook_gpu_window_send_offer(pair[0],&p,buffers[0],bad[0])==1);desktop_gpu_channel_poll(&c);
    /* The received fence fd is a duplicate; mark whichever fd the offer holds. */
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i)if(c.offers[i].state==1 && c.offers[i].packet.identity.window==5)failed_fence=c.offers[i].fence;
    uint64_t f=desktop_gpu_channel_begin_frame(&c);
    assert(!desktop_gpu_channel_select(&c,&layer,100,100,&scene,f));
    desktop_gpu_channel_finish(&c,f);
    f=desktop_gpu_channel_begin_frame(&c);
    assert(!desktop_gpu_channel_select(&c,&layer,100,100,&scene,f)); /* stays excluded */
    desktop_gpu_channel_finish(&c,f);
    p.identity.commit=2;p.identity.device=(uint64_t)st[1].st_dev;p.identity.inode=(uint64_t)st[1].st_ino;
    assert(brook_gpu_window_send_offer(pair[0],&p,buffers[1],good[0])==1);desktop_gpu_channel_poll(&c);
    f=desktop_gpu_channel_begin_frame(&c);
    struct DesktopGpuOffer *o=desktop_gpu_channel_select(&c,&layer,100,100,&scene,f);
    assert(o && o->packet.identity.commit==2);
    desktop_gpu_channel_finish(&c,f);
    failed_fence=-1;
    /* A sticky-failed buffer behind an offer whose fence succeeded (the
     * gen 377 retained-offer case) is excluded the same way. */
    failed_buffer=o->buffer;
    f=desktop_gpu_channel_begin_frame(&c);
    assert(!desktop_gpu_channel_select(&c,&layer,100,100,&scene,f) && o->failed);
    desktop_gpu_channel_finish(&c,f);
    failed_buffer=-1;
}
/* Version 3: a layered offer (16x16 page + 8x8 video at 4,5 scaled to 6x6
 * of window pixels) is selected as one unit, its layers are placed at the
 * client origin and clipped to the client/VFB crop, and SAFE releases every
 * received fd. */
static int open_fds(void){int n=0;for(int fd=0;fd<1024;++fd)if(fcntl(fd,F_GETFD)!=-1)++n;return n;}
static void check_layers(void){
    struct DesktopGpuChannel c;desktop_gpu_channel_init(&c);
    int pair[2],fence[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair) && !pipe(fence));
    c.peer=pair[1];c.listener=dup(pair[1]);
    char a[]="/tmp/brook-layers-XXXXXX",b[]="/tmp/brook-layers-XXXXXX";
    int page=mkstemp(a),video=mkstemp(b);assert(page>=0 && video>=0);unlink(a);unlink(b);
    assert(!ftruncate(page,16*16*4) && !ftruncate(video,8*8*4));
    struct stat sp,sv;assert(!fstat(page,&sp) && !fstat(video,&sv));
    struct BrookGpuWindowLayersPacket w;memset(&w,0,sizeof(w));
    w.header=(struct BrookGpuWindowPacket){.magic=BROOK_GPU_WINDOW_WIRE_MAGIC,.version=BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS,
        .type=BROOK_GPU_WINDOW_OFFER,.identity={4,0,400,1,(uint64_t)sp.st_dev,(uint64_t)sp.st_ino},
        .words=256,.stride=16,.width=16,.height=16,.fourcc=0x34325241};
    w.count=2;
    w.layers[0]=(struct BrookGpuWindowLayer){256,(uint64_t)sp.st_dev,(uint64_t)sp.st_ino,0,16,16,16,0x34325241,0,0,0,16,16,0,0,16,16};
    w.layers[1]=(struct BrookGpuWindowLayer){64,(uint64_t)sv.st_dev,(uint64_t)sv.st_ino,0,8,8,8,0x34325258,0,0,0,8,8,4,5,6,6};
    const int before=open_fds();
    int buffers[2]={page,video},fences[2]={fence[0],fence[0]};
    assert(brook_gpu_window_send_layers(pair[0],&w,buffers,fences)==1);
    desktop_gpu_channel_poll(&c);assert(open_fds()==before+4);
    uint64_t frame=desktop_gpu_channel_begin_frame(&c);
    /* Client 12x14 at 30,40 on a 16x16 VFB: crop 12x14. Layered offers are
     * not refused for the client/VFB mismatch. */
    struct BrookDesktopLayer l={.window=4,.incarnation=400,.width=16,.height=16,.clientWidth=12,.clientHeight=14,
        .clientX=30,.clientY=40,.upscale=1};
    struct BrookVkSceneLayer out={0};struct DesktopGpuOffer *o=desktop_gpu_channel_select(&c,&l,100,100,&out,frame);
    assert(o && desktop_gpu_offer_layer_count(o)==2 && desktop_gpu_offer_buffer(o,1)>=0 && desktop_gpu_offer_fence(o,1)>=0);
    assert(out.x==30 && out.y==40 && out.width==16 && out.sourceWidthUsed==16 && out.clipX==30 && out.clipY==40 &&
           out.clipWidth==12 && out.clipHeight==14 && !(out.opacity&BROOK_VK_OPAQUE_SOURCE));
    struct BrookVkSceneLayer v;desktop_gpu_offer_scene_layer(o,&l,100,100,1,&v);
    assert(v.x==34 && v.y==45 && v.width==6 && v.height==6 && v.sourceWidth==8 && v.sourceWidthUsed==8 &&
           v.clipWidth==12 && (v.opacity&BROOK_VK_OPAQUE_SOURCE) && (v.opacity&255)==255);
    /* Clipped to the screen as well. */
    struct BrookDesktopLayer edge=l;edge.clientX=95;desktop_gpu_offer_scene_layer(o,&edge,100,100,1,&v);assert(v.clipX==95 && v.clipWidth==5);
    desktop_gpu_channel_presented(&c,frame);
    struct BrookGpuWindowPacket reply;int fds[2];
    assert(brook_gpu_window_receive(pair[0],&reply,fds)==1 && reply.type==BROOK_GPU_WINDOW_PRESENTED &&
           reply.version==BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS && reply.identity.commit==1);
    desktop_gpu_channel_finish(&c,frame);
    /* A newer single offer for the window supersedes it once presented. */
    struct BrookGpuWindowPacket single=w.header;single.version=BROOK_GPU_WINDOW_WIRE_VERSION_RETAIN;single.identity.commit=2;
    assert(brook_gpu_window_send_offer(pair[0],&single,page,fence[0])==1);desktop_gpu_channel_poll(&c);
    frame=desktop_gpu_channel_begin_frame(&c);
    struct BrookDesktopLayer same={.window=4,.incarnation=400,.width=16,.height=16,.clientWidth=16,.clientHeight=16,.upscale=1};
    struct DesktopGpuOffer *n=desktop_gpu_channel_select(&c,&same,100,100,&out,frame);assert(n && n->packet.identity.commit==2);
    desktop_gpu_channel_presented(&c,frame);desktop_gpu_channel_finish(&c,frame);
    /* PRESENTED for 2, then SAFE for the layered 1: every fd it held is closed. */
    int sawSafe=0;for(int k=0;k<4;++k){if(brook_gpu_window_receive(pair[0],&reply,fds)!=1)break;
        if(reply.type==BROOK_GPU_WINDOW_SAFE && reply.identity.commit==1){assert(reply.version==BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS);sawSafe=1;}}
    assert(sawSafe && open_fds()==before+2);
    desktop_gpu_channel_close(&c);assert(open_fds()==before-2);
    close(page);close(video);close(fence[0]);close(fence[1]);close(pair[0]);
}
/* Gen 402 freeze: a layered offer that was never presented, followed only
 * by tagged CPU snapshots, must still be retired (SAFE), or the client
 * never gets its buffers back. */
static void check_unpresented_superseded_by_cpu(void){
    struct DesktopGpuChannel c;desktop_gpu_channel_init(&c);
    int pair[2],fence[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair) && !pipe(fence));
    c.peer=pair[1];c.listener=dup(pair[1]);
    char a[]="/tmp/brook-unpresented-XXXXXX",b[]="/tmp/brook-unpresented-XXXXXX";
    int page=mkstemp(a),ad=mkstemp(b);assert(page>=0 && ad>=0);unlink(a);unlink(b);
    assert(!ftruncate(page,16*16*4) && !ftruncate(ad,8*8*4));
    struct stat sp,sa;assert(!fstat(page,&sp) && !fstat(ad,&sa));
    struct BrookGpuWindowLayersPacket w;memset(&w,0,sizeof(w));
    w.header=(struct BrookGpuWindowPacket){.magic=BROOK_GPU_WINDOW_WIRE_MAGIC,.version=BROOK_GPU_WINDOW_WIRE_VERSION_LAYERS,
        .type=BROOK_GPU_WINDOW_OFFER,.identity={6,0,600,7,(uint64_t)sp.st_dev,(uint64_t)sp.st_ino},
        .words=256,.stride=16,.width=16,.height=16,.fourcc=0x34325241};
    w.count=2;
    w.layers[0]=(struct BrookGpuWindowLayer){256,(uint64_t)sp.st_dev,(uint64_t)sp.st_ino,0,16,16,16,0x34325241,0,0,0,16,16,0,0,16,16};
    w.layers[1]=(struct BrookGpuWindowLayer){64,(uint64_t)sa.st_dev,(uint64_t)sa.st_ino,0,8,8,8,0x34325241,0,0,0,8,8,2,2,8,8};
    int buffers[2]={page,ad},fences[2]={fence[0],fence[0]};
    assert(brook_gpu_window_send_layers(pair[0],&w,buffers,fences)==1);desktop_gpu_channel_poll(&c);
    /* The provider then fell back to CPU composition: snapshot tagged 8. */
    snapshot_tagged=1;snapshot_commit=8;
    struct BrookDesktopLayer l={.window=6,.incarnation=600,.width=16,.height=16,.clientWidth=16,.clientHeight=16,.upscale=1};
    uint64_t frame=desktop_gpu_channel_begin_frame(&c);struct BrookVkSceneLayer out;
    assert(!desktop_gpu_channel_select(&c,&l,100,100,&out,frame));
    desktop_gpu_channel_finish(&c,frame);
    struct BrookGpuWindowPacket reply;int fds[2];
    assert(brook_gpu_window_receive(pair[0],&reply,fds)==1 && reply.type==BROOK_GPU_WINDOW_SAFE && reply.identity.commit==7);
    for(unsigned i=0;i<DESKTOP_GPU_OFFERS;++i)assert(!c.offers[i].state);
    snapshot_tagged=0;snapshot_commit=0;
    desktop_gpu_channel_close(&c);close(page);close(ad);close(fence[0]);close(fence[1]);close(pair[0]);
}
int main(void){check_early_notify();check_failed_fence();check_layers();check_unpresented_superseded_by_cpu();
    char dir[]="/tmp/brook-channel-restart-XXXXXX";assert(mkdtemp(dir));
    char socketPath[108];assert(snprintf(socketPath,sizeof(socketPath),"%s/socket",dir)>0);
    assert(!setenv("BROOK_DESKTOP_GPU_SOCKET",socketPath,1));
    assert(!setenv("BROOK_DESKTOP_GPU_GID","invalid",1));
    struct DesktopGpuChannel invalid;desktop_gpu_channel_init(&invalid);assert(invalid.listener<0);
    struct stat missing;assert(lstat(socketPath,&missing)<0 && errno==ENOENT);
    char group[32];assert(snprintf(group,sizeof(group),"%lu",(unsigned long)getegid())>0);
    assert(!setenv("BROOK_DESKTOP_GPU_GID",group,1));
    struct DesktopGpuChannel first,second;desktop_gpu_channel_init(&first);assert(first.listener>=0);
    struct stat mode;assert(!lstat(socketPath,&mode) && S_ISSOCK(mode.st_mode));
    assert(mode.st_uid==geteuid() && mode.st_gid==getegid() && (mode.st_mode&0777)==0770);
    desktop_gpu_channel_init(&second);assert(second.listener<0);
    close(first.listener);first.listener=-1;first.path[0]=0; /* crash: pathname remains */
    desktop_gpu_channel_init(&second);assert(second.listener>=0);
    desktop_gpu_channel_close(&second);assert(!rmdir(dir));unsetenv("BROOK_DESKTOP_GPU_SOCKET");unsetenv("BROOK_DESKTOP_GPU_GID");
    struct DesktopGpuChannel receiver;desktop_gpu_channel_init(&receiver);
    uint64_t frame=desktop_gpu_channel_begin_frame(&receiver);assert(frame);
    int pair[2],fence[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair) && !pipe(fence));
    receiver.peer=pair[1];receiver.listener=dup(pair[1]);assert(receiver.listener>=0);
    char path[]="/tmp/brook-desktop-channel-XXXXXX";int buffer=mkstemp(path);assert(buffer>=0);unlink(path);assert(!ftruncate(buffer,4096));
    struct stat st;assert(!fstat(buffer,&st));
    struct BrookGpuWindowPacket p={.magic=BROOK_GPU_WINDOW_WIRE_MAGIC,.version=BROOK_GPU_WINDOW_WIRE_VERSION,.type=BROOK_GPU_WINDOW_OFFER,
        .identity={2,0,101,7,(uint64_t)st.st_dev,(uint64_t)st.st_ino},.words=1024,.stride=16,.width=16,.height=16,.fourcc=0x34325258};
    assert(brook_gpu_window_send_offer(pair[0],&p,buffer,fence[0])==1);
    struct BrookGpuWindowPacket newer=p;newer.identity.commit=8;assert(brook_gpu_window_send_offer(pair[0],&newer,buffer,fence[0])==1);
    desktop_gpu_channel_poll(&receiver);
    struct BrookDesktopLayer layer={.window=2,.incarnation=102,.width=32,.height=32,.clientWidth=32,.clientHeight=32,.clientX=10,.clientY=20,.upscale=1};
    struct BrookVkSceneLayer scene={0};assert(!desktop_gpu_channel_select(&receiver,&layer,100,100,&scene,frame));
    layer.incarnation=101;owner_valid=0;assert(!desktop_gpu_channel_select(&receiver,&layer,100,100,&scene,frame));owner_valid=1;
    struct DesktopGpuOffer *selected=desktop_gpu_channel_select(&receiver,&layer,100,100,&scene,frame);
    assert(selected && selected->packet.identity.commit==8 && scene.sourceWidth==16 && scene.width==32 && scene.x==10 && scene.y==20);
    struct BrookGpuWindowPacket reply;int fds[2];assert(brook_gpu_window_receive(pair[0],&reply,fds)==0);
    desktop_gpu_channel_presented(&receiver,frame);
    desktop_gpu_channel_finish(&receiver,frame);
    assert(brook_gpu_window_receive(pair[0],&reply,fds)==1 && reply.type==BROOK_GPU_WINDOW_SAFE && reply.identity.commit==7);
    assert(brook_gpu_window_receive(pair[0],&reply,fds)==1 && reply.type==BROOK_GPU_WINDOW_SAFE && reply.identity.commit==8);
    assert(brook_gpu_window_send_offer(pair[0],&p,buffer,fence[0])==1);desktop_gpu_channel_poll(&receiver);
    assert(!desktop_gpu_channel_select(&receiver,&layer,100,100,&scene,frame));
    desktop_gpu_channel_finish(&receiver,frame);assert(brook_gpu_window_receive(pair[0],&reply,fds)==1 && reply.identity.commit==7);
    p.identity.incarnation=103;layer.incarnation=103;assert(brook_gpu_window_send_offer(pair[0],&p,buffer,fence[0])==1);
    desktop_gpu_channel_poll(&receiver);assert(desktop_gpu_channel_select(&receiver,&layer,100,100,&scene,frame));
    desktop_gpu_channel_presented(&receiver,frame);desktop_gpu_channel_finish(&receiver,frame);
    assert(brook_gpu_window_receive(pair[0],&reply,fds)==1 && reply.identity.incarnation==103);
    /* Client size differs from the committed VFB (resize in flight): the
     * offer of the VFB itself is drawn 1:1 and cropped, like the CPU source. */
    {struct BrookDesktopLayer resized={.window=2,.incarnation=104,.width=16,.height=16,.clientWidth=12,.clientHeight=10,
        .clientX=3,.clientY=4,.upscale=1};
     p.identity.incarnation=104;assert(brook_gpu_window_send_offer(pair[0],&p,buffer,fence[0])==1);desktop_gpu_channel_poll(&receiver);
     const uint64_t shape=g_desktopGpuSelect[DESKTOP_GPU_REFUSE_SHAPE];
     struct BrookDesktopLayer bigger=resized;bigger.width=bigger.height=32;
     assert(!desktop_gpu_channel_select(&receiver,&bigger,100,100,&scene,frame));
     assert(g_desktopGpuSelect[DESKTOP_GPU_REFUSE_SHAPE]==shape+1);
     struct DesktopGpuOffer *cropped=desktop_gpu_channel_select(&receiver,&resized,100,100,&scene,frame);
     assert(cropped && scene.sourceWidth==16 && scene.sourceWidthUsed==12 && scene.sourceHeightUsed==10 &&
            scene.width==12 && scene.height==10 && scene.x==3 && scene.y==4);
     desktop_gpu_channel_presented(&receiver,frame);desktop_gpu_channel_finish(&receiver,frame);
     assert(brook_gpu_window_receive(pair[0],&reply,fds)==1 && reply.identity.incarnation==104);}
    /* Two windows may share a title. The channel never sees titles: exercise
     * interleaved sources and commits, then reuse one slot with a new owner. */
    char path2[]="/tmp/brook-desktop-channel-XXXXXX";int buffer2=mkstemp(path2);assert(buffer2>=0);unlink(path2);assert(!ftruncate(buffer2,4096));
    struct stat st2;assert(!fstat(buffer2,&st2));assert(st.st_ino!=st2.st_ino || st.st_dev!=st2.st_dev);
    struct BrookGpuWindowPacket a=p,b=p;
    a.identity=(struct BrookGpuWindowIdentity){2,0,103,8,(uint64_t)st.st_dev,(uint64_t)st.st_ino};
    b.identity=(struct BrookGpuWindowIdentity){3,0,201,2,(uint64_t)st2.st_dev,(uint64_t)st2.st_ino};
    assert(brook_gpu_window_send_offer(pair[0],&b,buffer2,fence[0])==1);
    assert(brook_gpu_window_send_offer(pair[0],&a,buffer,fence[0])==1);
    desktop_gpu_channel_poll(&receiver);
    struct BrookDesktopLayer la=layer,lb=layer;la.window=2;la.incarnation=103;lb.window=3;lb.incarnation=201;
    struct BrookVkSceneLayer sa={0},sb={0};
    struct DesktopGpuOffer *oa=desktop_gpu_channel_select(&receiver,&la,100,100,&sa,frame);
    struct DesktopGpuOffer *ob=desktop_gpu_channel_select(&receiver,&lb,100,100,&sb,frame);
    assert(oa && ob && oa!=ob && oa->packet.identity.inode==(uint64_t)st.st_ino && ob->packet.identity.inode==(uint64_t)st2.st_ino);
    desktop_gpu_channel_presented(&receiver,frame);desktop_gpu_channel_finish(&receiver,frame);
    assert(brook_gpu_window_receive(pair[0],&reply,fds)==1 && brook_gpu_window_identity_equal(&reply.identity,&b.identity));
    assert(brook_gpu_window_receive(pair[0],&reply,fds)==1 && brook_gpu_window_identity_equal(&reply.identity,&a.identity));
    a.identity.commit=9;assert(brook_gpu_window_send_offer(pair[0],&a,buffer,fence[0])==1);
    desktop_gpu_channel_poll(&receiver);la.incarnation=104;assert(!desktop_gpu_channel_select(&receiver,&la,100,100,&sa,frame));
    desktop_gpu_channel_finish(&receiver,frame);assert(brook_gpu_window_receive(pair[0],&reply,fds)==1);
    a.identity.incarnation=104;a.identity.commit=1;assert(brook_gpu_window_send_offer(pair[0],&a,buffer,fence[0])==1);
    desktop_gpu_channel_poll(&receiver);assert(desktop_gpu_channel_select(&receiver,&la,100,100,&sa,frame));
    desktop_gpu_channel_presented(&receiver,frame);desktop_gpu_channel_finish(&receiver,frame);
    assert(brook_gpu_window_receive(pair[0],&reply,fds)==1 && brook_gpu_window_identity_equal(&reply.identity,&a.identity));
    /* Completion and presentation of a newer frame must not ACK or publish
     * an offer retained by another in-flight frame. */
    uint64_t older=desktop_gpu_channel_begin_frame(&receiver),later=desktop_gpu_channel_begin_frame(&receiver);
    assert(older && later && older!=later);a.identity.commit=2;b.identity.commit=3;
    assert(brook_gpu_window_send_offer(pair[0],&a,buffer,fence[0])==1);
    assert(brook_gpu_window_send_offer(pair[0],&b,buffer2,fence[0])==1);desktop_gpu_channel_poll(&receiver);
    assert(desktop_gpu_channel_select(&receiver,&la,100,100,&sa,older));
    assert(desktop_gpu_channel_select(&receiver,&lb,100,100,&sb,later));
    desktop_gpu_channel_presented(&receiver,later);desktop_gpu_channel_finish(&receiver,later);
    assert(brook_gpu_window_receive(pair[0],&reply,fds)==1 && brook_gpu_window_identity_equal(&reply.identity,&b.identity));
    assert(brook_gpu_window_receive(pair[0],&reply,fds)==0);
    assert(receiver.presented[1].commit==1 && receiver.presented[2].commit==3);
    desktop_gpu_channel_presented(&receiver,older);desktop_gpu_channel_finish(&receiver,older);
    assert(brook_gpu_window_receive(pair[0],&reply,fds)==1 && brook_gpu_window_identity_equal(&reply.identity,&a.identity));
    /* Distinct offers can alias one DMA-BUF: neither ACK is safe until both
     * GPU users have completed, regardless of frame or window identity. */
    older=desktop_gpu_channel_begin_frame(&receiver);later=desktop_gpu_channel_begin_frame(&receiver);
    a.identity.commit=3;b.identity.commit=4;b.identity.device=(uint64_t)st.st_dev;b.identity.inode=(uint64_t)st.st_ino;
    assert(brook_gpu_window_send_offer(pair[0],&a,buffer,fence[0])==1);
    assert(brook_gpu_window_send_offer(pair[0],&b,buffer,fence[0])==1);desktop_gpu_channel_poll(&receiver);
    assert(desktop_gpu_channel_select(&receiver,&la,100,100,&sa,older));
    assert(desktop_gpu_channel_select(&receiver,&lb,100,100,&sb,later));
    desktop_gpu_channel_finish(&receiver,later);assert(brook_gpu_window_receive(pair[0],&reply,fds)==0);
    desktop_gpu_channel_finish(&receiver,older);
    assert(brook_gpu_window_receive(pair[0],&reply,fds)==1);
    assert(brook_gpu_window_receive(pair[0],&reply,fds)==1);
    /* Exhaustion fails closed instead of reusing an identifier. */
    receiver.nextFrame=UINT64_MAX;assert(!desktop_gpu_channel_begin_frame(&receiver));
    close(buffer2);
    desktop_gpu_channel_close(&receiver);close(pair[0]);close(buffer);close(fence[0]);close(fence[1]);
    check_retained();
    puts("DESKTOP_GPU_CHANNEL_PASS");return 0;
}
