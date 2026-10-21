// Actual frame pacing admission, shared with the nested-wait regression test.
static void CompositorWaitForWork(uint64_t frameGeneration)
{
    Process* self=ProcessCurrent();
    self->wakeupTick=ApicTickCount()+COMPOSITE_INTERVAL;
    FrameTimingEnd(self->wakeupTick);
    // Work arriving while a GPU/mutex wait ran must survive consumption of the
    // generic pendingWakeup latch by that inner wait. After this recheck there
    // are no other blocking calls before scheduler admission; its wake latch
    // still covers work arriving between the recheck and block.
    if(!g_compositorWakeSequence.Changed(frameGeneration))SchedulerBlock(self);
    else self->wakeupTick=0; // No timer wait was admitted; do not retain its deadline.
}
