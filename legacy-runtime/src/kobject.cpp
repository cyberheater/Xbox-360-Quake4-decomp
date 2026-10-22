#include "kobject.h"

#include <mutex>
#include <unordered_map>

#include "memory.h"

KObject::~KObject()
{
    if (host)
        CloseHandle(host);
}

namespace
{
    std::mutex g_mutex;
    std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_handles;
    std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_guestObjects;
    uint32_t g_nextHandle = 0xF8000004;
}

uint32_t HandleCreate(std::shared_ptr<KObject> obj)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t h = g_nextHandle;
    g_nextHandle += 4;
    g_handles[h] = std::move(obj);
    return h;
}

std::shared_ptr<KObject> HandleGet(uint32_t handle)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second;
}

bool HandleClose(uint32_t handle)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_handles.erase(handle) != 0;
}

uint32_t HandleDuplicate(uint32_t handle)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_handles.find(handle);
    if (it == g_handles.end())
        return 0;

    const uint32_t h = g_nextHandle;
    g_nextHandle += 4;
    g_handles[h] = it->second;
    return h;
}

void GuestObjectRegister(uint32_t guestPtr, std::shared_ptr<KObject> obj)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_guestObjects[guestPtr] = std::move(obj);
}

std::shared_ptr<KObject> GuestObjectResolve(uint32_t guestPtr)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_guestObjects.find(guestPtr);
        if (it != g_guestObjects.end())
            return it->second;
    }

    // First use of a KEVENT-style dispatcher header: byte 0 is the type
    // (0 = notification / manual-reset, 1 = synchronization / auto-reset), word at +4 the state.
    const uint8_t type = g_guestBase[guestPtr];
    if (type > 1)
        return nullptr;

    auto obj = std::make_shared<KObject>(KType::Event);
    obj->manualReset = (type == 0);
    obj->host = CreateEventW(nullptr, obj->manualReset ? TRUE : FALSE, GuestLoad32(guestPtr + 4) != 0 ? TRUE : FALSE, nullptr);

    std::lock_guard<std::mutex> lock(g_mutex);
    auto [it, inserted] = g_guestObjects.emplace(guestPtr, obj);
    return it->second;
}
