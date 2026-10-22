// Small xboxkrnl exports that do not belong to a larger subsystem. Each one is a strong
// definition that overrides the weak "unimplemented" stub in import_stubs.cpp.
#include "import_stubs.h"

#include "xex_headers.h"

// PVOID RtlImageXexHeaderField(PVOID headerBase, DWORD key)
// Returns a pointer to the optional-header field, or NULL when the XEX has no such field.
PPC_FUNC(__imp__RtlImageXexHeaderField)
{
    ctx.r3.u64 = XexHeaderFieldGuest(ctx.r4.u32);
}
