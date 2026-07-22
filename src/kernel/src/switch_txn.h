// switch_txn.h — BRO-208 self-verifying context-switch transaction tracer.
//
// A per-CPU "switch transaction" descriptor that context_switch.S updates in
// place as it crosses each irreversible boundary, plus a self-verify performed
// immediately before the RSP commit. Its purpose is to catch — AT the tear,
// not a timer-tick later — a context switch that loads the new thread's CR3 /
// ownership but resumes the OUTGOING thread (the proven BRO-208 mechanism), and
// to disambiguate the remaining root-cause classes.
//
// Audit result that shaped this design: DoSwitch runs `cli` as its first
// instruction and IF stays 0 through context_switch's masked popfq, so a
// MASKABLE LAPIC timer cannot nest inside the switch. The only asynchronous
// diversions left are exceptions / NMI / #MC (which ignore IF) — hence the
// fault-during-switch breadcrumb (stamped from HandleExceptionFull).
//
// The struct is a plain global in the kernel BSS (kernel-half, mapped in every
// address space and readable from the panic path under any CR3). Single-writer
// per CPU on the hot path: only the CPU executing the switch writes its slot.
//
// Offsets are #defined so context_switch.S can reference them directly; the
// C++ struct mirrors them with static_asserts so the two never drift.

#pragma once

// --- Phase values (written to SwitchTxn.phase) ---
#define SWP_IDLE          0   // no switch in flight on this CPU
#define SWP_PREPARED      1   // C++ SwitchTxnBegin filled expected-*, about to call asm
#define SWP_ENTRY         2   // context_switch entered
#define SWP_OLD_RELEASED  3   // after `movl $-1,(%r8)` (old published not-running)
#define SWP_CR3_DONE      4   // after CR3 load (now on new address space)
#define SWP_VALIDATED     5   // self-verify passed; about to commit RSP
#define SWP_RESUME        6   // reached .Lresume (transfer complete)
#define SWP_TEAR          0xFF // self-verify FAILED — restore source/values wrong

// --- Field byte offsets (must match the struct below; used by context_switch.S) ---
#define TXN_GENERATION      0
#define TXN_PHASE           8
#define TXN_EXPECTED_CTX    16   // &newProc->savedCtx
#define TXN_EXPECTED_RSP    24   // newProc->savedCtx.rsp
#define TXN_EXPECTED_RIP    32   // newProc->savedCtx.rip
#define TXN_EXPECTED_CR3    40   // newProc->savedCtx.cr3
#define TXN_OLD_PROC        48
#define TXN_NEW_PROC        56
#define TXN_LIVE_CTX        64   // RSI observed at RSP-commit (written by asm)
#define TXN_OBSERVED_RSP    72   // 48(%rsi) observed at commit
#define TXN_OBSERVED_RIP    80   // 56(%rsi) observed at commit
#define TXN_TSC_ENTRY       88
#define TXN_DRAIN_GEN       96   // generation seen at DrainPostSwitch entry
#define TXN_FAULT_RIP       104  // exception-during-switch interrupted RIP
#define TXN_FAULT_RFLAGS    112
#define TXN_FAULT_RSP       120
#define TXN_FAULT_VECTOR    128  // u32
#define TXN_FAULT_COUNT     132  // u32
#define TXN_ENTRY_IF        136  // u32
#define TXN_CPU             140  // u32
#define TXN_SIZE            192  // padded for cache-line isolation (3 lines)

#ifndef __ASSEMBLER__

#include <stdint.h>
#include "process.h"

namespace brook {

struct alignas(64) SwitchTxn
{
    volatile uint64_t generation;    // bumped each SwitchTxnBegin (single-writer/CPU)
    volatile uint64_t phase;         // SWP_*
    uint64_t          expectedNewCtx;// &newProc->savedCtx (snapshot in C++)
    uint64_t          expectedRsp;   // newProc->savedCtx.rsp
    uint64_t          expectedRip;   // newProc->savedCtx.rip
    uint64_t          expectedCr3;   // newProc->savedCtx.cr3
    uint64_t          oldProc;
    uint64_t          newProc;
    volatile uint64_t liveNewCtx;    // asm: RSI at commit
    volatile uint64_t observedRsp;   // asm: 48(%rsi) at commit
    volatile uint64_t observedRip;   // asm: 56(%rsi) at commit
    volatile uint64_t tscEntry;
    volatile uint64_t drainGeneration;
    volatile uint64_t faultRip;
    volatile uint64_t faultRflags;
    volatile uint64_t faultRsp;
    volatile uint32_t faultVector;
    volatile uint32_t faultCount;
    volatile uint32_t entryIf;
    volatile uint32_t cpu;
    uint8_t           _pad[TXN_SIZE - 144];
};

static_assert(sizeof(SwitchTxn) == TXN_SIZE, "SwitchTxn size must equal TXN_SIZE");
static_assert(offsetof(SwitchTxn, generation)      == TXN_GENERATION,   "gen off");
static_assert(offsetof(SwitchTxn, phase)           == TXN_PHASE,        "phase off");
static_assert(offsetof(SwitchTxn, expectedNewCtx)  == TXN_EXPECTED_CTX, "ectx off");
static_assert(offsetof(SwitchTxn, expectedRsp)     == TXN_EXPECTED_RSP, "ersp off");
static_assert(offsetof(SwitchTxn, expectedRip)     == TXN_EXPECTED_RIP, "erip off");
static_assert(offsetof(SwitchTxn, expectedCr3)     == TXN_EXPECTED_CR3, "ecr3 off");
static_assert(offsetof(SwitchTxn, oldProc)         == TXN_OLD_PROC,     "oldp off");
static_assert(offsetof(SwitchTxn, newProc)         == TXN_NEW_PROC,     "newp off");
static_assert(offsetof(SwitchTxn, liveNewCtx)      == TXN_LIVE_CTX,     "lctx off");
static_assert(offsetof(SwitchTxn, observedRsp)     == TXN_OBSERVED_RSP, "orsp off");
static_assert(offsetof(SwitchTxn, observedRip)     == TXN_OBSERVED_RIP, "orip off");
static_assert(offsetof(SwitchTxn, tscEntry)        == TXN_TSC_ENTRY,    "tsc off");
static_assert(offsetof(SwitchTxn, drainGeneration) == TXN_DRAIN_GEN,    "dgen off");
static_assert(offsetof(SwitchTxn, faultRip)        == TXN_FAULT_RIP,    "frip off");
static_assert(offsetof(SwitchTxn, faultRflags)     == TXN_FAULT_RFLAGS, "frfl off");
static_assert(offsetof(SwitchTxn, faultRsp)        == TXN_FAULT_RSP,    "frsp off");
static_assert(offsetof(SwitchTxn, faultVector)     == TXN_FAULT_VECTOR, "fvec off");
static_assert(offsetof(SwitchTxn, faultCount)      == TXN_FAULT_COUNT,  "fcnt off");
static_assert(offsetof(SwitchTxn, entryIf)         == TXN_ENTRY_IF,     "eif off");
static_assert(offsetof(SwitchTxn, cpu)             == TXN_CPU,          "cpu off");

// Record that an exception/NMI was taken while a switch was in flight on the
// current CPU. Called from HandleExceptionFull with IF=0. No-ops (cheaply) if
// no switch is in flight. This is the ONLY asynchronous diversion vector left
// after the IF=0 audit, so a nonzero faultCount on a torn switch is decisive.
void Bro208NoteFaultDuringSwitch(uint32_t vector, uint64_t rip,
                                 uint64_t rflags, uint64_t rsp);

} // namespace brook

#endif // __ASSEMBLER__
