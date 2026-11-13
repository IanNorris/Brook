#pragma once
namespace brook {
enum class RecoveryDebugCommand { Deny, Guard, Epoch, Hardware, JobCaps, UiCaps };
inline bool RecoveryDebugPrefix(const char* text,const char* prefix) {
    if(!text || !prefix)return false;
    while(*prefix)if(*text++!=*prefix++)return false;
    return true;
}
inline bool RecoveryDebugEqual(const char* a,const char* b) {
    if(!a || !b)return false;
    while(*a && *a==*b){++a;++b;}return !*a && !*b;
}
inline RecoveryDebugCommand RecoveryDebugClassify(const char* command) {
    if(RecoveryDebugPrefix(command,"bridge guard "))return RecoveryDebugCommand::Guard;
    if(RecoveryDebugPrefix(command,"bridge epoch init "))return RecoveryDebugCommand::Epoch;
    if(RecoveryDebugEqual(command,"bridge hardware caps") || RecoveryDebugPrefix(command,"bridge hardware logs ") ||
       RecoveryDebugPrefix(command,"bridge hardware recovery-panic ") ||
       RecoveryDebugPrefix(command,"bridge hardware recovery-panic-previous "))return RecoveryDebugCommand::Hardware;
    if(RecoveryDebugEqual(command,"bridge job caps"))return RecoveryDebugCommand::JobCaps;
    if(RecoveryDebugEqual(command,"bridge ui caps"))return RecoveryDebugCommand::UiCaps;
    return RecoveryDebugCommand::Deny;
}
}
