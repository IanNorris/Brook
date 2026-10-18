#pragma once
#include <stdint.h>

namespace brook {
// Narrow, boot-scoped COM2 framing. Raw sync requires callers to quiesce
// writers. Optional power verbs delegate to the shared shutdown coordinator.
// One worker owns this object. Retained IDs are never evicted during a boot.
class LifecycleChannel {
    struct Entry { char id[33]{}; int result=0; };
    char cookie[33]{};
    Entry entries[64]{};
    static bool Equal(const char* a,const char* b) {
        while(*a&&*a==*b){++a;++b;}return *a==*b;
    }
    static void Copy(char* to,const char* from) {while((*to++=*from++)){} }
    static bool Id(const char* value) {
        bool nonzero=false;
        for(unsigned i=0;i<32;++i) {
            char c=value[i];
            if(!c||!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
            nonzero|=c!='0';
        }
        return nonzero&&!value[32];
    }
    static void Reply(char out[160],const char* id,const char* boot,const char* state,int result) {
        char* p=out;
        auto text=[&](const char* s){while(*s)*p++=*s++;};
        text("BLC1 ");text(id);text(" ");text(boot);text(" ");text(state);text(" ");
        int64_t number=result;if(number<0){*p++='-';number=-number;}
        char digits[24];unsigned n=0;do{digits[n++]=char('0'+number%10);number/=10;}while(number);
        while(n)*p++=digits[--n];*p++='\n';*p=0;
    }
public:
    using Epoch = void (*)(const char* proposal,char output[33]);
    using Sync = int (*)();
    using Power = void (*)(const char* id,const char* verb,char state[24],int* result);
    bool Init(const char* sessionCookie) {
        if(!Id(sessionCookie))return false;
        Copy(cookie,sessionCookie);return true;
    }
    // Caller rejects UART line errors and discards overlong lines through LF.
    // Five exact, space-delimited ASCII tokens; no shells, paths or commands.
    bool Handle(const char* input,char output[160],Epoch epoch,Sync sync,Power power=nullptr) {
        output[0]=0;if(!cookie[0])return false;
        char line[160];unsigned n=0;
        while(input[n]&&n<sizeof(line)-1){line[n]=input[n];++n;}
        if(input[n])return false;line[n]=0;
        char* words[5];unsigned count=0;char* p=line;
        while(*p) {
            if(count==5||*p==' ')return false;
            words[count++]=p;
            while(*p&&*p!=' ')++p;
            if(*p){*p++=0;if(!*p)return false;}
        }
        if(count!=5||!Equal(words[0],"BLC1")||!Equal(words[1],cookie)||
           !Id(words[2])||!Id(words[3]))return false;
        const char* id=words[2];const char* expected=words[3];const char* verb=words[4];
        char boot[33];epoch(Equal(verb,"hello")?expected:nullptr,boot);
        if(!Id(boot))return false;
        if(Equal(verb,"hello")){Reply(output,id,boot,"ok",0);return true;}
        if(!Equal(boot,expected)){Reply(output,id,boot,"stale",-116);return true;}
        if(power && (Equal(verb,"shutdown")||Equal(verb,"restart")||Equal(verb,"power_status")||Equal(verb,"cancel")||Equal(verb,"commit"))) {
            char state[24];int result=0;power(id,verb,state,&result);Reply(output,id,boot,state,result);return true;
        }
        if(!Equal(verb,"sync")&&!Equal(verb,"status")){
            Reply(output,id,boot,"unsupported",-95);return true;
        }
        Entry* empty=nullptr;
        for(auto& entry:entries) {
            if(Equal(entry.id,id)){Reply(output,id,boot,"done",entry.result);return true;}
            if(!entry.id[0]&&!empty)empty=&entry;
        }
        if(Equal(verb,"status")){Reply(output,id,boot,"missing",-2);return true;}
        if(!empty){Reply(output,id,boot,"full",-28);return true;}
        Copy(empty->id,id);
        empty->result=sync();
        if(empty->result>0)empty->result=-5;
        Reply(output,id,boot,"done",empty->result);return true;
    }
};
}
