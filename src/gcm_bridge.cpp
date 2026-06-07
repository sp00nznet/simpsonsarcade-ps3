/*
 * The Simpsons Arcade Game — cellGcmSys -> ps3recomp RSX/D3D12 backend
 *
 * Wires the game's cellGcmSys imports to the ps3recomp runtime's RSX layer
 * (libs/video: cellGcmSys.c + rsx_commands.c + rsx_d3d12_backend.c). The
 * runtime owns the GPU state machine and D3D12 rendering; here we:
 *   - expose guest-memory-resident structs the game reads/writes directly
 *     (command-buffer control register, label array, GCM context + cmd buffer),
 *   - reuse the runtime's native cellGcm* for config/offset translation,
 *   - bring up the D3D12 backend window + a present thread.
 *
 * The control register is the unblock: the game polls cellGcmGetControlRegister
 * and waits for the FIFO to drain (get catches up to put). We advance get=put
 * with a raw byte copy so the comparison holds regardless of how the SDK reads
 * the register (RSX MMIO is little-endian / lwbrx; normal memory is big-endian).
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#if defined(_WIN32)
#include <windows.h>
#endif
#include "recomp/ppu_recomp.h"   /* lifter-layout ppu_context (matches simpsons_hle) */

extern "C" {
extern uint8_t* vm_base;
uint8_t  vm_read8(uint64_t);
uint16_t vm_read16(uint64_t);
uint32_t vm_read32(uint64_t);
void     vm_write8(uint64_t, uint8_t);
void     vm_write16(uint64_t, uint16_t);
void     vm_write32(uint64_t, uint32_t);
void     hle_guest_malloc(ppu_context*);

/* runtime cellVideoOut (native; structs passed as raw byte buffers matching the
 * runtime layout — we marshal field-by-field to guest big-endian). */
int32_t cellVideoOutGetState(uint32_t videoOut, uint32_t devIdx, void* state);
int32_t cellVideoOutGetResolution(uint32_t resId, void* res);
int32_t cellVideoOutGetConfiguration(uint32_t videoOut, void* config, void* option);
int32_t cellVideoOutGetDeviceInfo(uint32_t videoOut, uint32_t devIdx, void* info);
int32_t cellVideoOutGetNumberOfDevice(uint32_t videoOut);
int32_t cellVideoOutConfigure(uint32_t videoOut, void* config, void* option, uint32_t wait);

/* runtime cellGcmSys (native args, C linkage) */
int32_t cellGcmInit(uint32_t cmdSize, uint32_t ioSize, uint32_t ioAddress);
int32_t cellGcmGetConfiguration(void* config);          /* CellGcmConfig: 6 x u32 */
int32_t cellGcmAddressToOffset(uint32_t address, uint32_t* offset);
int32_t cellGcmMapMainMemory(uint32_t ea, uint32_t size, uint32_t* offset);
int32_t cellGcmGetTiledPitchSize(uint32_t size, uint32_t* pitch);

/* runtime RSX D3D12 backend */
int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
int  rsx_d3d12_backend_pump_messages(void);
void rsx_d3d12_backend_present(void);

/* runtime RSX command processor (rsx_state* declared as void* — ABI-identical
 * under C linkage; avoids pulling the heavy rsx_commands.h into this TU). */
void rsx_state_init(void* state);
int  rsx_process_command_buffer(void* state, const uint32_t* buf, uint32_t size);

/* guest-function invoker (indirect_dispatch.cpp): builds a ppu_context + runs. */
uint64_t simpsons_run_thread(uint32_t func, uint32_t toc, uint32_t arg,
                             uint32_t sp, uint32_t tid);
}

static const uint32_t WIN_W = 1280, WIN_H = 720;

static uint32_t g_ctrl_guest  = 0;   /* CellGcmControl {put,get,ref} in guest mem */
static uint32_t g_label_guest = 0;   /* 256 x u32 label array in guest mem */
static uint32_t g_flip_opd    = 0;   /* registered flip handler OPD */
static int      g_backend_up  = 0;
static uint32_t g_cmdbuf_begin = 0;  /* guest addr of the FIFO command buffer */
static uint32_t g_cmdbuf_size  = 0;
static uint32_t g_ctxdata_guest = 0; /* CellGcmContextData {begin,end,current,callback} */
/* rsx_state storage (real struct is a few KB of u32 state; 16 KB is ample). */
static uint64_t g_rsx_state[2048];
static int      g_rsx_inited = 0;

/* Allocate guest memory via the bump allocator, aligned. */
static uint32_t guest_alloc(uint32_t size, uint32_t align) {
    ppu_context t; std::memset(&t, 0, sizeof t);
    t.gpr[3] = size + align;
    hle_guest_malloc(&t);
    uint32_t p = (uint32_t)t.gpr[3];
    if (!p) return 0;
    return (p + (align - 1)) & ~(align - 1);
}

#if defined(_WIN32)
static DWORD WINAPI gcm_present_thread(LPVOID) {
    for (;;) {
        rsx_d3d12_backend_pump_messages();
        /* Continuously fake the FIFO as fully drained: get = ref = put. The render
         * thread (crCommandQueue) writes put then BUSY-POLLS get in guest memory
         * waiting for the GPU to consume the FIFO; we only updated get inside
         * cellGcmGetControlRegister (called once), so a direct poll loop never saw
         * it advance. Keep get==put every tick so the thread proceeds to submit
         * the next (real) commands. Endianness-agnostic raw copy. */
        if (g_ctrl_guest) {
            uint8_t* c = vm_base + g_ctrl_guest;
            std::memcpy(c + 4, c + 0, 4);   /* get = put */
            std::memcpy(c + 8, c + 0, 4);   /* ref = put */
        }
        /* Drive the FIFO: process the commands the game has flushed (put is the
         * write offset into the cmd buffer), then present. The command processor
         * dispatches clear/draw to the D3D12 backend. */
        if (g_rsx_inited && g_ctrl_guest && g_cmdbuf_begin) {
            uint32_t put;
            std::memcpy(&put, vm_base + g_ctrl_guest + 0, 4);   /* host-endian offset */
            if (put && put <= g_cmdbuf_size) {
                static uint32_t s_last = 0xFFFFFFFF;
                int n = rsx_process_command_buffer(
                            g_rsx_state, (const uint32_t*)(vm_base + g_cmdbuf_begin), put);
                if (put != s_last) {
                    const uint32_t* w = (const uint32_t*)(vm_base + g_cmdbuf_begin);
                    fprintf(stderr, "[gcm] processed FIFO put=0x%08X -> %d methods; head:", put, n);
                    for (int i = 0; i < 8; i++)
                        fprintf(stderr, " %08X", w[i]);
                    fprintf(stderr, " | bswap:");
                    for (int i = 0; i < 8; i++)
                        fprintf(stderr, " %08X", _byteswap_ulong(w[i]));
                    fprintf(stderr, "\n");
                    s_last = put;
                }
            }
        }
        rsx_d3d12_backend_present();
        /* Vblank simulation: once the game has registered a flip handler (and then
         * returned to OS), its per-frame loop is driven by this callback. Fire it
         * at ~60 Hz so the game advances frames. Resolve the OPD -> (func,toc).
         * Use a dedicated guest stack + tid so it doesn't clash with game threads. */
        if (g_flip_opd) {
            static int armed_logged = 0;
            uint32_t func = vm_read32(g_flip_opd);
            uint32_t toc  = vm_read32(g_flip_opd + 4);
            if (func >= 0x10000 && func < 0x10000000) {
                if (!armed_logged++) fprintf(stderr,
                    "[gcm] driving flip handler opd=0x%08X -> func=0x%08X toc=0x%08X @~60Hz\n",
                    g_flip_opd, func, toc);
                const uint32_t FLIP_TID = 200;
                uint32_t sp = 0x2F000000u + FLIP_TID * 0x40000u + 0x40000u - 0x200u;
                simpsons_run_thread(func, toc, 0 /*head*/, sp, FLIP_TID);
            }
        }
        Sleep(16);   /* ~60 Hz */
    }
}
#endif

static void ensure_backend(void) {
    if (g_backend_up) return;
    g_backend_up = 1;
#if defined(_WIN32)
    if (rsx_d3d12_backend_init(WIN_W, WIN_H, "The Simpsons Arcade Game (D3D12)") == 0) {
        fprintf(stderr, "[gcm] D3D12 backend up %ux%u\n", WIN_W, WIN_H);
        CreateThread(nullptr, 0, gcm_present_thread, nullptr, 0, nullptr);
    } else {
        fprintf(stderr, "[gcm] D3D12 backend init FAILED\n");
    }
#endif
}

/* ---- bridges (lifter ppu_context; return value in gpr[3]) -------------- */

/* _cellGcmInitBody(context_out, cmdSize, ioSize, ioAddress) */
extern "C" void simpsons_gcm_init_body(ppu_context* ctx) {
    uint32_t ctxPtrAddr = (uint32_t)ctx->gpr[3];
    uint32_t cmdSize    = (uint32_t)ctx->gpr[4];
    uint32_t ioSize     = (uint32_t)ctx->gpr[5];
    uint32_t ioAddr     = (uint32_t)ctx->gpr[6];
    fprintf(stderr, "[gcm] InitBody ctxpp=0x%08X cmdSize=0x%X ioSize=0x%X ioAddr=0x%08X\n",
            ctxPtrAddr, cmdSize, ioSize, ioAddr);

    cellGcmInit(cmdSize, ioSize, ioAddr);
    ensure_backend();

    if (cmdSize < 0x10000) cmdSize = 0x10000;
    /* The command buffer lives at the start of the game's IO region (ioAddr) —
     * the game writes the FIFO there and put/get are offsets into it. (Our own
     * allocation went unused: the game ignores it.) */
    uint32_t cmdbuf = ioAddr;
    uint32_t cdata  = guest_alloc(16, 16);   /* CellGcmContextData */
    g_cmdbuf_begin = cmdbuf; g_cmdbuf_size = cmdSize;
    if (!g_rsx_inited) { rsx_state_init(g_rsx_state); g_rsx_inited = 1; }
    /* CellGcmContextData layout (the GAME's, confirmed by func_00135624/func_00138520
     * reading current@+8, end@+4, and the flush callback OPD@+0xC): begin@+0,
     * end@+4, current@+8, callback@+0xC. (The old callback@+0/begin@+4 layout made
     * the game read `current` as the callback OPD -> garbage indirect call, and the
     * swapped begin/end made it think the buffer was instantly full -> stuck flush.) */
    vm_write32(cdata + 0x0, cmdbuf);              /* begin   */
    vm_write32(cdata + 0x4, cmdbuf + cmdSize);    /* end     */
    vm_write32(cdata + 0x8, cmdbuf);              /* current */
    vm_write32(cdata + 0xC, 0);                   /* callback OPD (filled below) */
    g_ctxdata_guest = cdata;
    if (ctxPtrAddr) vm_write32(ctxPtrAddr, cdata);
    fprintf(stderr, "[gcm]   ctxdata=0x%08X cmdbuf=0x%08X..0x%08X\n",
            cdata, cmdbuf, cmdbuf + cmdSize);
    ctx->gpr[3] = 0;
}

/* cellGcmGetConfiguration(config_out) */
extern "C" void simpsons_gcm_get_configuration(ppu_context* ctx) {
    uint32_t out = (uint32_t)ctx->gpr[3];
    uint32_t cfg[6] = {0};
    cellGcmGetConfiguration(cfg);
    if (out) for (int i = 0; i < 6; i++) vm_write32(out + i * 4, cfg[i]);
    ctx->gpr[3] = 0;
}

/* Set in vm_bridge.cpp: once tripped, the deadlocking CRI completion sems
 * (sem 2/6, posted only by the absent SPURS job) are faked as satisfied. The
 * render path going live (this call) provably precedes that fatal wait. */
extern "C" volatile int g_cri_wedged;

/* cellGcmGetControlRegister() -> guest ptr to {put,get,ref}. THE unblock. */
extern "C" void simpsons_gcm_get_control_register(ppu_context* ctx) {
    g_cri_wedged = 1;
    if (!g_ctrl_guest) {
        g_ctrl_guest = guest_alloc(64, 16);
        if (g_ctrl_guest) std::memset(vm_base + g_ctrl_guest, 0, 12);
        fprintf(stderr, "[gcm] control register @0x%08X\n", g_ctrl_guest);
    }
    if (g_ctrl_guest) {
        uint8_t* c = vm_base + g_ctrl_guest;
        /* observe put (is the game submitting commands?) */
        static uint32_t s_last_put = 0; static int s_put_logs = 0;
        uint32_t put_raw; std::memcpy(&put_raw, c + 0, 4);
        if (put_raw != s_last_put && s_put_logs < 30) {
            fprintf(stderr, "[gcm] put advanced: 0x%08X -> 0x%08X (bswap 0x%08X)\n",
                    s_last_put, put_raw, _byteswap_ulong(put_raw));
            s_last_put = put_raw; s_put_logs++;
        }
        std::memcpy(c + 4, c + 0, 4);   /* get = put (raw, endianness-agnostic) */
        std::memcpy(c + 8, c + 0, 4);   /* ref = put (coarse FIFO-complete signal) */
    }
    ctx->gpr[3] = g_ctrl_guest;
}

/* cellGcmSetFlipHandler(handler_opd) */
extern "C" void simpsons_gcm_set_flip_handler(ppu_context* ctx) {
    g_flip_opd = (uint32_t)ctx->gpr[3];
    fprintf(stderr, "[gcm] SetFlipHandler opd=0x%08X\n", g_flip_opd);
    ctx->gpr[3] = 0;
}

/* cellGcmAddressToOffset(address, offset_out) */
extern "C" void simpsons_gcm_address_to_offset(ppu_context* ctx) {
    uint32_t addr = (uint32_t)ctx->gpr[3];
    uint32_t outp = (uint32_t)ctx->gpr[4];
    uint32_t off  = 0;
    if (cellGcmAddressToOffset(addr, &off) != 0) off = addr;  /* identity fallback */
    if (outp) vm_write32(outp, off);
    ctx->gpr[3] = 0;
}

/* cellGcmMapMainMemory(ea, size, offset_out) */
extern "C" void simpsons_gcm_map_main_memory(ppu_context* ctx) {
    uint32_t ea   = (uint32_t)ctx->gpr[3];
    uint32_t size = (uint32_t)ctx->gpr[4];
    uint32_t outp = (uint32_t)ctx->gpr[5];
    uint32_t off  = 0;
    int32_t rc = cellGcmMapMainMemory(ea, size, &off);
    if (outp) vm_write32(outp, off);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

/* cellGcmGetLabelAddress(index) -> guest ptr */
extern "C" void simpsons_gcm_get_label_address(ppu_context* ctx) {
    uint32_t index = (uint32_t)ctx->gpr[3] & 0xFF;
    if (!g_label_guest) {
        g_label_guest = guest_alloc(256 * 4, 16);
        if (g_label_guest) std::memset(vm_base + g_label_guest, 0, 256 * 4);
        fprintf(stderr, "[gcm] label array @0x%08X\n", g_label_guest);
    }
    ctx->gpr[3] = g_label_guest ? (g_label_guest + index * 4) : 0;
}

/* cellGcmGetTiledPitchSize(size) -> pitch (return value) */
extern "C" void simpsons_gcm_get_tiled_pitch_size(ppu_context* ctx) {
    uint32_t size = (uint32_t)ctx->gpr[3];
    uint32_t pitch = 0;
    cellGcmGetTiledPitchSize(size, &pitch);
    ctx->gpr[3] = pitch;
}

/* cellGcmGetDefaultCommandWordSize() / cellGcmGetDefaultSegmentWordSize() */
extern "C" void simpsons_gcm_default_cmd_size(ppu_context* ctx)     { ctx->gpr[3] = 0x10000; }
extern "C" void simpsons_gcm_default_segment_size(ppu_context* ctx) { ctx->gpr[3] = 0x100; }

/* ---- cellVideoOut bridges (resolution/display state) ------------------- *
 * Unhandled before, these returned 0 and left the caller's struct as garbage
 * (zero resolution) — which fed the render-init descriptor walk a bad count.
 * Marshal the runtime cellVideoOut.c output to guest big-endian (per flОw). */

extern "C" void simpsons_vo_get_state(ppu_context* ctx) {
    uint8_t st[16]; std::memset(st, 0, sizeof st);
    int32_t rc = cellVideoOutGetState((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], st);
    uint32_t outp = (uint32_t)ctx->gpr[5];
    if (rc == 0 && outp) {
        vm_write8(outp + 0, st[0]);              /* state (0=enabled) */
        vm_write8(outp + 1, st[1]);              /* colorSpace */
        for (int i = 2; i < 8; i++) vm_write8(outp + i, 0);
        uint32_t dm; std::memcpy(&dm, st + 8, 4); /* packed displayMode */
        vm_write32(outp + 8, dm);
    }
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

extern "C" void simpsons_vo_get_resolution(ppu_context* ctx) {
    uint8_t r[4]; std::memset(r, 0, sizeof r);
    int32_t rc = cellVideoOutGetResolution((uint32_t)ctx->gpr[3], r);
    uint32_t outp = (uint32_t)ctx->gpr[4];
    if (rc == 0 && outp) {
        uint16_t w, h; std::memcpy(&w, r, 2); std::memcpy(&h, r + 2, 2);
        vm_write16(outp + 0, w); vm_write16(outp + 2, h);
    }
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

extern "C" void simpsons_vo_get_configuration(ppu_context* ctx) {
    uint8_t cfg[16]; std::memset(cfg, 0, sizeof cfg);
    int32_t rc = cellVideoOutGetConfiguration((uint32_t)ctx->gpr[3], cfg, nullptr);
    uint32_t outp = (uint32_t)ctx->gpr[4];
    if (rc == 0 && outp) {
        vm_write8(outp + 0, cfg[0]);   /* resolutionId */
        vm_write8(outp + 1, cfg[1]);   /* format */
        vm_write8(outp + 2, cfg[2]);   /* aspect */
        for (int i = 3; i < 12; i++) vm_write8(outp + i, 0);
        uint32_t pitch; std::memcpy(&pitch, cfg + 12, 4);
        vm_write32(outp + 12, pitch);
    }
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

extern "C" void simpsons_vo_get_device_info(ppu_context* ctx) {
    static uint8_t info[12 + 32 * 8];
    std::memset(info, 0, sizeof info);
    int32_t rc = cellVideoOutGetDeviceInfo((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], info);
    uint32_t outp = (uint32_t)ctx->gpr[5];
    if (rc == 0 && outp) {
        vm_write8(outp + 0, info[0]);                          /* portType */
        vm_write8(outp + 1, info[1]);                          /* colorSpace */
        uint16_t lat; std::memcpy(&lat, info + 2, 2); vm_write16(outp + 2, lat);
        vm_write8(outp + 4, info[4]);                          /* availableModeCount */
        vm_write8(outp + 5, info[5]);                          /* state */
        vm_write8(outp + 6, info[6]);                          /* rgbOutputRange */
        for (int i = 7; i < 12; i++) vm_write8(outp + i, 0);
        for (int m = 0; m < 32; m++) {
            int so = 12 + m * 8; uint32_t d = outp + 12 + m * 8;
            for (int i = 0; i < 4; i++) vm_write8(d + i, info[so + i]); /* resId/scan/conv/aspect */
            vm_write8(d + 4, 0); vm_write8(d + 5, 0);
            uint16_t rr; std::memcpy(&rr, info + so + 6, 2); vm_write16(d + 6, rr);
        }
    }
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

extern "C" void simpsons_vo_get_number_of_device(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)cellVideoOutGetNumberOfDevice((uint32_t)ctx->gpr[3]);
}

extern "C" void simpsons_vo_get_resolution_availability(ppu_context* ctx) {
    /* Not in the runtime; report standard resolutions (480/576/720/1080) as
     * available (1). resId is in r4. */
    ctx->gpr[3] = 1;
}

extern "C" void simpsons_vo_configure(ppu_context* ctx) {
    uint32_t cfgAddr = (uint32_t)ctx->gpr[4];
    uint8_t cfg[16]; std::memset(cfg, 0, sizeof cfg);
    if (cfgAddr) {
        cfg[0] = vm_read8(cfgAddr + 0);   /* resolutionId */
        cfg[1] = vm_read8(cfgAddr + 1);   /* format */
        cfg[2] = vm_read8(cfgAddr + 2);   /* aspect */
        uint32_t pitch = vm_read32(cfgAddr + 12); std::memcpy(cfg + 12, &pitch, 4);
    }
    int32_t rc = cellVideoOutConfigure((uint32_t)ctx->gpr[3], cfg, nullptr,
                                       (uint32_t)ctx->gpr[6]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}
