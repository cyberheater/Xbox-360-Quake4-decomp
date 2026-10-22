#pragma once
#include "ppc_config.h"
#include "ppc_context.h"

// Called by the auto-generated weak stubs for every kernel/XAM import that has no real
// implementation yet. Logs the call (once per import name, with a running total) and
// returns 0 in r3.
void UnimplementedImport(const char* name, PPCContext& ctx, uint8_t* base);

// Logs an implemented import's arguments (r3..r8) when the Q4_TRACE environment variable is set.
void TraceImport(const char* name, PPCContext& ctx);

// Number of unimplemented-import calls observed so far.
unsigned UnimplementedImportCalls();

extern const char* const kImportNames[];
extern const unsigned kImportCount;
