#include <cassert>
#include <sys/socket.h>
#include <initializer_list>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/if_addr.h>
#include "../../kernel/src/netlink_route.h"
using brook::InterfaceView;
struct Buffer {uint64_t before=0xabcdeffedcba1234;alignas(8) uint8_t bytes[8192]{};uint64_t after=0xabcdeffedcba1234;};
int main(){
 InterfaceView views[2]={{1,0x49,65536,0x0100007f,0xff,772,{},"lo"},{2,3,1500,0x0f02000a,0x00ffffff,1,{1,2,3,4,5,6},"eth0"}};
 struct Request {nlmsghdr h;rtgenmsg g;} req{{NLMSG_LENGTH(sizeof(rtgenmsg)),RTM_GETLINK,NLM_F_REQUEST|NLM_F_DUMP,900,0},{AF_UNSPEC}};
 Buffer buffer;
 auto run=[&](uint32_t size=8192){brook::RouteReply reply(buffer.bytes,size);assert(reply.build(&req,req.h.nlmsg_len,123,views,2)==0);return reply.size();};
 uint32_t length=run();uint32_t remaining=length;unsigned links=0,done=0;
 for(auto* h=reinterpret_cast<nlmsghdr*>(buffer.bytes);NLMSG_OK(h,remaining);h=NLMSG_NEXT(h,remaining)){
  assert(h->nlmsg_seq==900 && h->nlmsg_pid==123 && (h->nlmsg_flags&NLM_F_MULTI));
  if(h->nlmsg_type==NLMSG_DONE){assert(*reinterpret_cast<int*>(NLMSG_DATA(h))==0);++done;continue;}
  assert(h->nlmsg_type==RTM_NEWLINK && h->nlmsg_len>=NLMSG_LENGTH(sizeof(ifinfomsg)));
  auto* link=static_cast<ifinfomsg*>(NLMSG_DATA(h));assert(link->ifi_index==static_cast<int>(++links));
  int n=IFLA_PAYLOAD(h);bool name=false,mac=false,mtu=false;
  for(auto* a=IFLA_RTA(link);RTA_OK(a,n);a=RTA_NEXT(a,n)){
   if(a->rta_type==IFLA_IFNAME){assert(!strcmp(static_cast<char*>(RTA_DATA(a)),views[links-1].name));name=true;}
   if(a->rta_type==IFLA_ADDRESS){assert(RTA_PAYLOAD(a)==6 && !memcmp(RTA_DATA(a),views[links-1].mac,6));mac=true;}
   if(a->rta_type==IFLA_MTU){assert(*static_cast<uint32_t*>(RTA_DATA(a))==views[links-1].mtu);mtu=true;}
  }assert(n==0 && name && mac && mtu);
 }assert(remaining==0 && links==2 && done==1);
 req.h.nlmsg_type=RTM_GETADDR;length=run();remaining=length;unsigned addresses=0;
 for(auto* h=reinterpret_cast<nlmsghdr*>(buffer.bytes);NLMSG_OK(h,remaining);h=NLMSG_NEXT(h,remaining)){
  if(h->nlmsg_type==NLMSG_DONE)continue;assert(h->nlmsg_type==RTM_NEWADDR);
  auto* a=static_cast<ifaddrmsg*>(NLMSG_DATA(h));assert(a->ifa_index==++addresses && a->ifa_family==AF_INET);
  assert(a->ifa_prefixlen==(addresses==1?8:24));assert(a->ifa_scope==(addresses==1?254:0));
 }assert(addresses==2 && !remaining);
 for(unsigned type: {RTM_NEWADDR,RTM_GETROUTE}){req.h.nlmsg_type=type;length=run();auto* h=reinterpret_cast<nlmsghdr*>(buffer.bytes);assert(length==36 && h->nlmsg_type==NLMSG_ERROR);assert(static_cast<nlmsgerr*>(NLMSG_DATA(h))->error==-95);}
 req.h.nlmsg_type=RTM_GETLINK;req.h.nlmsg_len=15;{brook::RouteReply reply(buffer.bytes,8192);assert(reply.build(&req,17,123,views,2)==-22);}
 req.h.nlmsg_len=17;{brook::RouteReply reply(buffer.bytes,16);assert(reply.build(&req,17,123,views,2)==-105 && !reply.size());}
 // Bounded random framing and unaligned input exercise validation without
 // trusting any length from a sender. Guard words must survive every case.
 uint32_t state=90210;uint8_t input[128];
 for(unsigned trial=0;trial<20000;++trial){for(auto& b:input){state^=state<<13;state^=state>>17;state^=state<<5;b=state;}
  uint32_t n=state%sizeof(input);brook::RouteReply reply(buffer.bytes,8192);reply.build(input,n,123,views,2);
  assert(buffer.before==0xabcdeffedcba1234 && buffer.after==0xabcdeffedcba1234);
 }
 puts("netlink route wire checks passed");
}
