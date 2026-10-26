// Native Linux x86-64 SysV ABI control for the public SteamClient020 prefix.
// No SDK headers, implementations, credentials or game binaries are included.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
static uint64_t origin;
static uint64_t milliseconds(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void stage(const char* name){printf("STEAM_NATIVE phase=%s ms=%llu pid=%lu\n",name,(unsigned long long)(milliseconds()-origin),(unsigned long)getpid());fflush(stdout);}
int main(int argc,char** argv){
 if(argc!=3)return 2;
 setvbuf(stdout,NULL,_IONBF,0);alarm(30);origin=milliseconds();stage("BEGIN");
 stage("LIBRARY_BEGIN");void* module=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);printf("STEAM_NATIVE library_loaded=%u\n",module!=NULL);stage("LIBRARY_END");if(!module)return 1;
 void* (*factory)(const char*,int*)=(void*(*)(const char*,int*))dlsym(module,"CreateInterface");if(!factory){stage("MISSING_FACTORY");return 1;}
 int error=0;stage("CLIENT_BEGIN");void* client=factory("SteamClient020",&error);printf("STEAM_NATIVE client_present=%u error=%d\n",client!=NULL,error);stage("CLIENT_END");if(!client)return 1;
 void** table=*(void***)client;
 int32_t (*create_pipe)(void*)=(int32_t(*)(void*))table[0];bool (*release_pipe)(void*,int32_t)=(bool(*)(void*,int32_t))table[1];
 int32_t (*connect_user)(void*,int32_t)=(int32_t(*)(void*,int32_t))table[2];void (*release_user)(void*,int32_t,int32_t)=(void(*)(void*,int32_t,int32_t))table[4];
 void* (*get_friends)(void*,int32_t,int32_t,const char*)=(void*(*)(void*,int32_t,int32_t,const char*))table[8];
 stage("PIPE_BEGIN");int32_t pipe=create_pipe(client);printf("STEAM_NATIVE pipe_present=%u\n",pipe!=0);stage("PIPE_END");if(!pipe)return 1;
 stage("USER_BEGIN");int32_t user=connect_user(client,pipe);printf("STEAM_NATIVE user_present=%u\n",user!=0);stage("USER_END");bool passed=user!=0;
 if(user){stage("FRIENDS_BEGIN");void* friends=get_friends(client,user,pipe,argv[2]);printf("STEAM_NATIVE friends_present=%u\n",friends!=NULL);stage("FRIENDS_END");passed=passed && friends;
  if(friends){const char* (*persona)(void*)=(const char*(*)(void*))(*(void***)friends)[0];for(unsigned call=1;call<=2;++call){stage("PERSONA_BEGIN");const char* name=persona(friends);bool nonempty=name && name[0];printf("STEAM_NATIVE persona_call=%u nonnull=%u nonempty=%u\n",call,name!=NULL,nonempty);stage("PERSONA_END");passed=passed && nonempty;}}
  stage("RELEASE_USER_BEGIN");release_user(client,pipe,user);stage("RELEASE_USER_END");
 }
 stage("RELEASE_PIPE_BEGIN");bool released=release_pipe(client,pipe);printf("STEAM_NATIVE pipe_released=%u\n",released);stage("RELEASE_PIPE_END");passed=passed && released;
 // Keep the DSO loaded until process exit, as in the ordinary client path.
 stage(passed?"PASS":"FAIL");return passed?0:1;
}
