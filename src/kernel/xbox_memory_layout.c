/**
 * Xbox Memory Layout Implementation
 *
 * Maps the XBE data sections to their expected virtual addresses on Windows.
 * This is critical for the recompiled code which references globals by
 * absolute address (e.g., mov eax, [0x004D532C]).
 *
 * Implementation:
 * 1. VirtualAlloc a contiguous region at XBOX_BASE_ADDRESS
 * 2. Copy .rdata and initialized .data from the XBE
 * 3. Zero-fill the BSS region
 * 4. Set memory protection (read-only for .rdata)
 */

#include "xbox_memory_layout.h"
#include "kernel.h"
#include "recomp_config.h"
#include "xbox_watchpoint.h"
#include <stdio.h>
/* <stdlib.h> is load-bearing, not tidiness.
 *
 * Without it MSVC applies the implicit-declaration rule and assumes
 * `int getenv()`, so the returned pointer is truncated to 32 bits and sign
 * extended. RECOMP_WATCHDOG_SECS was the visible symptom: `if (!secs ||
 * !*secs)` dereferenced 0xFFFFFFFFA94859D5 and faulted about one run in
 * three, and when the truncated byte happened to read zero it returned
 * early instead -- so the watchdog was either a crash or silently inert,
 * and never once fired. The `getenv(...) != NULL` tests elsewhere in this
 * file survived only because a truncated non-zero value is still non-zero.
 */
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#if !defined(_WIN32)
#include <unistd.h>   /* _exit */
#endif

/* XBE header field offsets (per xboxdevwiki.net/Xbe) */
#define XBE_MAGIC_OFFSET        0x0000
#define XBE_BASE_ADDR_OFFSET    0x0104
#define XBE_HEADER_SIZE_OFFSET  0x0108
#define XBE_SECTION_COUNT_OFFSET 0x011C
#define XBE_SECTION_HEADERS_OFFSET 0x0120
#define XBE_TLS_ADDR_OFFSET     0x012C

/* XBE certificate field offsets (per xboxdevwiki.net/Xbe). The certificate
 * address itself is at header+0x0118 and is read inline below. */
#define CERT_TITLE_NAME         0x000C   /* UTF-16, fixed width, see below */
#define CERT_TITLE_NAME_CHARS   40

/* XBE section header layout (56 bytes each) */
#define SECTHDR_FLAGS       0x00
#define SECTHDR_VA          0x04
#define SECTHDR_VSIZE       0x08
#define SECTHDR_RAW_OFFSET  0x0C
#define SECTHDR_RAW_SIZE    0x10
#define SECTHDR_NAME_ADDR   0x14
#define SECTHDR_SIZE        56

/* The running title's own name, out of its XBE certificate, so the window
 * says what is being played rather than what is playing it. UTF-8, empty
 * until the headers below are parsed and empty for a certificate that does
 * not fit in the file -- callers fall back to a generic caption. */
static char g_xbe_title_name[128];

const char *xbox_XbeTitleName(void)
{
    return g_xbe_title_name[0] ? g_xbe_title_name : NULL;
}

/* wszTitleName is a fixed 40 UTF-16 code units, padded rather than
 * terminated on some discs, so the decode stops at a NUL or at the field's
 * end and then trims. BMP only: a surrogate pair would encode as CESU-8
 * here, which is not UTF-8, and no title name needs one. */
static void xbe_title_name_store(const unsigned char *field)
{
    char *out = g_xbe_title_name;
    char *end = g_xbe_title_name + sizeof g_xbe_title_name - 1;   /* room for NUL */
    int i;

    for (i = 0; i < CERT_TITLE_NAME_CHARS; i++) {
        unsigned c = (unsigned)field[i * 2] | ((unsigned)field[i * 2 + 1] << 8);

        if (c == 0)
            break;
        if (c >= 0xD800 && c <= 0xDFFF)
            continue;
        if (c < 0x80) {
            if (end - out < 1) break;
            *out++ = (char)c;
        } else if (c < 0x800) {
            if (end - out < 2) break;
            *out++ = (char)(0xC0 | (c >> 6));
            *out++ = (char)(0x80 | (c & 0x3F));
        } else {
            if (end - out < 3) break;
            *out++ = (char)(0xE0 | (c >> 12));
            *out++ = (char)(0x80 | ((c >> 6) & 0x3F));
            *out++ = (char)(0x80 | (c & 0x3F));
        }
    }
    while (out > g_xbe_title_name && (out[-1] == ' ' || out[-1] == '\t'))
        out--;
    *out = '\0';
}

static void *g_memory_base = NULL;
static size_t g_memory_size = 0;
static ptrdiff_t g_memory_offset = 0;  /* actual_base - XBOX_BASE_ADDRESS */

/* Actual mapped RAM for this run; see the header. Default retail 64 MB. */
size_t g_xbox_total_ram = XBOX_TOTAL_RAM;
size_t g_xbox_map_size = 0;   /* 0 = same as RAM */

void xbox_SetTotalRam(size_t bytes)
{
    g_xbox_total_ram = bytes;
}


void xbox_SetMapSize(size_t bytes)
{
    g_xbox_map_size = bytes;
}

/* File mapping handle for the Xbox memory region.
 * Using CreateFileMapping + MapViewOfFileEx allows mirror views to alias
 * the same physical pages as the base region, so writes to mirror addresses
 * (which wrap modulo 64 MB on real Xbox hardware) correctly modify the
 * underlying data. */
static HANDLE g_mapping_handle = NULL;

/* Mirror view pointers for cleanup */
static void *g_mirror_views[XBOX_NUM_MIRRORS] = {0};
static void *g_tiled_view = NULL;

/* Contiguous / physical memory window (see MemoryLayoutInit).
 * XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE come from kernel.h - the bridges need
 * the same numbers for MmClaimGpuInstanceMemory. */
static void *g_contig_memory = NULL;

/* NV2A GPU register aperture (see MemoryLayoutInit). Backed as plain RAM so
 * that D3D8 code linked into the title can poke it without faulting. */
#define XBOX_NV2A_BASE 0xFD000000u
#define XBOX_NV2A_SIZE (16u * 1024u * 1024u)
static void *g_nv2a_memory = NULL;

/* MCPX southbridge register span: APU 0xFE800000 through NIC 0xFEF00000. */
#define XBOX_MCPX_BASE 0xFE800000u
#define XBOX_MCPX_SIZE (8u * 1024u * 1024u)
/* The AC97 page, offset from the MCPX base. Same constant as apu.h; the
 * kernel does not link the APU library, so it is repeated rather than
 * shared. */
#define XBOX_MCPX_AC97_PAGE 0x00400000u
/* The CRTC page, offset from the NV2A base. Same constant as apu.h. */
#define XBOX_NV2A_PCRTC_PAGE 0x00600000u
static void *g_mcpx_memory = NULL;

/* Flash ROM. The console's 256 KB flash is mirrored through the top of the
 * address space, and the MCPX span above stops one page short of it -- so a
 * title that touches it faulted on an address that is perfectly ordinary on
 * hardware.
 *
 * The Xbox Dashboard does, from two directions at once: its XIP workers hash
 * 64 KB from 0xFF000000 (it verifies archives against digests), and its render
 * path writes to 0xFF000040. Both are hard faults today, and they kill the
 * process a few dozen lines after its first frame clears.
 *
 * Plain memory, like the other two apertures, and mapped for the same stated
 * reason: a read of zero is survivable, a fault is not. Zeros are not the
 * console's BIOS, so a digest taken over this will not match one taken over
 * real flash -- that is a separate question from whether the access should
 * fault, and this is the half that has an obviously right answer. */
#define XBOX_FLASH_BASE 0xFF000000u
#define XBOX_FLASH_SIZE (1u * 1024u * 1024u)
static void *g_flash_memory = NULL;
/* The contiguous window's backing section. It is a file mapping rather than
 * plain committed memory for one reason: the tiled aperture has to be a
 * second view of the very same bytes, and only a mapping can be mapped
 * twice. See the tiled aperture below for why that matters.
 */
static HANDLE g_contig_mapping = NULL;
/* How much of the tiled aperture can exist.
 *
 * Two ceilings, both below the mapped RAM size once that is large:
 *
 *   - it starts at 0xF0000000 in a 32-bit guest address space, so it can
 *     never reach past 0x100000000; and
 *   - the NV2A register aperture sits at 0xFD000000, which is where the
 *     window really ends on hardware.
 *
 * Asking for the full RAM size overlapped both and MapViewOfFileEx failed
 * with ERROR_INVALID_ADDRESS -- a warning at startup and then a fault on the
 * title's first surface write, with nothing connecting the two. */
/* How much guest address space is mapped.
 *
 * For anything that dereferences an address it read out of guest memory --
 * a descriptor pointer a device model follows, say. Those are attacker-ish
 * input in the only sense that matters here: the title can leave one
 * uninitialised, and 0xCCCCCCCC dereferenced is a crash in the runtime
 * rather than a fault the title would have taken. */
size_t xbox_GetMappedSize(void)
{
    return g_memory_size;
}

static size_t xbox_TiledApertureSize(void)
{
    uint64_t end = XBOX_NV2A_BASE < 0x100000000ULL
                 ? XBOX_NV2A_BASE : 0x100000000ULL;
    size_t max = (size_t)(end - XBOX_TILED_BASE);
    return g_memory_size < max ? g_memory_size : max;
}

static HANDLE g_nv2a_ack_thread = NULL;
static volatile LONG g_nv2a_ack_stop = 0;

/*
 * NV2A busy-bit acknowledgement.
 *
 * D3D8 talks to the GPU through set-a-bit / wait-for-hardware-to-clear-it
 * handshakes. Against plain RAM the bit is set and nothing ever clears it, so
 * the title spins forever. Halo hangs in the push-buffer kick at 0x001EF930:
 *
 *     mov  [eax+0x100410], edx     ; set 0x10000
 *   L: test [eax+0x100410], 0x10000
 *     jne  L                       ; wait for the GPU
 *
 * Clearing those bits from a thread is not a hack around the handshake, it is
 * the handshake: on hardware the GPU clears them asynchronously, which is
 * exactly what this does. Work that would have been submitted is being done by
 * the D3D11 layer instead, so acknowledging immediately is honest.
 *
 * Only registers listed here are touched. Blanket-zeroing the aperture would
 * also wipe registers holding real state.
 *
 * ponytail: table-driven, extend as more handshakes turn up. A spin on a bit
 * that is not listed still hangs -- run the title and the watchdog sample will
 * name the register.
 */
/* The interrupt-status registers, as opposed to the busy bits that share the
 * same table. Only these are the title's to clear. */
static int intr_status_reg(uint32_t offset)
{
    return offset == 0x000100u      /* PMC_INTR_0   */
        || offset == 0x600100u;     /* PCRTC_INTR_0 */
}

/* Set when the vblank tick is delivering interrupts, so the acknowledgement
 * thread knows a real ISR is servicing them. */
/* Written by the timer thread, read by the acknowledgement thread, so it is
 * shared state and gets the same interlocked treatment as g_nv2a_ack_stop. */
static volatile LONG s_vblank_owns_intr = 0;

void xbox_NV2A_VblankOwnsInterrupts(int owns)
{
    InterlockedExchange(&s_vblank_owns_intr, owns ? 1 : 0);
}

static const struct { uint32_t offset; uint32_t busy_mask; } NV2A_ACK[] = {
    { 0x100410, 0x00010000u },  /* PFB flush kick, Halo 0x001EF930 */

    /* Interrupt status registers. These are write-1-to-clear on hardware, so
     * an ISR "clearing" one writes the pending bit back -- against plain RAM
     * that sets it instead, the interrupt stays pending forever, and the
     * service routine re-enters until the stack is gone. Halo dies exactly
     * that way: CMiniport::ServiceGrInterrupt writes 0x1000 to PGRAPH_INTR to
     * acknowledge, reads it back still pending, and recurses into a native
     * stack overflow.
     *
     * Holding them at zero is correct rather than convenient: nothing here
     * ever raises a GPU interrupt, so "none pending" is the truth. */
    { 0x000100, 0xFFFFFFFFu },  /* PMC_INTR_0    */
    { 0x001100, 0xFFFFFFFFu },  /* PBUS_INTR_0   */
    { 0x002100, 0xFFFFFFFFu },  /* PFIFO_INTR_0  */
    { 0x400100, 0xFFFFFFFFu },  /* PGRAPH_INTR   */
    { 0x600100, 0xFFFFFFFFu },  /* PCRTC_INTR_0  */
};

/*
 * Bits that must always read as SET. The mirror image of the table above:
 * where an interrupt-pending bit is false because nothing raises interrupts,
 * a queue-empty bit is true because nothing is queued.
 *
 * Halo's CMiniport::TilingUpdateIdle spins until the PFIFO caches report
 * empty (0x001F5CD1). Zeroed RAM says "not empty" forever, so tile setup
 * during CDevice::InitializeFrameBuffers never completes.
 *
 * Note 0x003220 is deliberately absent -- that one exits on the bit being
 * CLEAR, which zeroed memory already gives.
 */
static const struct { uint32_t offset; uint32_t idle_mask; } NV2A_IDLE[] = {
    { 0x002400, 0x00000010u },  /* PFIFO_RUNOUT_STATUS  LOW_MARK (empty) */
    { 0x003214, 0x00000010u },  /* PFIFO_CACHE1_STATUS  LOW_MARK (empty) */
};

/*
 * PFIFO channel DMA pointers. Software writes DMA_PUT and spins until the GPU
 * advances DMA_GET to match -- "you have consumed everything I submitted".
 * Halo's wait is at 0x001F3948:
 *
 *   L: call BusyLoop
 *      ecx = [[dev+0x2304] + 0x44]   ; DMA_GET
 *      edx = [dev]                   ; DMA_PUT
 *      test (edx ^ ecx), 0xfffffff
 *      jne L
 *
 * [dev+0x2304] is 0xFD800000, so the channel's USER area sits at aperture
 * offset 0x800000 and the two pointers are at +0x40 / +0x44. Copying PUT to
 * GET is the acknowledgement; the commands are not executed from the push
 * buffer here -- the D3D11 layer draws -- so reporting them consumed is the
 * truthful answer.
 *
 * This was written once, removed, and restored. It was removed because
 * [dev+0x2304] read as 0x0080F7FF, i.e. no register to acknowledge -- but that
 * garbage was a downstream symptom of ordinal 47 having no stdcall arg size,
 * which walked esp 8 bytes off and made D3D initialise the DMA channel with
 * `this` = 1. With that fixed the pointer is correct and so is this.
 */
#define NV2A_USER_DMA_PUT 0x800040u
#define NV2A_USER_DMA_GET 0x800044u

/*
 * Free-running counters in the MCPX aperture.
 *
 * Some hardware registers are clocks, not flags: software reads them and waits
 * until the value passes a target. Against zeroed RAM the value never moves and
 * the wait is forever. DirectSound's CMcpxCore::SetupVoiceProcessor spins on
 * the APU sample counter at 0xFE820010 exactly this way, which is where Halo
 * stopped once input initialisation started working.
 *
 * Ticking it is the honest model: on hardware this counter advances on its own
 * whether or not anything is listening.
 *
 * ponytail: the rate is "as fast as this thread loops", not 48 kHz. Nothing
 * paces audio off it yet. Derive it from a real clock if timing starts to
 * matter.
 */
static const uint32_t MCPX_COUNTERS[] = {
    0x020010,   /* APU GP sample counter, DirectSound SetupVoiceProcessor */
};

static void *g_mcpx_regs = NULL;
/* Set when the APU's registers are unmapped so they can be routed to the
 * emulated APU. Once that happens they are no longer plain memory, and the
 * counter ticking below must leave them alone -- writing through the pointer
 * faults, and the emulated APU owns those registers anyway. */
static int g_apu_mmio_trapped = 0;

/*
 * GPU completion fences the title waits on in guest memory rather than in the
 * aperture. See xbox_Nv2aMirrorFence in the header for why this is the same
 * acknowledgement the NV2A_ACK table makes, and why the address has to be
 * followed through the device struct instead of being a constant.
 */
#define XBOX_MAX_FENCE_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t put_off;
    uint32_t get_ptr_off;
} g_fence_mirrors[XBOX_MAX_FENCE_MIRRORS];
static int g_fence_mirror_count = 0;

int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                         uint32_t put_off, uint32_t get_ptr_off)
{
    if (g_fence_mirror_count >= XBOX_MAX_FENCE_MIRRORS)
        return -1;
    g_fence_mirrors[g_fence_mirror_count].device_ptr_va = device_ptr_va;
    g_fence_mirrors[g_fence_mirror_count].put_off = put_off;
    g_fence_mirrors[g_fence_mirror_count].get_ptr_off = get_ptr_off;
    g_fence_mirror_count++;
    fprintf(stderr, "  NV2A fence mirror: device at 0x%08X,"
            " PUT +0x%X -> *(GET +0x%X)\n",
            device_ptr_va, put_off, get_ptr_off);
    return 0;
}
/* A guest address is usable only once the window is mapped and it lands
 * inside it; the chain is followed fresh every poll because the title may not
 * have built it yet. */
static int fence_readable(uint32_t va, uint32_t bytes)
{
    /* Page zero is unmapped, so the bound is the first mapped page rather than
     * just "not null": the device pointer is zero until the title creates the
     * device, and this thread polls from before that. Rejecting only 0 let
     * dev + get_ptr_off through as 0x34 and faulted on the very first tick. */
    if (g_memory_base == NULL || va < XBOX_FS_BASE)
        return 0;
    /* The contiguous window is mapped separately and sits far above the main
     * range, so a size check against g_memory_size rejects it. The fence a
     * title waits on is exactly the kind of block that lives there --
     * MmAllocateContiguousMemory is where a GPU-written semaphore comes
     * from -- so a chain ending in that window has to be followed, not
     * discarded. */
    if (va >= XBOX_CONTIG_BASE
            && (uint64_t)va + bytes <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        return g_contig_memory != NULL;
    return (size_t)va + bytes <= g_memory_size;
}

/*
 * Two counters inside the device, one of which the GPU owns.
 *
 * D3D's swap throttle is a pair: the title bumps "frames submitted" itself and
 * waits for "frames completed", which on hardware only the GPU moves. The Xbox
 * dashboard's is exactly that, at guest 0x000AF121 --
 *
 *     eax = [esi+0x2518]        ; completed
 *     ecx = [esi+0x2B60]        ; submitted
 *     ecx = ecx - eax
 *     if (ecx < 2) proceed      ; else spin on a 400-iteration delay loop
 *
 * -- and with nothing moving completed it spins there forever once two frames
 * are outstanding. That delay loop was 99.8 million of the dashboard's calls,
 * against 35 thousand for the next function down.
 *
 * This differs from xbox_Nv2aFrameCounter, which advances a counter on a 60 Hz
 * clock, in the way that matters for this pair: a free-running counter can
 * pass submitted, and then submitted - completed underflows to about four
 * billion, which is >= 2, and the spin never ends again. Mirroring cannot do
 * that, because completed is only ever whatever submitted already is.
 *
 * It is also simply true here. The pushbuffer is executed at submit, so by the
 * time the title asks whether the frame is finished, it is.
 */
#define XBOX_MAX_COUNTER_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t src_off, dst_off;
} g_counter_mirrors[XBOX_MAX_COUNTER_MIRRORS];
static int g_counter_mirror_count = 0;

int xbox_Nv2aMirrorCounter(uint32_t device_ptr_va,
                           uint32_t src_off, uint32_t dst_off)
{
    if (g_counter_mirror_count >= XBOX_MAX_COUNTER_MIRRORS)
        return -1;
    g_counter_mirrors[g_counter_mirror_count].device_ptr_va = device_ptr_va;
    g_counter_mirrors[g_counter_mirror_count].src_off = src_off;
    g_counter_mirrors[g_counter_mirror_count].dst_off = dst_off;
    g_counter_mirror_count++;
    fprintf(stderr, "  NV2A counter mirror: device at 0x%08X,"
            " +0x%X -> +0x%X\n", device_ptr_va, src_off, dst_off);
    return 0;
}

/* Bumped by the ack thread's pass whenever it changed a guest-visible word,
 * so the thread can tell a quiet spell from a busy one (nv2a_ack_wait). */
static LONG g_ack_activity;

static void counter_mirrors_tick(void)
{
    for (int i = 0; i < g_counter_mirror_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_counter_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_counter_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_counter_mirrors[i].src_off, 4)
                || !fence_readable(dev + g_counter_mirrors[i].dst_off, 4))
            continue;
        {
            volatile uint32_t *dst =
                (volatile uint32_t *)((uintptr_t)(dev + g_counter_mirrors[i].dst_off)
                                      + g_memory_offset);
            uint32_t src =
                *(volatile uint32_t *)((uintptr_t)(dev + g_counter_mirrors[i].src_off)
                                       + g_memory_offset);
            if (*dst != src) {
                *dst = src;
                g_ack_activity++;
            }
        }
    }
}

/*
 * Frame counters the title polls to pace itself.
 *
 * D3D keeps a swap count inside the device and bumps it once per presented
 * frame; a title that wants to wait a frame reads it and spins until it moves.
 * Wreckless does exactly that at guest 0x000DC5E0 -- "loop while the counter
 * has advanced by less than 2" -- so a counter that never moves is not a
 * dropped frame, it is a hang with a full asset load behind it.
 *
 * Nothing here presents, so nothing would ever move it. Advancing it on a
 * clock is what makes the wait terminate, and 60 Hz is the rate the title
 * expects the display to run at. Followed through the device pointer for the
 * same reason the fence is: the device is allocated at runtime.
 */
#define XBOX_MAX_FRAME_COUNTERS 4
#define XBOX_FRAME_PERIOD_MS    16      /* ~60 Hz */

static struct {
    uint32_t device_ptr_va;
    uint32_t counter_off;
} g_frame_counters[XBOX_MAX_FRAME_COUNTERS];
static int   g_frame_counter_count = 0;
static DWORD g_frame_counter_last_ms = 0;

int xbox_Nv2aFrameCounter(uint32_t device_ptr_va, uint32_t counter_off)
{
    if (g_frame_counter_count >= XBOX_MAX_FRAME_COUNTERS)
        return -1;
    g_frame_counters[g_frame_counter_count].device_ptr_va = device_ptr_va;
    g_frame_counters[g_frame_counter_count].counter_off   = counter_off;
    g_frame_counter_count++;
    fprintf(stderr, "  Frame counter: device at 0x%08X, count +0x%X @ %d Hz\n",
            device_ptr_va, counter_off, 1000 / XBOX_FRAME_PERIOD_MS);
    return 0;
}

/* A real swap happened: advance every registered counter, and remember when.
 *
 * The timer below exists for a title nothing presents for. Once the
 * pushbuffer executor is actually running flips, the timer is the wrong
 * clock and an actively harmful one: Half-Life 2's loader paces its intro on
 * this count, so a 62 Hz timer against an executor managing a fraction of a
 * frame per second ran the video forward in virtual time far faster than it
 * could be drawn. Only every few hundredth frame was ever presented, each one
 * sampled part way through its own decode -- which looks exactly like a
 * stalling, blocky video rather than a clock running away.
 */
static DWORD g_frame_counter_flip_ms;

void xbox_Nv2aFrameCounterFlip(void)
{
    int i;

    g_frame_counter_flip_ms = GetTickCount();
    if (!g_frame_counter_flip_ms)
        g_frame_counter_flip_ms = 1;          /* 0 means "never" */
    for (i = 0; i < g_frame_counter_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_frame_counters[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_frame_counters[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_frame_counters[i].counter_off, 4))
            continue;
        *(volatile uint32_t *)((uintptr_t)(dev + g_frame_counters[i].counter_off)
                               + g_memory_offset) += 1;
    }
}

static void frame_counters_tick(void)
{
    DWORD now = GetTickCount();
    int i;

    if (!g_frame_counter_count)
        return;
    if (g_frame_counter_last_ms
            && (now - g_frame_counter_last_ms) < XBOX_FRAME_PERIOD_MS)
        return;
    /* Something is presenting: let it drive the count instead. Two seconds,
     * because the executor's flips are not evenly spaced and a title that
     * genuinely stops presenting still has to be got moving again. */
    if (g_frame_counter_flip_ms && (now - g_frame_counter_flip_ms) < 2000) {
        g_frame_counter_last_ms = now;
        return;
    }
    g_frame_counter_last_ms = now;

    for (i = 0; i < g_frame_counter_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_frame_counters[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_frame_counters[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_frame_counters[i].counter_off, 4))
            continue;
        *(volatile uint32_t *)((uintptr_t)(dev + g_frame_counters[i].counter_off)
                               + g_memory_offset) += 1;
    }
}

/* The flip gate (xbox_memory_layout.h).
 *
 * The HLE Swap calls xbox_Nv2aFlipGateArm() before it runs the title's own
 * Swap, and that call sleeps until the kernel's vblank tick releases it. So
 * there is one Swap per vblank, the guest thread sleeps for the rest of the
 * frame instead of spinning, and the title's own fence wait is not involved.
 *
 * It is not involved on purpose. The first version held the fence mirror
 * instead, so that the title's wait inside Swap would block; but a title
 * waits for the *previous* frame's fence there (its flips are double
 * buffered), which the mirror had already completed at the last vblank, and
 * two Swaps got through per vblank: TimeSplitters 2's menus measured 120-140
 * fps with a 60 Hz vblank.
 *
 * The gate only holds once a vblank has ever been delivered -- without
 * RECOMP_VBLANK nothing would release it -- and never for more than a quarter
 * of a second, so a vblank thread that stops cannot hang the title; the first
 * timeout is logged, because it means pacing is not happening. A vblank that
 * arrived while the title was still drawing does not count: the wait is for
 * the next one, or the frame after it could present again in the same period.
 *
 * Adaptive by default: a Swap that arrives after the vblank it should have
 * waited for presents at once, and only a frame that finished inside the
 * period is held for the next vblank. RECOMP_FPS_CAP=<fps> is the strict
 * console cadence instead -- every Swap waits for the next release, and the
 * release comes every (vblank rate / fps)th vblank; 30 with a 60 Hz vblank
 * is what a title sees on hardware when it misses every other frame, fine
 * for a fixed-30 title and half speed for one that steps its logic per
 * presented frame. RECOMP_FPS_CAP=0 switches the gate off, for measuring.
 *
 * Why adaptive won (TimeSplitters 2's Siberia, 19 Sep 2026, quiet machine,
 * three scripted runs of each, docs/technical/resolution-and-framerate.md):
 * uncapped the level ran 79-89 fps with a 12.4 ms frame, so the title was
 * not paced at all; adaptive held 59.9-60.2 in every five-second window with
 * 3.7 ms of gate wait per frame; strict 60 held 60 most of the time but
 * dipped to 55-58 and once to 45 at the same points in all three runs,
 * because a frame that just misses its vblank waits out a whole extra one.
 * Burnout 2's front end, 1 ms of work a frame, holds 60.0 under either. */
static HANDLE        g_flip_gate_event;
static volatile LONG g_flip_gate_vblanks;
static int           g_flip_gate_divisor = -1;      /* -1: not configured */
static int           g_flip_gate_strict;            /* RECOMP_FPS_CAP given */

/* What the on-screen toggle cycles through: the settings a player picks
 * between, in that order. A RECOMP_FPS_CAP outside this list still works --
 * it is simply not one of the stops, and the first press moves to the
 * first one. */
static const struct { int divisor, strict; const char *name; } g_gate_modes[] = {
    { 1, 0, "adaptive" },
    { 1, 1, "60" },
    { 2, 1, "30" },
    { 0, 0, "off" },
};

/* A switch that is on unless it is turned off, or off unless turned on.
 *
 * The things a title needs in order to run at all -- the vblank, the audio
 * codec's ready bit, the replacement renderer -- began as experiments, and an
 * experiment is off until asked for. They are not experiments any more: a
 * player double-clicking the executable should get the game, not a black
 * window, so they default on and the variable turns them off. "0", "off",
 * "no" and "false" mean off; anything else, including an empty value, means
 * on. */
/* ── The guest lock ─────────────────────────────────────────────────────
 * See xbox_memory_layout.h for what it is for. A CRITICAL_SECTION because it
 * is recursive and uncontended acquisition is cheap, which matters: this is
 * taken and dropped once per kernel call, and Dino Crisis 3 makes over a
 * million of those a minute. */
static CRITICAL_SECTION g_guest_cs;
static int g_guest_cs_ready;
static int g_guest_lock_on = -1;
static RECOMP_TLS int g_guest_depth;

/* How often two guest threads are inside lifted code at the same time.
 *
 * The threading hypothesis rests on that happening at all, and waiting for
 * the resulting corruption to show is a poor way to find out: Dino Crisis 3
 * faults on roughly one run in twenty, so an A/B of the guest lock over eight
 * runs a side produced zero events on both sides and settled nothing.
 *
 * This measures the hazard instead of the damage, and it is deterministic.
 * A guest thread is "in lifted code" whenever it is not inside a kernel
 * bridge, which the existing drop/restore around bridge() already brackets.
 * If the peak is 1, guest threads never overlap and threading cannot be the
 * cause whatever else is wrong; if it is above 1, the uniprocessor
 * assumption is being violated continuously and that is worth fixing on its
 * own terms, crash or no crash.
 *
 * RECOMP_GUEST_CONCURRENCY=1. Independent of the lock, so the hazard can be
 * measured with the lock off, which is the configuration that ships.
 */
static volatile LONG g_lifted_now;
static volatile LONG g_lifted_peak;
static volatile LONG g_lifted_overlaps;
static int g_concurrency_on = -1;

int xbox_GuestConcurrencyOn(void)
{
    if (g_concurrency_on < 0)
        g_concurrency_on = xbox_EnvSwitch("RECOMP_GUEST_CONCURRENCY", 0);
    return g_concurrency_on;
}

/* One guest thread at DISPATCH_LEVEL stops the others.
 *
 * On the Xbox's single CPU a thread at DISPATCH_LEVEL is the only thing
 * running until it lowers, and titles build on that: Marvel vs Capcom 2's
 * ADX sound thread runs its server at DISPATCH_LEVEL, guarded only by a busy
 * flag, and its main thread edits the same stream buffers at PASSIVE_LEVEL.
 * Here the two run on two host cores, and a watchpoint saw both write one
 * ring buffer's positions. No failure has been traced to that yet.
 *
 * The parking points are the bridge boundaries every guest thread already
 * passes (the Lifted enter/leave pair): g_park_running counts guest threads
 * in lifted code. A thread raising to DISPATCH_LEVEL (xbox_DispatchParkOthers,
 * from kernel_hal.c) becomes the owner and waits for that count to drain;
 * any other thread coming back from a bridge waits while an owner exists.
 *
 * Both waits are bounded. A thread spinning in lifted code never reaches a
 * boundary, and one raised thread blocking on another would be a deadlock;
 * past the bound each side proceeds and the first time says so. Off by
 * default; RECOMP_DISPATCH_LOCK=1, the same switch as the DPC hold-off. */
static volatile LONG  g_park_running;
static volatile DWORD g_park_owner;
static HANDLE         g_park_free;     /* manual reset, set when no owner */
static int            g_park_on = -1;

static int park_on(void)
{
    if (g_park_on < 0) {
        HANDLE ev = CreateEventW(NULL, TRUE, TRUE, NULL);
        if (InterlockedCompareExchangePointer((PVOID *)&g_park_free, ev, NULL))
            CloseHandle(ev);
        g_park_on = xbox_EnvSwitch("RECOMP_DISPATCH_LOCK", 0);
    }
    return g_park_on;
}

void xbox_DispatchParkOthers(void)
{
    ULONGLONG deadline;
    DWORD me = GetCurrentThreadId();

    if (!park_on())
        return;
    g_park_owner = me;
    ResetEvent(g_park_free);
    /* This thread is inside a bridge, so it is not counted itself. */
    if (g_park_running <= 0)
        return;
    deadline = GetTickCount64() + 50;
    while (g_park_running > 0) {
        if (GetTickCount64() >= deadline) {
            static volatile LONG said;
            if (InterlockedIncrement(&said) <= 3) {
                fprintf(stderr, "  [KERNEL] DISPATCH_LEVEL: %ld other guest "
                                "thread(s) still in lifted code after 50 ms; "
                                "going on without them (RECOMP_DISPATCH_LOCK)\n",
                        (long)g_park_running);
                fflush(stderr);
            }
            return;
        }
        SwitchToThread();
    }
}

void xbox_DispatchReleaseOthers(void)
{
    if (!park_on())
        return;
    g_park_owner = 0;
    SetEvent(g_park_free);
}

/* A guest thread leaving lifted code for good (its thread function returned). */
void xbox_GuestLiftedExit(void)
{
    if (park_on())
        InterlockedDecrement(&g_park_running);
}

static void park_enter(void)
{
    DWORD me;
    ULONGLONG deadline = 0;

    if (!park_on())
        return;
    me = GetCurrentThreadId();
    for (;;) {
        DWORD owner;
        InterlockedIncrement(&g_park_running);
        owner = g_park_owner;
        if (!owner || owner == me)
            return;
        InterlockedDecrement(&g_park_running);
        if (!deadline)
            deadline = GetTickCount64() + 100;
        else if (GetTickCount64() >= deadline) {
            static volatile LONG said;
            if (InterlockedIncrement(&said) <= 3) {
                fprintf(stderr, "  [KERNEL] DISPATCH_LEVEL held 100 ms by "
                                "thread %lu; this thread goes on "
                                "(RECOMP_DISPATCH_LOCK)\n", (unsigned long)owner);
                fflush(stderr);
            }
            InterlockedIncrement(&g_park_running);
            return;
        }
        WaitForSingleObject(g_park_free, 10);
    }
}

/* RECOMP_GUEST_ONE_CPU: every guest thread on one host core, as on the Xbox.
 *
 * Guest code is written for a single CPU. It finishes a job and publishes
 * the semaphore for it in two steps, or checks a count and then sleeps on
 * it, trusting that nothing runs in between unless it blocks or its quantum
 * ends. Run on several host cores at once, other guest threads land in those
 * gaps all the time: Outrun 2 hung on its AM2 logo in 4 of 4 runs, a task
 * finishing between "is it still running?" and "here is your semaphore".
 * Host threads (the renderer, audio, the window) are left free.
 *
 * 1 picks a core for the guest: the highest-numbered core of the fastest
 * class the process may run on. A number above 1 is taken as a core index
 * + 1 (2 = core 1, ...). Off when unset or 0.
 *
 * "The fastest class" matters on a hybrid CPU. Windows numbers the
 * efficiency cores last, so "the highest core" on an i7-12700H was CPU 19,
 * an E-core, and the whole title -- lifted code, HLE, renderer submission --
 * ran on it: Outrun 2's race measured 12-15 fps there and 35-40 fps on a
 * P-core, the same build, the same drive (1 Oct 2026). CPU sets report each
 * logical processor's EfficiencyClass (higher is faster); the pick is the
 * highest-numbered processor of the highest class, which also keeps the
 * guest off CPU 0. */
typedef BOOL (WINAPI *GetSystemCpuSetInformation_t)(PVOID, ULONG, PULONG, HANDLE, ULONG);

static DWORD_PTR fastest_core_mask(DWORD_PTR proc)
{
    GetSystemCpuSetInformation_t get_sets =
        (GetSystemCpuSetInformation_t)(void *)GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "GetSystemCpuSetInformation");
    ULONG len = 0;
    BYTE *buf, *p;
    DWORD_PTR best = 0;
    int best_class = -1;

    if (!get_sets)
        return 0;
    get_sets(NULL, 0, &len, GetCurrentProcess(), 0);
    if (!len || !(buf = (BYTE *)malloc(len)))
        return 0;
    if (!get_sets(buf, len, &len, GetCurrentProcess(), 0)) {
        free(buf);
        return 0;
    }
    /* SYSTEM_CPU_SET_INFORMATION: Size at +0, Type at +4 (0 = CpuSet), then
     * the CpuSet record at +8: Id (ULONG), Group (USHORT, +12),
     * LogicalProcessorIndex (BYTE, +14), CoreIndex (+15),
     * LastLevelCacheIndex (+16), NumaNodeIndex (+17), EfficiencyClass (+18).
     * Spelt out so this builds against older SDK headers. */
    for (p = buf; p + 24 <= buf + len; p += *(ULONG *)p) {
        ULONG size = *(ULONG *)p;
        BYTE lp = p[14], cls = p[18];

        if (size < 24)
            break;
        if (*(ULONG *)(p + 4) != 0 || lp >= sizeof(DWORD_PTR) * 8)
            continue;
        if (!(proc & ((DWORD_PTR)1 << lp)))
            continue;
        if ((int)cls > best_class || ((int)cls == best_class && lp > 0)) {
            if ((int)cls > best_class)
                best = 0;
            best_class = cls;
            best = (DWORD_PTR)1 << lp;      /* highest index wins: ascending order */
        }
    }
    free(buf);
    return best;
}

static DWORD_PTR guest_cpu_mask(void)
{
    static DWORD_PTR mask = (DWORD_PTR)-1;
    if (mask == (DWORD_PTR)-1) {
        const char *v = getenv("RECOMP_GUEST_ONE_CPU");
        long n = v ? strtol(v, NULL, 0) : 0;
        DWORD_PTR proc = 0, sys = 0;
        mask = 0;
        if (n > 0 && GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc) {
            if (n == 1) {
                mask = fastest_core_mask(proc);
                if (!mask) {
                    /* No CPU-set information: the highest core allowed. */
                    for (mask = (DWORD_PTR)1 << (sizeof mask * 8 - 1); !(mask & proc); mask >>= 1)
                        ;
                }
            } else {
                mask = ((DWORD_PTR)1 << (n - 2)) & proc;
            }
            fprintf(stderr, "[THREAD] guest threads on one host core (mask 0x%llX, "
                    "RECOMP_GUEST_ONE_CPU)\n", (unsigned long long)mask);
        }
    }
    return mask;
}

void xbox_GuestLiftedEnter(void)
{
    LONG n;
    {
        static RECOMP_TLS int pinned;
        if (!pinned) {
            DWORD_PTR m = guest_cpu_mask();
            pinned = 1;
            if (m)
                SetThreadAffinityMask(GetCurrentThread(), m);
        }
    }
    park_enter();
    if (!xbox_GuestConcurrencyOn())
        return;
    n = InterlockedIncrement(&g_lifted_now);
    if (n > 1)
        InterlockedIncrement(&g_lifted_overlaps);
    for (;;) {
        LONG peak = g_lifted_peak;
        if (n <= peak ||
            InterlockedCompareExchange(&g_lifted_peak, n, peak) == peak)
            break;
    }
}

void xbox_GuestLiftedLeave(void)
{
    if (park_on())
        InterlockedDecrement(&g_park_running);
    if (!xbox_GuestConcurrencyOn())
        return;
    InterlockedDecrement(&g_lifted_now);
}

void xbox_GuestConcurrencyReport(void)
{
    if (!xbox_GuestConcurrencyOn())
        return;
    fprintf(stderr, "  [CONCURRENCY] peak guest threads in lifted code: %ld; "
                    "entries that found another already there: %ld\n",
            (long)g_lifted_peak, (long)g_lifted_overlaps);
    fflush(stderr);
}

int xbox_GuestLockOn(void)
{
    if (g_guest_lock_on < 0)
        g_guest_lock_on = xbox_EnvSwitch("RECOMP_GUEST_LOCK", 0);
    return g_guest_lock_on;
}

void xbox_GuestLockInit(void)
{
    if (g_guest_cs_ready)
        return;
    InitializeCriticalSection(&g_guest_cs);
    g_guest_cs_ready = 1;
    if (xbox_GuestLockOn()) {
        fprintf(stderr, "  Guest lock: on -- one guest thread runs lifted "
                        "code at a time (RECOMP_GUEST_LOCK=0 to switch off)\n");
        fflush(stderr);
    }
}

void xbox_GuestLockEnter(void)
{
    if (!xbox_GuestLockOn() || !g_guest_cs_ready)
        return;
    EnterCriticalSection(&g_guest_cs);
    g_guest_depth++;
}

void xbox_GuestLockLeave(void)
{
    if (!xbox_GuestLockOn() || !g_guest_cs_ready || g_guest_depth <= 0)
        return;
    g_guest_depth--;
    LeaveCriticalSection(&g_guest_cs);
}

/* Release every level this thread holds, so it cannot block while holding
 * the lock, and report how many to take back afterwards. */
int xbox_GuestLockDrop(void)
{
    int held = 0;
    if (!xbox_GuestLockOn() || !g_guest_cs_ready)
        return 0;
    while (g_guest_depth > 0) {
        g_guest_depth--;
        LeaveCriticalSection(&g_guest_cs);
        held++;
    }
    return held;
}

void xbox_GuestLockRestore(int held)
{
    if (!xbox_GuestLockOn() || !g_guest_cs_ready)
        return;
    while (held-- > 0) {
        EnterCriticalSection(&g_guest_cs);
        g_guest_depth++;
    }
}

int xbox_EnvSwitch(const char *name, int default_on)
{
    const char *v = name ? getenv(name) : NULL;

    if (!v)
        return default_on;
    if (!*v)
        return 1;
    return !(strcmp(v, "0") == 0 || _stricmp(v, "off") == 0 ||
             _stricmp(v, "no") == 0 || _stricmp(v, "false") == 0);
}

static int flip_gate_divisor(void)
{
    if (g_flip_gate_divisor < 0) {
        const char *cap = recomp_config_lookup("RECOMP_FPS_CAP", "frame_cap");
        const char *hz = getenv("RECOMP_VBLANK_HZ");
        int vblank = hz && atoi(hz) > 0 ? atoi(hz) : 60;
        int d = 1;                                      /* adaptive unless asked */

        if (cap && *cap) {
            int fps = atoi(cap);
            if (strcmp(cap, "adaptive") == 0)
                d = 1;
            else if (strcmp(cap, "0") == 0 || strcmp(cap, "off") == 0)
                d = 0;
            else if (fps > 0) {
                d = (vblank + fps / 2) / fps;
                if (d < 1) d = 1;
                g_flip_gate_strict = 1;
            }
        }
        g_flip_gate_divisor = d;
        fprintf(stderr, "  [NV2A] flip gate: %s\n",
                d == 0 ? "off, Swap never waits (RECOMP_FPS_CAP=adaptive or 60 to pace)"
                       : !g_flip_gate_strict ? "adaptive, at most one Swap per vblank (RECOMP_FPS_CAP=0 to switch off)"
                       : d == 1 ? "strict, one Swap per vblank" : "strict, one Swap per N vblanks");
        if (d > 1)
            fprintf(stderr, "  [NV2A] flip gate divisor %d (RECOMP_FPS_CAP=%s at %d Hz)\n",
                    d, cap, vblank);
        fflush(stderr);
    }
    return g_flip_gate_divisor;
}

/* Where the current setting sits in that list, or -1 for one that is not in
 * it. */
static int flip_gate_mode_index(void)
{
    int d = flip_gate_divisor(), i;

    for (i = 0; i < (int)(sizeof g_gate_modes / sizeof g_gate_modes[0]); i++)
        if (g_gate_modes[i].divisor == d &&
            (d == 0 || g_gate_modes[i].strict == g_flip_gate_strict))
            return i;
    return -1;
}

static int flip_gate_held_for_hook(int d);

const char *xbox_Nv2aFlipGateModeName(void)
{
    int i = flip_gate_mode_index();
    if (flip_gate_held_for_hook(flip_gate_divisor()))
        return "60 (for interpolation)";
    return i < 0 ? "custom" : g_gate_modes[i].name;
}

int xbox_Nv2aFlipGateModeIndex(void)
{
    return flip_gate_mode_index();
}

void xbox_Nv2aFlipGateSetMode(int i)
{
    if (i < 0 || i >= (int)(sizeof g_gate_modes / sizeof g_gate_modes[0]))
        return;
    g_flip_gate_divisor = g_gate_modes[i].divisor;
    g_flip_gate_strict = g_gate_modes[i].strict;
    fprintf(stderr, "  [NV2A] flip gate: %s\n", g_gate_modes[i].name);
    fflush(stderr);
}

void xbox_Nv2aFlipGateCycle(void)
{
    int i = flip_gate_mode_index();

    i = i < 0 ? 0 : (i + 1) % (int)(sizeof g_gate_modes / sizeof g_gate_modes[0]);
    g_flip_gate_divisor = g_gate_modes[i].divisor;
    g_flip_gate_strict = g_gate_modes[i].strict;
    fprintf(stderr, "  [NV2A] flip gate: %s\n", g_gate_modes[i].name);
    fflush(stderr);
}

/* The gate's wait, lent out (xbox_Nv2aFlipGateSetIdle). The time of the
 * last vblank and the period are what the hook is told the release will be:
 * the next vblank is the last one plus a period. */
static xbox_FlipGateIdleFn g_flip_gate_idle;
static volatile LONGLONG   g_flip_gate_last_vblank;
static LONGLONG            g_flip_gate_period, g_flip_gate_qpf;

void xbox_Nv2aFlipGateSetIdle(xbox_FlipGateIdleFn fn)
{
    g_flip_gate_idle = fn;
}

/* Lend the wait to the hook until it wants nothing more or the vblank comes.
 * TRUE if the vblank came (the gate is released). */
static BOOL flip_gate_lend(void)
{
    static HANDLE timer;
    HANDLE both[2];

    if (!g_flip_gate_period)
        return FALSE;
    if (!timer) {
        /* High resolution: a slot a few milliseconds away has to be met to
         * well under a millisecond, which the default timer cannot do. */
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */,
                                       TIMER_ALL_ACCESS);
        if (!timer)
            timer = CreateWaitableTimerW(NULL, FALSE, NULL);
        if (!timer)
            return FALSE;
    }
    both[0] = g_flip_gate_event;
    both[1] = timer;
    for (;;) {
        LARGE_INTEGER now, due;
        LONGLONG want;
        DWORD r;

        if (WaitForSingleObject(g_flip_gate_event, 0) == WAIT_OBJECT_0)
            return TRUE;
        QueryPerformanceCounter(&now);
        want = g_flip_gate_idle(now.QuadPart, g_flip_gate_last_vblank + g_flip_gate_period);
        if (want <= 0)
            return FALSE;
        QueryPerformanceCounter(&now);
        if (want <= now.QuadPart)
            continue;
        /* Relative, in 100 ns units. */
        due.QuadPart = -(LONGLONG)((double)(want - now.QuadPart) * 1e7 /
                                   (double)g_flip_gate_qpf);
        if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
            return FALSE;
        r = WaitForMultipleObjects(2, both, FALSE, 250);
        if (r == WAIT_OBJECT_0) {
            CancelWaitableTimer(timer);
            return TRUE;
        }
        if (r != WAIT_OBJECT_0 + 1)
            return FALSE;                               /* the plain wait reports it */
    }
}

/* A title that waits for the vblank itself before each frame (Future
 * Perfect spins on its own counter) reaches Swap a few milliseconds after
 * the vblank it waited for, which has already signalled the gate: adaptive
 * never holds it, and the wait the hook would be lent is spent in the
 * title's own loop instead. With a hook (frame interpolation), such a title
 * is held to the next vblank under adaptive and off too -- "off" has no
 * meaning for a title that paces itself. A cap of 30 is left as it is. */
static int g_flip_gate_title_paced;

void xbox_Nv2aFlipGateSetTitlePaced(int on)
{
    g_flip_gate_title_paced = on;
}

static int flip_gate_held_for_hook(int d)
{
    return g_flip_gate_idle && g_flip_gate_title_paced && (d == 0 || !g_flip_gate_strict);
}

void xbox_Nv2aFlipGateArm(void)
{
    static int said;
    int d = flip_gate_divisor();
    int hold = flip_gate_held_for_hook(d);

    if (d == 0 && !hold)
        return;
    if (!InterlockedCompareExchange(&g_flip_gate_vblanks, 0, 0))
        return;                                         /* no vblank has ever come */
    if (!g_flip_gate_event)
        return;
    if (g_flip_gate_strict || hold)
        ResetEvent(g_flip_gate_event);                  /* the next vblank, not a past one */
    /* Only while one Swap a vblank is the cadence: the release is then
     * always the next vblank, which is what the hook is told. */
    if (g_flip_gate_idle && (d == 1 || hold) && flip_gate_lend())
        return;
    if (WaitForSingleObject(g_flip_gate_event, 250) == WAIT_TIMEOUT && !said++) {
        fprintf(stderr, "  [NV2A] flip gate timed out: no vblank for 250 ms, "
                        "the title is not being paced\n");
        fflush(stderr);
    }
}

void xbox_Nv2aFlipGateRelease(void)
{
    LONG n = InterlockedIncrement(&g_flip_gate_vblanks);
    int d = flip_gate_divisor();
    LARGE_INTEGER now;

    QueryPerformanceCounter(&now);
    if (!g_flip_gate_qpf) {
        LARGE_INTEGER f;
        const char *hz = getenv("RECOMP_VBLANK_HZ");
        double rate = hz && atof(hz) >= 1.0 ? atof(hz) : 60.0;

        QueryPerformanceFrequency(&f);
        g_flip_gate_period = (LONGLONG)((double)f.QuadPart / rate);
        g_flip_gate_qpf = f.QuadPart;
    }
    g_flip_gate_last_vblank = now.QuadPart;
    if (!g_flip_gate_event)
        g_flip_gate_event = CreateEventW(NULL, FALSE, FALSE, NULL);   /* auto-reset */
    if (g_flip_gate_event && (d <= 1 || n % d == 0))
        SetEvent(g_flip_gate_event);
}

static void fence_mirrors_tick(void)
{
    for (int i = 0; i < g_fence_mirror_count; i++) {
        uint32_t dev, get_ptr;

        if (!fence_readable(g_fence_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_fence_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_fence_mirrors[i].get_ptr_off, 4)
                || !fence_readable(dev + g_fence_mirrors[i].put_off, 4))
            continue;
        get_ptr = *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].get_ptr_off)
                                         + g_memory_offset);
        if (!fence_readable(get_ptr, 4))
            continue;
        {
            volatile uint32_t *fence =
                (volatile uint32_t *)((uintptr_t)get_ptr + g_memory_offset);
            uint32_t put =
                *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].put_off)
                                       + g_memory_offset);
            if (*fence != put) {
                *fence = put;
                g_ack_activity++;
            }
        }
    }
}

static int s_nv2a_trace = 0;

/* The display framebuffer, as reported by AvSetDisplayMode. Checksummed once a
 * second so a run can answer the only question that matters before building a
 * presenter: is the guest putting pixels anywhere at all, and do they change
 * from frame to frame. */
static uint32_t s_fb_va, s_fb_pitch, s_fb_height = 480;

void xbox_SetDisplayFramebuffer(uint32_t fb_va, uint32_t pitch)
{
    s_fb_va = fb_va;
    s_fb_pitch = pitch;
}

/* Where the title last said its framebuffer is, for anything that wants to read
 * guest pixels directly. There was only a setter, so every such reader carried
 * its own hardcoded address instead -- and a title that moves its framebuffer
 * (which is most of them, once it owns one) left that constant pointing at
 * uninitialised memory. A dump taken there is not empty, it is noise, which
 * reads as "the title drew garbage" rather than "you read the wrong page".
 *
 * This is the resolved address, not the physical one AvSetDisplayMode states:
 * the caller resolves before storing, because a physical framebuffer address
 * read directly lands in the loaded image. Getting that wrong is the same bug
 * twice over -- once in the probe below, once in a caller of this. */
uint32_t xbox_GetDisplayFramebuffer(uint32_t *pitch)
{
    if (pitch)
        *pitch = s_fb_pitch;
    return s_fb_va;
}

static void framebuffer_probe_tick(void)
{
    static DWORD last_ms;
    static uint32_t last_sum;
    DWORD now = GetTickCount();
    uint32_t sum = 0, nonzero = 0, i, n;
    const uint32_t *p;

    if (!s_nv2a_trace || !s_fb_va || !s_fb_pitch)
        return;
    if (last_ms && (now - last_ms) < 1000)
        return;
    last_ms = now;
    if ((size_t)s_fb_va + s_fb_pitch * s_fb_height > g_memory_size)
        return;
    p = (const uint32_t *)((uintptr_t)s_fb_va + g_memory_offset);
    n = (s_fb_pitch * s_fb_height) / 4;
    for (i = 0; i < n; i++) {
        sum = sum * 33u + p[i];
        if (p[i]) nonzero++;
    }
    fprintf(stderr, "  [FB] 0x%08X sum=%08X nonzero=%u/%u %s\n",
            s_fb_va, sum, nonzero, n,
            sum != last_sum ? "CHANGED" : "same");
    last_sum = sum;
    fflush(stderr);
}

/* Between passes of the ack thread.
 *
 * It used to be Sleep(0) and nothing else, which returns at once unless a
 * thread of the same priority is waiting, so the thread spun a whole host
 * core for the life of the process (OutRun 2: 172 s of CPU in a 180 s run)
 * -- on a laptop that is power and thermal headroom the guest's own core
 * needs. A guest waiting on one of these words is spinning on another core,
 * so while words keep changing the thread still goes round at full speed;
 * once nothing has changed for a millisecond it waits on a high-resolution
 * timer between passes instead (RECOMP_NV2A_ACK_IDLE_US, default 500; 0 for
 * the old spin). The cost is that the first wait after a quiet spell -- the
 * first kickoff after a vblank wait, say -- can be answered up to that much
 * later. */
static void nv2a_ack_wait(void)
{
    static int      idle_us = -1;
    static HANDLE   timer;
    static LONG     seen;
    static LONGLONG quiet_since, qpf;
    LARGE_INTEGER   now, due;

    if (idle_us < 0) {
        const char *v = getenv("RECOMP_NV2A_ACK_IDLE_US");
        LARGE_INTEGER f;

        idle_us = v ? atoi(v) : 500;
        if (idle_us < 0)
            idle_us = 0;
        if (idle_us) {
            timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */,
                                           TIMER_ALL_ACCESS);
            if (!timer)
                timer = CreateWaitableTimerW(NULL, FALSE, NULL);
            if (!timer)
                idle_us = 0;
        }
        QueryPerformanceFrequency(&f);
        qpf = f.QuadPart;
        fprintf(stderr, "  NV2A busy-bit ack: %s\n",
                idle_us ? "waits between passes once idle (RECOMP_NV2A_ACK_IDLE_US)"
                        : "spins (RECOMP_NV2A_ACK_IDLE_US=0)");
    }
    if (!idle_us) {
        Sleep(0);
        return;
    }
    QueryPerformanceCounter(&now);
    if (g_ack_activity != seen) {
        seen = g_ack_activity;
        quiet_since = now.QuadPart;
    }
    if (now.QuadPart - quiet_since < qpf / 1000) {
        Sleep(0);   /* busy: a waiter is spinning on another core */
        return;
    }
    due.QuadPart = -(LONGLONG)idle_us * 10;   /* relative, 100 ns units */
    if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(timer, 50);
    else
        Sleep(1);
}

static DWORD WINAPI nv2a_ack_thread(LPVOID param)
{
    volatile uint32_t *regs = (volatile uint32_t *)param;
    xbox_NameCurrentThread(L"nv2a ack");
    while (!InterlockedCompareExchange(&g_nv2a_ack_stop, 0, 0)) {
        for (size_t i = 0; i < sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]); i++) {
            volatile uint32_t *r =
                (volatile uint32_t *)((char *)regs + NV2A_ACK[i].offset);

            /* Leave the interrupt status alone once something actually
             * raises interrupts.
             *
             * Holding these at zero was correct while nothing here ever
             * raised a GPU interrupt -- "none pending" was the truth, and a
             * title's ISR re-entering on a bit that never cleared is how
             * Halo reached a native stack overflow.
             *
             * With RECOMP_VBLANK the vblank tick raises one and the title's
             * own ISR services it, so the premise is gone. Burnout 2's
             * deferred routine reads PMC_INTR_0 through the context the
             * kernel handed it (0x00226EB8 holds 0xFD000000, so the flags it
             * tests are at 0xFD000100) and decides from those bits what work
             * to do. Clearing them from here meant the routine ran 4,818
             * times and found nothing pending every time.
             *
             * The ISR clears them itself, which is what write-1-to-clear is
             * for; this thread stops competing with it. */
            if (InterlockedCompareExchange(&s_vblank_owns_intr, 0, 0)
                && intr_status_reg(NV2A_ACK[i].offset))
                continue;

            if (*r & NV2A_ACK[i].busy_mask) {
                *r &= ~NV2A_ACK[i].busy_mask;
                g_ack_activity++;
            }
        }
        for (size_t i = 0; i < sizeof(NV2A_IDLE) / sizeof(NV2A_IDLE[0]); i++) {
            volatile uint32_t *r =
                (volatile uint32_t *)((char *)regs + NV2A_IDLE[i].offset);
            if ((*r & NV2A_IDLE[i].idle_mask) != NV2A_IDLE[i].idle_mask) {
                *r |= NV2A_IDLE[i].idle_mask;
                g_ack_activity++;
            }
        }
        {
            volatile uint32_t *put =
                (volatile uint32_t *)((char *)regs + NV2A_USER_DMA_PUT);
            volatile uint32_t *get =
                (volatile uint32_t *)((char *)regs + NV2A_USER_DMA_GET);
            if (*get != *put) {
                *get = *put;
                g_ack_activity++;
            }
        }
        fence_mirrors_tick();
        counter_mirrors_tick();
        frame_counters_tick();
        framebuffer_probe_tick();

        /* Which framebuffer the display would be scanning out.
         *
         * PCRTC_START holds the address the CRTC reads pixels from, so
         * whatever the title last set there is the frame it believes is on
         * screen. Nothing here scans out, so this is the one place that says
         * whether the guest is producing an image at all -- and where it is.
         * Gated, because it is a bring-up question, not a runtime one. */
        /* Not gated on the trace flag: nv2a_pb_scan is what drives the
         * executor, and it already returns unless RECOMP_PB_SCAN or
         * RECOMP_PB_EXEC asked for it. Gating the call as well meant
         * RECOMP_PB_EXEC on its own did nothing at all, and the executor
         * only ran when someone happened to also be tracing. */
        {
            /* Is the title submitting GPU work at all? PUT is where the
             * title's pushbuffer writer has got to; if it never moves, nothing
             * is being drawn and the missing piece is upstream of the GPU. */
            static DWORD  last_put_ms;
            static uint32_t last_put;
            DWORD now_ms = GetTickCount();
            uint32_t put = *(volatile uint32_t *)((char *)regs + NV2A_USER_DMA_PUT);
            if (put != last_put || (now_ms - last_put_ms) > 2000) {
                /* Survey the segment the title just submitted, once. */
                {
                    extern void nv2a_pb_scan(uint32_t, uint32_t);
                    extern void nv2a_pb_scan_report(void);
                    static DWORD last_report;

                    /* DMA_PUT holds a PHYSICAL address -- Xbox D3D writes
                     * `VA & 0x0FFFFFFF` and reads the GPU's position back as
                     * `GET | 0x80000000`. nv2a_pb_scan reads guest VAs, so
                     * handing it the raw register value pointed it at low
                     * memory: for the Xbox Dashboard, whose pushbuffer is at
                     * 0x80001000, PUT reads 0x1000 and the survey walked the
                     * fake TIB. It reported a plausible-looking inventory of
                     * nothing, which is worse than reporting none -- the
                     * conclusion drawn was "the title submits no methods"
                     * while it was submitting them the whole time.
                     *
                     * The contiguous window IS the physical-address view, so
                     * OR-ing its base is the documented round trip, not a
                     * guess. */
                    if (last_put && put > last_put)
                        nv2a_pb_scan(XBOX_CONTIG_BASE | (last_put & 0x0FFFFFFFu),
                                     XBOX_CONTIG_BASE | (put      & 0x0FFFFFFFu));
                    /* Periodic, because what the title submits at init is not
                     * what it submits once it is drawing a menu, and the
                     * question the survey answers is about the latter. */
                    if (s_nv2a_trace && now_ms - last_report > 10000) {
                        last_report = now_ms;
                        nv2a_pb_scan_report();
                    }
                }
                last_put = put; last_put_ms = now_ms;
                /* GET as well as PUT. A title that stops submitting has either
                 * finished or is spinning on the GPU catching up, and only GET
                 * tells those apart -- D3D waits for GET to reach PUT before it
                 * reuses the buffer, so GET stuck behind PUT is the shape of a
                 * pushbuffer-full hang. Also show the same pair as the Xbox
                 * Dashboard reads them: its D3D holds a register-block pointer
                 * in its device struct rather than assuming 0xFD800000, and
                 * mirroring the wrong block leaves it spinning on a GET that
                 * never moves. */
                if (s_nv2a_trace) {
                    uint32_t g = *(volatile uint32_t *)
                                 ((char *)regs + NV2A_USER_DMA_GET);
                    fprintf(stderr, "  [NV2A] DMA_PUT = 0x%08X  DMA_GET = "
                            "0x%08X%s\n", put, g,
                            g == put ? "" : "  (GPU behind)");
                }
                fflush(stderr);
            }
        }
        if (s_nv2a_trace) {
            static uint32_t last_start = 0xFFFFFFFFu;
            uint32_t start = *(volatile uint32_t *)((char *)regs + 0x600800);
            if (start != last_start) {
                last_start = start;
                fprintf(stderr, "  [NV2A] PCRTC_START = 0x%08X\n", start);
                fflush(stderr);
            }
        }

        if (g_mcpx_regs && !g_apu_mmio_trapped) {
            for (size_t i = 0; i < sizeof(MCPX_COUNTERS) / sizeof(MCPX_COUNTERS[0]); i++) {
                volatile uint32_t *c =
                    (volatile uint32_t *)((char *)g_mcpx_regs + MCPX_COUNTERS[i]);
                *c += 1;
            }
        }

        /* Advance KeTickCount. It was written once at init and left frozen,
         * which silently breaks every timeout that polls it: Halo's DHCP setup
         * waits on a tick deadline that never arrives and spins forever bringing
         * up XNet. A live clock is also just the truth -- KeTickCount ticks on
         * hardware whether or not anyone is asleep. GetTickCount() shares the
         * millisecond unit, so the rate matches. */
        *(volatile uint32_t *)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT)
                               + g_memory_offset) =
            xbox_LockstepClockOn()
                /* lockstep: the frames only -- a loading screen's vblanks are
                 * as many as the load took here (hle_dsound.c says more) */
                ? 0x00100000u + (uint32_t)(xbox_LockstepFrames() * 1000 / 60)
                : GetTickCount();

        nv2a_ack_wait();
    }
    return 0;
}

static void xbox_Nv2aAckStart(void)
{
    g_nv2a_ack_stop = 0;
    g_nv2a_ack_thread = CreateThread(NULL, 0, nv2a_ack_thread,
                                     g_nv2a_memory, 0, NULL);
    if (g_nv2a_ack_thread) {
        fprintf(stderr, "  NV2A busy-bit ack: %zu register(s) acknowledged\n",
                sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]));
    }
}

/* Separate allocation for Xbox kernel address space (0x80010000+).
 * Some RenderWare code reads the kernel PE header to detect features. */
static void *g_kernel_memory = NULL;

/* Global offset accessible by recompiled code (via recomp_types.h) */
ptrdiff_t g_xbox_mem_offset = 0;

/* Bounds of the title's executable sections, from its own XBE section table.
 *
 * RECOMP_ICALL uses these to decide whether an indirect-call target is code
 * before dispatching it. This used to be a hardcoded "0x00400000..0xFE000000 is
 * not code" test, which is true for Burnout 3 -- its .text ends at 0x002CC200,
 * so everything above 0x400000 really is data -- and false for any title with
 * more code than that. Half-Life 2's .text runs to 0x005F4A6C, so the constant
 * silently discarded every indirect call into the top two thirds of the game,
 * including the one that enters its main. No log, no crash: eax = 0 and carry
 * on, which looks exactly like a function that returned early.
 *
 * Zero until the layout is initialised, which the macro treats as "allow" so
 * nothing breaks before the title is loaded. */
uint32_t g_xbox_image_lo = 0;
uint32_t g_xbox_image_hi = 0;
uint32_t g_xbox_code_lo = 0;
uint32_t g_xbox_code_hi = 0;

/* Global registers for recompiled code (via recomp_types.h) */
/* Each guest thread's TIB. The first thread uses the one the loader built;
 * a spawned thread gets its own from xbox_AllocThreadTib(). */
RECOMP_TLS uint32_t g_fs_base = XBOX_TIB_MAIN;

/* How far the runtime's low memory moved to clear the image; see XBOX_LOW_VA. */
uint32_t g_xbox_low_shift = 0;

/* The current-thread object, reached through fs:[0x28].
 *
 * On the console fs points at the KPCR and the processor control block is
 * embedded at 0x28, whose first field is the pointer to the running thread.
 * Titles read it to find out which thread they are on: Black computes
 * MEM32(MEM32(fs:[0x28]) + 0x12C), compares it against a table of thread ids
 * it recorded earlier, and if they match it sets an error code and executes a
 * deliberate `jmp $`. That is a re-entrancy assertion -- "this must not be
 * the thread that already owns this" -- and it is a reasonable thing for a
 * title to check.
 *
 * It used to be one fixed address shared by every guest thread, because
 * xbox_AllocThreadTib copies the main thread's whole block. So every thread
 * reported the same identity, every such comparison said yes, and Black hung
 * itself on purpose after loading. Each thread now gets its own copy with a
 * distinct id, and everything else in the block is inherited exactly as
 * before so that nothing which already worked changes. */
#define XBOX_THREAD_OBJ_MAIN XBOX_LOW_VA(0x00760000u) /* the main thread's */
#define XBOX_THREAD_OBJ_SIZE 0x200u
#define XBOX_THREAD_ID_OFF   0x12Cu

static uint32_t g_next_guest_thread_id = 0x1000;

uint32_t xbox_CurrentThreadObject(void)
{
    uintptr_t fs = (uintptr_t)g_fs_base + g_memory_offset;
    return *(const uint32_t *)(fs + 0x28);
}

/* The shape of the TLS block the loader built, so a new thread can be
 * given one just like it: where the initialised image data starts, how
 * big the block is, and how big the per-thread structure slot 0 points
 * at is. Zero total means the image had no TLS directory. */
static uint32_t g_tls_template_va, g_tls_total, g_tls_thread_size = 64;

RECOMP_TLS uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;

/* The guest esp an indirect-call dispatch captured, for the diagnostics that
 * need the call site. g_esp is not it: a lifted caller pushes its return
 * address onto a *local* esp and only syncs g_esp at certain points, so by
 * the time a refused call is reported g_esp is stale and reads as 0. The
 * dispatch macros set this to the esp they were handed; a title whose
 * generated header predates them leaves it 0, and the log says so rather
 * than inventing a caller. */
RECOMP_TLS uint32_t g_icall_saved_esp = 0;

/* Which dispatch form was refused: 0 unknown, 1 call, 2 jump. */
RECOMP_TLS uint32_t g_icall_dispatch_form = 0;
RECOMP_TLS uint32_t g_ebx = 0, g_esi = 0, g_edi = 0;

#ifdef RECOMP_ABI_CHECK
/* Report a lifted function that returned without restoring ebx/esi/edi.
 *
 * Those are callee-saved on x86, and the recompiler keeps them in globals, so
 * a function whose epilogue was never lifted corrupts its caller rather than
 * itself -- an error with no crash and no message, just less work silently
 * done. Ranked by hit count so the routine breaking a hot loop stands out from
 * the one-offs; -DRECOMP_ABI_CHECK only, since it costs three compares on
 * every indirect call.
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0,
                              uint32_t edi0, uint32_t esp0)
{
    enum { SLOTS = 32 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
        fprintf(stderr, "[ABI] sub_%08X:%s%s%s%s\n"
                        "      ebx %08X->%08X esi %08X->%08X"
                        " edi %08X->%08X esp %08X->%08X\n",
                va,
                g_ebx != ebx0 ? " ebx" : "",
                g_esi != esi0 ? " esi" : "",
                g_edi != edi0 ? " edi" : "",
                g_esp < esp0 + 4 ? " esp(epilogue never ran)" : "",
                ebx0, g_ebx, esi0, g_esi, edi0, g_edi, esp0, g_esp);
        /* esp coming back too HIGH means some callee popped arguments that
         * were never pushed -- a convention mismatch the one-sided invariant
         * above cannot see. The most recent indirect targets are the usual
         * suspects, so name them. */
        {
            int t;
            fprintf(stderr, "      esp delta %+d, recent icall targets:",
                    (int)(g_esp - esp0));
            for (t = 4; t >= 1; t--)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx - t) & 15]);
            fputc('\n', stderr);
        }
        fflush(stderr);
    }
    hits[i]++;
}

/* The exact form, for a direct call whose callee's `ret N` is known: esp must
 * come back exactly 4 + N higher. Reports once per callee, and says which way
 * esp is off, since "too high" is the case the one-sided check above misses. */
void recomp_abi_pop_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0,
                                  uint32_t edi0, uint32_t esp0, uint32_t pop)
{
    enum { SLOTS = 64 };
    static uint32_t seen[SLOTS];
    static int count;
    int i;
    int want = 4 + (int)pop, got = (int)(g_esp - esp0);

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            return;
    if (count == SLOTS)
        return;
    seen[count++] = va;
    fprintf(stderr, "[ABI] sub_%08X:%s%s%s%s\n"
                    "      esp %+d, expected %+d (ret %u); ebx %08X->%08X"
                    " esi %08X->%08X edi %08X->%08X esp %08X->%08X\n",
            va,
            g_ebx != ebx0 ? " ebx" : "",
            g_esi != esi0 ? " esi" : "",
            g_edi != edi0 ? " edi" : "",
            got > want ? " esp-too-high" : got < want ? " esp-too-low" : "",
            got, want, pop, ebx0, g_ebx, esi0, g_esi, edi0, g_edi,
            esp0, g_esp);
    /* The callee's own epilogue is usually right; what moved esp is an
     * indirect call it made, whose target popped too much. Name the recent
     * ones, as the one-sided form above does. */
    {
        int t;
        fprintf(stderr, "      recent icall targets:");
        for (t = 4; t >= 1; t--)
            fprintf(stderr, " %08X",
                    g_icall_trace[(g_icall_trace_idx - t) & 15]);
        fputc('\n', stderr);
    }
    fflush(stderr);
}
#endif

/* SEH frame pointer bridge (see recomp_types.h for explanation) */
RECOMP_TLS uint32_t g_seh_ebp = 0;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top = 0;

/* Set once at startup. The generated code reads it at the ret of every
 * --force-return function, so it has to be cheap and it has to default to
 * off: a build carrying forced functions behaves normally until the
 * variable is set. */
int g_force_return = 0;
/* x87 control and status. The reset default masks every exception and
 * rounds to nearest, which is what the CRT expects before _control87. */
RECOMP_TLS uint16_t g_fp_control_word = 0x037Fu;
RECOMP_TLS int g_fp_cmp = 0;
RECOMP_TLS uint16_t g_fp_cc = 0x4000;

/* Defined below, with the other guest registers. */
extern RECOMP_TLS uint32_t g_ebp;
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi;

/* ---- non-local jumps ---------------------------------------------------
 *
 * The native half of the guest's setjmp/longjmp. See recomp_types.h for why a
 * guest-only longjmp is not enough; in short, the recompiled frames are C
 * frames and something has to unwind them.
 *
 * Keyed by guest buffer address, per thread. Buffers nest, so jumping to an
 * outer one discards every inner entry -- those frames are gone.
 */
#define RECOMP_JMPBUF_SLOTS 32

typedef struct {
    uint32_t  buf_va;
    uintptr_t stack;    /* a native address on the stack it was armed on */
    jmp_buf   native;
} recomp_jmp_slot;

static RECOMP_TLS recomp_jmp_slot s_jmp[RECOMP_JMPBUF_SLOTS];
static RECOMP_TLS int             s_jmp_used;

/* A native address on the current stack: this frame's, which sits just below
 * the caller's setjmp. Taken as an integer so it is never dereferenced. */
static uintptr_t recomp_native_sp(void)
{
    volatile char here = 0;
    return (uintptr_t)&here;
}

/* A title that runs guest code on more than one native stack -- host fibers
 * standing in for a coroutine scheduler, as Mortal Kombat: Deadly Alliance's
 * mk_tasks.c does -- can see a guest longjmp whose buffer was armed on another
 * stack. On hardware that is ordinary: everything shares one stack, and MKDA's
 * main loop arms a buffer that its tasks jump back to. A native longjmp across
 * stacks cannot work (Windows refuses the unwind with STATUS_BAD_STACK), so the
 * runtime detects it and hands it to whoever owns the stacks. The handler must
 * not return: it switches to the owning stack and does the jump there. */
/* Same type as recomp_types.h declares for the title side. */
typedef void (*recomp_foreign_longjmp_fn)(jmp_buf *target, int value,
                                          uintptr_t armed_stack);
static recomp_foreign_longjmp_fn s_foreign_longjmp;

void recomp_set_foreign_longjmp(recomp_foreign_longjmp_fn fn)
{
    s_foreign_longjmp = fn;
}

static int recomp_on_current_stack(uintptr_t a)
{
#if defined(_WIN32)
    ULONG_PTR lo, hi;
    GetCurrentThreadStackLimits(&lo, &hi);
    return a >= lo && a < hi;
#else
    (void)a;
    return 1;   /* nothing switches native stacks here yet */
#endif
}

jmp_buf *recomp_setjmp_slot(uint32_t buf_va)
{
    int i;

    for (i = 0; i < s_jmp_used; i++)
        if (s_jmp[i].buf_va == buf_va) {
            s_jmp[i].stack = recomp_native_sp();
            return &s_jmp[i].native;      /* the same buffer, re-armed */
        }
    if (s_jmp_used >= RECOMP_JMPBUF_SLOTS)
        s_jmp_used = RECOMP_JMPBUF_SLOTS - 1;   /* keep the deepest */
    s_jmp[s_jmp_used].buf_va = buf_va;
    s_jmp[s_jmp_used].stack = recomp_native_sp();
    return &s_jmp[s_jmp_used++].native;
}

int recomp_guest_longjmp(uint32_t buf_va, uint32_t value)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    int i;

    for (i = s_jmp_used - 1; i >= 0; i--) {
        if (s_jmp[i].buf_va != buf_va)
            continue;

        /* The callee-saved registers and the stack, exactly as the CRT's
         * longjmp restores them: esp is the setjmp-time esp plus the return
         * address that setjmp's own ret would have popped. */
        g_ebx = *(const uint32_t *)(mem + buf_va + 0x04);
        g_edi = *(const uint32_t *)(mem + buf_va + 0x08);
        g_esi = *(const uint32_t *)(mem + buf_va + 0x0C);
        g_esp = *(const uint32_t *)(mem + buf_va + 0x10) + 4;

        /* ebp is a C local in every translated function, and a local modified
         * after setjmp is indeterminate once longjmp lands. Hand the resumed
         * frame its saved value back through the globals it already reads. */
        g_seh_ebp = *(const uint32_t *)(mem + buf_va + 0x00);
        g_ebp     = g_seh_ebp;

        s_jmp_used = i + 1;   /* the inner buffers died with their frames */
        if (!recomp_on_current_stack(s_jmp[i].stack)) {
            fprintf(stderr, "[LONGJMP] guest longjmp to buffer 0x%08X, which was "
                            "armed on another native stack (%s)\n", buf_va,
                    s_foreign_longjmp ? "handing it to that stack's owner"
                                      : "no handler: the unwind will fail");
            fflush(stderr);
            if (s_foreign_longjmp)
                s_foreign_longjmp(&s_jmp[i].native, value ? (int)value : 1,
                                  s_jmp[i].stack);
        }
        longjmp(s_jmp[i].native, value ? (int)value : 1);
    }
    return 0;
}

/* Watchdog: dump the guest call stack if the title stops making progress.
 *
 * A hang gives nothing to work from -- no crash, no last log line, no native
 * stack that means anything, because the guest frames live in guest memory and
 * the native one only shows whichever translated function is spinning. Sampling
 * the guest stack from a second thread is the one view that says where the
 * title actually is. Same GS format the crash handler uses, so tools/
 * stackwalk.py reads either.
 *
 * Off unless RECOMP_WATCHDOG_SECS is set, so it costs a getenv in normal runs.
 */
/* Defined below, after the watchdog. */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

static uint32_t *s_watchdog_esp;
/* The other guest registers are thread-local too, so the watchdog has to be
 * handed the guest thread's copies rather than reading its own -- which are
 * always zero, and read as "every register is null" at exactly the moment the
 * registers are the thing being asked about. */
static uint32_t *s_watchdog_regs[6];
static unsigned  s_watchdog_secs;

static DWORD WINAPI xbox_watchdog_thread(LPVOID unused)
{
    const uint8_t *mem;
    uint32_t esp, i;

    (void)unused;
    Sleep(s_watchdog_secs * 1000u);

    mem = (const uint8_t *)g_memory_offset;
    esp = s_watchdog_esp ? *s_watchdog_esp : 0;
    fprintf(stderr, "[WATCHDOG] no exit after %us; guest esp=0x%08X\n"
            "  regs: eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X\n",
            s_watchdog_secs, esp,
            s_watchdog_regs[0] ? *s_watchdog_regs[0] : 0,
            s_watchdog_regs[1] ? *s_watchdog_regs[1] : 0,
            s_watchdog_regs[2] ? *s_watchdog_regs[2] : 0,
            s_watchdog_regs[3] ? *s_watchdog_regs[3] : 0,
            s_watchdog_regs[4] ? *s_watchdog_regs[4] : 0,
            s_watchdog_regs[5] ? *s_watchdog_regs[5] : 0);
    /* The recent indirect-call targets name whatever is spinning: a stuck loop
     * inside a function reached through a pointer leaves no clue on the stack
     * beyond the return address of the call that entered it. */
    {
        uint32_t k;
        /* The running indirect-call total separates a hang from mere
         * slowness. Kernel calls cannot: a pure CPU loop makes none, so
         * "same count at 20s and 60s" proves nothing about it. */
        fprintf(stderr, "  icalls so far: %llu\n",
                (unsigned long long)g_icall_count);
        fprintf(stderr, "  recent ICALL targets:");
        for (k = 0; k < 16; k++)
            fprintf(stderr, " %08X",
                    g_icall_trace[(g_icall_trace_idx + k) & 15]);
        fprintf(stderr, "\n");
    }
    /* Guest globals worth seeing at the moment of the hang.
     *
     * RECOMP_PEEK is otherwise only sampled by the pushbuffer reporter, which
     * a title that hangs before rendering never reaches -- and a spin that
     * makes no kernel calls is invisible to RECOMP_KERNEL_WATCH too. A pure
     * CPU loop polling a global is exactly the case neither of those covers.
     */
    {
        const char *spec = getenv("RECOMP_PEEK");
        char buf[256], *q, *end;
        if (spec && *spec) {
            strncpy(buf, spec, sizeof buf - 1);
            buf[sizeof buf - 1] = 0;
            fprintf(stderr, "  peek:");
            for (q = buf; *q; ) {
                unsigned long va = strtoul(q, &end, 0);
                if (end == q)
                    break;
                if (va >= XBOX_BASE_ADDRESS && va < XBOX_TOTAL_RAM)
                    fprintf(stderr, " [%08lX]=%08X", va,
                            *(const uint32_t *)(mem + va));
                q = (*end == ',') ? end + 1 : end;
            }
            fprintf(stderr, "\n");
        }
    }
    /* RECOMP_FIND_VALUE: who holds this value. A hang has no crash handler
     * to run the scan, and "which object still points at the freed block"
     * is as much a hang question as a crash one (Max Payne's heap spin). */
    xbox_watch_scan_on_crash();

    for (i = 0; i < 400 && esp; i++) {
        uint32_t a = esp + i * 4;
        if (a < XBOX_STACK_BASE || a >= XBOX_STACK_TOP) break;
        fprintf(stderr, "    GS %08X %08X\n", a,
                *(const uint32_t *)(mem + a));
    }
    fflush(stderr);
    _exit(3);
    return 0;
}

void xbox_WatchdogStart(void)
{
    const char *secs = getenv("RECOMP_WATCHDOG_SECS");
    HANDLE h;

    if (!secs || !*secs)
        return;
    s_watchdog_secs = (unsigned)atoi(secs);
    if (!s_watchdog_secs)
        return;

    /* Taken on the guest thread: g_esp is thread-local, so the watchdog has to
     * be handed the address of the one that matters rather than reading its
     * own, which is always zero. */
    s_watchdog_esp = &g_esp;
    s_watchdog_regs[0] = &g_eax; s_watchdog_regs[1] = &g_ecx;
    s_watchdog_regs[2] = &g_edx; s_watchdog_regs[3] = &g_ebx;
    s_watchdog_regs[4] = &g_esi; s_watchdog_regs[5] = &g_edi;
    h = CreateThread(NULL, 0, xbox_watchdog_thread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
}

/* SSE. 128 bits of architectural state, per-thread like the rest. */
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
/* Last frame established by `mov ebp, esp`. Read by frameless functions
 * that address their caller's frame through ebp. */
RECOMP_TLS uint32_t g_ebp = 0;

/* EFLAGS.DF. Zero means the string instructions walk forwards, which is the
 * ABI's resting state and what almost every one of them does -- so this is
 * almost always 0 and costs a predictable branch. The exceptions are the ones
 * that matter: MSVC's strrchr/wcsrchr scan backwards from the terminator with
 * `std; repne scasb`, and memmove goes backwards when its regions overlap the
 * wrong way. Thread-local, because `std` and the `cld` that undoes it can land
 * in different lifted bodies of the same guest routine. */
RECOMP_TLS int g_df = 0;

/* ICALL trace ring buffer */
volatile uint32_t g_icall_trace[16] = {0};
volatile uint32_t g_icall_trace_idx = 0;
volatile uint64_t g_icall_count = 0;

BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size)
{
    g_force_return = getenv("RECOMP_FORCE_RETURN") != NULL;
    DWORD old_protect;
    const uint8_t *xbe = (const uint8_t *)xbe_data;

    if (g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: already initialized\n");
        return FALSE;
    }

    /*
     * Calculate the full range we need to map.
     * From XBOX_MAP_START (0x0) to the end of the furthest section.
     * This includes low memory (KPCR at 0x0-0xFF) which game code reads
     * from, the XBE sections, and the simulated stack.
     */
    /* Map the full Xbox address space (covers all sections + stack + heap).
     * Size is runtime-configurable: retail 64 MB, devkit debug builds 128 MB. */
    /* The mapped range, which is not necessarily RAM. Mirrors are placed
     * at multiples of this, so growing it is what stops a title's
     * above-RAM allocations from aliasing low memory. */
    g_memory_size = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

    /*
     * Create a file mapping backed by the page file.
     *
     * Using file mapping instead of VirtualAlloc allows us to map the same
     * physical pages at multiple virtual addresses via MapViewOfFileEx.
     * This is critical for the Xbox RAM mirror: the Xbox memory controller
     * uses a 26-bit address bus, so ALL addresses wrap modulo 64 MB.
     * Code that writes to address 0x20000448 is really writing to 0x00000448.
     * With file mapping views, we create aliased mappings at 64 MB intervals
     * that all point to the same physical memory.
     */
    g_mapping_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,   /* page file backed */
        NULL,                   /* default security */
        PAGE_READWRITE,         /* read-write access */
        0,                      /* high DWORD of size */
        (DWORD)g_memory_size,   /* low DWORD of size (64 MB) */
        NULL                    /* unnamed mapping */
    );
    if (!g_mapping_handle) {
        fprintf(stderr, "xbox_MemoryLayoutInit: CreateFileMapping failed (error %lu)\n",
                GetLastError());
        return FALSE;
    }

    /*
     * Map the base view at the desired virtual address.
     * Try the original Xbox base address first. If that fails (common on
     * Windows 11 where low addresses are often reserved), try page-aligned
     * addresses upward until we find a free region.
     */
    {
        static const uintptr_t try_bases[] = {
            XBOX_BASE_ADDRESS,      /* 0x00010000 - original Xbox address */
            0x00800000,             /* 8 MB - above typical PEB/TEB region */
            0x01000000,             /* 16 MB */
            0x02000000,             /* 32 MB */
            0x10000000,             /* 256 MB */
            0,                      /* sentinel - let OS choose */
        };

        for (int i = 0; try_bases[i] != 0 || i == 0; i++) {
            LPVOID hint = try_bases[i] ? (LPVOID)try_bases[i] : NULL;
            g_memory_base = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,           /* offset into mapping */
                g_memory_size,  /* size */
                hint            /* desired base address */
            );
            if (g_memory_base) {
                if (try_bases[i] != 0 && (uintptr_t)g_memory_base != try_bases[i]) {
                    /* OS gave us a different address, retry */
                    UnmapViewOfFile(g_memory_base);
                    g_memory_base = NULL;
                    continue;
                }
                break;
            }
        }
    }

    if (!g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: failed to map base view (%zu KB)\n",
                g_memory_size / 1024);
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
        return FALSE;
    }

    g_memory_offset = (uintptr_t)g_memory_base - XBOX_MAP_START;

    /* Guest page zero: no access.
     *
     * Nothing legitimate lives there -- every XBE's image base is 0x00010000
     * and the TIB now sits at XBOX_FS_BASE -- so any access is a null pointer
     * the title dereferenced. Left readable it did quiet damage: a null check
     * of the form `cmp byte [ecx], 0` read whatever happened to be at 0 and
     * decided the pointer was fine, and a store through a null pointer landed
     * on real memory and surfaced as corruption somewhere unrelated. Faulting
     * here turns both into one access violation at the instruction that made
     * the mistake, which the crash handler can name.
     *
     * Opt-in through RECOMP_TRAP_NULL, because it converts a class of bug the
     * title currently survives into a hard stop: a guest that dereferences null
     * and ignores the result keeps running while page zero reads as zero, and
     * stops dead once it faults. That is the right default for hunting one of
     * these and the wrong one for making progress past the rest, so it is a
     * switch rather than a policy.
     *
     * Note this is separate from moving the TIB off page zero, which is not
     * optional: with the TIB gone, address 0 reads as plain zero, so a null
     * check written as a load through the pointer now gets the answer it
     * expects whether or not the page is trapped.
     *
     * Best-effort: failing to protect it costs only the diagnostic. */
    if (XBOX_MAP_START == 0 && getenv("RECOMP_TRAP_NULL")) {
        DWORD old_protect;
        if (VirtualProtect(g_memory_base, 0x1000, PAGE_NOACCESS, &old_protect))
            fprintf(stderr, "  guest page 0 is PAGE_NOACCESS"
                            " (null dereferences fault)\n");
    }

    if (g_memory_offset == 0) {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%08X (original Xbox address)\n",
                g_memory_size / 1024, XBOX_MAP_START);
    } else {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%p (offset %+td from Xbox base)\n",
                g_memory_size / 1024, g_memory_base, g_memory_offset);
    }

    /*
     * Helper macro: convert Xbox VA to actual mapped address.
     * When g_memory_offset == 0 (ideal case), this is identity.
     */
    #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))

    /*
     * Copy XBE header to base address.
     * The Xbox kernel maps the XBE image header at 0x00010000.
     * Game code reads kernel thunk table, certificate data, and
     * section info from this region.
     */
    {
        /* XBE header size is at file offset 0x0108 (SizeOfImageHeader) */
        DWORD header_size = 0;
        if (xbe_size >= 0x10C) {
            header_size = *(const DWORD *)(xbe + 0x0108);
        }
        if (header_size == 0 || header_size > 0x10000)
            header_size = 0x1000;  /* fallback: 4KB */
        if (header_size > xbe_size)
            header_size = (DWORD)xbe_size;
        memcpy(XBOX_VA(XBOX_BASE_ADDRESS), xbe, header_size);
        fprintf(stderr, "  XBE header: %u bytes at %p (Xbox VA 0x%08X)\n",
                header_size, XBOX_VA(XBOX_BASE_ADDRESS), XBOX_BASE_ADDRESS);
    }

    /*
     * Move the runtime's low memory clear of the image (see XBOX_LOW_VA).
     *
     * SizeOfImage (header offset 0x010C) covers every section including BSS,
     * which the section table's raw sizes do not. Rounded to 64 KB so the
     * stack and heap keep the alignment they had at their old addresses.
     */
    if (xbe_size >= 0x0110) {
        DWORD base_addr  = *(const DWORD *)(xbe + XBE_BASE_ADDR_OFFSET);
        DWORD image_size = *(const DWORD *)(xbe + 0x010C);
        uint32_t image_end = (uint32_t)base_addr + (uint32_t)image_size;

        if (image_end > XBOX_LOW_REGION_START) {
            g_xbox_low_shift = (image_end - XBOX_LOW_REGION_START + 0xFFFFu)
                               & ~0xFFFFu;
            fprintf(stderr, "  Image ends at 0x%08X, past 0x%08X: runtime low "
                    "memory moved up 0x%08X (stack 0x%08X, heap 0x%08X)\n",
                    image_end, XBOX_LOW_REGION_START, g_xbox_low_shift,
                    XBOX_STACK_BASE, XBOX_HEAP_BASE);
        }
    }

    /*
     * Dynamically load ALL XBE sections by parsing the section headers.
     *
     * This replaces the old approach of hardcoding section addresses for
     * a specific game (Burnout 3). By reading the section table from the
     * XBE header, any game's sections are loaded automatically.
     *
     * Every section is copied to its original Xbox VA:
     * - .text: needed because memory walkers may scan code pages
     * - .rdata: constants, vtables, kernel thunk table
     * - .data: global variables (initialized portion from XBE, BSS zeroed)
     * - XDK library sections (D3D, DSOUND, WMADEC, XPP, etc.)
     * - DOLBY, BINK, XTIMAGE, etc.
     */
    {
        DWORD base_addr = *(const DWORD *)(xbe + XBE_BASE_ADDR_OFFSET);
        DWORD num_sections = *(const DWORD *)(xbe + XBE_SECTION_COUNT_OFFSET);
        DWORD sect_headers_va = *(const DWORD *)(xbe + XBE_SECTION_HEADERS_OFFSET);
        DWORD sect_headers_off = sect_headers_va - base_addr;
        int sections_loaded = 0;
        size_t total_bytes = 0;

        if (num_sections > 64) num_sections = 64;  /* sanity cap */

        fprintf(stderr, "  XBE sections: %u (headers at file offset 0x%08X)\n",
                num_sections, sect_headers_off);

        for (DWORD si = 0; si < num_sections; si++) {
            if (sect_headers_off + (si + 1) * SECTHDR_SIZE > xbe_size) break;

            const uint8_t *sh = xbe + sect_headers_off + si * SECTHDR_SIZE;
            DWORD sec_va       = *(const DWORD *)(sh + SECTHDR_VA);
            DWORD sec_vsize    = *(const DWORD *)(sh + SECTHDR_VSIZE);
            DWORD sec_raw_off  = *(const DWORD *)(sh + SECTHDR_RAW_OFFSET);
            DWORD sec_raw_size = *(const DWORD *)(sh + SECTHDR_RAW_SIZE);
            DWORD sec_name_va  = *(const DWORD *)(sh + SECTHDR_NAME_ADDR);

            /* Read section name from XBE header */
            const char *sec_name = "?";
            DWORD name_off = sec_name_va - base_addr;
            if (name_off < xbe_size && name_off + 8 <= xbe_size)
                sec_name = (const char *)(xbe + name_off);

            /* Validate: section must fit within our 64MB mapped region */
            if (sec_va < XBOX_BASE_ADDRESS || sec_va + sec_vsize > XBOX_TOTAL_RAM)
                continue;

            /* Determine copy size (raw_size may exceed vsize due to alignment) */
            DWORD copy_size = (sec_raw_size < sec_vsize) ? sec_raw_size : sec_vsize;

            /* Zero the full virtual size first (handles BSS) */
            memset(XBOX_VA(sec_va), 0, sec_vsize);

            /* Copy initialized data from XBE */
            if (copy_size > 0 && sec_raw_off + copy_size <= xbe_size) {
                memcpy(XBOX_VA(sec_va), xbe + sec_raw_off, copy_size);
            }

            /* Every loaded section, executable or not. Anything that writes
             * guest memory from outside the title -- the pushbuffer executor
             * clearing a surface, say -- needs to know where the title itself
             * lives, because scribbling on it is not a rendering artefact, it
             * is the title's code and globals gone. */
            if (!g_xbox_image_lo || sec_va < g_xbox_image_lo)
                g_xbox_image_lo = sec_va;
            if (sec_va + sec_vsize > g_xbox_image_hi)
                g_xbox_image_hi = sec_va + sec_vsize;

            /* Executable sections define the range indirect calls may target.
             * XBE section flag 0x04 is EXECUTABLE. */
            if (*(const DWORD *)(sh + SECTHDR_FLAGS) & 0x00000004u) {
                if (!g_xbox_code_lo || sec_va < g_xbox_code_lo)
                    g_xbox_code_lo = sec_va;
                if (sec_va + sec_vsize > g_xbox_code_hi)
                    g_xbox_code_hi = sec_va + sec_vsize;
            }

            sections_loaded++;
            total_bytes += copy_size;

            fprintf(stderr, "  [%2u] %-12s VA=0x%08X vsize=%-8u raw=0x%08X rsize=%-8u%s\n",
                    si, sec_name, sec_va, sec_vsize, sec_raw_off, sec_raw_size,
                    (sec_raw_size < sec_vsize) ? " (BSS)" : "");
        }

        fprintf(stderr, "  Loaded %d/%u sections (%zu bytes total)\n",
                sections_loaded, num_sections, total_bytes);
    }

    /*
     * Parse the kernel thunk table address from the XBE header.
     * The XBE stores KernelImageThunkAddress at offset 0x0158, XOR-encrypted.
     * The key differs between retail and debug XBEs, and there is no flag
     * saying which was used -- decode with both and keep whichever lands in
     * the mapped address range (this is what tools/xbe_parser does).
     *
     * Debug XBEs are not an edge case here: they are the builds most worth
     * recompiling, since they still carry assert strings and symbols. Halo's
     * cachebeta.xbe is one, and assuming the retail key decoded its thunk
     * table to 0xB4F98174 instead of 0x00253090, which silently fell back to
     * the compile-time default and resolved 0 of 378 kernel imports.
     */
    if (xbe_size >= 0x015C) {
        uint32_t thunk_raw = *(const uint32_t *)(xbe + 0x0158);
        uint32_t thunk_retail = thunk_raw ^ 0x5B6D40B6;  /* retail XOR key */
        uint32_t thunk_debug  = thunk_raw ^ 0xEFB1F152;  /* debug XOR key  */
        uint32_t thunk_va;

        if (thunk_retail >= XBOX_BASE_ADDRESS && thunk_retail < XBOX_TOTAL_RAM) {
            thunk_va = thunk_retail;
        } else {
            thunk_va = thunk_debug;
        }

        /* Validate: thunk VA should be within our mapped region */
        if (thunk_va >= XBOX_BASE_ADDRESS && thunk_va < XBOX_TOTAL_RAM) {
            /* Count thunk entries by scanning until we hit 0 */
            uint32_t thunk_count = 0;
            /* XBOX_KERNEL_THUNK_TABLE_SIZE, not 366: the kernel exports 378
             * slots, and kernel.h notes 366 is short by 12. A title importing
             * a high ordinal would have had its table truncated here. */
            for (uint32_t t = 0; t < XBOX_KERNEL_THUNK_TABLE_SIZE; t++) {
                uint32_t entry = *(volatile uint32_t *)((uintptr_t)(thunk_va + t * 4) + g_memory_offset);
                if (entry == 0) break;
                thunk_count++;
            }
            xbox_kernel_set_thunk_address(thunk_va, thunk_count);
            fprintf(stderr, "  Kernel thunks: %u entries at Xbox VA 0x%08X\n",
                    thunk_count, thunk_va);
        } else {
            fprintf(stderr, "  WARNING: kernel thunk VA 0x%08X out of range (raw=0x%08X)\n",
                    thunk_va, thunk_raw);
        }
    }

    /* The certificate, for the region a title is allowed to run in.
     *
     * dwCertificateAddr sits at header+0x0118 and is a plain VA -- unlike the
     * thunk pointer above it is not XOR-obfuscated. dwGameRegion is at
     * certificate+0xA0. A title reads XC_FACTORY_GAME_REGION and ANDs it
     * against this, so reporting a console region the disc does not allow is
     * indistinguishable to the title from a region-locked-out console.
     */
    if (xbe_size >= 0x011C) {
        uint32_t cert_va = *(const uint32_t *)(xbe + 0x0118);
        uint32_t base_va = *(const uint32_t *)(xbe + 0x0104);

        /* The headers map 1:1 from file offset 0, so VA minus base is the
         * file offset -- true for the certificate, which always lives in
         * them. */
        uint32_t cert_off = cert_va - base_va;

        if (cert_va >= base_va && (uint64_t)cert_off + 0xA4 <= (uint64_t)xbe_size) {
            uint32_t region = *(const uint32_t *)(xbe + cert_off + 0xA0);
            xbox_kernel_set_xbe_game_region(region);
            fprintf(stderr, "  XBE certificate: game region 0x%08X\n", region);

            /* The bound above reaches past the region word, so the title
             * name at +0x0C is already known to be inside the file. */
            /* The settings file is named for the title, so the id goes
             * over as soon as it is known -- before anything reads a
             * setting, since nothing has drawn yet. */
            recomp_config_set_title(*(const uint32_t *)(xbe + cert_off + 0x08));

            xbe_title_name_store((const unsigned char *)xbe + cert_off + CERT_TITLE_NAME);
            if (g_xbe_title_name[0])
                fprintf(stderr, "  XBE certificate: title \"%s\"\n", g_xbe_title_name);
        }
    }

    /*
     * NOTE: .rdata is NOT set read-only.
     * VirtualProtect rounds to page boundaries, and the .rdata end (0x003B2454)
     * and .data start (0x003B2360) share the same 4KB page (0x003B2000-0x003B2FFF).
     * Making .rdata read-only also makes the first ~0xCA0 bytes of .data read-only,
     * which causes game initialization code to fault when writing to .data globals
     * in that overlap range.
     */
    (void)old_protect;

    #undef XBOX_VA

    /* Set the global offset for recompiled code MEM macros */
    g_xbox_mem_offset = g_memory_offset;

    /*
     * Initialize the Xbox stack for recompiled code.
     * The stack area lives at XBOX_STACK_BASE in Xbox address space.
     * g_esp is the global stack pointer shared by all translated functions.
     */
    g_esp = XBOX_STACK_TOP;
    fprintf(stderr, "  Stack: %u KB at Xbox VA 0x%08X (ESP = 0x%08X)\n",
            XBOX_STACK_SIZE / 1024, XBOX_STACK_BASE, g_esp);

    /*
     * Populate the fake Thread Information Block (TIB) at Xbox VA 0x0.
     *
     * The original Xbox code uses fs:[offset] to read per-thread data,
     * but the recompiler drops the fs: segment prefix and generates
     * MEM32(offset) instead. Since we mapped low memory (0x0-0xFFFF),
     * we populate the TIB fields that game code accesses:
     *
     *   fs:[0x00] = SEH exception list (-1 = end of chain)
     *   fs:[0x04] = stack base (top of stack)
     *   fs:[0x08] = stack limit (bottom of stack)
     *   fs:[0x18] = self pointer (TIB address)
     *   fs:[0x20] = KPCR Prcb pointer (→ fake structure)
     *   fs:[0x28] = TLS / RW engine context pointer
     *
     * We use free space in the BSS area for the fake structures.
     */
    {
        #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
        #define MEM32_INIT(va, val) (*(uint32_t *)XBOX_VA(va) = (uint32_t)(val))

        /* Fake TIB at address 0x0 */
        MEM32_INIT(XBOX_FS_BASE + 0x00, 0xFFFFFFFF);       /* SEH: end of chain */
        MEM32_INIT(XBOX_FS_BASE + 0x04, XBOX_STACK_TOP);   /* Stack base (high address) */
        MEM32_INIT(XBOX_FS_BASE + 0x08, XBOX_STACK_BASE);  /* Stack limit (low address) */
        MEM32_INIT(XBOX_FS_BASE + 0x18, XBOX_FS_BASE);     /* Self pointer */

        /*
         * fs:[0x20] - On Xbox KPCR, this is the Prcb pointer.
         * Game code reads [fs:[0x20] + 0x250] which on the real Xbox
         * accesses a D3D cache structure. We set it to 0 so the read
         * at offset 0x250 returns 0, causing the cache init to be skipped.
         */
        /* A zeroed block rather than a null pointer. The read is
         * [fs:[0x20] + 0x250], and this used to be left at 0 so that read
         * landed on guest address 0x250 and returned zero by accident -- which
         * only worked while page zero was mapped. Pointing at real zeroed
         * memory says the same thing to the title and survives that page being
         * unmapped, which is what makes a genuine null dereference visible. */
        #define FAKE_PRCB_VA XBOX_LOW_VA(0x00761000)  /* zeroed KPCR Prcb stand-in */
        memset(XBOX_VA(FAKE_PRCB_VA), 0, 0x400);
        MEM32_INIT(XBOX_FS_BASE + 0x20, FAKE_PRCB_VA);
        #undef FAKE_PRCB_VA

        /*
         * fs:[0x28] - Thread local storage / RW engine context.
         * The RW engine reads [fs:[0x28] + 0x28] to get a pointer
         * to its data area. We allocate a fake structure at 0x00760000
         * (in the BSS area) and a data buffer at 0x00700000.
         */
        #define FAKE_TLS_VA     XBOX_LOW_VA(0x00760000)  /* Fake TLS structure (in BSS) */
        #define FAKE_RWDATA_VA  XBOX_LOW_VA(0x00700000)  /* RW engine data area (in BSS) */

        MEM32_INIT(XBOX_FS_BASE + 0x28, FAKE_TLS_VA);
        /* TLS[0x28] = pointer to RW data area */
        MEM32_INIT(FAKE_TLS_VA + 0x28, FAKE_RWDATA_VA);

        /* The running thread's identity. Zero here made every guest thread
         * look like the same thread; see XBOX_THREAD_OBJ_MAIN above. */
        MEM32_INIT(FAKE_TLS_VA + XBOX_THREAD_ID_OFF, g_next_guest_thread_id++);

        /*
         * XBE TLS directory.
         *
         * An image with __declspec(thread) data carries one, and on hardware
         * the loader acts on it. Nothing here did, so thread-local access read
         * whatever memory happened to be under fs:[4].
         *
         * Xbox reaches thread-local data through NtTib.StackBase -- fs:[4] --
         * not Win32's fs:[0x2C], and the block sits BELOW that pointer: the
         * image's entry point computes its own index, negative, as
         * -(blocksize/4). Wreckless does this at guest 0x000EB57E and arrives
         * at -5 for its 20-byte block, so [fs:[4] + index*4] is the block's
         * first dword. The rounding below mirrors that arithmetic exactly,
         * because fs:[4] has to land where the title's own index says it is.
         *
         * The index itself is deliberately NOT written here: the title
         * computes and stores it. What the loader owes it is a block in the
         * right place.
         *
         * Slot 0 holds a pointer to per-thread data -- XAPI's SetLastError is
         * [[fs:[4] + index*4] + 4] = err -- so it gets a zeroed block rather
         * than being left NULL, which had SetLastError writing the error code
         * over fs:[4] itself and the next call faulting at guest 0xFFFFFFEF.
         *
         * ponytail: one block for the whole process, not one per thread.
         * Every guest thread therefore shares LastError. Give this a per-thread
         * allocation when a title is observed to care.
         */
        #define FAKE_TLS_BLOCK_VA  XBOX_LOW_VA(0x00770000)  /* image TLS data          */
        #define FAKE_TLS_THREAD_VA XBOX_LOW_VA(0x00770200)  /* what slot 0 points at   */
        {
            DWORD tls_dir_va = *(const DWORD *)(xbe + XBE_TLS_ADDR_OFFSET);

            if (tls_dir_va) {
                const uint32_t *tls = (const uint32_t *)XBOX_VA(tls_dir_va);
                uint32_t data_start = tls[0];
                uint32_t data_end   = tls[1];
                uint32_t zero_fill  = tls[4];
                uint32_t init_size  = (data_end > data_start)
                                    ? data_end - data_start : 0;
                uint32_t total      = ((init_size + zero_fill + 0xF) & ~0xFu) + 4;

                memset(XBOX_VA(FAKE_TLS_BLOCK_VA), 0, total);
                memset(XBOX_VA(FAKE_TLS_THREAD_VA), 0, 64);
                if (init_size)
                    memcpy(XBOX_VA(FAKE_TLS_BLOCK_VA),
                           XBOX_VA(data_start), init_size);

                MEM32_INIT(FAKE_TLS_BLOCK_VA, FAKE_TLS_THREAD_VA);
                MEM32_INIT(XBOX_FS_BASE + 0x04, FAKE_TLS_BLOCK_VA + total);

                /* KTHREAD.TlsData, which is what fs:[0x28] leads to at +0x28
                 * (Cxbx-Reloaded types.h: KPCR.PrcbData at 0x28, KTHREAD at
                 * PrcbData+0 with TlsData at 0x28). On hardware that is this
                 * block -- a thread's TLS data and the image's TLS block are
                 * the same memory.
                 *
                 * It used to point at FAKE_RWDATA_VA instead, a separate
                 * buffer that is only ever zeroed. The name came from the
                 * first title that needed something there, and "somewhere
                 * writable" was enough for RenderWare, but a title that keeps
                 * real per-thread state in TLS reads zeros. Jet Set Radio
                 * Future reads TlsData+0x10 -- the last dword of its 20-byte
                 * block -- as a function pointer, called it through null, and
                 * relaunched itself from \Device\Cdrom0 rather than start.
                 *
                 * Only when the image has a TLS directory; without one there
                 * is no block to point at and the old buffer still stands. */
                MEM32_INIT(FAKE_TLS_VA + 0x28, FAKE_TLS_BLOCK_VA);

                g_tls_template_va = FAKE_TLS_BLOCK_VA;
                g_tls_total       = total;

                fprintf(stderr, "  TLS: %u-byte block at 0x%08X,"
                        " fs:[4] = 0x%08X (index will be %d)\n",
                        total, FAKE_TLS_BLOCK_VA, FAKE_TLS_BLOCK_VA + total,
                        -(int)(total / 4));
            }
        }
        #undef FAKE_TLS_BLOCK_VA
        #undef FAKE_TLS_THREAD_VA

        fprintf(stderr, "  TIB: fake TIB at VA 0x%X, TLS at 0x%08X, RW data at 0x%08X\n",
                XBOX_FS_BASE, FAKE_TLS_VA, FAKE_RWDATA_VA);

        #undef FAKE_TLS_VA
        #undef FAKE_RWDATA_VA
        #undef MEM32_INIT
        #undef XBOX_VA
    }

    /*
     * Contiguous / physical memory window at 0x80000000.
     *
     * MmAllocateContiguousMemory hands back addresses in this window: physical
     * page P is visible at 0x80000000 + P. Titles that pin buffers at fixed
     * physical addresses then use the whole range, so it has to be backed for
     * its full length - Halo pins 3.4 MB at 0x61000 and 22 MB at 0x3A6000, and
     * with only the fake kernel page mapped here a write walked off the end of
     * it a few pages in.
     *
     * Deliberately NOT a view of the 64 MB RAM mapping. On hardware this window
     * aliases physical RAM, but we load the XBE image into the low addresses of
     * that same region, so aliasing would put a title's pinned pools on top of
     * its own code. Separate storage costs an extra mapping and behaves
     * correctly; nothing here depends on the aliasing.
     *
     * Reserved before the kernel page below, which lives inside it.
     */
    {
        uintptr_t contig_native = XBOX_CONTIG_BASE + g_memory_offset;
        g_contig_mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
            0, (DWORD)XBOX_CONTIG_SIZE, NULL);
        g_contig_memory = g_contig_mapping
            ? MapViewOfFileEx(g_contig_mapping, FILE_MAP_ALL_ACCESS,
                              0, 0, XBOX_CONTIG_SIZE, (LPVOID)contig_native)
            : NULL;
        if (!g_contig_memory)
            g_contig_memory = VirtualAlloc(
                (LPVOID)contig_native,
                XBOX_CONTIG_SIZE,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE
            );
        if (g_contig_memory) {
            fprintf(stderr, "  Contiguous window: %u MB at Xbox VA 0x%08X\n",
                    XBOX_CONTIG_SIZE / (1024 * 1024), XBOX_CONTIG_BASE);
        } else {
            fprintf(stderr, "  WARNING: contiguous window at 0x%08X failed "
                    "(error %lu); pinned physical allocations will fault\n",
                    XBOX_CONTIG_BASE, GetLastError());
        }
    }

    /*
     * NV2A hardware register aperture at 0xFD000000 (16 MB).
     *
     * The GPU's registers are memory-mapped here on real hardware. A title
     * that only calls D3D never notices, but the D3D8 library is linked into
     * the XBE rather than provided by the kernel, so once execution is inside
     * it the register pokes are just loads and stores in recompiled code.
     * Halo faults reading 0xFD001804 during rasterizer_preinitialize, a few
     * instructions after Direct3DCreate8 returns.
     *
     * Backed as ordinary zeroed RAM. That is enough to get through
     * initialisation, and reads returning zero are the benign answer for the
     * status and capability registers touched here.
     *
     * ponytail: plain memory, no register semantics. A spin loop waiting for
     * a bit to *set* would hang rather than fault -- if that shows up, the fix
     * is to bridge the D3D8 entry point that owns the loop, not to start
     * emulating NV2A. Nothing has needed that yet.
     */
    {
        uintptr_t nv2a_native = XBOX_NV2A_BASE + g_memory_offset;
        g_nv2a_memory = VirtualAlloc(
            (LPVOID)nv2a_native,
            XBOX_NV2A_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        /* The pushbuffer survey rides on the same poll, so either
         * variable arms it. */
        s_nv2a_trace = getenv("RECOMP_NV2A_TRACE") != NULL
                    || getenv("RECOMP_PB_SCAN") != NULL
                    || getenv("RECOMP_PB_EXEC") != NULL;
        /* Trap writes to the CRTC interrupt page when the vblank chain runs.
         *
         * PCRTC_INTR_0 is write-1-to-clear and PMC_INTR_0 bit 24 is a summary
         * of it. A guest ISR acknowledges by writing the one and spinning on
         * the other, and against plain memory that spin never ends: Burnout
         * 2's D3D8 does it on the DPC, which this runtime dispatches from the
         * timer thread -- the same thread that raises the next vblank. One
         * frame was delivered and the clock stopped.
         *
         * PAGE_READONLY, not PAGE_NOACCESS: the status registers are read far
         * more than written, and only the write needs different semantics.
         * Only when RECOMP_VBLANK is set, because that is the only thing that
         * raises an interrupt for anyone to acknowledge.
         */
        if (g_nv2a_memory && xbox_EnvSwitch("RECOMP_VBLANK", 1)) {
            DWORD old_nv;
            if (VirtualProtect((char *)g_nv2a_memory + XBOX_NV2A_PCRTC_PAGE,
                               4096, PAGE_READONLY, &old_nv))
                fprintf(stderr, "  NV2A: 0x%08X..0x%08X write-trapped "
                        "(PCRTC interrupt status)\n",
                        XBOX_NV2A_BASE + XBOX_NV2A_PCRTC_PAGE,
                        XBOX_NV2A_BASE + XBOX_NV2A_PCRTC_PAGE + 4096);
            else
                fprintf(stderr, "  WARNING: NV2A PCRTC page protect failed "
                        "(error %lu); the vblank ack will spin\n",
                        GetLastError());
        }

        if (g_nv2a_memory) {
            fprintf(stderr, "  NV2A register aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, no register semantics)\n",
                    XBOX_NV2A_SIZE / (1024 * 1024), XBOX_NV2A_BASE);
        } else {
            fprintf(stderr, "  WARNING: NV2A aperture at 0x%08X failed "
                    "(error %lu); D3D register access will fault\n",
                    XBOX_NV2A_BASE, GetLastError());
        }
    }

    /*
     * MCPX device apertures.
     *
     * The NV2A block above is not the only hardware the title touches
     * directly. The southbridge devices live higher up:
     *
     *   0xFE800000  APU (audio processing unit)
     *   0xFEC00000  AC97
     *   0xFED00000  USB0 / USB1
     *   0xFEF00000  NIC
     *
     * Halo faults reading 0xFED00000 during input initialisation -- the XDK's
     * USB code talks to the host controller's registers rather than going
     * through a driver. Back the whole span as plain RAM for the same reason
     * the NV2A aperture is backed: a read of zero is survivable, a fault is
     * not.
     *
     * ponytail: no register semantics anywhere in here. If something spins
     * waiting for a bit to set, extend the NV2A ack thread's table rather than
     * emulating the device.
     */
    {
        uintptr_t mcpx_native = XBOX_MCPX_BASE + g_memory_offset;
        g_mcpx_memory = VirtualAlloc(
            (LPVOID)mcpx_native,
            XBOX_MCPX_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        g_mcpx_regs = g_mcpx_memory;
        if (g_mcpx_memory) {
            /* AC'97 codec ready.
             *
             * DirectSound resets the codec by setting a bit in 0xFEC0012C and
             * then polls 0xFEC00130 for bit 8 a thousand times waiting for the
             * codec to come up. On zeroed registers that bit never appears, so
             * the wait times out and DirectSoundCreate returns DSERR_NODRIVER
             * (0x88780078).
             *
             * That failure is not confined to audio. Wreckless initialises its
             * whole engine object behind `if (DirectSoundCreate() >= 0)`, so a
             * failed create skips the initialisation, leaves the object's table
             * pointer null, and the null propagates: a null-derived divisor
             * produces a NaN transform matrix, which produces a garbage index,
             * which crashes. Reporting the codec as present is what lets the
             * engine initialise at all.
             *
             * The aperture is plain memory, so setting the bit once is enough:
             * nothing clears it, and the poll reads it on the first pass. */
            #define MCPX_AC97_CODEC_STATUS 0x00400130u   /* 0xFEC00130 */
            #define MCPX_AC97_CODEC_READY  0x00000100u
            /* Opt-in, and not because it is wrong.
             *
             * Reporting the codec is the correct answer -- DSERR_NODRIVER is
             * not what hardware returns -- but it is only correct as far as it
             * goes. DirectSound then hands the audio DSP a command block in
             * RAM and spins until the DSP clears it, and there is no DSP here,
             * so the title trades a late crash for an early hang: 44 assets
             * loaded and then a fault, versus one asset and a stall in audio
             * init. Until the DSP handshake is answered, the honest default is
             * the failure that gets further, with the correct behaviour one
             * variable away. */
            if (xbox_EnvSwitch("RECOMP_AC97_READY", 1)) {
                /* The APU's registers have to fault so they can be routed to
                 * the emulated APU, which is the half that answers the DSP
                 * handshake. Backed as plain memory the guest's writes go
                 * nowhere the APU can see, so it initialises and then waits
                 * forever. Only the APU's own 512K is unmapped: AC'97 above it
                 * stays plain memory, which is what the codec-ready bit needs.
                 *
                 * Enabled by the same variable, because neither half is any
                 * use without the other. */
                /* Registers only, not the DSP's memory.
                 *
                 * The aperture's first 512 KB holds two different kinds of
                 * thing. Up to 0x30000 are registers the emulated APU models:
                 * the PAPU block below 0x20000 and the voice processor from
                 * 0x20000. From 0x30000 up are the GP and EP DSPs' own program
                 * and data memory, which apu_core.c ignores -- so trapping
                 * them buys nothing and costs correctness.
                 *
                 * It cost a crash, not just cycles. A title loads DSP firmware
                 * by copying it into that window, and MSVC compiles the lifted
                 * copy into AVX: the fault at offset 0x30314 was C5 FE 6F 02,
                 * `vmovdqu ymm0, [rdx]`, which the MMIO decoder does not
                 * handle and never should -- a 32-byte vector load is not a
                 * register access. Left as plain memory the copy just lands,
                 * which is what the hardware does and what a stubbed DSP
                 * needs.
                 */
                DWORD old_protect;
                #define XBOX_APU_REG_BYTES 0x00030000u
                if (VirtualProtect((char *)g_mcpx_memory, XBOX_APU_REG_BYTES,
                                   PAGE_NOACCESS, &old_protect))
                    g_apu_mmio_trapped = 1;
                if (g_apu_mmio_trapped)
                    fprintf(stderr, "  APU: 0x%08X..0x%08X trapped for MMIO"
                            " (registers; DSP memory above stays plain)\n",
                            XBOX_MCPX_BASE,
                            XBOX_MCPX_BASE + XBOX_APU_REG_BYTES);
                *(volatile uint32_t *)((char *)g_mcpx_memory
                                       + MCPX_AC97_CODEC_STATUS)
                    |= MCPX_AC97_CODEC_READY;
                fprintf(stderr, "  AC97: codec reported ready at 0x%08X"
                                " (DirectSound will initialise)\n",
                        XBOX_MCPX_BASE + MCPX_AC97_CODEC_STATUS);

                /* Writes to this page trap; reads do not.
                 *
                 * PAGE_READONLY rather than PAGE_NOACCESS, because the two
                 * things on it want opposite treatment. The codec status is
                 * read and has to stay plain memory -- it is polled a
                 * thousand times in a row. The DSP command bytes have to be
                 * caught at the instant they are written, because the wait
                 * that follows reads the byte once and then spins on the
                 * register copy, so anything that changes memory afterwards
                 * arrives too late to be seen.
                 *
                 * Set the codec bit before protecting: afterwards this is not
                 * writable from here either.
                 */
                if (VirtualProtect((char *)g_mcpx_memory + XBOX_MCPX_AC97_PAGE,
                                   4096, PAGE_READONLY, &old_protect)) {
                    fprintf(stderr, "  AC97: 0x%08X..0x%08X write-trapped\n",
                            XBOX_MCPX_BASE + XBOX_MCPX_AC97_PAGE,
                            XBOX_MCPX_BASE + XBOX_MCPX_AC97_PAGE + 4096);
                } else {
                    fprintf(stderr, "  WARNING: AC97 page protect failed "
                            "(error %lu); the DSP wait will not clear\n",
                            GetLastError());
                }
            }
            fprintf(stderr, "  MCPX device aperture: %u MB at Xbox VA "
                    "0x%08X (APU/AC97/USB/NIC, zeroed)\n",
                    XBOX_MCPX_SIZE / (1024 * 1024), XBOX_MCPX_BASE);
        } else {
            fprintf(stderr, "  WARNING: MCPX aperture at 0x%08X failed "
                    "(error %lu); USB/audio register access will fault\n",
                    XBOX_MCPX_BASE, GetLastError());
        }
    }

    /* Flash ROM aperture -- see XBOX_FLASH_BASE for why. */
    {
        uintptr_t flash_native = XBOX_FLASH_BASE + g_memory_offset;

        g_flash_memory = VirtualAlloc(
            (LPVOID)flash_native,
            XBOX_FLASH_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        if (g_flash_memory) {
            fprintf(stderr, "  Flash ROM aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, not a real BIOS image)\n",
                    XBOX_FLASH_SIZE / (1024 * 1024), XBOX_FLASH_BASE);
        } else {
            fprintf(stderr, "  WARNING: flash aperture at 0x%08X failed "
                    "(error %lu); a title reading flash will fault\n",
                    XBOX_FLASH_BASE, GetLastError());
        }
    }

    if (g_nv2a_memory) {
        xbox_Nv2aAckStart();
    }

    /*
     * Allocate a page at Xbox kernel address space (0x80010000).
     *
     * RenderWare's Xbox driver code (xbcache.c) reads MEM32(0x8001003C)
     * to parse the Xbox kernel's PE header and find the INIT section for
     * CPU cache line sizing. On PC, we provide a minimal fake PE header
     * with 0 sections so the function gracefully skips the cache init.
     *
     * The actual native address is 0x80010000 + g_memory_offset.
     */
    {
        #define XBOX_KERNEL_BASE 0x80010000u
        #define KERNEL_PAGE_SIZE 4096
        uintptr_t kernel_native = XBOX_KERNEL_BASE + g_memory_offset;
        /* Already committed if the contiguous window above succeeded -
         * 0x80010000 sits inside it - so just use that storage. */
        g_kernel_memory = g_contig_memory
            ? (void *)kernel_native
            : VirtualAlloc((LPVOID)kernel_native, KERNEL_PAGE_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (g_kernel_memory) {
            /* Zero-fill then set e_lfanew = 0x80 (offset to PE header).
             * With the rest zeroed, NumberOfSections = 0 and the INIT
             * section search finds nothing, which is the safe path. */
            memset(g_kernel_memory, 0, KERNEL_PAGE_SIZE);
            *(uint32_t *)((uint8_t *)g_kernel_memory + 0x3C) = 0x80;  /* e_lfanew */
            fprintf(stderr, "  Kernel: fake PE header at Xbox VA 0x%08X (native %p)\n",
                    XBOX_KERNEL_BASE, g_kernel_memory);
        } else {
            fprintf(stderr, "  WARNING: could not map Xbox kernel VA 0x%08X\n",
                    XBOX_KERNEL_BASE);
        }
        #undef XBOX_KERNEL_BASE
        #undef KERNEL_PAGE_SIZE
    }

    /* Initialize the dynamic heap. */
    fprintf(stderr, "  Heap: %u MB at Xbox VA 0x%08X-0x%08X\n",
            (unsigned)((XBOX_HEAP_TOP - XBOX_HEAP_BASE) / (1024 * 1024)),
            XBOX_HEAP_BASE, XBOX_HEAP_TOP);

    /*
     * Map mirror views of the 64 MB region.
     *
     * On retail Xbox, physical RAM wraps at 64 MB due to the 26-bit
     * address bus. Address 0x04070000 reads the same data as 0x00070000.
     * The RenderWare engine's memory walker crosses 64 MB and accesses
     * mirrored data for an extended walk covering 256+ MB of virtual
     * addresses. Game init code also writes large data structures past
     * 64 MB that on real hardware wrap into physical RAM.
     *
     * We map additional views of the SAME file mapping section at 64 MB
     * intervals. All views alias the same physical pages, so reads and
     * writes at any mirror address correctly access the base data.
     */
    {
        int mirrors_ok = 0;
        /* The tiled aperture is a specific architectural alias -- physical RAM
         * a second time at 0xF0000000, which is where titles render -- while
         * these mirrors are a generic emulation of the address wrap. When the
         * mapped size is large enough that a mirror would cover 0xF0000000,
         * the mirror wins the address and the tiled mapping fails with
         * ERROR_INVALID_ADDRESS; Half-Life 2 then faults on its first surface
         * write. The specific alias is worth more than one wrap mirror, so
         * skip any that would overlap it.
         *
         * Guest addresses, not host: mirror m covers guest
         * (m + 1) * g_memory_size. */
        uint64_t tiled_lo = XBOX_TILED_BASE;
        uint64_t tiled_hi = tiled_lo + xbox_TiledApertureSize();

        for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
            uintptr_t mirror_base = (uintptr_t)g_memory_base +
                                    (uintptr_t)(m + 1) * g_memory_size;
            uint64_t guest_lo = (uint64_t)(m + 1) * g_memory_size;
            uint64_t guest_hi = guest_lo + g_memory_size;

            if (guest_lo < tiled_hi && tiled_lo < guest_hi) {
                fprintf(stderr, "  Mirror %d: skipped, overlaps the tiled"
                                " aperture at 0x%08X\n",
                        m + 1, (unsigned)XBOX_TILED_BASE);
                continue;
            }
            g_mirror_views[m] = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                g_memory_size,
                (LPVOID)mirror_base
            );
            if (g_mirror_views[m]) {
                mirrors_ok++;
            } else {
                fprintf(stderr, "  Mirror %d: FAILED at %p (error %lu)\n",
                        m + 1, (void *)mirror_base, GetLastError());
            }
        }
        fprintf(stderr, "  RAM mirror: %d/%d views mapped (covers %d MB)\n",
                mirrors_ok, XBOX_NUM_MIRRORS,
                (int)((mirrors_ok + 1) * g_memory_size / (1024 * 1024)));
    }

    /*
     * Tiled / write-combined aperture at 0xF0000000.
     *
     * The NV2A exposes physical RAM a second time here and titles render
     * through it. Wreckless's first surface write goes to guest 0xF1954000 --
     * the tiled alias of physical 0x01954000, already inside our RAM -- and
     * faulted because nothing was mapped there.
     *
     * A view of the same section rather than fresh storage: the title writes a
     * surface through the tiled address and reads it back through the normal
     * one, so the two have to be the same bytes. That is the whole reason the
     * RAM lives in a file mapping.
     */
    {
        uintptr_t tiled_native = XBOX_TILED_BASE + g_memory_offset;
        size_t tiled_size = xbox_TiledApertureSize();
        /* A view of the CONTIGUOUS window, not of RAM.
         *
         * On hardware all three -- physical P, 0x80000000+P and 0xF0000000+P
         * -- are one and the same memory. Here they cannot be: the XBE image
         * is loaded at its own VA in the RAM mapping, so aliasing the
         * contiguous window onto RAM would drop a title's pinned physical
         * pools on top of its own code (Halo pins 3.4 MB at 0x61000, which is
         * inside its image). The contiguous window therefore has separate
         * storage, and the question becomes which of the two the tiled
         * aperture should be a view of.
         *
         * It is the contiguous one. A tiled address is a GPU surface address
         * by construction, and GPU surfaces come from
         * MmAllocateContiguousMemory -- so the pairing that has to hold is
         * tiled to contiguous. Against RAM instead, Half-Life 2's loader wrote
         * every decoded video frame through 0xF1C63000 while D3D sampled the
         * texture at 0x81C63000, and the sampler read zeros: 1.8 billion black
         * pixels rasterised, perfectly, from an empty texture.
         */
        if (tiled_size > XBOX_CONTIG_SIZE)
            tiled_size = XBOX_CONTIG_SIZE;
        g_tiled_view = g_contig_mapping
            ? MapViewOfFileEx(
                g_contig_mapping,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                tiled_size,
                (LPVOID)tiled_native)
            : NULL;
        if (g_tiled_view) {
            /* Prove the alias rather than assert it. Everything the title
             * renders goes through this window and is read back through the
             * physical address, so if the two are not the same bytes the GPU
             * sees empty buffers and the screen stays black -- with nothing
             * anywhere to say why. One write and one read turns that into a
             * startup line. */
            {
                volatile uint32_t *via_tiled =
                    (volatile uint32_t *)((uintptr_t)(XBOX_TILED_BASE + 0x1000)
                                          + g_memory_offset);
                volatile uint32_t *via_contig =
                    (volatile uint32_t *)((uintptr_t)(XBOX_CONTIG_BASE + 0x1000)
                                          + g_memory_offset);
                uint32_t saved = *via_contig;

                *via_tiled = 0xA5C30F17u;
                if (*via_contig != 0xA5C30F17u)
                    fprintf(stderr, "  WARNING: tiled aperture does NOT alias"
                            " the contiguous window (wrote A5C30F17, read"
                            " %08X) -- the GPU will sample empty textures\n",
                            *via_contig);
                else
                    fprintf(stderr, "  Tiled aperture alias verified"
                            " (tiled 0x%08X == contiguous 0x%08X)\n",
                            XBOX_TILED_BASE, XBOX_CONTIG_BASE);
                *via_contig = saved;
            }
            /* RECOMP_ALIAS_HIGH=1: a second alias of the same window at
             * 0xF8000000, for finding out whether a title that dereferences
             * an address there means physical memory or is simply holding a
             * bad pointer. Outrun 2 faults on a this-pointer of 0xF8604020,
             * which is a valid cached pointer 0x80604020 biased by
             * 0x78000000 -- suggestive, but suggestive is not evidence, and
             * mapping it settles the question in one run: real memory and
             * the title carries on, garbage and it faults again at once.
             *
             * Off by default. Nothing is known to need it, and silently
             * backing an address no console ever had would hide the next
             * title's bad pointer instead of reporting it. */
            if (xbox_EnvSwitch("RECOMP_ALIAS_HIGH", 0) && g_contig_mapping) {
                void *high = MapViewOfFileEx(
                    g_contig_mapping, FILE_MAP_ALL_ACCESS, 0, 0, tiled_size,
                    (LPVOID)(uintptr_t)(0xF8000000u + g_memory_offset));
                fprintf(stderr, "  RECOMP_ALIAS_HIGH: 0x%08X %s\n", 0xF8000000u,
                        high ? "aliased to the contiguous window (an experiment)"
                             : "could not be mapped");
            }
            fprintf(stderr, "  Tiled aperture: %u MB at Xbox VA 0x%08X"
                    " (aliases the contiguous window)\n",
                    (unsigned)(g_memory_size / (1024 * 1024)),
                    XBOX_TILED_BASE);
        } else {
            fprintf(stderr, "  WARNING: tiled aperture at 0x%08X failed"
                    " (error %lu); rendering writes will fault\n",
                    XBOX_TILED_BASE, GetLastError());
        }
    }

    fprintf(stderr, "xbox_MemoryLayoutInit: complete\n");
    return TRUE;
}

/*
 * Make every RAM mirror read-only, for finding writes that reach low memory
 * through an alias.
 *
 * Xbox RAM is visible at 28 virtual addresses that alias the same pages, so a
 * store to 0x04000004 changes Xbox VA 4 without ever touching VA 4. Both a
 * page-protection watchpoint and a DR0 hardware watchpoint on VA 4 therefore
 * report nothing while the memory demonstrably changes -- which is exactly
 * what happened chasing Halo's fs:[4] corruption.
 *
 * Debug aid, not part of normal startup: a title that legitimately writes
 * through a mirror will fault here too, and the fault address names the alias
 * and the code.
 */
void xbox_ProtectMirrorsForDebug(void)
{
    int n = 0;
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        DWORD old;
        if (g_mirror_views[m] &&
            VirtualProtect(g_mirror_views[m], g_memory_size,
                           PAGE_READONLY, &old)) {
            n++;
        }
    }
    fprintf(stderr, "  Mirrors: %d/%d made read-only (debug)\n",
            n, XBOX_NUM_MIRRORS);
}

void xbox_MemoryLayoutShutdown(void)
{
    if (g_kernel_memory) {
        VirtualFree(g_kernel_memory, 0, MEM_RELEASE);
        g_kernel_memory = NULL;
    }
    if (g_nv2a_ack_thread) {
        InterlockedExchange(&g_nv2a_ack_stop, 1);
        WaitForSingleObject(g_nv2a_ack_thread, 1000);
        CloseHandle(g_nv2a_ack_thread);
        g_nv2a_ack_thread = NULL;
    }
    if (g_nv2a_memory) {
        VirtualFree(g_nv2a_memory, 0, MEM_RELEASE);
        g_nv2a_memory = NULL;
    }
    /* Unmap mirror views first */
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        if (g_mirror_views[m]) {
            UnmapViewOfFile(g_mirror_views[m]);
            g_mirror_views[m] = NULL;
        }
    }
    /* Unmap base view */
    if (g_memory_base) {
        UnmapViewOfFile(g_memory_base);
        g_memory_base = NULL;
        g_memory_size = 0;
    }
    /* Close file mapping handle */
    if (g_mapping_handle) {
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
    }
    fprintf(stderr, "xbox_MemoryLayoutShutdown: released\n");
}

/* Bump allocator for pure address-space reservations, above RAM.
 *
 * A MEM_RESERVE costs no memory on real hardware -- it takes address space out
 * of a 4 GB range, not pages out of the 64 MB the console has -- so titles
 * reserve far more than exists and commit a fraction. Satisfying that out of
 * the RAM heap does not work: Half-Life 2 asks for 128 MB and then 200 MB, and
 * clamping those to what the heap can back left it sub-allocating across a
 * range it believed it owned, walking past the top of RAM and aliasing low
 * memory through the mirrors.
 *
 * So reservations come from the mapped space *above* RAM instead. Those pages
 * are already backed and distinct, nothing else hands them out, and a commit
 * inside one is a no-op because it is real memory already.
 *
 * Returns 0 when the mapping is no larger than RAM -- the default for titles
 * that never call xbox_SetMapSize -- which leaves the old behaviour untouched.
 *
 * ponytail: a bump allocator with no free. A reservation is address space, the
 * range is large, and a title that reserves and releases repeatedly would need
 * a real allocator; none has yet.
 */
static uint32_t g_reserve_next;

uint32_t xbox_ReserveAlloc(uint32_t size, uint32_t align)
{
    uint32_t base;

    if (g_memory_size <= g_xbox_total_ram || size == 0)
        return 0;
    if (!align)
        align = 4096;
    if (!g_reserve_next)
        g_reserve_next = (uint32_t)g_xbox_total_ram;

    base = (g_reserve_next + align - 1) & ~(align - 1);
    if ((size_t)base + size > g_memory_size)
        return 0;
    g_reserve_next = base + size;
    return base;
}

BOOL xbox_IsXboxAddress(uintptr_t address)
{
    return (address >= XBOX_BASE_ADDRESS &&
            address < XBOX_BASE_ADDRESS + g_memory_size);
}

void *xbox_GetMemoryBase(void)
{
    return g_memory_base;
}

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return g_memory_offset;
}

/* ── Dynamic heap allocator ────────────────────────────────
 *
 * Simple bump allocator for MmAllocateContiguousMemory and similar.
 * Returns Xbox VAs within the mapped region so MEM32() works correctly.
 * No free support (bump-only for now).
 */
/* Set by xbox_MemoryLayoutInit: XBOX_HEAP_BASE moves with the image
 * (see XBOX_LOW_VA), so it is not a constant any more. */
static uint32_t g_heap_next;

static int g_heap_alloc_count = 0;

/* Block table backing xbox_HeapFree. A bump pointer alone never reclaims,
 * which is fine for a title that allocates once and fatal for a debug build
 * that churns. Flat array rather than an intrusive list: allocations come back
 * in bump order, so index order is address order and coalescing is a
 * neighbour check. */
#define XBOX_HEAP_MAX_BLOCKS 65536
static struct { uint32_t addr; uint32_t size; uint8_t free; }
    g_heap_blocks[XBOX_HEAP_MAX_BLOCKS];
static int g_heap_block_count = 0;

/*
 * Simulated stacks for spawned threads.
 *
 * The main thread owns the top of the XBOX_STACK region and grows down; worker
 * stacks are carved from the bottom upward so the two cannot meet until the
 * whole 8 MB is gone. Xbox VAs, not host memory: recompiled code addresses its
 * stack through MEM32() like any other Xbox pointer.
 */
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

#define XBOX_THREAD_STACK_SIZE  (512 * 1024)
#define XBOX_MAX_THREAD_STACKS  8

static int g_thread_stacks_used = 0;

/* A TIB and TLS block for a newly spawned guest thread.
 *
 * A TIB is per-thread on the console and was per-process here: one address,
 * 0x1000, for everyone. Two things live in it that must not be shared. fs:[0]
 * is the SEH chain head, so two threads unwinding at once walk each other's
 * frames. fs:[4] points at the image's TLS block, whose slot 0 is the CRT's
 * per-thread data -- errno, the locale, and the bookkeeping _lock() uses to
 * decide who owns which lock.
 *
 * Half-Life 2 deadlocked on the last of those: two threads inside _lock(),
 * each holding the CRT lock the other was waiting for, because "which thread
 * am I" was a single shared answer.
 *
 * The new block is a copy of the template the loader built, so a thread starts
 * with the image's initialised thread-local data rather than zeros, and its
 * own per-thread structure behind slot 0.
 */
uint32_t xbox_AllocThreadTib(void)
{
    /* XBOX_VA is scoped to the loader; the same arithmetic, spelled here. */
    #define TIB_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
    const uint32_t tib_size = 0x40;
    uint32_t tib, block, thread_data, total;

    if (!g_tls_total)
        return 0;                    /* image has no TLS; nothing to copy */

    total = g_tls_total;
    tib = xbox_HeapAlloc(tib_size + total + g_tls_thread_size, 16);
    if (!tib)
        return 0;
    block       = tib + tib_size;
    thread_data = block + total;

    /* The TIB itself, copied so stack bounds and the fields the title filled
     * in are inherited, then the two that must not be. */
    memcpy(TIB_VA(tib), TIB_VA(XBOX_TIB_MAIN), tib_size);
    memcpy(TIB_VA(block), TIB_VA(g_tls_template_va), total);
    memset(TIB_VA(thread_data), 0, g_tls_thread_size);

    *(uint32_t *)TIB_VA(tib + 0x00) = 0xFFFFFFFFu;   /* own SEH chain    */
    *(uint32_t *)TIB_VA(block)      = thread_data;   /* slot 0           */
    *(uint32_t *)TIB_VA(tib + 0x04) = block + total; /* fs:[4], see above*/

    /* This thread's own current-thread object.
     *
     * The copy above inherited fs:[0x28] from the main thread, so without
     * this every guest thread answers "which thread am I" with the same
     * value. Copy the main thread's object so every field a title already
     * relies on is inherited -- the RenderWare data pointer at +0x28 among
     * them -- and change only the identity. */
    {
        uint32_t obj = xbox_HeapAlloc(XBOX_THREAD_OBJ_SIZE, 16);
        if (obj) {
            memcpy(TIB_VA(obj), TIB_VA(XBOX_THREAD_OBJ_MAIN),
                   XBOX_THREAD_OBJ_SIZE);
            *(uint32_t *)TIB_VA(obj + XBOX_THREAD_ID_OFF) =
                g_next_guest_thread_id++;
            *(uint32_t *)TIB_VA(tib + 0x28) = obj;
        }
    }

    return tib;
    #undef TIB_VA
}

uint32_t xbox_AllocThreadStack(void)
{
    uint32_t base;

    if (g_thread_stacks_used >= XBOX_MAX_THREAD_STACKS) {
        return 0;
    }

    /* From the heap, not from XBOX_STACK_BASE.
     *
     * The stack region begins at 0x00780000, which is fine only while the
     * title's image ends below that. Half-Life 2's image runs to 0x009B68C0,
     * so the first thread stack (0x00780000..0x00800000) landed inside its
     * .rdata and .data: the worker spawned during engine init wrote its
     * frames over the game's own static data. Nothing faults -- the pages are
     * mapped and writable -- so it shows up later as globals that were
     * correct when written and wrong when read.
     *
     * The heap already starts above the image and knows how big it is, so
     * taking slices from it is correct for any image size instead of only
     * for small ones.
     */
    base = xbox_HeapAlloc(XBOX_THREAD_STACK_SIZE, 4096);
    if (!base)
        return 0;
    g_thread_stacks_used++;

    /* Top of the slice, 16-byte aligned, growing down. */
    return base + XBOX_THREAD_STACK_SIZE - 16;
}

/* Give a worker's stack back when the worker ends.
 *
 * The counter used to only ever go up, so a title that creates and destroys
 * threads ran the pool dry no matter how few were alive at once. The Xbox
 * Dashboard spawns one worker per ambient WAV and terminates it before loading
 * the next; after XBOX_MAX_THREAD_STACKS files the pool was empty and
 * PsCreateSystemThreadEx fell back to running the worker inline. That fallback
 * is a deadlock here rather than a slowdown: the worker ran to completion
 * before the caller reached its wait, so the main thread then waited forever on
 * events whose only signaller had already finished. It looked like an audio
 * hang, three layers away from the cause.
 *
 * Takes the value AllocThreadStack returned, so callers never do the arithmetic.
 */
void xbox_FreeThreadStack(uint32_t stack_top)
{
    if (!stack_top)
        return;
    xbox_HeapFree(stack_top + 16 - XBOX_THREAD_STACK_SIZE);
    if (g_thread_stacks_used > 0)
        g_thread_stacks_used--;
}

/* Bump allocator over the contiguous window mapped at XBOX_CONTIG_BASE.
 *
 * MmAllocateContiguousMemory hands back physical memory, and on Xbox physical
 * page P is visible at 0x80000000 + P. Drivers rely on that being an exact
 * round trip: Xbox D3D writes its pushbuffer position to the NV2A as
 * `VA & 0x0FFFFFFF` and reads the GPU's position back as `GET | 0x80000000`,
 * then compares the two. That holds for any address in this window and for
 * nothing in the general heap, whose position depends on what the title
 * reserved first -- Half-Life 2 reserves 128 MB and then 200 MB before D3D
 * allocates its pushbuffer, which put the buffer at 0x15782000 and left the
 * engine comparing 0x857844C0 against it forever.
 *
 * Grows up from the base; XBOX_GPU_INSTANCE_DEFAULT is carved off the top by
 * the GPU-instance bridge, so the two do not meet until the window is full.
 *
 * Freed blocks are reused. The arena was bump-only on the assumption that
 * contiguous blocks are framebuffers and pushbuffers, allocated once. Dino
 * Crisis 3 also takes each stage's data from here, megabytes at a time, and
 * frees it again -- and MmFreeContiguousMemory handed the address to the
 * general heap, which does not own it, so nothing ever came back. Its front
 * end left 62 MB of the 64 MB window "in use"; the first stage's 5 MB request
 * then returned NULL, the title read the stage file into NULL + offset, over
 * its own code and data, and called through what the file put there (the
 * 0x04XX04XX "function pointers" of its long-standing crash). */
static uint32_t g_contig_next =
    XBOX_CONTIG_BASE + XBOX_CONTIG_RESERVED_LOW;

/* Every contiguous block, live or free, in address order.
 *
 * MmQueryAllocationSize has to be able to answer for these addresses, and
 * without a record the only honest answer is zero. DirectSound allocates its
 * DSP buffers here and then asks how big they are; a zero told it the block
 * was not real, and it retried, which is why a run spent 103 of its last 400
 * kernel calls back in MmAllocateContiguousMemoryEx.
 *
 * `size` is what the caller asked for, `span` the page-rounded extent the
 * block owns. Free blocks are reused first-fit, split when the remainder is a
 * page or more, and merged with free neighbours; a free block at the top of
 * the arena is given back to the bump pointer.
 */
#define XBOX_CONTIG_MAX_BLOCKS 4096
#define XBOX_CONTIG_PAGE       4096u
static struct {
    uint32_t addr, size, span;
    int      free;
} g_contig_blocks[XBOX_CONTIG_MAX_BLOCKS];
static int g_contig_block_count;
static uint32_t g_contig_frees, g_contig_reuses;

static uint32_t contig_round(uint32_t n)
{
    return (n + XBOX_CONTIG_PAGE - 1u) & ~(XBOX_CONTIG_PAGE - 1u);
}

/* Insert a block record at index i, shifting the rest up. 0 if full. */
static int contig_insert(int i, uint32_t addr, uint32_t size, uint32_t span,
                         int free)
{
    if (g_contig_block_count >= XBOX_CONTIG_MAX_BLOCKS)
        return 0;
    memmove(&g_contig_blocks[i + 1], &g_contig_blocks[i],
            (size_t)(g_contig_block_count - i) * sizeof g_contig_blocks[0]);
    g_contig_blocks[i].addr = addr;
    g_contig_blocks[i].size = size;
    g_contig_blocks[i].span = span;
    g_contig_blocks[i].free = free;
    g_contig_block_count++;
    return 1;
}

static void contig_remove(int i)
{
    memmove(&g_contig_blocks[i], &g_contig_blocks[i + 1],
            (size_t)(g_contig_block_count - i - 1) * sizeof g_contig_blocks[0]);
    g_contig_block_count--;
}

uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t result, span;
    int i;

    if (alignment < XBOX_CONTIG_PAGE) alignment = XBOX_CONTIG_PAGE;
    span = contig_round(size ? size : 1u);

    /* First fit among freed blocks. Only a block that already starts on the
     * alignment is taken: every block starts on a page, and asking for more
     * than a page is rare enough that the bump pointer can serve it. */
    for (i = 0; i < g_contig_block_count; i++) {
        uint32_t spare;
        if (!g_contig_blocks[i].free || g_contig_blocks[i].span < span ||
            (g_contig_blocks[i].addr & (alignment - 1u)))
            continue;
        spare = g_contig_blocks[i].span - span;
        if (spare >= XBOX_CONTIG_PAGE &&
            contig_insert(i + 1, g_contig_blocks[i].addr + span, 0u, spare, 1))
            g_contig_blocks[i].span = span;
        g_contig_blocks[i].free = 0;
        g_contig_blocks[i].size = size;
        g_contig_reuses++;
        result = g_contig_blocks[i].addr;
        memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
        return result;
    }

    result = (g_contig_next + alignment - 1) & ~(alignment - 1);

    /* Leave the top of the window for GPU instance memory. */
    if ((uint64_t)result + span >
            (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE
                - XBOX_GPU_INSTANCE_DEFAULT) {
        uint32_t live = 0;
        for (i = 0; i < g_contig_block_count; i++)
            if (!g_contig_blocks[i].free)
                live += g_contig_blocks[i].span;
        fprintf(stderr, "  [CONTIG] arena exhausted (%u requested, %u of %u "
                        "used; %u live in blocks, %u frees, %u reuses)\n",
                size, g_contig_next - XBOX_CONTIG_BASE,
                (unsigned)XBOX_CONTIG_SIZE, live, g_contig_frees,
                g_contig_reuses);
        fflush(stderr);
        return 0;
    }

    /* The gap an alignment above a page leaves is a free block too. */
    if (result > g_contig_next)
        contig_insert(g_contig_block_count, g_contig_next, 0u,
                      result - g_contig_next, 1);
    g_contig_next = result + span;

    if (!contig_insert(g_contig_block_count, result, size, span, 0)) {
        static int said;
        if (!said++) {
            fprintf(stderr, "  [CONTIG] more than %d blocks: later ones cannot "
                            "be freed\n", XBOX_CONTIG_MAX_BLOCKS);
            fflush(stderr);
        }
    }

    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
    return result;
}

/* MmFreeContiguousMemory. Returns 0 when `xbox_va` is not the start of a live
 * block this arena handed out (a pinned MmAllocateContiguousMemoryEx address,
 * say), which the caller may then treat as not contiguous at all. */
int xbox_ContiguousFree(uint32_t xbox_va)
{
    int i;

    for (i = 0; i < g_contig_block_count; i++)
        if (g_contig_blocks[i].addr == xbox_va && !g_contig_blocks[i].free)
            break;
    if (i == g_contig_block_count)
        return 0;

    g_contig_blocks[i].free = 1;
    g_contig_blocks[i].size = 0u;
    if (++g_contig_frees <= 4) {
        fprintf(stderr, "  [CONTIG] free #%u va=0x%08X (%u bytes)\n",
                g_contig_frees, xbox_va, g_contig_blocks[i].span);
        fflush(stderr);
    }

    /* Merge with a free neighbour on either side. */
    if (i + 1 < g_contig_block_count && g_contig_blocks[i + 1].free &&
        g_contig_blocks[i].addr + g_contig_blocks[i].span == g_contig_blocks[i + 1].addr) {
        g_contig_blocks[i].span += g_contig_blocks[i + 1].span;
        contig_remove(i + 1);
    }
    if (i > 0 && g_contig_blocks[i - 1].free &&
        g_contig_blocks[i - 1].addr + g_contig_blocks[i - 1].span == g_contig_blocks[i].addr) {
        g_contig_blocks[i - 1].span += g_contig_blocks[i].span;
        contig_remove(i);
        i--;
    }

    /* A free block at the top goes back to the bump pointer. */
    if (i == g_contig_block_count - 1 &&
        g_contig_blocks[i].addr + g_contig_blocks[i].span == g_contig_next) {
        g_contig_next = g_contig_blocks[i].addr;
        contig_remove(i);
    }
    return 1;
}

/* Pinned physical ranges.
 *
 * MmAllocateContiguousMemoryEx with a range that brackets exactly one
 * allocation names a physical address -- XPhysicalAlloc(size, phys, ...) sends
 * low = phys, high = phys + size - 1 -- and the bridge answers it with that
 * address without asking this arena. The arena still has to know: its free
 * blocks and its bump pointer can cover the pinned pages, and the next
 * ordinary request is then carved across them. Halo pins its game state at
 * physical 0x61000 and its tag cache at 0x3A6000 after its first D3D device
 * has freed the blocks there; its 22 MB texture cache, asked for anywhere,
 * landed on both, and texture uploads overwrote the game state.
 *
 * So a pin takes the pages it covers out of the arena. Free blocks are split
 * around it, and a pin above the bump pointer moves the pointer past it (the
 * gap below becomes a free block). Pages a live block already holds stay with
 * it and are reported: the title asked for that address, and moving either
 * would be wrong. The pinned part becomes a live block of its own, so
 * MmFreeContiguousMemory on the pinned address gives it back. */
static int contig_take_free(int i, uint32_t lo, uint32_t hi)
{
    uint32_t a = g_contig_blocks[i].addr;
    uint32_t e = a + g_contig_blocks[i].span;

    if (lo < a) lo = a;
    if (hi > e) hi = e;
    if (hi < e)                                   /* the free tail */
        contig_insert(i + 1, hi, 0u, e - hi, 1);
    if (lo > a) {                                 /* the free head */
        g_contig_blocks[i].span = lo - a;
        contig_insert(i + 1, lo, hi - lo, hi - lo, 0);
        return i + 2;
    }
    g_contig_blocks[i].span = hi - lo;
    g_contig_blocks[i].size = hi - lo;
    g_contig_blocks[i].free = 0;
    return i + 1;
}

void xbox_ContiguousPin(uint32_t xbox_va, uint32_t size)
{
    uint32_t lo = xbox_va & ~(XBOX_CONTIG_PAGE - 1u);
    uint32_t hi = contig_round(xbox_va + (size ? size : 1u));
    uint32_t floor = XBOX_CONTIG_BASE + XBOX_CONTIG_RESERVED_LOW;
    uint32_t top = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE - XBOX_GPU_INSTANCE_DEFAULT;
    uint32_t bump_was = g_contig_next, live_lo = 0, live_hi = 0;
    int i;

    if (lo < floor)
        lo = floor;             /* below the arena: never handed out anyway */
    if (hi > top)
        hi = top;
    if (hi <= lo)
        return;
    for (i = 0; i < g_contig_block_count; ) {
        uint32_t a = g_contig_blocks[i].addr;
        uint32_t e = a + g_contig_blocks[i].span;

        if (e <= lo) { i++; continue; }
        if (a >= hi) break;
        if (!g_contig_blocks[i].free) {
            if (!live_lo) live_lo = a;
            live_hi = e;
            i++;
            continue;
        }
        i = contig_take_free(i, lo, hi);
    }
    if (hi > g_contig_next) {
        uint32_t start = lo > g_contig_next ? lo : g_contig_next;

        if (start > g_contig_next)
            contig_insert(g_contig_block_count, g_contig_next, 0u,
                          start - g_contig_next, 1);
        contig_insert(g_contig_block_count, start, hi - start, hi - start, 0);
        g_contig_next = hi;
    }
    fprintf(stderr, "  [CONTIG] pinned 0x%08X..0x%08X taken out of the arena "
                    "(bump pointer 0x%08X -> 0x%08X)\n",
            lo, hi, bump_was, g_contig_next);
    if (live_lo)
        fprintf(stderr, "  [CONTIG] pinned 0x%08X..0x%08X overlaps live blocks "
                        "0x%08X..0x%08X; both now use those pages\n",
                lo, hi, live_lo, live_hi);
    fflush(stderr);
}

/* Walk the contiguous blocks this runtime handed out.
 *
 * Exposed because the DirectSound DSP doorbell lives inside one of them and
 * its address is not derivable from any APU register: GPSADDR and friends
 * point at the DSP's own scratch, while the command block is an ordinary
 * contiguous allocation. Rather than have each title name the address, the
 * APU can look for it.
 *
 * Returns 0 when `index` is past the end, so a caller can just count up. A
 * freed block reads as size 0.
 */
int xbox_ContiguousBlock(int index, uint32_t *addr, uint32_t *size)
{
    if (index < 0 || index >= g_contig_block_count)
        return 0;
    if (addr) *addr = g_contig_blocks[index].addr;
    if (size) *size = g_contig_blocks[index].free ? 0u : g_contig_blocks[index].size;
    return 1;
}

/* How much of the window has been handed out.
 *
 * Lets a caller holding a physical address decide whether it names contiguous
 * memory this runtime allocated. The pushbuffer executor needs exactly that:
 * a surface offset is physical, and only the window makes it addressable. */
uint32_t xbox_ContiguousAllocatedBytes(void)
{
    return g_contig_next - XBOX_CONTIG_BASE;
}


uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t result;

    if (alignment < 4) alignment = 4;
    if (!g_heap_next)
        g_heap_next = XBOX_HEAP_BASE;

    /* Enforce minimum allocation size.
     * The Xbox D3D8 code sometimes computes resource sizes from GPU
     * capabilities that return 0 (since we don't have real NV2A hardware),
     * resulting in zero-size allocations. With a bump allocator, these all
     * return the same address, causing overlapping structures. Enforce a
     * minimum of 4096 bytes so each allocation gets its own memory. */
    if (size < 16) size = 16;

    /* Reuse a freed block first. Without this the heap only ever grows: Halo's
     * debug build allocates and releases heavily through init, exhausted all
     * 48 MB in 4,726 allocations, and its second D3D CreateDevice then failed
     * with E_OUTOFMEMORY -- which the title reports by clearing
     * global_d3d_device, so the rasterizer asserts and startup stops. */
    for (int i = 0; i < g_heap_block_count; i++) {
        if (!g_heap_blocks[i].free || g_heap_blocks[i].size < size) {
            continue;
        }
        if (g_heap_blocks[i].addr & (alignment - 1)) {
            continue;   /* wrong alignment for this request */
        }
        /* Hand back only what was asked for, and keep the rest available.
         *
         * Taking the whole block is what first-fit does if you let it, and
         * the waste is not marginal: free a 29 MB heap, ask for 16 bytes,
         * and the 29 MB goes with it until that 16-byte pointer is freed.
         * Mortal Kombat: Deadly Alliance allocates and frees exactly that
         * sized block while sizing its heaps.
         *
         * The remainder becomes its own free block immediately after this
         * one. Index order is address order -- xbox_HeapFree's coalescing
         * depends on that -- so it is inserted at i + 1 rather than appended,
         * and the two merge back together when this block is freed.
         *
         * doaxbv-re (GPL-3.0) fixed the same exhaustion the same way; this is
         * the same idea written against our block table.
         */
        {
            uint32_t spare = g_heap_blocks[i].size - size;
            /* Not worth a table entry, and a split that leaves a few bytes
             * fragments the heap faster than it saves it. */
            if (spare >= 64u && g_heap_block_count < XBOX_HEAP_MAX_BLOCKS) {
                memmove(&g_heap_blocks[i + 2], &g_heap_blocks[i + 1],
                        (size_t)(g_heap_block_count - i - 1)
                            * sizeof g_heap_blocks[0]);
                g_heap_block_count++;
                g_heap_blocks[i + 1].addr = g_heap_blocks[i].addr + size;
                g_heap_blocks[i + 1].size = spare;
                g_heap_blocks[i + 1].free = 1;
                g_heap_blocks[i].size = size;
            }
        }
        g_heap_blocks[i].free = 0;
        result = g_heap_blocks[i].addr;
        memset((void *)((uintptr_t)result + g_memory_offset), 0,
               g_heap_blocks[i].size);
        return result;
    }

    /* Align the next pointer */
    result = (g_heap_next + alignment - 1) & ~(alignment - 1);

    if (result + size > XBOX_HEAP_TOP) {
        fprintf(stderr, "xbox_HeapAlloc: out of memory (requested %u, used %u/%u)\n",
                size, g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        /* Who ate the heap? Group live blocks by size -- an exhausted heap is
         * nearly always one request size repeated, and the count names it. */
        {
            static int dumped = 0;
            static struct { uint32_t size; int n; } hist[256];
            if (!dumped) {
                int used = 0;
                dumped = 1;
                for (int i = 0; i < g_heap_block_count; i++) {
                    int j = 0;
                    if (g_heap_blocks[i].free || !g_heap_blocks[i].size) continue;
                    while (j < used && hist[j].size != g_heap_blocks[i].size) j++;
                    if (j == used) {
                        if (used == 256) continue;   /* ponytail: 256 distinct sizes is plenty */
                        hist[used].size = g_heap_blocks[i].size;
                        hist[used++].n = 0;
                    }
                    hist[j].n++;
                }
                for (int j = 0; j < used; j++) {
                    if ((uint64_t)hist[j].n * hist[j].size < 1024 * 1024) continue;
                    fprintf(stderr, "  [HEAP] %d live blocks of %u bytes (%u KB)\n",
                            hist[j].n, hist[j].size,
                            (unsigned)((uint64_t)hist[j].n * hist[j].size / 1024));
                }
                fflush(stderr);
            }
        }
        return 0;
    }

    g_heap_next = result + size;

    /* Zero-fill the allocated block (Xbox memory is always zeroed) */
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);

    if (g_heap_block_count < XBOX_HEAP_MAX_BLOCKS) {
        g_heap_blocks[g_heap_block_count].addr = result;
        g_heap_blocks[g_heap_block_count].size = size;
        g_heap_blocks[g_heap_block_count].free = 0;
        g_heap_block_count++;
    }

    g_heap_alloc_count++;
    /* Rate-limited: a debug title makes thousands of these and the log is a
     * diagnostic, not a transaction record. */
    if (g_heap_alloc_count <= 32 || (g_heap_alloc_count % 512) == 0) {
        fprintf(stderr, "  [HEAP] #%d: size=%u align=%u → 0x%08X..0x%08X (used %u/%u)\n",
                g_heap_alloc_count, size, alignment, result, result + size,
                g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        fflush(stderr);
    }

    return result;
}

/* How big is the block at this guest address?
 *
 * MmQueryAllocationSize and ExQueryPoolBlockSize both ask this, and both used
 * to answer 0 -- ExQueryPoolBlockSize by returning a literal, and
 * MmQueryAllocationSize by having no bridge at all. The host cannot answer it:
 * VirtualQuery on the translated address reports the size of the whole 64 MB
 * guest mapping, which is a worse answer than none. The block table already
 * has the real one, and it is the same table xbox_HeapFree matches against.
 *
 * Interior addresses count: a title that asks about a pointer it has walked
 * forward is asking about the block that contains it. Returns 0 for an address
 * this heap never handed out, which is what "not one of mine" has to look like.
 */
uint32_t xbox_HeapBlockSize(uint32_t xbox_va)
{
    int i;

    if (!xbox_va)
        return 0;
    for (i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].free)
            continue;
        if (xbox_va >= g_heap_blocks[i].addr &&
            xbox_va <  g_heap_blocks[i].addr + g_heap_blocks[i].size)
            return g_heap_blocks[i].size - (xbox_va - g_heap_blocks[i].addr);
    }

    /* Contiguous memory is a separate arena, and a caller asking about a
     * block from MmAllocateContiguousMemory is asking the same question. */
    for (i = 0; i < g_contig_block_count; i++) {
        if (g_contig_blocks[i].free)
            continue;
        if (xbox_va >= g_contig_blocks[i].addr &&
            xbox_va <  g_contig_blocks[i].addr + g_contig_blocks[i].size)
            return g_contig_blocks[i].size - (xbox_va - g_contig_blocks[i].addr);
    }

    return 0;
}

void xbox_HeapFree(uint32_t xbox_va)
{
    static int frees = 0, matched = 0;

    if (!xbox_va) {
        return;
    }
    frees++;
    if (frees <= 8) {
        fprintf(stderr, "  [HEAP] free #%d va=0x%08X blocks=%d\n",
                frees, xbox_va, g_heap_block_count);
        fflush(stderr);
    }
    for (int i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].addr != xbox_va || g_heap_blocks[i].free) {
            continue;
        }
        g_heap_blocks[i].free = 1;
        if (++matched % 512 == 0) {
            fprintf(stderr, "  [HEAP] frees=%d matched=%d blocks=%d\n",
                    frees, matched, g_heap_block_count);
            fflush(stderr);
        }

        /* Coalesce with neighbours. Blocks are recorded in bump order, so
         * index order is address order and adjacency is a simple end==start
         * test. Keeps large contiguous requests satisfiable after a lot of
         * small churn. */
        if (i + 1 < g_heap_block_count && g_heap_blocks[i + 1].free &&
            g_heap_blocks[i].addr + g_heap_blocks[i].size == g_heap_blocks[i + 1].addr) {
            g_heap_blocks[i].size += g_heap_blocks[i + 1].size;
            g_heap_blocks[i + 1].size = 0;
            g_heap_blocks[i + 1].addr = 0;
        }
        if (i > 0 && g_heap_blocks[i - 1].free &&
            g_heap_blocks[i - 1].addr + g_heap_blocks[i - 1].size == g_heap_blocks[i].addr) {
            g_heap_blocks[i - 1].size += g_heap_blocks[i].size;
            g_heap_blocks[i].size = 0;
            g_heap_blocks[i].addr = 0;
        }
        return;
    }
}

HANDLE xbox_GetMappingHandle(void)
{
    return g_mapping_handle;
}
