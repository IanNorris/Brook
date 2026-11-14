#define _GNU_SOURCE
static long test_syscall(long number,...);
#define syscall test_syscall
#include "desktop_gpu_channel.h"
#undef syscall
#include <assert.h>
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
int main(void){
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
