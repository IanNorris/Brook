#pragma once
// Named FIFO description types, shared by named_fifo.inc and the descriptor
// code that reads, writes or polls an opened FIFO. Included inside namespace
// brook after VfsPath, PipeBuffer and Process are declared (split from named_fifo.inc).
struct NamedFifoWait { Process* process;NamedFifoWait* next; };
struct NamedFifoState {
    NamedFifoState* next;
    VfsPath identity;
    PipeBuffer* pipe;
    uint64_t descriptions,readerEpoch,writerEpoch;
    NamedFifoWait* waiters;
};
struct NamedFifoOpen { NamedFifoState* state;uint32_t access;uint64_t writerEpoch;bool writerSeen; };
