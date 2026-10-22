#include "gpu.h"
#include "renderer.h"

#include <atomic>
#include <windows.h>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

#include "memory.h"
#include "watch.h"
#include "texture_dump.h"
#include "thread.h"

namespace
{
    // ---- PM4 opcodes ---------------------------------------------------------------------------
    constexpr uint32_t PM4_NOP                = 0x10;
    constexpr uint32_t PM4_REG_RMW            = 0x21;
    constexpr uint32_t PM4_DRAW_INDX          = 0x22;
    constexpr uint32_t PM4_VIZ_QUERY          = 0x23;
    constexpr uint32_t PM4_WAIT_FOR_IDLE      = 0x26;
    constexpr uint32_t PM4_IM_LOAD            = 0x27;
    constexpr uint32_t PM4_IM_LOAD_IMMEDIATE  = 0x2B;
    constexpr uint32_t PM4_SET_CONSTANT       = 0x2D;
    constexpr uint32_t PM4_LOAD_ALU_CONSTANT  = 0x2F;
    constexpr uint32_t PM4_DRAW_INDX_2        = 0x36;
    constexpr uint32_t PM4_INDIRECT_BUFFER_PFD = 0x37;
    constexpr uint32_t PM4_INVALIDATE_STATE   = 0x3B;
    constexpr uint32_t PM4_WAIT_REG_MEM       = 0x3C;
    constexpr uint32_t PM4_MEM_WRITE          = 0x3D;
    constexpr uint32_t PM4_INDIRECT_BUFFER    = 0x3F;
    constexpr uint32_t PM4_COND_WRITE         = 0x45;
    constexpr uint32_t PM4_EVENT_WRITE        = 0x46;
    constexpr uint32_t PM4_ME_INIT            = 0x48;
    constexpr uint32_t PM4_INTERRUPT          = 0x54;
    constexpr uint32_t PM4_EVENT_WRITE_SHD    = 0x58;
    constexpr uint32_t PM4_EVENT_WRITE_EXT    = 0x5A;
    constexpr uint32_t PM4_SET_SHADER_CONSTANTS = 0x56;

    constexpr uint32_t kRegCpRbRptr = 0x01C4;
    constexpr uint32_t kRegCpRbWptr = 0x01C5;
    constexpr uint32_t kEventSwap = 0x14;

    // Location of the packet currently being executed (for diagnostics).
    thread_local uint32_t t_curBase = 0;
    thread_local uint32_t t_curPos = 0;
    thread_local uint32_t t_ringRptr = 0, t_ringWptr = 0, t_ringSize = 0;
    thread_local uint64_t t_binMask = ~0ull;   // SET_BIN_MASK (0x50)
    thread_local uint64_t t_binSelect = ~0ull; // SET_BIN_SELECT (0x51)
    thread_local int t_curDepth = 0;
    thread_local uint32_t t_lastIbSize = 0;

    // The last packets this thread executed, newest last (for post-mortems).
    struct PacketRecord
    {
        uint32_t base, pos, hdr;
        uint32_t first, second; // first two payload dwords
        int depth;
    };
    constexpr size_t kHistory = 512;
    thread_local PacketRecord t_history[kHistory];
    thread_local size_t t_historyCount = 0;

    struct CpState
    {
        std::atomic<uint32_t> ringPhys{ 0 };
        std::atomic<uint32_t> ringSizeLog2{ 0 };
        std::atomic<uint32_t> writebackPhys{ 0 };
        std::atomic<bool> started{ false };

        std::atomic<uint32_t> packets{ 0 }, type0{ 0 }, type3{ 0 }, ibs{ 0 }, memWrites{ 0 }, interrupts{ 0 },
            swaps{ 0 }, unknown{ 0 };

        std::atomic<uint32_t> rptr{ 0 }, curHeader{ 0 }, curDepth{ 0 }, curOpcode{ 0xFF };
        std::atomic<bool> busy{ false };
        std::atomic<uint64_t> batchesStarted{ 0 };
        std::atomic<uint64_t> batchesDone{ 0 };
    } g_cp;

    uint32_t PhysToVirt(uint32_t phys) { return guest::kPhysicalBegin + (phys & 0x1FFFFFFF); }

    // Scratch registers: when a register selected in SCRATCH_UMSK is written, the processor also
    // writes its value to SCRATCH_ADDR + 4 * n in memory. Command streams use this as a
    // synchronisation channel (WAIT_REG_MEM on the mirrored words).
    constexpr uint32_t kRegScratchUmsk = 0x01DC;
    constexpr uint32_t kRegScratchAddr = 0x01DD;
    constexpr uint32_t kRegScratchReg0 = 0x0578;
    constexpr uint32_t kScratchRegCount = 8;

    // Last scratch register values already reflected in memory (see SyncScratchRegisters).
    uint32_t g_lastScratch[kScratchRegCount] = {};
    bool g_lastScratchValid = false;

    uint32_t ReadReg(uint32_t index) { return GuestLoad32(guest::kGpuRegBase + index * 4); }

    bool ScratchTrace()
    {
        static const bool on = std::getenv("Q4_SCRATCH") != nullptr;
        return on;
    }

    void MirrorScratchRegister(uint32_t n, uint32_t value)
    {
        const uint32_t umsk = ReadReg(kRegScratchUmsk);
        if (!(umsk & (1u << n)))
            return;

        // SCRATCH_ADDR is a physical address, in bytes.
        GuestStore32(guest::kPhysicalBegin + (((ReadReg(kRegScratchAddr) & ~3u) + n * 4) & 0x1FFFFFFF), value);
    }

    void WriteReg(uint32_t index, uint32_t v)
    {
        if (index * 4 >= guest::kGpuRegSize)
            return;
        if (index >= 0x01C0 && index < 0x01D0 && index != 0x01C4)
            return; // CP ring control registers are not writable from the packet stream

        GuestStore32(guest::kGpuRegBase + index * 4, v);

        if (index == kRegCpRbWptr)
        {
            static std::atomic<unsigned> seen{ 0 };
            if (seen++ < 10)
            {
                std::printf("[reg] CP-side write of CP_RB_WPTR = 0x%08X (hdr 0x%08X at %s 0x%08X dword %u)\n", v,
                            g_cp.curHeader.load(), t_curDepth ? "IB" : "ring", t_curBase, t_curPos);
                std::fflush(stdout);
            }
        }

        if (index == 0x0A31)
        {
            static std::atomic<unsigned> seen{ 0 };
            if (seen++ < 12)
            {
                std::printf("[reg] 0x0A31 = 0x%08X (hdr 0x%08X at %s 0x%08X dword %u)\n", v, g_cp.curHeader.load(),
                            t_curDepth ? "IB" : "ring", t_curBase, t_curPos);
                std::fflush(stdout);
            }
        }

        if ((index == kRegScratchUmsk || index == kRegScratchAddr) && ScratchTrace())
            std::printf("[scratch] reg 0x%04X = 0x%08X at %s 0x%08X dword %u hdr 0x%08X\n", index, v,
                        t_curDepth ? "IB" : "ring", t_curBase, t_curPos, g_cp.curHeader.load());

        if (index >= kRegScratchReg0 && index < kRegScratchReg0 + kScratchRegCount)
        {
            if (ScratchTrace() && index == kRegScratchReg0 + 4)
                std::printf("[scratch4] packet writes 0x%08X at %s 0x%08X dword %u (reg index %u)\n", v,
                            t_curDepth ? "IB" : "ring", t_curBase, t_curPos, index);
            g_lastScratch[index - kRegScratchReg0] = v; // handled here: SyncScratchRegisters must not repeat it
            MirrorScratchRegister(index - kRegScratchReg0, v);
        }
    }

    // The CPU can also write the register window directly (plain stores). Those writes must reach
    // the mirrored memory too, but ONLY when the register value actually changes: the CPU also
    // stores into the mirrored words itself (D3D keeps its CPU-interrupt callback pointer in one
    // of them), and copying a stale register value back would wipe such stores.
    void SyncScratchRegisters()
    {
        const uint32_t umsk = ReadReg(kRegScratchUmsk);

        if (!g_lastScratchValid)
        {
            // Adopt the current register values as the baseline.
            for (uint32_t n = 0; n < kScratchRegCount; n++)
                g_lastScratch[n] = ReadReg(kRegScratchReg0 + n);
            g_lastScratchValid = true;
            return;
        }

        const uint32_t base = guest::kPhysicalBegin + ((ReadReg(kRegScratchAddr) & ~3u) & 0x1FFFFFFF);
        for (uint32_t n = 0; n < kScratchRegCount; n++)
        {
            const uint32_t value = ReadReg(kRegScratchReg0 + n);
            if (value == g_lastScratch[n])
                continue;

            if (ScratchTrace() && n == 4)
                std::printf("[scratch4] CPU store detected: 0x%08X -> 0x%08X\n", g_lastScratch[n], value);

            g_lastScratch[n] = value;
            if (umsk & (1u << n))
                GuestStore32(base + n * 4, value);
        }
    }

    // Memory the GPU writes is visible to the (big-endian) CPU as the logical value.
    void WriteGuestDword(uint32_t phys, uint32_t value)
    {
        static std::atomic<uint32_t> logged{ 0 };
        if (logged++ < 60)
        {
            std::printf("[gpu] CP write 0x%08X = 0x%08X\n", phys, value);
            std::fflush(stdout);
        }
        // Diagnostics: report the first write into every 64 KB physical page (a desynchronised
        // stream shows up as writes to pages the title never gave the GPU).
        {
            static std::set<uint32_t> pages;
            const uint32_t key = (phys & 0x1FFFFFFF) >> 16;
            if (pages.size() < 400 && pages.insert(key).second)
            {
                std::printf("[gpu] CP first write to page 0x%08X: 0x%08X = 0x%08X (packet 0x%08X at %s 0x%08X dword %u)\n", key << 16, phys, value, g_cp.curHeader.load(), t_curDepth ? "IB" : "ring", t_curBase,
                            t_curPos);
            }
        }
        GuestStore32(PhysToVirt(phys), value);
    }

    bool Compare(uint32_t func, uint32_t value, uint32_t ref)
    {
        switch (func & 7)
        {
        case 0: return false;           // never
        case 1: return value < ref;
        case 2: return value <= ref;
        case 3: return value == ref;
        case 4: return value != ref;
        case 5: return value >= ref;
        case 6: return value > ref;
        default: return true;           // always
        }
    }

    // Reads dwords from either the ring (wraps) or an indirect buffer (does not wrap).
    struct Reader
    {
        uint32_t base;       // guest virtual address
        uint32_t wrap;       // size in dwords for modular indexing (0 = linear)
        uint32_t pos;        // current dword index

        uint32_t At(uint32_t i) const
        {
            uint32_t idx = pos + i;
            if (wrap)
                idx %= wrap;
            return GuestLoad32(base + idx * 4);
        }
    };

    // ---- draw tracking (diagnostics) ---------------------------------------------------------
    // Follows the state the program sets (shaders, constants, draws) so a frame can be
    // summarised. Nothing is rendered yet; this is the groundwork for the renderer.
    struct DrawRec
    {
        uint32_t prim, indices, srcSel;
        uint32_t vsHash, vsSize, psHash, psSize;
        uint32_t rtInfo, surface, tex0[6], viewportScaleX, viewportScaleY;
    };

    struct DrawTracker
    {
        uint32_t shaderHash[2] = {};
        uint32_t shaderSize[2] = {};
        std::vector<DrawRec> frame;
        uint32_t frameIndex = 0;
        uint64_t totalDraws = 0;
    } g_draw;

    uint32_t Fnv(const uint32_t* w, uint32_t n)
    {
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < n; i++)
        {
            h ^= w[i];
            h *= 16777619u;
        }
        return h;
    }

    uint32_t RegValue(uint32_t index)
    {
        return index * 4 < guest::kGpuRegSize ? GuestLoad32(guest::kGpuRegBase + index * 4) : 0;
    }

    uint32_t ConstantRegister(uint32_t type, uint32_t index)
    {
        switch (type)
        {
        case 0: return 0x4000 + index; // ALU
        case 1: return 0x4800 + index; // fetch
        case 2: return 0x4900 + index; // bool
        case 3: return 0x4908 + index; // loop
        default: return 0x2000 + index; // registers
        }
    }

    void RecordDraw(uint32_t initiator, uint32_t indices)
    {
        static const bool rbLog = std::getenv("Q4_RBLOG") != nullptr;
        if (rbLog)
        {
            static int n = 0;
            const uint32_t mode = RegValue(0x2208) & 7;
            if (n < 400 && (mode == 6 || n < 60))
            {
                n++;
                std::printf("[rb] draw prim=%u src=%u cnt=%u mode=%u surf=%08X col=%08X dep=%08X vpx=%08X/%08X winoff=%08X "
                            "scis=%08X/%08X sc=%08X/%08X ctl=%08X dbase=%08X dpitch=%08X dinfo=%08X mask=%08X blend=%08X depthc=%08X "
                            "vte=%08X clip=%08X mode=%08X sq=%08X\n",
                            initiator & 0x3F, (initiator >> 6) & 3, indices, mode, RegValue(0x2000), RegValue(0x2001),
                            RegValue(0x2002), RegValue(0x210F), RegValue(0x2110), RegValue(0x2080), RegValue(0x2081),
                            RegValue(0x2082), RegValue(0x200E), RegValue(0x200F), RegValue(0x2318), RegValue(0x2319),
                            RegValue(0x231A), RegValue(0x231B), RegValue(0x2104), RegValue(0x2201), RegValue(0x2200),
                            RegValue(0x2206), RegValue(0x2204), RegValue(0x2205), RegValue(0x2180));
            }
        }
        DrawRec d{};
        d.prim = initiator & 0x3F;
        d.srcSel = (initiator >> 6) & 3;
        d.indices = indices;
        d.vsHash = g_draw.shaderHash[0];
        d.vsSize = g_draw.shaderSize[0];
        d.psHash = g_draw.shaderHash[1];
        d.psSize = g_draw.shaderSize[1];
        d.surface = RegValue(0x2000);
        d.rtInfo = RegValue(0x2001);
        for (uint32_t i = 0; i < 6; i++)
            d.tex0[i] = RegValue(0x4800 + i);
        d.viewportScaleX = RegValue(0x210F);
        d.viewportScaleY = RegValue(0x2111);
        g_draw.frame.push_back(d);
        g_draw.totalDraws++;
    }

    void DrawFrameEnd()
    {
        static const int dumpFrame = [] {
            const char* e = std::getenv("Q4_DRAWLOG");
            return e ? std::atoi(e) : -1;
        }();

        const uint32_t frame = g_draw.frameIndex++;
        static bool dumped = false;
        const bool dump = dumpFrame >= 0 && frame >= static_cast<uint32_t>(dumpFrame) && g_draw.frame.size() > 15 && !dumped;
        if (dump)
            dumped = true;
        const bool summary = frame < 5 || frame % 120 == 0;

        if (summary || dump)
        {
            uint64_t indices = 0;
            std::vector<uint64_t> pairs;
            for (const DrawRec& d : g_draw.frame)
            {
                indices += d.indices;
                pairs.push_back((uint64_t(d.vsHash) << 32) | d.psHash);
            }
            std::sort(pairs.begin(), pairs.end());
            pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
            std::printf("[draw] frame %u: %zu draws, %llu indices, %zu unique shader pairs\n",
                        frame, g_draw.frame.size(), static_cast<unsigned long long>(indices), pairs.size());
        }

        if (dump)
        {
            for (size_t i = 0; i < g_draw.frame.size() && i < 400; i++)
            {
                const DrawRec& d = g_draw.frame[i];
                std::printf("[draw]  #%zu prim=%u src=%u n=%u vs=%08X/%u ps=%08X/%u rt=%08X surf=%08X tex0=%08X %08X %08X %08X %08X %08X vp=%08X,%08X\n",
                            i, d.prim, d.srcSel, d.indices, d.vsHash, d.vsSize, d.psHash, d.psSize, d.rtInfo, d.surface,
                            d.tex0[0], d.tex0[1], d.tex0[2], d.tex0[3], d.tex0[4], d.tex0[5], d.viewportScaleX,
                            d.viewportScaleY);
            }
        }

        if (dump)
        {
            CreateDirectoryA("C:\\Quake4\\dump", nullptr);
            std::set<std::tuple<uint32_t, uint32_t, uint32_t>> seen;
            int written = 0;
            for (const DrawRec& d : g_draw.frame)
            {
                if (!seen.insert({ d.tex0[0], d.tex0[1], d.tex0[2] }).second)
                    continue;
                if (written >= 24)
                    break;
                char path[96];
                std::snprintf(path, sizeof(path), "C:\\Quake4\\dump\\tex_%02d_%08X.png", written, d.tex0[1]);
                if (TextureDump(d.tex0, path))
                {
                    std::printf("[tex] wrote %s (fetch %08X %08X %08X)\n", path, d.tex0[0], d.tex0[1], d.tex0[2]);
                    written++;
                }
            }
        }

        std::fflush(stdout);
        g_draw.frame.clear();
    }

    void ExecuteBuffer(PPCContext& ctx, uint32_t baseVirt, uint32_t startPos, uint32_t dwords, uint32_t wrap, int depth);

    void ExecuteType3(PPCContext& ctx, Reader& r, uint32_t opcode, uint32_t count, int depth)
    {
        auto p = [&](uint32_t i) { return r.At(1 + i); }; // payload dword i

        switch (opcode)
        {
        case PM4_NOP:
        case PM4_ME_INIT:
        case PM4_WAIT_FOR_IDLE:
        case PM4_INVALIDATE_STATE:
        case PM4_VIZ_QUERY:
            break;

        case PM4_SET_SHADER_CONSTANTS:
        case 0x55: // SET_CONSTANT2: consecutive register writes starting at the given register index
        {
            const uint32_t index = p(0) & 0xFFFF;
            for (uint32_t i = 1; i < count; i++)
                WriteReg(index + i - 1, p(i));
            break;
        }

        case PM4_IM_LOAD:
        {
            // payload0: shader address | type (0 vertex, 1 pixel); payload1: start << 16 | size
            const uint32_t type = p(0) & 3;
            const uint32_t size = p(1) & 0xFFFF;
            {
                static uint32_t imLog = 0;
                if (std::getenv("Q4_IMLOG") && (imLog++ - std::atoi(std::getenv("Q4_IMLOG"))) < 60u && imLog > static_cast<uint32_t>(std::atoi(std::getenv("Q4_IMLOG"))))
                    std::printf("[im] IM_LOAD type=%u addr=%08X size=%u start=%u (count=%u)\n", type, p(0) & ~3u, size, p(1) >> 16, count);
            }
            const uint32_t addr = p(0) & ~3u;
            if (type < 2)
            {
                g_draw.shaderSize[type] = size;
                g_draw.shaderHash[type] = 0;
                RendererSetShaderAddress(type, addr, size);
                const uint32_t virt = PhysToVirt(addr);
                if (size && size < 0x2000 && virt >= guest::kPhysicalBegin)
                {
                    uint32_t words[0x2000];
                    for (uint32_t i = 0; i < size; i++)
                        words[i] = GuestLoad32(virt + i * 4);
                    g_draw.shaderHash[type] = Fnv(words, size);
                }
            }
            break;
        }

        case PM4_IM_LOAD_IMMEDIATE:
        {
            // payload0: type; payload1: start << 16 | size; then the program itself.
            const uint32_t type = p(0) & 3;
            const uint32_t size = p(1) & 0xFFFF;
            if (type < 2)
            {
                uint32_t words[0x400];
                const uint32_t n = size < 0x400 && size + 2 <= count ? size : 0;
                for (uint32_t i = 0; i < n; i++)
                    words[i] = p(2 + i);
                g_draw.shaderSize[type] = size;
                g_draw.shaderHash[type] = Fnv(words, n);
                RendererSetShaderImmediate(type, words, n);
            }
            break;
        }

        case PM4_SET_CONSTANT:
        {
            const uint32_t type = (p(0) >> 16) & 0xFF;
            const uint32_t index = p(0) & 0x7FF;
            for (uint32_t i = 0; i + 1 < count; i++)
                WriteReg(ConstantRegister(type, index + i), p(1 + i));
            break;
        }

        case PM4_LOAD_ALU_CONSTANT:
        {
            const uint32_t addr = p(0) & 0x3FFFFFFF;
            const uint32_t type = (p(1) >> 16) & 0xFF;
            const uint32_t index = p(1) & 0x7FF;
            const uint32_t size = p(2) & 0xFFF;
            const uint32_t virt = PhysToVirt(addr);
            if (size && size < 0x1000 && virt >= guest::kPhysicalBegin)
            {
                for (uint32_t i = 0; i < size; i++)
                    WriteReg(ConstantRegister(type, index + i), GuestLoad32(virt + i * 4));
            }
            break;
        }

        case PM4_DRAW_INDX:
        {
            RecordDraw(p(1), p(1) >> 16);
            const uint32_t init = p(1);
            std::vector<uint32_t> imm;
            if (((init >> 6) & 3) == 1)
                for (uint32_t i = 2; i < count; i++)
                    imm.push_back(p(i));
            RendererDraw(init, imm.data(), static_cast<uint32_t>(imm.size()), count > 2 ? p(2) : 0, count > 3 ? p(3) : 0);
            break;
        }

        case PM4_DRAW_INDX_2:
        {
            RecordDraw(p(0), p(0) >> 16);
            if ((p(0) & 0x3F) == 8 && std::getenv("Q4_RECTLOG") && g_cp.packets > static_cast<uint32_t>(std::atoi(std::getenv("Q4_RECTLOG"))))
            {
                static std::atomic<unsigned> n{ 0 };
                if (n++ < 30)
                    std::printf("[rect] DRAW_INDX_2 prim 8: mode=%X copyctl=%08X hdr=%08X mask=%016llX sel=%016llX\n", RegValue(0x2208), RegValue(0x2318),
                                g_cp.curHeader.load(), static_cast<unsigned long long>(t_binMask), static_cast<unsigned long long>(t_binSelect));
            }
            const uint32_t init = p(0);
            std::vector<uint32_t> imm;
            if (((init >> 6) & 3) == 1)
                for (uint32_t i = 1; i < count; i++)
                    imm.push_back(p(i));
            RendererDraw(init, imm.data(), static_cast<uint32_t>(imm.size()), 0, 0);
            break;
        }

        case PM4_INDIRECT_BUFFER:
        case PM4_INDIRECT_BUFFER_PFD:
        {
            const uint32_t addr = p(0);
            const uint32_t size = p(1) & 0xFFFFF;
            {
                static int ibLog = 0;
                if (std::getenv("Q4_IBLOG") && (addr == 0x0AA201C0u || addr == 0x0AA80660u - 0x60u || (addr >= 0x0AA20000u && addr < 0x0AA20300u)) && ibLog++ < 40)
                    std::printf("[ib] IB packet hdr=%08X addr=%08X dw1=%08X (size %u) at %s dword %u\n", g_cp.curHeader.load(), addr,
                                p(1), size, t_curDepth ? "IB" : "ring", t_curPos);
            }
            g_cp.ibs++;
            bool plausibleIb = true;
            if (size)
            {
                const uint32_t first = GuestLoad32(PhysToVirt(addr));
                const uint32_t ftype = first >> 30;
                static const uint32_t kKnown[] = { 0x10, 0x21, 0x22, 0x23, 0x25, 0x26, 0x27, 0x2B, 0x2D, 0x2E, 0x2F,
                                                   0x34, 0x35, 0x36, 0x37, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x44,
                                                   0x45, 0x46, 0x48, 0x50, 0x51, 0x54, 0x55, 0x56, 0x58, 0x5A, 0x5B,
                                                   0x5E, 0x60, 0x61, 0x62, 0x63, 0x64, 0x7F };
                bool knownOp = false;
                for (uint32_t k : kKnown)
                    knownOp |= k == ((first >> 8) & 0x7F);
                const uint32_t fcount = ((first >> 16) & 0x3FFF) + 1;
                const uint32_t fop = (first >> 8) & 0x7F;
                const bool memWriteLike = fop == 0x3D || fop == 0x58 || fop == 0x5A || fop == 0x45 || fop == 0x46;
                const bool plausible = ftype == 0 || ftype == 2 || ftype == 1 || fop == 0 ||
                                       (ftype == 3 && knownOp && fcount < 0x1000 && !(memWriteLike && fcount > 16));
                plausibleIb = plausible;
                static std::atomic<unsigned> bogus{ 0 };
                if (!plausible && bogus++ < 8)
                {
                    std::printf("[gpu] INDIRECT_BUFFER to implausible target: addr=0x%08X size=%u first=0x%08X "
                                "(from %s 0x%08X dword %u, depth %d)\n", addr, size, first, depth ? "IB" : "ring",
                                t_curBase, t_curPos, depth);
                    std::printf("[gpu]   ring state: rptr=%u wptr=%u size=%u (this IB packet at dword %u)\n", t_ringRptr,
                                t_ringWptr, t_ringSize, t_curPos);
                    GpuCpDecodeBefore(24);
                    {
                        const uint32_t virt = PhysToVirt(addr);
                        std::printf("[gpu]   now:");
                        for (uint32_t i = 0; i < 16; i++)
                            std::printf(" %08X", GuestLoad32(virt + i * 4));
                        std::printf("\n");
                        std::thread([virt, addr]()
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            std::printf("[gpu]   100 ms later (0x%08X):", addr);
                            for (uint32_t i = 0; i < 16; i++)
                                std::printf(" %08X", GuestLoad32(virt + i * 4));
                            std::printf("\n");
                            std::fflush(stdout);
                        }).detach();
                    }
                    std::fflush(stdout);
                }
            }
            if (size > 0x10000)
            {
                static std::atomic<unsigned> huge{ 0 };
                if (huge++ < 8)
                    std::printf("[gpu] INDIRECT_BUFFER huge size=%u addr=0x%08X first=0x%08X depth %d -- skipped\n", size,
                                addr, GuestLoad32(PhysToVirt(addr)), depth);
                break;
            }
            if (size && !plausibleIb)
                break;
            if (depth < 8 && size)
            {
                t_lastIbSize = size;
                ExecuteBuffer(ctx, PhysToVirt(addr), 0, size, 0, depth + 1);
            }
            break;
        }

        case PM4_MEM_WRITE:
        {
            const uint32_t addr = p(0) & ~3u;
            for (uint32_t i = 1; i < count; i++)
                WriteGuestDword(addr + (i - 1) * 4, p(i));
            g_cp.memWrites++;
            break;
        }

        case PM4_EVENT_WRITE_SHD:
        {
            const uint32_t initiator = p(0);
            const uint32_t addr = p(1) & ~3u;
            const uint32_t value = p(2);
            // Bit 31 of the initiator selects the "timestamp" (here: a monotonically
            // increasing counter) instead of the immediate value.
            static std::atomic<uint32_t> counter{ 1 };
            WriteGuestDword(addr, (initiator >> 31) & 1 ? counter++ : value);
            g_cp.memWrites++;
            break;
        }

        case PM4_EVENT_WRITE_EXT:
        {
            const uint32_t addr = p(1) & ~3u;
            // Screen-extent / visibility data: report an empty (0,0,0,0) result.
            for (uint32_t i = 0; i < 4; i++)
                WriteGuestDword(addr + i * 4, 0);
            g_cp.memWrites++;
            break;
        }

        case PM4_EVENT_WRITE:
        {
            const uint32_t initiator = p(0);
            if ((initiator & 0x3F) == kEventSwap)
            {
                g_cp.swaps++;
                VideoNotifySwap();
            }
            // An event with an address payload also stores a value (fence-style).
            if (count >= 3)
                WriteGuestDword(p(1) & ~3u, p(2));
            break;
        }

        case PM4_COND_WRITE:
        {
            const uint32_t waitInfo = p(0);
            const uint32_t pollAddr = p(1);
            const uint32_t ref = p(2);
            const uint32_t mask = p(3);
            const uint32_t writeAddr = p(4);
            const uint32_t writeData = p(5);

            const bool pollMemory = (waitInfo & 0x10) != 0;
            const uint32_t value = pollMemory ? GuestLoad32(PhysToVirt(pollAddr & ~3u)) : ReadReg(pollAddr & 0x7FFF);
            if (Compare(waitInfo, value & mask, ref))
            {
                if (waitInfo & 0x100)
                    WriteGuestDword(writeAddr & ~3u, writeData);
                else
                    WriteReg(writeAddr & 0x7FFF, writeData);
            }
            break;
        }

        case PM4_WAIT_REG_MEM:
        {
            const uint32_t waitInfo = p(0);
            const uint32_t pollAddr = p(1);
            const uint32_t ref = p(2);
            const uint32_t mask = p(3);
            const bool pollMemory = (waitInfo & 0x10) != 0;

            auto sample = [&]()
            {
                return pollMemory ? GuestLoad32(PhysToVirt(pollAddr & ~3u)) : ReadReg(pollAddr & 0x7FFF);
            };

            // Bounded by wall-clock time: a condition that depends on hardware we do not emulate
            // (drawing, GPU idle status, ...) must not stall the processor for long. A wait that
            // already timed out once on the same location/value is unlikely to ever be satisfied
            // (nothing here produces it), so later identical waits only poll briefly.
            static std::mutex learnedMutex;
            static std::set<std::tuple<bool, uint32_t, uint32_t, uint32_t>> neverSatisfied;
            const auto key = std::make_tuple(pollMemory, pollAddr, ref, waitInfo & 7);

            bool learned;
            {
                std::lock_guard<std::mutex> lock(learnedMutex);
                learned = neverSatisfied.count(key) != 0;
            }

            const auto deadline = std::chrono::steady_clock::now() +
                                  (learned ? std::chrono::microseconds(100) : std::chrono::milliseconds(5));
            // Register polls other than the scratch registers read hardware status (RBBM idle,
            // flush handshakes) that this emulation never makes busy: treat them as satisfied.
            const uint32_t regIndex = pollAddr & 0x7FFF;
            if (!pollMemory && !(regIndex >= 0x0570 && regIndex <= 0x057F))
                break;

            bool satisfied = Compare(waitInfo, sample() & mask, ref);
            while (!satisfied && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::yield();
                SyncScratchRegisters();
                satisfied = Compare(waitInfo, sample() & mask, ref);
            }

            if (!satisfied && !learned)
            {
                std::lock_guard<std::mutex> lock(learnedMutex);
                neverSatisfied.insert(key);
            }

            if (!satisfied && !learned)
            {
                static std::atomic<uint32_t> timeouts{ 0 };
                const uint32_t n = ++timeouts;
                if (n <= 12)
                {
                    std::printf("[gpu] WAIT_REG_MEM timed out: %s 0x%08X func=%u ref=0x%08X mask=0x%08X value=0x%08X\n",
                                pollMemory ? "mem" : "reg", pollAddr, waitInfo & 7, ref, mask, sample());
                    std::fflush(stdout);
                }
            }
            break;
        }

        case PM4_REG_RMW:
        {
            const uint32_t reg = p(0) & 0x1FFF;
            const uint32_t andMask = p(1);
            const uint32_t orMask = p(2);
            WriteReg(reg, (ReadReg(reg) & andMask) | orMask);
            break;
        }

        case PM4_INTERRUPT:
        {
            const uint32_t n = ++g_cp.interrupts;
            if (n <= 6)
            {
                std::printf("[gpu] PM4_INTERRUPT #%u mask=0x%X in %s at 0x%08X + dword %u (depth %d), nearby:", n, p(0),
                            t_curDepth ? "indirect buffer" : "ring", t_curBase, t_curPos, t_curDepth);
                for (int i = -4; i < 6; i++)
                    std::printf(" %08X", GuestLoad32(t_curBase + (t_curPos + i) * 4));
                std::printf("\n");
                std::fflush(stdout);
            }
            VideoDispatchInterrupt(ctx, 1);
            break;
        }

        default:
            if (g_cp.unknown++ < 16)
            {
                std::printf("[gpu] unhandled PM4 opcode 0x%02X (count %u, header 0x%08X at %s 0x%08X dword %u):", opcode, count,
                            g_cp.curHeader.load(), t_curDepth ? "IB" : "ring", t_curBase, t_curPos);
                for (uint32_t i = 0; i < count && i < 8; i++)
                    std::printf(" %08X", p(i));
                std::printf("\n");
                if (g_cp.unknown <= 2)
                {
                    std::printf("[gpu]   (innermost IB size = %u dwords)\n", t_lastIbSize);
                    GpuCpDecodeBefore(24);
                    GpuCpDumpAroundCurrent(248, 74);
                }
            }
            break;
        }
    }

    bool KnownType3(uint32_t op)
    {
        switch (op)
        {
        case 0x00: case 0x10: case 0x21: case 0x22: case 0x23: case 0x25: case 0x26: case 0x27: case 0x2B: case 0x2D: case 0x2E:
        case 0x2F: case 0x36: case 0x37: case 0x3B: case 0x3C: case 0x3D: case 0x3F: case 0x45: case 0x46: case 0x48: case 0x50:
        case 0x51: case 0x54: case 0x55: case 0x56: case 0x58: case 0x5A: case 0x7F:
            return true;
        default:
            return false;
        }
    }

    // Executes `dwords` dwords starting at index `startPos` of the buffer at `baseVirt`.
    void ExecuteBuffer(PPCContext& ctx, uint32_t baseVirt, uint32_t startPos, uint32_t dwords, uint32_t wrap, int depth)
    {
        Reader r{ baseVirt, wrap, startPos };
        uint32_t consumed = 0;

        while (consumed < dwords)
        {
            // A CPU store to a scratch register takes effect immediately on the console; keep the
            // mirrored memory current before every packet (interrupts and waits depend on it).
            SyncScratchRegisters();

            const uint32_t hdr = r.At(0);
            const uint32_t type = hdr >> 30;
            if (depth > 0 && type == 3 && !KnownType3((hdr >> 8) & 0x7F))
            {
                // Not a packet: inline data the title left in the buffer. Resume at the next position from which
                // well-formed packets tile the rest of the buffer exactly.
                const uint32_t remaining = dwords - consumed;
                uint32_t resume = 0;
                for (uint32_t off = 1; off < remaining && off < 8192 && !resume; off++)
                {
                    uint32_t k = off;
                    bool ok = true;
                    while (k < remaining)
                    {
                        const uint32_t h = r.At(k);
                        const uint32_t t = h >> 30;
                        if (t == 3 && !KnownType3((h >> 8) & 0x7F)) { ok = false; break; }
                        if (t == 1 || t == 2) { ok = false; break; }
                        k += ((h >> 16) & 0x3FFF) + 2;
                    }
                    if (ok && k == remaining)
                        resume = off;
                }
                static std::atomic<unsigned> resyncs{ 0 };
                if (resyncs++ < 400)
                    std::printf("[gpu] inline data in IB 0x%08X at dword %u (hdr 0x%08X): %s %u dwords\n", baseVirt, r.pos, hdr,
                                resume ? "skipping" : "cannot resync; abandoning buffer,", resume);
                if (!resume)
                    break;
                r.pos += resume;
                consumed += resume;
                continue;
            }
            uint32_t length = 1;
            g_cp.packets++;
            t_curBase = baseVirt;
            t_curPos = r.pos;
            t_curDepth = depth;
            {
                PacketRecord& rec = t_history[t_historyCount++ % kHistory];
                rec.base = baseVirt;
                rec.pos = r.pos;
                rec.hdr = hdr;
                rec.first = r.At(1);
                rec.second = r.At(2);
                rec.depth = depth;
            }
            g_cp.curHeader = hdr;
            g_cp.curDepth = static_cast<uint32_t>(depth);
            g_cp.curOpcode = type == 3 ? ((hdr >> 8) & 0x7F) : 0xFF;

            switch (type)
            {
            case 0:
            {
                g_cp.type0++;
                const uint32_t count = ((hdr >> 16) & 0x3FFF) + 1;
                const uint32_t baseReg = hdr & 0x7FFF;
                const bool oneReg = (hdr & 0x8000) != 0;
                if (count > 256 && depth > 0)
                    return; // garbage (float data read as packets): abandon this indirect buffer
                if (count > 256)
                {
                    static int bigReported = 0;
                    if (bigReported++ < 2)
                    {
                        std::printf("[gpu] suspicious type-0 packet: reg 0x%04X count %u hdr 0x%08X\n", baseReg, count, hdr);
                        GpuCpDecodeBefore(60);
                        GpuCpDumpAroundCurrent(70, 6);
                    }
                }
                for (uint32_t i = 0; i < count; i++)
                    WriteReg(oneReg ? baseReg : baseReg + i, r.At(1 + i));
                length = 1 + count;
                break;
            }
            case 1:
                WriteReg(hdr & 0x7FF, r.At(1));
                WriteReg((hdr >> 11) & 0x7FF, r.At(2));
                length = 3;
                break;
            case 2:
                length = 1;
                break;
            default:
            {
                g_cp.type3++;
                const uint32_t opcode = (hdr >> 8) & 0x7F;
                const uint32_t count = ((hdr >> 16) & 0x3FFF) + 1;
                if (opcode == 0x50 && count >= 2)
                    t_binMask = r.At(1) | (static_cast<uint64_t>(r.At(2)) << 32);
                else if (opcode == 0x51 && count >= 2)
                    t_binSelect = r.At(1) | (static_cast<uint64_t>(r.At(2)) << 32);
                else if ((hdr & 1) && (t_binSelect & t_binMask) == 0)
                {
                    // predicated packet whose bin is not selected: skipped
                    static std::atomic<unsigned> skipped{ 0 };
                    static std::atomic<unsigned> logged{ 0 };
                    skipped++;
                    if (std::getenv("Q4_PREDLOG") && logged++ < 40)
                        std::printf("[pred] skip op 0x%02X count %u (mask %016llX select %016llX) at %s dword %u\n", opcode, count,
                                    static_cast<unsigned long long>(t_binMask), static_cast<unsigned long long>(t_binSelect),
                                    depth ? "IB" : "ring", r.pos);
                }
                else
                    ExecuteType3(ctx, r, opcode, count, depth);
                length = 1 + count;
                break;
            }
            }

            r.pos += length;
            if (wrap)
                r.pos %= wrap;
            consumed += length;
        }
        if (depth > 0 && std::getenv("Q4_IBTAIL") && dwords > 1000 && g_cp.packets > static_cast<uint32_t>(std::atoi(std::getenv("Q4_IBTAIL"))))
        {
            static std::atomic<unsigned> shown{ 0 };
            if (shown++ < 6)
            {
                std::printf("[ibtail] IB 0x%08X size %u executed %u; last packets:\n", baseVirt, dwords, consumed);
                GpuCpDecodeBefore(40);
            }
        }
    }

    thread_local bool t_isProcessor = false;

    void ProcessorLoop(PPCContext& ctx)
    {
        t_isProcessor = true;
        uint32_t rptr = 0;

        for (;;)
        {
            const uint32_t ringPhys = g_cp.ringPhys.load();
            const uint32_t log2 = g_cp.ringSizeLog2.load();
            const uint32_t sizeDwords = (1u << log2) * 2;      // (1 << log2) * 8 bytes
            uint32_t wptr = ReadReg(kRegCpRbWptr) % sizeDwords;
            if (ringPhys && rptr != wptr && (wptr + sizeDwords - rptr) % sizeDwords > sizeDwords / 2)
            {
                // A write pointer far ahead of the read pointer is a torn or transient value (the title
                // never fills more than half the ring); re-read before replaying stale ring contents.
                for (int retry = 0; retry < 50; retry++)
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    wptr = ReadReg(kRegCpRbWptr) % sizeDwords;
                    if ((wptr + sizeDwords - rptr) % sizeDwords <= sizeDwords / 2)
                        break;
                }
            }
            SyncScratchRegisters();

            if (ringPhys && rptr != wptr)
            {
                const uint32_t pending = (wptr + sizeDwords - rptr) % sizeDwords;
                t_ringRptr = rptr;
                t_ringWptr = wptr;
                t_ringSize = sizeDwords;
                if (pending > sizeDwords / 2)
                {
                    static std::atomic<unsigned> bigBatch{ 0 };
                    if (bigBatch++ < 20)
                    {
                        std::printf("[gpu] large ring batch: rptr=%u wptr=%u pending=%u size=%u\n", rptr, wptr, pending, sizeDwords);
                        std::fflush(stdout);
                    }
                }
                g_cp.busy = true;
                g_cp.batchesStarted++;
                ExecuteBuffer(ctx, PhysToVirt(ringPhys), rptr, pending, sizeDwords, 0);
                g_cp.batchesDone++;
                g_cp.busy = false;
                rptr = wptr;
                g_cp.rptr = rptr;

                WriteReg(kRegCpRbRptr, rptr);
                if (const uint32_t wb = g_cp.writebackPhys.load())
                    WriteGuestDword(wb, rptr);
            }
            else
            {
                for (int spin = 0; spin < 4000 && rptr == ReadReg(kRegCpRbWptr) % sizeDwords; spin++)
                    YieldProcessor();
                if (rptr == ReadReg(kRegCpRbWptr) % sizeDwords)
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    }
}

void GpuCpSetRing(uint32_t physBase, uint32_t sizeLog2)
{
    g_cp.ringPhys = physBase;
    g_cp.ringSizeLog2 = sizeLog2;
}

void GpuCpSetWriteback(uint32_t physAddr)
{
    g_cp.writebackPhys = physAddr;
}

void GpuCpStart()
{
    bool expected = false;
    if (!g_cp.started.compare_exchange_strong(expected, true))
        return;

    CreateGuestWorker([](PPCContext& ctx) { ProcessorLoop(ctx); });
}

void GpuCpDumpAroundCurrent(int before, int after)
{
    std::printf("[gpu] stream around the current packet (buffer 0x%08X, dword %u):\n", t_curBase, t_curPos);
    for (int i = -before; i < after; i++)
    {
        if (i % 8 == 0 || i == -before)
            std::printf("%s  [%+4d]:", i == -before ? "" : "\n", i);
        std::printf(" %08X", GuestLoad32(t_curBase + (t_curPos + i) * 4));
    }
    std::printf("\n");
    std::fflush(stdout);
}

void GpuCpDecodeBefore(int count)
{
    const size_t have = t_historyCount < kHistory ? t_historyCount : kHistory;
    const size_t want = static_cast<size_t>(count) < have ? static_cast<size_t>(count) : have;

    std::printf("[gpu] last %zu executed packets (oldest first; plain state writes and filler hidden):\n", want);
    for (size_t i = want; i > 0; i--)
    {
        const PacketRecord& rec = t_history[(t_historyCount - i) % kHistory];
        const uint32_t type = rec.hdr >> 30;

        // Hide filler and ordinary register/state traffic; keep scratch writes and all type-3 packets.
        const uint32_t reg = rec.hdr & 0x7FFF;
        const bool scratch = type == 0 && reg >= 0x0570 && reg <= 0x057F;
        if (!(type == 3 || scratch))
            continue;
        char what[96];
        if (type == 0)
            std::snprintf(what, sizeof(what), "type0 reg 0x%04X x%u%s", rec.hdr & 0x7FFF, ((rec.hdr >> 16) & 0x3FFF) + 1,
                          (rec.hdr & 0x8000) ? " (one reg)" : "");
        else if (type == 3)
            std::snprintf(what, sizeof(what), "type3 op 0x%02X payload %u%s", (rec.hdr >> 8) & 0x7F,
                          ((rec.hdr >> 16) & 0x3FFF) + 1, (rec.hdr & 1) ? " PREDICATED" : "");
        else
            std::snprintf(what, sizeof(what), "type%u", type);

        std::printf("  %s0x%08X[%5u] %08X  %-32s %08X %08X\n", rec.depth ? "IB " : "ring ", rec.base, rec.pos, rec.hdr,
                    what, rec.first, rec.second);
    }
    std::fflush(stdout);
}

void GpuCpDescribeScratch(char* out, size_t outSize)
{
    const uint32_t umsk = ReadReg(kRegScratchUmsk);
    const uint32_t addr = (ReadReg(kRegScratchAddr) & ~3u);
    const uint32_t memAddr = guest::kPhysicalBegin + ((addr + 4 * 4) & 0x1FFFFFFF);
    std::snprintf(out, outSize,
                  "UMSK=0x%08X ADDR=0x%08X -> scratch4 mirror @0x%08X = 0x%08X; reg4=0x%08X last4=0x%08X valid=%d",
                  umsk, addr, memAddr, GuestLoad32(memAddr), ReadReg(kRegScratchReg0 + 4), g_lastScratch[4],
                  g_lastScratchValid ? 1 : 0);
}

void GpuCpDescribeCurrent(char* out, size_t outSize)
{
    std::snprintf(out, outSize, "%s buffer 0x%08X dword %u, header 0x%08X", t_curDepth ? "indirect" : "ring", t_curBase,
                  t_curPos, g_cp.curHeader.load());
}

bool GpuCpIsIdle()
{
    if (g_cp.busy.load())
        return false;

    const uint32_t log2 = g_cp.ringSizeLog2.load();
    if (g_cp.ringPhys.load() == 0 || log2 == 0)
        return false;

    const uint32_t sizeDwords = (1u << log2) * 2;
    return (ReadReg(kRegCpRbWptr) % sizeDwords) == g_cp.rptr.load();
}

GpuCpStats GpuCpGetStats()
{
    GpuCpStats s;
    s.packets = g_cp.packets;
    s.type0 = g_cp.type0;
    s.type3 = g_cp.type3;
    s.indirectBuffers = g_cp.ibs;
    s.memoryWrites = g_cp.memWrites;
    s.interrupts = g_cp.interrupts;
    s.swaps = g_cp.swaps;
    s.unknownOpcodes = g_cp.unknown;
    s.rptr = g_cp.rptr;
    s.curHeader = g_cp.curHeader;
    s.curDepth = g_cp.curDepth;
    s.curOpcode = g_cp.curOpcode;
    s.busy = g_cp.busy;
    return s;
}


// Frame boundary for the draw tracker (called when the title presents).
void GpuCpFrameEnd()
{
    DrawFrameEnd();
}

// Blocks until the command processor has consumed everything up to the current write pointer.
void GpuCpWaitDrained(uint32_t timeoutMs)
{
    if (t_isProcessor || !g_cp.started.load())
        return;

    // Barrier: everything written to the ring before this call must be executed. Waiting for a
    // fully idle CP would starve while another thread keeps submitting, so wait for the first batch
    // that starts after now (it picks up the current write pointer) to complete.
    const uint32_t sizeDwords = (1u << g_cp.ringSizeLog2.load()) * 2;
    const uint64_t t0 = GetTickCount64();
    const uint64_t target = g_cp.batchesStarted.load() + 1;
    for (;;)
    {
        const uint32_t wptr = ReadReg(kRegCpRbWptr) % sizeDwords;
        if (g_cp.rptr == wptr && !g_cp.busy.load())
            return;
        if (g_cp.batchesDone.load() >= target)
            return;
        if (GetTickCount64() - t0 > timeoutMs)
            return;
        YieldProcessor();
        Sleep(0);
    }
}

uint32_t GpuRegRead(uint32_t index)
{
    return RegValue(index);
}
