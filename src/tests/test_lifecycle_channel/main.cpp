#include <cstdio>
#include <cstring>
#include <string>
#include "lifecycle_channel.h"
using brook::LifecycleChannel;
namespace {
int failures=0,calls=0,result=0;
std::string boot;
constexpr const char* cookie="11111111111111111111111111111111";
constexpr const char* proposal="22222222222222222222222222222222";
constexpr const char* old="33333333333333333333333333333333";
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"LIFECYCLE FAIL %d: %s\n",__LINE__,#x);++failures;}}while(0)
void epoch(const char* p,char out[33]){if(boot.empty()&&p)boot=p;std::strcpy(out,boot.empty()?"00000000000000000000000000000000":boot.c_str());}
int powerCalls=0;
void power(const char*,const char*,char state[24],int* error){++powerCalls;std::strcpy(state,"closing");*error=0;}
int sync(){++calls;return result;}
std::string id(unsigned n){char out[33];std::snprintf(out,sizeof(out),"%032x",n);return out;}
bool send(LifecycleChannel& c,unsigned request,const char* verb,char out[160],const char* expected=proposal) {
    std::string line=std::string("BLC1 ")+cookie+" "+id(request)+" "+expected+" "+verb;
    return c.Handle(line.c_str(),out,epoch,sync);
}
}
int main() {
    LifecycleChannel c;char out[160];
    CHECK(!c.Init("bad"));CHECK(!send(c,1,"hello",out));CHECK(c.Init(cookie));
    CHECK(!send(c,1,"sync",out));CHECK(calls==0); // Uninitialized boot cannot authorize sync.
    CHECK(send(c,1,"hello",out));CHECK(boot==proposal);CHECK(calls==0);
    CHECK(send(c,2,"hello",out,old));CHECK(boot==proposal); // Immutable boot.
    CHECK(send(c,3,"sync",out,old));CHECK(std::strstr(out," stale -116\n"));CHECK(calls==0);
    CHECK(send(c,3,"status",out));CHECK(std::strstr(out," missing -2\n"));CHECK(calls==0);
    CHECK(send(c,3,"sync",out));CHECK(std::strstr(out," done 0\n"));CHECK(calls==1);
    CHECK(send(c,3,"sync",out));CHECK(send(c,3,"status",out));CHECK(calls==1);
    result=-5;CHECK(send(c,4,"sync",out));CHECK(std::strstr(out," done -5\n"));CHECK(calls==2);
    result=0;CHECK(send(c,4,"sync",out));CHECK(std::strstr(out," done -5\n"));CHECK(calls==2);
    CHECK(send(c,5,"exec",out));CHECK(std::strstr(out," unsupported -95\n"));CHECK(calls==2);
    for(auto text:{"", "BLC1", " BLC1", "BLC1  bad", "\n", "\t"})CHECK(!c.Handle(text,out,epoch,sync));
    std::string good=std::string("BLC1 ")+cookie+" "+id(5)+" "+proposal+" sync";
    for(unsigned at:{5U,38U,71U}){auto bad=good;bad[at]='z';CHECK(!c.Handle(bad.c_str(),out,epoch,sync));}
    for(auto suffix:{" extra", " ", "\n"})CHECK(!c.Handle((good+suffix).c_str(),out,epoch,sync)||std::strstr(out,"unsupported"));
    CHECK(!c.Handle(std::string(500,'a').c_str(),out,epoch,sync));CHECK(calls==2);
    for(unsigned i=5;i<67;++i)CHECK(send(c,i,"sync",out));
    CHECK(calls==64);CHECK(send(c,67,"sync",out));CHECK(std::strstr(out," full -28\n"));CHECK(calls==64);
    CHECK(send(c,3,"sync",out));CHECK(std::strstr(out," done 0\n"));CHECK(calls==64);
    CHECK(send(c,67,"status",out));CHECK(std::strstr(out," missing -2\n"));
    std::string powerLine=std::string("BLC1 ")+cookie+" "+id(90)+" "+proposal+" shutdown";
    CHECK(c.Handle(powerLine.c_str(),out,epoch,sync,power));CHECK(powerCalls==1);CHECK(std::strstr(out," closing 0\n"));
    powerLine=std::string("BLC1 ")+cookie+" "+id(90)+" "+old+" shutdown";
    CHECK(c.Handle(powerLine.c_str(),out,epoch,sync,power));CHECK(powerCalls==1);CHECK(std::strstr(out," stale -116\n"));
    LifecycleChannel reboot;CHECK(reboot.Init(cookie));boot=old;
    CHECK(send(reboot,3,"sync",out));CHECK(std::strstr(out," stale -116\n"));CHECK(calls==64);
    CHECK(send(reboot,3,"status",out,old));CHECK(std::strstr(out," missing -2\n"));
    std::printf("LIFECYCLE_CHANNEL %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
