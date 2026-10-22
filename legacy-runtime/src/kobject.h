#pragma once
#include <cstdint>
#include <memory>
#include <windows.h>

struct GuestThread;

enum class KType
{
    Event,
    Timer,
    Thread,
    File,
};

// A kernel object referenced by guest handles. Win32 objects back the waitable types.
struct KObject
{
    explicit KObject(KType t) : type(t) {}
    ~KObject();

    KType type;
    HANDLE host = nullptr;                    // event / timer / thread handle
    std::shared_ptr<GuestThread> thread;      // for KType::Thread
    bool manualReset = false;                 // events and timers
};

// Guest handle table. Handles look like 0xF8000004, 0xF8000008, ...
uint32_t HandleCreate(std::shared_ptr<KObject> obj);
std::shared_ptr<KObject> HandleGet(uint32_t handle);
bool HandleClose(uint32_t handle);
uint32_t HandleDuplicate(uint32_t handle);

// Dispatcher objects that live in guest memory (KEVENT, fake KTHREAD, ...). The host-side object is
// created on first use and keyed by the guest pointer.
void GuestObjectRegister(uint32_t guestPtr, std::shared_ptr<KObject> obj);
std::shared_ptr<KObject> GuestObjectResolve(uint32_t guestPtr);
