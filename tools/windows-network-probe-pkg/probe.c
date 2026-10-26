#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;Sleep(20000);
    puts("WINDOWS_NETWORK_WATCHDOG");fflush(stdout);ExitProcess(124);
    return 0;
}

int main(int argc,char** argv) {
    int adapters_only=argc>=2 && !strcmp(argv[1],"--adapters");
    if((argc!=2 && argc!=3) || (!adapters_only && strcmp(argv[1],"--if-table")))return 2;
    // Proton may create a console without forwarding the Win32 standard
    // handles. An explicit log path keeps the diagnostic observable there.
    if(argc==3 && !freopen(argv[2],"w",stdout))return 3;
    puts("WINDOWS_NETWORK_BEGIN");fflush(stdout);
    HANDLE timer=CreateThread(NULL,0,watchdog,NULL,0,NULL);
    if(!timer)return 3;
    CloseHandle(timer);
    puts("WINDOWS_NETWORK_DLL_BEGIN");fflush(stdout);
    HMODULE library=LoadLibraryA("iphlpapi.dll");
    printf("WINDOWS_NETWORK_DLL_END result=%lu\n",library?0UL:(unsigned long)GetLastError());fflush(stdout);
    if(!library)return 1;
    typedef NETIO_STATUS (WINAPI *IfTableFn)(PMIB_IF_TABLE2*);
    typedef VOID (WINAPI *FreeTableFn)(PVOID);
    typedef ULONG (WINAPI *AdaptersFn)(ULONG,ULONG,PVOID,PIP_ADAPTER_ADDRESSES,PULONG);
    union {FARPROC raw;IfTableFn call;} getTable={.raw=GetProcAddress(library,"GetIfTable2")};
    union {FARPROC raw;FreeTableFn call;} freeTable={.raw=GetProcAddress(library,"FreeMibTable")};
    union {FARPROC raw;AdaptersFn call;} getAdapters={.raw=GetProcAddress(library,"GetAdaptersAddresses")};
    if(!getTable.raw || !freeTable.raw || !getAdapters.raw)return 1;
    if(!adapters_only) {
        int passed=1;
        for(unsigned call=1;call<=2;++call) {
            printf("WINDOWS_NETWORK_IF_BEGIN call=%u\n",call);fflush(stdout);
            PMIB_IF_TABLE2 table=NULL;
            DWORD result=getTable.call(&table);
            unsigned long count=result==NO_ERROR && table?(unsigned long)table->NumEntries:0;
            printf("WINDOWS_NETWORK_IF_TABLE call=%u result=%lu count=%lu\n",call,(unsigned long)result,count);fflush(stdout);
            if(table)freeTable.call(table);
            if(result || !count)passed=0;
        }
        return passed?0:1;
    }
    ULONG bytes=0;
    ULONG result=getAdapters.call(AF_INET,0,NULL,NULL,&bytes);
    printf("WINDOWS_NETWORK_ADAPTER_SIZE result=%lu bytes=%lu\n",(unsigned long)result,(unsigned long)bytes);fflush(stdout);
    if(result!=ERROR_BUFFER_OVERFLOW || !bytes || bytes>16*1024*1024)return 1;
    IP_ADAPTER_ADDRESSES* adapters=malloc(bytes);
    if(!adapters)return 2;
    result=getAdapters.call(AF_INET,0,NULL,adapters,&bytes);
    printf("WINDOWS_NETWORK_ADAPTERS result=%lu bytes=%lu\n",(unsigned long)result,(unsigned long)bytes);fflush(stdout);
    free(adapters);
    if(result)return 1;
    puts("WINDOWS_NETWORK_PASS");return 0;
}
