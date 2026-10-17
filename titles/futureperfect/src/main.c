/**
 * Timesplitters - Future Perfect - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       Timesplitters - Future Perfect
 *   Title ID:    0x00000000
 *   Base addr:   0x00010000
 *   Entry point: 0x00000000
 *   Code size:   ~??? KB (.text)
 *   Sections:    ?? (list them)
 *   Kernel imports: ??
 */

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>
#include "xbox_watchpoint.h"

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x0038024D  /* XBE entry point VA */
/* Relative to the executable's own directory, titles/<title>/build/<Config>/,
 * which is where scripts/run_and_report.py starts it. Run it from there by
 * hand too, or the XBE is not found. */
#define YOUR_GAME_XBE_PATH      "..\\..\\..\\..\\games\\Timesplitters - Future Perfect (USA)\\default.xbe"
#define YOUR_GAME_DIR            "..\\..\\..\\..\\games\\Timesplitters - Future Perfect (USA)"

/* ── Forward declarations ──────────────────────────────────── */

/* Defined in recomp_trace.c. Declared here rather than pulled from
 * the generated headers, which this file does not include. */
void recomp_profile_dump(void);
void recomp_exit_trace_init(void);      /* src/kernel/exit_trace.c */

/* Defined in kernel_bridge.c: the APU is a separate library the kernel must
 * not need to link, so the kernel takes its interrupt line as a callback. */
void xbox_SetApuInterruptSource(int (*pending)(void));

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* ============================================================
 * Finding the game, from wherever the executable was started
 *
 * A player double-clicks the executable, or runs a shortcut to it, and in
 * neither case is the working directory anything in particular. So the game
 * is looked for beside the executable rather than beside the caller:
 *
 *   1. <exe dir>\\game\\               -- what a distributed build looks like
 *   2. <exe dir>\\YOUR_GAME_DIR     -- the development tree, where the build
 *                                     sits several levels under the repo
 *
 * RECOMP_GAME_DIR overrides both, for running one build against another copy
 * of the game files.
 * ============================================================ */

static char g_game_dir[MAX_PATH];
static char g_xbe_path[MAX_PATH];

static BOOL file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Fills g_game_dir and g_xbe_path, or returns FALSE having left a message in
 * `tried` naming every place that was looked at -- which is the only useful
 * thing to tell somebody whose game files are somewhere else. */
static BOOL find_game(char *tried, size_t tried_bytes)
{
    char exe[MAX_PATH], dir[MAX_PATH], candidate[MAX_PATH];
    const char *env = getenv("RECOMP_GAME_DIR");
    char *slash;
    int i;

    tried[0] = '\0';
    if (env && *env) {
        snprintf(g_game_dir, sizeof g_game_dir, "%s", env);
        snprintf(g_xbe_path, sizeof g_xbe_path, "%s\\default.xbe", env);
        if (file_exists(g_xbe_path))
            return TRUE;
        snprintf(tried, tried_bytes, "RECOMP_GAME_DIR: %s", g_xbe_path);
        return FALSE;
    }

    if (!GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe))
        return FALSE;
    snprintf(dir, sizeof dir, "%s", exe);
    slash = strrchr(dir, '\\');
    if (slash)
        *slash = '\0';

    for (i = 0; i < 2; i++) {
        if (i == 0)
            snprintf(candidate, sizeof candidate, "%s\\game", dir);
        else
            snprintf(candidate, sizeof candidate, "%s\\%s", dir, YOUR_GAME_DIR);
        snprintf(g_xbe_path, sizeof g_xbe_path, "%s\\default.xbe", candidate);
        if (file_exists(g_xbe_path)) {
            snprintf(g_game_dir, sizeof g_game_dir, "%s", candidate);
            return TRUE;
        }
        {
            size_t n = strlen(tried);
            snprintf(tried + n, tried_bytes - n, "%s%s", n ? "\n" : "", g_xbe_path);
        }
    }
    return FALSE;
}



/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* And the raw words, because the useful thing is often not a
         * return address. A fault through a garbage pointer is diagnosed
         * by finding where the pointer came from, and the object it was
         * loaded out of is usually sitting in the frame -- invisible in
         * the filtered list above, which keeps only code addresses. */
        fprintf(stderr, "  guest stack (raw):\n");
        for (i = 0; i < 48; i += 4)
            fprintf(stderr, "    [esp+%-3d] %08X %08X %08X %08X\n",
                    i * 4, sp[i], sp[i + 1], sp[i + 2], sp[i + 3]);
    }
}

/* Register pages the runtime deliberately makes fault, so a guest access to
 * them can be given hardware semantics instead of landing in plain memory.
 * Each has a handler in the runtime that decodes the faulting instruction,
 * performs the access and moves RIP past it; this handler's job is only to
 * route the fault to the right one. The pages are trapped only when the
 * matching switch is set (RECOMP_VBLANK, RECOMP_AC97_READY), so without it
 * these ranges never fault and this code is never reached. */
#define GUEST_NV2A_BASE        0xFD000000u
#define GUEST_NV2A_PCRTC_PAGE  0xFD600000u   /* interrupt status: write-trapped */
#define GUEST_APU_REGS_BASE    0xFE800000u   /* APU registers: PAGE_NOACCESS   */
#define GUEST_APU_REGS_SIZE    0x00030000u   /* the DSP memory above stays RAM */
#define GUEST_AC97_PAGE        0xFEC00000u   /* codec / DSP command: write-trapped */

static LONG route_device_fault(PEXCEPTION_POINTERS ep, uintptr_t fault_addr,
                               int is_write)
{
    uint32_t va = (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);

    if (is_write && va >= GUEST_NV2A_PCRTC_PAGE && va < GUEST_NV2A_PCRTC_PAGE + 0x1000u) {
        if (nv2a_intr_handle_write(ep->ContextRecord, fault_addr,
                                   va - GUEST_NV2A_BASE,
                                   (uintptr_t)g_xbox_mem_offset + GUEST_NV2A_BASE))
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (va >= GUEST_APU_REGS_BASE && va < GUEST_APU_REGS_BASE + GUEST_APU_REGS_SIZE) {
        if (apu_hook_handle_mmio(ep->ContextRecord, fault_addr, va, is_write))
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (is_write && va >= GUEST_AC97_PAGE && va < GUEST_AC97_PAGE + 0x1000u) {
        if (mcpx_ac97_handle_write(ep->ContextRecord, fault_addr,
                                   va - GUEST_APU_REGS_BASE))
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    /* Say something about every exception, not only access violations. A
     * fault this handler stays silent on reads as the process simply
     * vanishing: exit code 0xC0000005 and nothing in the log. The GPU
     * register range used to be skipped here outright, and Burnout 2's first
     * run on a fresh machine died that way, in a write to the vblank
     * interrupt-enable register. Breakpoints and the debugger's thread-naming
     * exception are the only ones not worth a line. */
    /* A watchpoint stepping over the instruction it just trapped. This
     * has to come first: it is a single-step exception this process
     * asked for, not a fault, and reporting it would bury the watch
     * output in noise. */
    if (code == EXCEPTION_SINGLE_STEP && xbox_watch_handle_step(ep))
        return EXCEPTION_CONTINUE_EXECUTION;

    if (code == EXCEPTION_BREAKPOINT || code == 0x406D1388)
        return EXCEPTION_CONTINUE_SEARCH;
    if (code != EXCEPTION_ACCESS_VIOLATION) {
        fprintf(stderr, "[EXCEPTION] code 0x%08lX at RIP=0x%llX (first chance)\n",
                (unsigned long)code,
                (unsigned long long)ep->ContextRecord->Rip);
        print_guest_context((void *)ep->ContextRecord->Rip);
        fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];
        int is_write = ep->ExceptionRecord->ExceptionInformation[0] == 1;

        /* An armed watchpoint, which protected the page on purpose.
         * Checked before the device ranges because a watch is a
         * deliberate trap and the device hooks would not know it. */
        if (xbox_watch_handle_av(ep, fault_addr, is_write))
            return EXCEPTION_CONTINUE_EXECUTION;

        /* A trapped device register: serviced and resumed, not a crash. */
        if (g_xbox_mem_offset &&
            route_device_fault(ep, fault_addr, is_write) == EXCEPTION_CONTINUE_EXECUTION)
            return EXCEPTION_CONTINUE_EXECUTION;

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            is_write ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        /* The faulting instruction's bytes. When the fault is in a trapped
         * device page, this is the instruction form the runtime's decoder
         * did not know, which is exactly what has to be added to it. */
        {
            const uint8_t *ip = (const uint8_t *)ep->ContextRecord->Rip;
            int i;
            fprintf(stderr, "  host instruction:");
            for (i = 0; i < 12; i++)
                fprintf(stderr, " %02X", ip[i]);
            fprintf(stderr, "\n");
        }
        print_guest_context((void *)ep->ContextRecord->Rip);

        /* Force the profile table out here, not at the next scheduled report.
         * The report interval means the file on disk lags the run, and the
         * fault lands inside that lag -- so without this, the functions that
         * ran immediately before the crash are missing from it, and their
         * absence reads as "never ran". */
        recomp_profile_dump();

        /* RECOMP_FIND_VALUE: when the fault is through a garbage pointer,
         * the next question is who is holding it. Scanning guest RAM for
         * the value names the field, and that field is what to point
         * RECOMP_WATCH_WRITE at on the next run. */
        xbox_watch_scan_on_crash();

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging, named.
         *
         * The window is this module's own range. It used to be the preferred
         * base, 0x140000000-0x150000000, which ASLR moves: the image loads
         * near 0x7FF7..., so the loop matched nothing and every crash printed
         * an empty list under this header. The header and the leading
         * "[i] 0x..." are kept as they were for anything that parses them. */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            HMODULE mod = GetModuleHandleW(NULL);
            const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)
                ((const BYTE *)mod + ((const IMAGE_DOS_HEADER *)mod)->e_lfanew);
            uintptr_t lo = (uintptr_t)mod;
            uintptr_t hi = lo + nt->OptionalHeader.SizeOfImage;
            int shown = 0;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 256 && shown < 12; i++) {
                char buf[sizeof(SYMBOL_INFO) + 256];
                SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
                DWORD64 disp = 0;

                if (sp[i] < lo || sp[i] >= hi)
                    continue;
                memset(buf, 0, sizeof(buf));
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                if (SymFromAddr(GetCurrentProcess(), (DWORD64)sp[i], &disp, sym))
                    fprintf(stderr, "    [%d] 0x%llX %s+0x%llX\n", i,
                            (unsigned long long)sp[i], sym->Name,
                            (unsigned long long)disp);
                else
                    fprintf(stderr, "    [%d] 0x%llX (module+0x%llX)\n", i,
                            (unsigned long long)sp[i],
                            (unsigned long long)(sp[i] - lo));
                shown++;
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── WinMain ───────────────────────────────────────────────── */

/* ============================================================
 * Where the diagnostics go
 *
 * This is a windowed program, so a double-click gives it no console and
 * every printf would be thrown away -- which is the worst of both worlds:
 * no window full of text, and no record either. Three cases, in order:
 *
 *   1. Somebody redirected the output (a script capturing stderr to a file,
 *      a pipe). Those handles are already what was wanted: leave them.
 *   2. It was started from a terminal, which shares its console. Write
 *      there, so running it by hand behaves as it always has.
 *   3. It was double-clicked. Write to <executable>.log beside the program,
 *      truncated each run, so there is something to read after a crash.
 *
 * The log's path is remembered so a failure can name it in its message box:
 * "it did not start" is not a bug report, and the file is.
 * ============================================================ */

static char g_log_path[MAX_PATH];

static BOOL handle_is_real(DWORD which)
{
    HANDLE h = GetStdHandle(which);

    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return FALSE;
    return GetFileType(h) != FILE_TYPE_UNKNOWN;
}

static void setup_output(void)
{
    char exe[MAX_PATH];
    char *dot;
    FILE *f;

    if (handle_is_real(STD_OUTPUT_HANDLE) || handle_is_real(STD_ERROR_HANDLE))
        return;                                  /* redirected: leave it */

    if (AttachConsole(ATTACH_PARENT_PROCESS)) {  /* started from a terminal */
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        return;
    }

    if (!GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe))
        return;
    snprintf(g_log_path, sizeof g_log_path, "%s", exe);
    dot = strrchr(g_log_path, '.');
    if (dot && !strchr(dot, '\\'))
        *dot = '\0';
    strncat(g_log_path, ".log", sizeof g_log_path - strlen(g_log_path) - 1);

    f = freopen(g_log_path, "w", stderr);
    if (!f) {                                    /* read-only folder */
        g_log_path[0] = '\0';
        return;
    }
    freopen(g_log_path, "a", stdout);
}

/* Appended to a message box, when there is a file worth reading. */
static void log_hint(char *buf, size_t bytes)
{
    size_t n = strlen(buf);

    if (g_log_path[0] && bytes > n)
        snprintf(buf + n, bytes - n, "\n\nThere is more in:\n%s", g_log_path);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Before anything prints: a windowed program has nowhere to print
     * unless this says where. */
    setup_output();

    /* Unbuffered output for immediate visibility during debugging, and so a
     * crash keeps the tail of the log rather than losing it in a buffer. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=== Timesplitters - Future Perfect - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
    /* And the other way a run ends: an exit nobody logged. Prints [EXIT]
     * with the code and both stacks before the process goes (exit_trace.c). */
    recomp_exit_trace_init();

    /* Step 1: Find and load the XBE */
    {
        char tried[1024], message[1400];

        if (!find_game(tried, sizeof tried)) {
            snprintf(message, sizeof message,
                     "This needs the game's own files, which are not included.\n\n"
                     "Put them in a folder called \"game\" next to this program, so that\n"
                     "\"game\\default.xbe\" exists, or set RECOMP_GAME_DIR to where they are.\n\n"
                     "Looked for:\n%s", tried);
            fprintf(stderr, "%s\n", message);
            log_hint(message, sizeof message);
            MessageBoxA(NULL, message, "Timesplitters - Future Perfect", MB_ICONERROR);
            return 1;
        }
        printf("Game files: %s\n", g_game_dir);
        if (!load_xbe(g_xbe_path, &xbe_data, &xbe_size)) {
            snprintf(message, sizeof message,
                     "Found the game at\n%s\nbut could not read default.xbe.",
                     g_game_dir);
            log_hint(message, sizeof message);
            MessageBoxA(NULL, message, "Timesplitters - Future Perfect", MB_ICONERROR);
            return 1;
        }
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 1b: an online session (RECOMP_NET), set up before anything else
     * reads a setting or a save: the two machines must start from the same
     * ones (hle_netplay.h). Before the memory layout too, whose timer thread
     * reads the lockstep clock's switch the moment it starts. */
    {
        extern int hle_net_session_begin(const void *xbe, size_t xbe_size, const char *game_dir);
        if (!hle_net_session_begin(xbe_data, xbe_size, g_game_dir)) {
            free(xbe_data);
            return 1;
        }
    }

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(g_game_dir, NULL);
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 5a: memory watchpoints, if any were asked for. After the
     * bridge, so the pages a watch names are mapped and committed. */
    xbox_watch_init();

    /* Step 5b: the emulated APU, when its registers are trapped.
     *
     * RECOMP_AC97_READY makes the memory layout report the audio codec as
     * present and turn the APU's register pages into faults, so the title's
     * DirectSound can be answered by the emulated APU instead of by plain
     * memory. The fault handler above routes those faults to
     * apu_hook_handle_mmio(), which declines every access -- silently --
     * until the device exists. Create it under the same switch, and hand the
     * kernel its interrupt line so the timer thread can deliver it. */
    /* On unless turned off: the emulated audio hardware is not an
     * experiment any more, and without it the first DirectSound register
     * the title touches faults. xbox_EnvSwitch, so RECOMP_AC97_READY=0
     * still takes it away. */
    if (xbox_EnvSwitch("RECOMP_AC97_READY", 1)) {
        /* The fault handler serves from g_apu_state, and init returns the
         * device rather than storing it: left unassigned, every APU register
         * access was declined and reported as a crash reading 0xFE801100
         * (NV_PAPU_FECTL), the first register DirectSound touches. */
        /* The APU addresses guest memory physically, and in this runtime
         * physical page P lives in the contiguous window at
         * XBOX_CONTIG_BASE + P, not at guest VA P: the low 64 MB of VA is
         * the image and the heap, a different mapping. Handing it VA 0 made
         * every DirectSound buffer and DSP page table read the wrong bytes;
         * TimeSplitters 2's scatter-gather tables all read as empty. */
        g_apu_state = mcpx_apu_init_standalone(
            (uint8_t *)g_xbox_mem_offset + XBOX_CONTIG_BASE);
        if (!g_apu_state)
            fprintf(stderr, "[BOOT] APU init failed; APU register accesses "
                            "will fault\n");
        else
            xbox_SetApuInterruptSource(mcpx_apu_irq_pending);
    }

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();

    /* What this title tells the runtime about itself: its intro videos, the
     * toggles a held key can follow (recomp_manual.c). */
    {
        extern void fp_title_setup(void);
        fp_title_setup();
    }

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
