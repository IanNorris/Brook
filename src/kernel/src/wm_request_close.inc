// Called with the WM lock held by the chrome, shortcut and window-menu paths.
static bool RequestCloseLocked(int idx,uint32_t* method)
{
    if (idx<0 || idx>=static_cast<int>(WM_MAX_WINDOWS)) return false;
    auto& w=g_windows[idx];
    if (!w.proc || !w.focusable) return false;
    if (w.vfb || w.cooperativeClose) {
        *method=1;
        return WmPushWmEvent(&w,WM_EVT_CLOSE_REQUESTED,0,0);
    }
    if(w.proc->state==ProcessState::Terminated)return false;
    TerminalGeometry terminal{};
    const bool isTerminal=TerminalSnapshotGeometry(w.proc,&terminal);
    *method=isTerminal?2:3;
    // Terminal hangup is cooperative too: no hidden SIGKILL on close.
    ProcessSendSignal(w.proc,isTerminal?1:15);
    return true;
}
