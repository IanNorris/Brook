#pragma once
#include "frame_timing.h"
#include "bridge_reply.h"
namespace brook {
// Batch a complete row into bounded chunks; no per-number socket writes.
template<void (*Send)(const char*)>
void FrameTimingWriteRow(const FrameTimingRow& row,bool comma) {
    BridgeReplyBuffer<Send> f;if(comma)f.text(",");f.text("{");f.field("frame",row.frame);
    f.text(",");f.field("begin_ns",row.beginNs);f.text(",");f.field("end_ns",row.endNs);
    f.text(",");f.field("previous_end_ns",row.previousEndNs);f.text(",");f.field("sleep_deadline_ticks",row.sleepDeadlineTicks);
    f.text(",");f.field("commands",row.commands);f.text(",");f.field("command_bytes",row.commandBytes);
    f.text(",");f.field("fenced_commands",row.fencedCommands);
    f.text(",");f.field("first_fence",row.firstFence);f.text(",");f.field("last_fence",row.lastFence);
    f.text(",");f.field("command_preparation_ns",row.commandPreparationNs);
    f.text(",\"phase_ns\":[");
    for(unsigned k=0;k<FramePhaseCount;++k){if(k)f.text(",");f.number(row.phases[k]);}
    f.text("],\"command_ns_by_parent\":[");
    for(unsigned k=0;k<FramePhaseCount;++k){if(k)f.text(",");f.number(row.commandNsByParent[k]);}
    f.text("],\"commands_by_parent\":[");
    for(unsigned k=0;k<FramePhaseCount;++k){if(k)f.text(",");f.number(row.commandsByParent[k]);}
    f.text("]}");f.send();
}
}
