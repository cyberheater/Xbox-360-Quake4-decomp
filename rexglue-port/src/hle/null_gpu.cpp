// q4hle - "null GPU": a minimal stand-in for the SDK's Xenos GPU emulation.
//
// With the native renderer, nothing needs to be rasterised from the guest's PM4 command stream:
// draws are replayed with D3D11 from the hooked D3D API calls. The guest's D3D library still
// expects a GPU that consumes its ring buffer, retires fences, raises interrupts and ticks vblank,
// otherwise it blocks forever (BlockOnFence, ring-space waits, swap pacing).
//
// This graphics system provides exactly that: GPU MMIO registers, a command processor that walks
// the ring and indirect buffers executing only the synchronisation packets (register and scratch
// writes, memory writes, event writes with fence values, waits, interrupts) and skips everything
// that would render, and a 60 Hz vblank interrupt. The packet semantics follow Xenia's
// command processor (BSD licence), which ReXGlue is built on.

#include "hle/null_gpu.h"
#include "hle/tickrate.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xtimer.h>
#include <rex/thread.h>

#include "hle/log.h"

namespace q4hle {
namespace {

using rex::X_STATUS;

constexpr uint32_t kRegCount = 0x10000;
constexpr uint32_t REG_CP_RB_WPTR = 0x01C5;
constexpr uint32_t REG_SCRATCH_UMSK = 0x01DC;
constexpr uint32_t REG_SCRATCH_ADDR = 0x01DD;
constexpr uint32_t REG_SCRATCH_REG0 = 0x0578;
constexpr uint32_t REG_COHER_STATUS_HOST = 0x0A31;
constexpr uint32_t REG_VGT_EVENT_INITIATOR = 0x21F9;
constexpr uint32_t REG_RB_SAMPLE_COUNT_ADDR = 0x2325;

enum : uint32_t {
  PM4_ME_INIT = 0x48,
  PM4_NOP = 0x10,
  PM4_INDIRECT_BUFFER = 0x3F,
  PM4_INDIRECT_BUFFER_PFD = 0x37,
  PM4_WAIT_REG_MEM = 0x3C,
  PM4_REG_RMW = 0x21,
  PM4_REG_TO_MEM = 0x3E,
  PM4_MEM_WRITE = 0x3D,
  PM4_COND_WRITE = 0x45,
  PM4_EVENT_WRITE = 0x46,
  PM4_EVENT_WRITE_SHD = 0x58,
  PM4_EVENT_WRITE_EXT = 0x59,
  PM4_EVENT_WRITE_ZPD = 0x5B,
  PM4_INTERRUPT = 0x54,
  PM4_XE_SWAP = 0x64,
};

inline uint32_t Swap32(uint32_t v) { return __builtin_bswap32(v); }

// Xenos endian modes for memory accesses (low 2 bits of addresses in packets).
inline uint32_t GpuSwap(uint32_t v, uint32_t endian) {
  switch (endian & 3) {
    case 1:  // 8in16
      return ((v >> 8) & 0x00FF00FF) | ((v << 8) & 0xFF00FF00);
    case 2:  // 8in32
      return Swap32(v);
    case 3:  // 16in32
      return (v >> 16) | (v << 16);
    default:
      return v;
  }
}

class NullGpu final : public rex::system::IGraphicsSystem {
 public:
  ~NullGpu() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext*) override { return X_STATUS_SUCCESS; }
  bool has_presentation() const override { return true; }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* fd, rex::system::KernelState* ks) override {
    dispatcher_ = fd;
    kernel_state_ = ks;
    memory_ = fd->memory();
    regs_.assign(kRegCount, 0);
    memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this,
                                   reinterpret_cast<rex::runtime::MMIOReadCallback>(&ReadThunk),
                                   reinterpret_cast<rex::runtime::MMIOWriteCallback>(&WriteThunk));
    running_ = true;
    cp_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(ks, 128 * 1024, 0, [this]() {
          CpMain();
          return 0;
        }));
    cp_thread_->set_name("q4hle null GPU");
    cp_thread_->Create();
    vsync_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(ks, 128 * 1024, 0, [this]() {
          VsyncMain();
          return 0;
        }));
    vsync_thread_->set_name("q4hle vblank");
    vsync_thread_->Create();
    Log("null GPU: started");
    return X_STATUS_SUCCESS;
  }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    interrupt_callback_ = callback;
    interrupt_data_ = user_data;
    Log("null GPU: interrupt callback %08X data %08X", callback, user_data);
  }
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    primary_ptr_ = ptr;
    primary_size_ = uint32_t(1) << (size_log2 + 3);
    read_index_ = 0;
    Log("null GPU: ring %08X size %u", ptr, primary_size_);
  }
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t) override { rptr_writeback_ = ptr; }

  void Shutdown() override {
    if (!running_) return;
    running_ = false;
    if (cp_thread_) cp_thread_->Wait(0, 0, 0, nullptr);
    if (vsync_thread_) vsync_thread_->Wait(0, 0, 0, nullptr);
    cp_thread_.reset();
    vsync_thread_.reset();
  }

  uint32_t vblank_count() const { return counter_.load(); }

 private:
  static uint32_t ReadThunk(void*, NullGpu* self, uint32_t addr) { return self->ReadRegister(addr); }
  static void WriteThunk(void*, NullGpu* self, uint32_t addr, uint32_t value) { self->WriteMmio(addr, value); }

  uint32_t ReadRegister(uint32_t addr) {
    const uint32_t r = (addr & 0xFFFF) / 4;
    if (r < 0x4000 && !seen_read_[r]) {
      seen_read_[r] = true;
      Log("null GPU: first MMIO read of register %04X", r);
    }
    switch (r) {
      case 0x0F00:  // RB_EDRAM_TIMING
        return 0x08100748;
      case 0x0F01:  // RB_BC_CONTROL
        return 0x0000200E;
      case 0x194C:  // D1MODE_V_COUNTER
        return 720;
      case 0x1951:  // interrupt status: vblank
        return 1;
      case 0x1961:  // D1MODE_VIEWPORT_SIZE
        return (1280u << 16) | 720u;
      default:
        return regs_[r];
    }
  }

  void WriteMmio(uint32_t addr, uint32_t value) {
    const uint32_t r = (addr & 0xFFFF) / 4;
    regs_[r] = value;
    if (r == REG_CP_RB_WPTR) write_index_.store(value);
  }

  uint8_t* Phys(uint32_t addr) { return memory_->TranslatePhysical(addr); }
  uint32_t LoadPhys(uint32_t addr) {
    uint32_t v;
    std::memcpy(&v, Phys(addr), 4);
    return v;  // raw (big-endian in memory)
  }
  void StorePhysRaw(uint32_t addr, uint32_t v) { std::memcpy(Phys(addr), &v, 4); }

  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kRegCount) return;
    regs_[index] = value;
    if ((index == REG_SCRATCH_ADDR || index == REG_SCRATCH_UMSK) && logged_scratch_++ < 8)
      Log("null GPU: %s = %08X", index == REG_SCRATCH_ADDR ? "SCRATCH_ADDR" : "SCRATCH_UMSK", value);
    if (index >= REG_SCRATCH_REG0 && index <= REG_SCRATCH_REG0 + 7) {
      const uint32_t i = index - REG_SCRATCH_REG0;
      if ((1u << i) & regs_[REG_SCRATCH_UMSK]) StorePhysRaw(regs_[REG_SCRATCH_ADDR] + i * 4, Swap32(value));
    } else if (index == REG_COHER_STATUS_HOST) {
      regs_[index] = 0;  // coherency is immediate
    }
  }

  void DispatchInterrupt(uint32_t source, uint32_t cpu) {
    if (!interrupt_callback_) return;
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) return;
    if (cpu == 0xFFFFFFFF) cpu = 2;
    thread->SetActiveCpu(static_cast<uint8_t>(cpu));
    uint64_t args[] = {source, interrupt_data_};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args, 2);
  }

  // ---- ring execution ----
  struct Reader {
    NullGpu* gpu;
    uint32_t base;      // physical address
    uint32_t capacity;  // bytes
    uint32_t offset;    // bytes
    uint32_t remaining; // bytes
    uint32_t Read() {
      const uint32_t v = Swap32(gpu->LoadPhys(base + offset));
      offset = (offset + 4) % capacity;
      remaining -= 4;
      return v;
    }
    void Skip(uint32_t dwords) {
      offset = (offset + dwords * 4) % capacity;
      remaining -= dwords * 4;
    }
  };

  void ExecuteBuffer(Reader& rd, int depth) {
    while (rd.remaining >= 4 && running_) {
      if (!ExecutePacket(rd, depth)) break;
    }
  }

  bool Compare(uint32_t func, uint32_t value, uint32_t ref) {
    switch (func & 7) {
      case 0: return false;
      case 1: return value < ref;
      case 2: return value <= ref;
      case 3: return value == ref;
      case 4: return value != ref;
      case 5: return value >= ref;
      case 6: return value > ref;
      default: return true;
    }
  }

  bool ExecutePacket(Reader& rd, int depth) {
    const uint32_t packet = rd.Read();
    if (packet == 0) return true;
    switch (packet >> 30) {
      case 0: {
        const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
        if (rd.remaining < count * 4) return false;
        const uint32_t base = packet & 0x7FFF;
        const bool one = (packet >> 15) & 1;
        for (uint32_t m = 0; m < count; ++m) WriteRegister(one ? base : base + m, rd.Read());
        return true;
      }
      case 1: {
        const uint32_t a = rd.Read(), b = rd.Read();
        WriteRegister(packet & 0x7FF, a);
        WriteRegister((packet >> 11) & 0x7FF, b);
        return true;
      }
      case 2:
        return true;
      default:
        break;
    }
    const uint32_t opcode = (packet >> 8) & 0x7F;
    const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
    if (rd.remaining < count * 4) return false;
    const uint32_t end_offset = (rd.offset + count * 4) % rd.capacity;
    const uint32_t end_remaining = rd.remaining - count * 4;
    switch (opcode) {
      case PM4_INTERRUPT: {
        const uint32_t mask = rd.Read();
        for (uint32_t n = 0; n < 6; ++n)
          if (mask & (1u << n)) DispatchInterrupt(1, n);
        break;
      }
      case PM4_XE_SWAP:
        ++counter_;
        break;
      case PM4_INDIRECT_BUFFER:
      case PM4_INDIRECT_BUFFER_PFD: {
        const uint32_t ptr = rd.Read() & 0x1FFFFFFF;
        const uint32_t len = rd.Read() & 0xFFFFF;
        if (depth < 4 && len) {
          Reader sub{this, ptr, len * 4, 0, len * 4};
          ExecuteBuffer(sub, depth + 1);
        }
        break;
      }
      case PM4_WAIT_REG_MEM: {
        const uint32_t info = rd.Read(), poll = rd.Read(), ref = rd.Read(), mask = rd.Read(), wait = rd.Read();
        (void)wait;
        const bool is_mem = (info & 0x10) != 0;
        // The swap-pacing wait (the D3D waits for the interrupt-cleared "swap pending" flag at
        // SCRATCH_ADDR + 4) is skipped: presentation is paced by the native swap chain instead.
        if (is_mem && skip_swap_wait_ && (poll & ~3u) == regs_[REG_SCRATCH_ADDR] + 4) break;
        const auto t0 = std::chrono::steady_clock::now();
        for (uint32_t spins = 0; running_; ++spins) {
          const uint32_t value = is_mem ? GpuSwap(LoadPhys(poll & ~3u), poll & 3) : regs_[poll & 0xFFFF];
          if (Compare(info, value & mask, ref)) break;
          if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(200)) {
            const uint32_t dev = interrupt_data_;
            Log("null GPU: WAIT_REG_MEM timeout (%s %08X & %08X func %u ref %08X) value %08X | vblanks %u last %u countdown %d calls %u",
                is_mem ? "mem" : "reg", poll, mask, info & 7, ref, value, dev ? GuestLoad(dev + 0x3CE0) : 0,
                dev ? GuestLoad(dev + 0x3CE4) : 0, dev ? int(GuestLoad(dev + 0x3CE8)) : 0, dev ? GuestLoad(dev + 0x3CEC) : 0);
            break;
          }
          rex::thread::Sleep(std::chrono::microseconds(100));
        }
        break;
      }
      case PM4_REG_RMW: {
        const uint32_t info = rd.Read(), and_mask = rd.Read(), or_mask = rd.Read();
        uint32_t v = regs_[info & 0x1FFF];
        v &= (info >> 31) ? regs_[and_mask & 0x1FFF] : and_mask;
        v |= ((info >> 30) & 1) ? regs_[or_mask & 0x1FFF] : or_mask;
        WriteRegister(info & 0x1FFF, v);
        break;
      }
      case PM4_REG_TO_MEM: {
        const uint32_t reg = rd.Read(), addr = rd.Read();
        StorePhysRaw(addr & ~3u, GpuSwap(regs_[reg & 0xFFFF], addr & 3));
        break;
      }
      case PM4_MEM_WRITE: {
        uint32_t addr = rd.Read();
        for (uint32_t i = 0; i + 1 < count; ++i, addr += 4) StorePhysRaw(addr & ~3u, GpuSwap(rd.Read(), addr & 3));
        break;
      }
      case PM4_COND_WRITE: {
        const uint32_t info = rd.Read(), poll = rd.Read(), ref = rd.Read(), mask = rd.Read();
        const uint32_t waddr = rd.Read(), wdata = rd.Read();
        const uint32_t value = (info & 0x10) ? GpuSwap(LoadPhys(poll & ~3u), poll & 3) : regs_[poll & 0xFFFF];
        if (Compare(info, value & mask, ref)) {
          if (info & 0x100)
            StorePhysRaw(waddr & ~3u, GpuSwap(wdata, waddr & 3));
          else
            WriteRegister(waddr & 0xFFFF, wdata);
        }
        break;
      }
      case PM4_EVENT_WRITE:
        WriteRegister(REG_VGT_EVENT_INITIATOR, rd.Read() & 0x3F);
        break;
      case PM4_EVENT_WRITE_SHD: {
        const uint32_t initiator = rd.Read(), addr = rd.Read(), value = rd.Read();
        WriteRegister(REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        const uint32_t data = (initiator >> 31) ? counter_.load() : value;
        StorePhysRaw(addr & ~3u, GpuSwap(data, addr & 3));
        break;
      }
      case PM4_EVENT_WRITE_EXT: {
        const uint32_t initiator = rd.Read(), addr = rd.Read();
        WriteRegister(REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        const uint16_t ext[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
        uint8_t* p = Phys(addr & ~3u);
        for (int i = 0; i < 6; ++i) {
          const uint16_t v = __builtin_bswap16(ext[i]);
          std::memcpy(p + 2 * i, &v, 2);
        }
        break;
      }
      case PM4_EVENT_WRITE_ZPD: {
        WriteRegister(REG_VGT_EVENT_INITIATOR, rd.Read() & 0x3F);
        // Occlusion query: report everything visible.
        const uint32_t a = regs_[REG_RB_SAMPLE_COUNT_ADDR];
        if (a) {
          uint8_t* p = Phys(a);
          std::memset(p, 0, 0x20);
          const uint32_t n = Swap32(1000);
          std::memcpy(p + 0x00, &n, 4);  // Total_A (layout: Total A/B, ZFail A/B, ZPass A/B ...)
          std::memcpy(p + 0x10, &n, 4);  // ZPass_A
        }
        break;
      }
      default:
        break;  // draws, constants, shader loads, bin masks, ...: nothing to do
    }
    rd.offset = end_offset;
    rd.remaining = end_remaining;
    return true;
  }

  void CpMain() {
    while (running_) {
      const uint32_t w = write_index_.load();
      if (!primary_ptr_ || w == read_index_ || w == 0xBAADF00D) {
        rex::thread::Sleep(std::chrono::microseconds(100));
        continue;
      }
      const uint32_t cap = primary_size_;
      const uint32_t r = read_index_;
      const uint32_t bytes = ((w + cap / 4 - r) % (cap / 4)) * 4;
      Reader rd{this, primary_ptr_ & 0x1FFFFFFF, cap, r * 4, bytes};
      ExecuteBuffer(rd, 0);
      read_index_ = w;
      if (rptr_writeback_) StorePhysRaw(rptr_writeback_, Swap32(read_index_));
    }
  }

  uint32_t GuestLoad(uint32_t va) {
    uint32_t v;
    std::memcpy(&v, memory_->TranslateVirtual(va), 4);
    return Swap32(v);
  }

  void VsyncMain() {
    uint32_t ticks = 0;
    using clock = std::chrono::steady_clock;
    const auto interval = std::chrono::nanoseconds(1000000000 / q4hle::tick::RefreshHz());
    auto next = clock::now() + interval;
    while (running_) {
      const auto now = clock::now();
      if (now >= next) {
        ++counter_;
        rex::system::NotifyGuestVblank();  // VdSwap paces the game to these (see the SDK's VdSwap)
        DispatchInterrupt(0, 2);
        if (++ticks % 300 == 1 && interrupt_data_) {
          const uint32_t dev = interrupt_data_;
          const uint32_t blk = GuestLoad(dev + 0x2894);
          Log("null GPU: vblank %u dev.vblanks %u dev.countdown %d swap flag %u", ticks, GuestLoad(dev + 0x3CE0),
              int(GuestLoad(dev + 0x3CE8)), blk ? GuestLoad(blk + 4) : 0xFFFFFFFFu);
        }
        next += interval;
        if (now - next > interval * 4) next = now + interval;
      } else {
        rex::thread::Sleep(std::chrono::microseconds(500));
      }
    }
  }

  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  std::vector<uint32_t> regs_;
  bool seen_read_[0x4000] = {};
  bool skip_swap_wait_ = [] {
    const char* e = std::getenv("Q4_HLE_SWAP_WAIT");
    return !(e && *e == '1');
  }();
  int logged_scratch_ = 0;
  std::atomic<bool> running_{false};
  std::atomic<uint32_t> counter_{0};
  uint32_t interrupt_callback_ = 0, interrupt_data_ = 0;
  uint32_t primary_ptr_ = 0, primary_size_ = 0, rptr_writeback_ = 0;
  uint32_t read_index_ = 0;
  std::atomic<uint32_t> write_index_{0xBAADF00D};
  rex::system::object_ref<rex::system::XHostThread> cp_thread_, vsync_thread_;
};

}  // namespace

std::unique_ptr<rex::system::IGraphicsSystem> CreateNullGpu() { return std::make_unique<NullGpu>(); }

}  // namespace q4hle
