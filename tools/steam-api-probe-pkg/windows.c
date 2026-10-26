// Owned, dynamically bound Steam API diagnostic; no SDK or game code linked.
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
static FILE* log_file;
static DWORD origin;
static void stage(const char* name){fprintf(log_file,"STEAM_API phase=%s ms=%lu pid=%lu tid=%lu\n",name,(unsigned long)(GetTickCount()-origin),(unsigned long)GetCurrentProcessId(),(unsigned long)GetCurrentThreadId());fflush(log_file);}
static DWORD WINAPI watchdog(LPVOID unused){(void)unused;Sleep(30000);stage("WATCHDOG");TerminateProcess(GetCurrentProcess(),124);return 0;}
static void trim(char* p){p[strcspn(p,"\r\n")]=0;}
int main(int argc,char** argv){
 char directory[2048],config[2304],library[2048]={0},version[80]={0};
 DWORD n=GetModuleFileNameA(NULL,directory,sizeof(directory));if(!n || n>=sizeof(directory))return 2;
 char* slash=strrchr(directory,'\\');if(!slash)slash=strrchr(directory,'/');if(!slash)return 2;slash[1]=0;
 snprintf(config,sizeof(config),"%sbrook-steam-api-probe.log",directory);log_file=fopen(config,"w");if(!log_file)return 3;setvbuf(log_file,NULL,_IONBF,0);origin=GetTickCount();stage("BEGIN");
 HANDLE timer=CreateThread(NULL,0,watchdog,NULL,0,NULL);if(!timer)return 3;CloseHandle(timer);
 if(argc==3){if(strlen(argv[1])>=sizeof(library) || strlen(argv[2])>=sizeof(version))return 2;strcpy(library,argv[1]);strcpy(version,argv[2]);}
 else if(argc==1){snprintf(config,sizeof(config),"%sbrook-steam-api-probe.cfg",directory);FILE* input=fopen(config,"r");if(!input)return 2;bool read=fgets(library,sizeof(library),input) && fgets(version,sizeof(version),input);fclose(input);if(!read)return 2;trim(library);trim(version);}
 else return 2;
 if(strcmp(version,"SteamFriends017")){stage("UNSUPPORTED_INTERFACE");return 2;}
 stage("DLL_BEGIN");HMODULE api=LoadLibraryA(library);fprintf(log_file,"STEAM_API dll_loaded=%u error=%lu\n",api!=NULL,api?0UL:(unsigned long)GetLastError());if(!api)return 1;stage("DLL_END");
 typedef bool (__cdecl *InitFn)(void);typedef void (__cdecl *VoidFn)(void);typedef void* (__cdecl *FriendsFn)(void);typedef const char* (__cdecl *PersonaFn)(void*);
 union {FARPROC raw;InitFn call;} init={.raw=GetProcAddress(api,"SteamAPI_Init")};
 union {FARPROC raw;VoidFn call;} shutdown={.raw=GetProcAddress(api,"SteamAPI_Shutdown")},callbacks={.raw=GetProcAddress(api,"SteamAPI_RunCallbacks")};
 union {FARPROC raw;FriendsFn call;} friends={.raw=GetProcAddress(api,"SteamAPI_SteamFriends_v017")};
 union {FARPROC raw;PersonaFn call;} persona={.raw=GetProcAddress(api,"SteamAPI_ISteamFriends_GetPersonaName")};
 if(!init.raw || !shutdown.raw || !callbacks.raw || !friends.raw || !persona.raw){stage("MISSING_EXPORT");return 1;}
 stage("INIT_BEGIN");bool initialized=init.call();fprintf(log_file,"STEAM_API initialized=%u\n",initialized);stage("INIT_END");if(!initialized)return 1;
 stage("FRIENDS_BEGIN");void* iface=friends.call();fprintf(log_file,"STEAM_API friends_present=%u\n",iface!=NULL);stage("FRIENDS_END");bool passed=iface!=NULL;
 if(iface)for(unsigned call=1;call<=2;++call){stage("PERSONA_BEGIN");const char* name=persona.call(iface);bool nonempty=name && name[0];fprintf(log_file,"STEAM_API persona_call=%u nonnull=%u nonempty=%u\n",call,name!=NULL,nonempty);stage("PERSONA_END");passed=passed && nonempty;}
 stage("CALLBACKS_BEGIN");for(unsigned i=0;i<10;++i){callbacks.call();Sleep(50);}stage("CALLBACKS_END");
 stage("SHUTDOWN_BEGIN");shutdown.call();stage("SHUTDOWN_END");stage(passed?"PASS":"FAIL");return passed?0:1;
}
