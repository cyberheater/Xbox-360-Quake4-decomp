#pragma once
#include <cstdint>
#include <windows.h>

// Guest memory write watchpoints.
//
// A watch covers [addr, addr + size) of guest memory. The host pages that hold it are made
// read-only; a store traps into WatchHandleException, which lets that single instruction
// execute, re-arms the page and, if the watched bytes changed, reports the old and new value
// together with the writer's host function and the guest call stack.
//
// Environment: Q4_WATCH="0x2039271C:4,0x2039278C:4"  (addr:size, comma or semicolon separated)
//              Q4_WATCH_MAX=N                         (max reports per watch, default 25)
//
// Limitations: pages are only trapped while armed, so writes by other threads during the
// single-step window are missed, and host API calls that write into a watched page (e.g. a
// ReadFile into it) fail instead of trapping. Watch heap objects, not I/O buffers.

void WatchInitFromEnv();

// Adds a watch. Returns its id. Safe to call before the memory is committed.
uint32_t WatchAdd(uint32_t guestAddr, uint32_t size, const char* label);

// Called after [guestAddr, guestAddr + size) became committed so pending watches can arm.
void WatchOnCommit(uint32_t guestAddr, uint32_t size);

// Offered every exception first. Returns true if it was a watchpoint trap (resume execution).
bool WatchHandleException(EXCEPTION_POINTERS* info);

// Page hook: after every store into the page containing `guestAddr` completes, `callback` runs
// on the storing thread with the guest address that was written. Used to turn plain stores to the
// GPU register window (ring-buffer write pointer) into events.
void WatchAddPageHook(uint32_t guestAddr, void (*callback)(uint32_t writtenAddr));
