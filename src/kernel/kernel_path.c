/*
 * kernel_path.c - Xbox device-path translation
 *
 * Translates Xbox device-style paths to host filesystem paths:
 *   \Device\CdRom0\  -> <game_dir>/
 *   D:\               -> <game_dir>/
 *   T:\               -> <save_dir>/TitleData/
 *   U:\               -> <save_dir>/UserData/
 *   Z:\               -> <save_dir>/Cache/<title id>/
 *
 * The Win32 build emits UTF-16 paths (for CreateFileW); the Linux build
 * emits UTF-8 paths with '/' separators (for open()).
 */

#include "kernel.h"
#include "xbox_watchpoint.h"
#include "recomp_config.h"   /* recomp_config_title_id: per-title cache */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/*
 * The cache partitions (X:, Y:, Z:, Partition3-5) are one host folder per
 * title, <save_dir>/Cache/<title id>.
 *
 * They were all one folder, shared by every title, and a cache is the one
 * place titles reuse file names: a title copies its data from the disc to the
 * cache once and afterwards trusts what it finds there. OutRun 2 found
 * another title's Voice1.xwb (880,640 bytes, from 21 Sep 2026) where it
 * keeps its own voice bank, read a wave-bank header out of it, asked its own
 * heap for that much and stopped at "* OUT OF MEMORY ERROR *" on the start-up
 * screens -- on every boot, for a player whose save folder had run other
 * titles, and never on a fresh one. On a console a cache partition is wiped
 * when another title claims it; a folder per title gets the same result
 * without wiping anything. The title id comes from the XBE certificate,
 * read before any file is opened; 0 (not known) keeps the shared folder.
 */
static const char *cache_sub_dir(const char *sub, char sep, char *buf, size_t n)
{
    uint32_t id = recomp_config_title_id();

    if (!sub || !id || (strcmp(sub, "\\Cache") != 0 && strcmp(sub, "/Cache") != 0))
        return sub;
    snprintf(buf, n, "%s%c%08X", sub, sep, (unsigned)id);
    return buf;
}

/*
 * Helper: check if an ANSI string starts with a prefix (case-insensitive).
 * Returns the number of chars consumed from the prefix, or 0 if no match.
 * Platform-independent.
 */
static int match_prefix(const char* path, const char* prefix)
{
    int i = 0;
    while (prefix[i]) {
        if (tolower((unsigned char)path[i]) != tolower((unsigned char)prefix[i]))
            return 0;
        i++;
    }
    return i;
}

/* A device-path translation rule, shared by both backends. */
typedef struct {
    const char* prefix;     /* Xbox path prefix (backslash form)         */
    int         to_save;    /* 1 = under save_dir, 0 = under game_dir    */
    const char* sub_win;    /* sub-directory, Win32 backslash form       */
    const char* sub_posix;  /* sub-directory, POSIX slash form           */
} path_rule;

static const path_rule s_rules[] = {
    { "\\Device\\CdRom0\\",                   0, NULL,         NULL          },
    /* The DVD drive itself, with nothing after it. A title whose certificate
     * allows only the DVD-X2 media type (TimeSplitters 2: AllowedMedia 0x2)
     * has XAPI open the bare device at start-up and send it a SCSI mode
     * sense for the disc's security page before main ever runs; with no
     * rule this returned STATUS_OBJECT_PATH_NOT_FOUND and the title went
     * back to the dashboard after fifteen kernel calls. Listed after the
     * slashed form so paths under the device keep their own mapping; the
     * empty remainder makes it the game directory, which NtCreateFile opens
     * as a directory handle. */
    { "\\Device\\CdRom0",                     0, NULL,         NULL          },
    /* E:\TDATA and E:\UDATA are a title's own data and its saves. XAPI 5849
     * reaches them by device path, not through T: and U:: TimeSplitters:
     * Future Perfect writes TitleMeta.xbx, its profiles and every save under
     * Partition1\UDATA\45410066 and never opens U: at all. Under the
     * Partition1 rule below they went into the game folder -- beside the
     * disc's files, outside RECOMP_SAVE_DIR, and into any copy of the game
     * folder handed to someone else -- so every scripted run inherited the
     * last one's profile and took a different path through the front end.
     * They belong with the saves. Listed before the Partition1 rule, which
     * keeps the rest of E: in the game folder for titles that load assets
     * from it (Wreckless links Z: there; kernel_bridge.c). Saves a player
     * already made beside the game are copied over by migrate_saves(). */
    { "\\Device\\Harddisk0\\Partition1\\TDATA\\", 1, "\\TDATA", "/TDATA" },
    { "\\Device\\Harddisk0\\Partition1\\TDATA",   1, "\\TDATA", "/TDATA" },
    { "\\Device\\Harddisk0\\Partition1\\UDATA\\", 1, "\\UDATA", "/UDATA" },
    { "\\Device\\Harddisk0\\Partition1\\UDATA",   1, "\\UDATA", "/UDATA" },
    { "\\Device\\Harddisk0\\Partition1\\",    0, NULL,         NULL          },
    /* The rest of the disk. Partition 0 is the whole raw device, 2 holds
     * system data, and 3-5 are the per-title caches behind X:, Y: and Z:.
     * Without these the path layer reported "Unrecognized Xbox path" and
     * NtOpenFile returned STATUS_OBJECT_PATH_NOT_FOUND; DSTEAL_JP probes
     * partition0 at startup and treats the failure as fatal.
     *
     * No trailing separator: the probe opens the device itself, with nothing
     * after it. Listed after Partition1 so that keeps its own mapping. */
    /* Partition2 is C:, the system partition. On a console that is where the
     * dashboard and its own assets live, and the dashboard opens them by
     * device path as well as through Y:, so a path *under* partition2 is an
     * ordinary asset read and belongs in the game dir -- the same place Y:
     * already goes. Only the bare device keeps the system-data image, which
     * is why this rule carries the trailing separator and is listed first. */
    { "\\Device\\Harddisk0\\Partition2\\",    0, NULL,         NULL          },
    { "C:\\",                                 0, NULL,         NULL          },
    { "\\??\\C:\\",                           0, NULL,         NULL          },
    { "\\Device\\Harddisk0\\Partition2",     1, "\\SystemData", "/SystemData" },
    { "\\Device\\Harddisk0\\Partition3",     1, "\\Cache",   "/Cache"      },
    { "\\Device\\Harddisk0\\Partition4",     1, "\\Cache",   "/Cache"      },
    { "\\Device\\Harddisk0\\Partition5",     1, "\\Cache",   "/Cache"      },
    { "D:\\",                                 0, NULL,         NULL          },
    { "d:\\",                                 0, NULL,         NULL          },
    /* Y: is the Xbox dashboard partition; the dashboard opens its assets
     * (e.g. "Y:\default.xip") from there. Map it to the game dir. */
    { "Y:\\",                                 0, NULL,         NULL          },
    { "y:\\",                                 0, NULL,         NULL          },
    { "T:\\",                                 1, "\\TitleData","/TitleData"  },
    { "U:\\",                                 1, "\\UserData", "/UserData"   },
    { "Z:\\",                                 1, "\\Cache",    "/Cache"      },
    { "\\??\\D:\\",                           0, NULL,         NULL          },
    { "\\??\\Y:\\",                           0, NULL,         NULL          },
    { "\\??\\y:\\",                           0, NULL,         NULL          },
    { "\\??\\T:\\",                           1, "\\TitleData","/TitleData"  },
};
#define PATH_RULE_COUNT ((int)(sizeof(s_rules) / sizeof(s_rules[0])))

/*
 * Rewrite a path through a drive letter the title mapped for itself.
 *
 * Titles do their own mounting. Wreckless calls IoCreateSymbolicLink at guest
 * 0x000E9B1D to link "\??\Z:" to "\Device\Harddisk0\Partition1\", then loads
 * every asset through z:\. kernel_io.c has recorded that link since it was
 * written, but nothing ever read the table back, so the static rule below --
 * Z: is the cache partition, which is true of the console in general and wrong
 * for this title -- sent every asset open to the save directory and it failed
 * with ERROR_FILE_NOT_FOUND.
 *
 * Accepts "Z:\rest" and "\??\Z:\rest"; the table is keyed on the "\??\Z:" form
 * the kernel actually registers.
 *
 * Deliberately conservative: the rewritten path is returned only when it
 * matches a static rule. A link whose target this layer has no rule for would
 * otherwise turn a path that translated adequately into one that does not
 * translate at all.
 *
 * Returns 1 and fills `out` on success, 0 to leave the path alone.
 */
static int resolve_symlink(const char* xbox_path, char* out, size_t out_size)
{
    char        link[8];
    const char* rest;
    const char* target;
    size_t      tlen, rlen;
    int         i;

    if (!xbox_path || !out || out_size == 0)
        return 0;

    if (match_prefix(xbox_path, "\\??\\") && xbox_path[4] && xbox_path[5] == ':')
        xbox_path += 4;
    if (!xbox_path[0] || xbox_path[1] != ':')
        return 0;
    rest = xbox_path + 2;

    link[0] = '\\';
    link[1] = '?';
    link[2] = '?';
    link[3] = '\\';
    link[4] = (char)toupper((unsigned char)xbox_path[0]);
    link[5] = ':';
    link[6] = '\0';

    target = xbox_LookupSymbolicLink(link);
    if (!target || !target[0])
        return 0;

    while (*rest == '\\' || *rest == '/')
        rest++;

    tlen = strlen(target);
    rlen = strlen(rest);
    if (tlen + rlen + 2 > out_size)
        return 0;

    memcpy(out, target, tlen);
    if (tlen && target[tlen - 1] != '\\' && target[tlen - 1] != '/')
        out[tlen++] = '\\';
    memcpy(out + tlen, rest, rlen);
    out[tlen + rlen] = '\0';

    for (i = 0; i < PATH_RULE_COUNT; i++) {
        if (match_prefix(out, s_rules[i].prefix))
            return 1;           /* the target is somewhere we can place */
    }
    return 0;
}

/* ======================================================================== */
#if defined(_WIN32)
/* ======================================================================== */

#include <shlobj.h>
#include <winioctl.h>

static WCHAR s_game_dir[MAX_PATH];
static WCHAR s_save_dir[MAX_PATH];
static BOOL  s_initialized = FALSE;

/*
 * The raw disk device, \Device\Harddisk0\Partition0.
 *
 * A title opens it to read the Xbox partition table at sector 4 (offset 0x800)
 * and find the cache partitions behind X:, Y: and Z: -- XAPI's utility-drive
 * mount does exactly that, and DSTEAL_JP calls HalReturnToFirmware when the
 * read fails. A directory cannot answer a 512-byte read at a file offset, so
 * the device is backed by an image file instead.
 *
 * The table is the standard retail geometry. This is emulating a device that
 * has to be there, not fabricating anything the title owns.
 */
#define XBOX_DISK_IMAGE_NAME   L"Partition0.img"
#define XBOX_PART_TABLE_OFFSET 0x800
#define XBOX_PART_IN_USE       0x80000000u

static void xbox_write_partition_table(const WCHAR *path)
{
    /* name[16], flags, lba_start, lba_size, reserved -- 32 bytes each */
    static const struct { const char *name; ULONG start, size; } parts[] = {
        { "XBOX_PART_X",  0x00000400, 0x00177000 },  /* X: cache      */
        { "XBOX_PART_Y",  0x00177400, 0x00177000 },  /* Y: cache      */
        { "XBOX_PART_Z",  0x002EE400, 0x00177000 },  /* Z: cache      */
        { "XBOX_PART_C",  0x00465400, 0x000FA000 },  /* C: system     */
        { "XBOX_PART_E",  0x0055F400, 0x00465400 },  /* E: game/save  */
    };
    unsigned char sector[512];
    HANDLE h;
    DWORD written;
    LARGE_INTEGER off;
    size_t i;

    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    memset(sector, 0, sizeof(sector));
    memcpy(sector, "****PARTINFO****", 16);
    for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        unsigned char *e = sector + 48 + i * 32;   /* 16 magic + 32 reserved */
        size_t n = strlen(parts[i].name);
        memset(e, ' ', 16);
        memcpy(e, parts[i].name, n < 16 ? n : 16);
        *(ULONG *)(e + 16) = XBOX_PART_IN_USE;
        *(ULONG *)(e + 20) = parts[i].start;
        *(ULONG *)(e + 24) = parts[i].size;
        *(ULONG *)(e + 28) = 0;
    }

    off.QuadPart = XBOX_PART_TABLE_OFFSET;
    if (SetFilePointerEx(h, off, NULL, FILE_BEGIN))
        WriteFile(h, sector, sizeof(sector), &written, NULL);
    CloseHandle(h);
}

/*
 * Map a partition device onto an image file.
 *
 * Matches "\Device\Harddisk0\PartitionN" with nothing below it, which is how a
 * device is opened. Anything with a path under it -- Partition1\TDATA and the
 * like -- is an ordinary filesystem access and falls through to the rules
 * table, which puts it in a directory.
 *
 * Partition0 is the whole disk and carries the partition table written above.
 * The rest start empty, which is what an unformatted partition looks like, so
 * a title that wants a filesystem there formats one.
 */
static BOOL xbox_partition_device_path(const char *xbox_path, WCHAR *out, DWORD n)
{
    static const char *prefix = "\\Device\\Harddisk0\\Partition";
    int len = match_prefix(xbox_path, prefix);
    int digit;
    const char *rest;

    if (!len)
        return FALSE;
    digit = xbox_path[len];
    if (digit < '0' || digit > '9')
        return FALSE;

    rest = xbox_path + len + 1;
    /* Nothing below it, allowing for a single trailing separator. */
    if (*rest == '\\' || *rest == '/')
        rest++;
    if (*rest != '\0')
        return FALSE;

    swprintf_s(out, n, L"%s\\Partition%c.img", s_save_dir, (WCHAR)digit);
    return TRUE;
}

/* Copy a directory tree. Existing files are left alone. */
static BOOL copy_tree(const WCHAR *from, const WCHAR *to)
{
    WCHAR pattern[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    BOOL ok = TRUE;

    SHCreateDirectoryExW(NULL, to, NULL);
    swprintf_s(pattern, MAX_PATH, L"%s\\*", from);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        swprintf_s(src, MAX_PATH, L"%s\\%s", from, fd.cFileName);
        swprintf_s(dst, MAX_PATH, L"%s\\%s", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok = copy_tree(src, dst) && ok;
        else if (!CopyFileW(src, dst, TRUE) && GetLastError() != ERROR_FILE_EXISTS)
            ok = FALSE;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/*
 * Saves made before the TDATA/UDATA rules above sit in the game folder, in
 * <game>\UDATA\<title id> and <game>\TDATA\<title id>. Copy each title's
 * folder to the save directory the first time this build runs, so a player's
 * profile is still there after the update.
 *
 * Copied, not moved: the game folder is the player's, and a failed copy must
 * not cost them a save. A title folder that already exists on the save side
 * is never overwritten -- that is a save made since, and newer. Not for a
 * RECOMP_SAVE_DIR run: that asked for a folder of its own, and filling it
 * from the game folder is the leak the rules close. The per-user default and
 * a folder the caller chose are where a player's saves live, so both get it.
 */
static void migrate_saves(void)
{
    static const WCHAR *const areas[] = { L"TDATA", L"UDATA" };
    int a;

    for (a = 0; a < (int)(sizeof areas / sizeof areas[0]); a++) {
        WCHAR pattern[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE h;

        swprintf_s(pattern, MAX_PATH, L"%s\\%s\\*", s_game_dir, areas[a]);
        h = FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            swprintf_s(from, MAX_PATH, L"%s\\%s\\%s", s_game_dir, areas[a], fd.cFileName);
            swprintf_s(to, MAX_PATH, L"%s\\%s\\%s", s_save_dir, areas[a], fd.cFileName);
            if (GetFileAttributesW(to) != INVALID_FILE_ATTRIBUTES)
                continue;
            if (copy_tree(from, to))
                fprintf(stderr, "[PATH] saves moved: copied %ls to %ls. This build keeps "
                                "saves there; the copy in the game folder is no longer "
                                "read and can be deleted.\n", from, to);
            else
                fprintf(stderr, "[PATH] could not copy every file of %ls to %ls; "
                                "the game folder's copy is untouched\n", from, to);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    fflush(stderr);
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    WCHAR save_base[MAX_PATH];

    /* The fallbacks used to name Burnout 3 specifically, so any other title
     * that passed NULL silently pointed its game dir and its saves at another
     * game's folders. Generic now -- a caller that wants a title-specific
     * location should pass one. */
    if (game_dir) {
        MultiByteToWideChar(CP_UTF8, 0, game_dir, -1, s_game_dir, MAX_PATH);
    } else {
        /* Neutral default: the CWD's "game" subdirectory, which is the
         * layout the game projects use (see templates/new-game). */
        GetCurrentDirectoryW(MAX_PATH, s_game_dir);
    }

    /* RECOMP_SAVE_DIR moves T:, U:, Z: and the partition images out of the
     * per-user default. The default is shared by every title and by the
     * player's own games, so a scripted or test run that does not set this
     * reads and writes the player's real saves -- which is what happened on
     * 30 Sep 2026, and what also made those runs depend on each other. A
     * caller that passes a save_dir of its own still wins. */
    const char *save_source = save_dir ? "caller" : "default";
    if (!save_dir) {
        const char *env = getenv("RECOMP_SAVE_DIR");
        if (env && env[0]) {
            save_dir = env;
            save_source = "RECOMP_SAVE_DIR";
        }
    }

    if (save_dir) {
        MultiByteToWideChar(CP_UTF8, 0, save_dir, -1, s_save_dir, MAX_PATH);
    } else {
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, save_base))) {
            swprintf_s(s_save_dir, MAX_PATH, L"%s\\xboxrecomp", save_base);
        } else {
            GetCurrentDirectoryW(MAX_PATH, s_save_dir);
            wcscat_s(s_save_dir, MAX_PATH, L"\\SaveData");
        }
    }

    size_t len = wcslen(s_game_dir);
    if (len > 0 && s_game_dir[len - 1] == L'\\')
        s_game_dir[len - 1] = L'\0';

    len = wcslen(s_save_dir);
    if (len > 0 && s_save_dir[len - 1] == L'\\')
        s_save_dir[len - 1] = L'\0';

    /* Create the save-side directories. T:/U:/Z: map into subdirectories of
     * save_dir, and a title that opens a file there with a create disposition
     * fails if the parent does not exist -- which reads as "cannot create save
     * file" and sends the title down its init-failure path. Halo asserts
     * exactly that at saved games/game_state_xbox.c:97 and then unwinds,
     * clearing global_d3d_device on the way out, so a missing directory
     * surfaces as a graphics failure.
     *
     * Cheap and idempotent: SHCreateDirectoryExW builds intermediates and is
     * happy if they already exist. */
    {
        static const WCHAR *subs[] = { L"TitleData", L"UserData", L"Cache",
                                       L"SystemData" };
        WCHAR image[MAX_PATH];
        WCHAR dir[MAX_PATH];
        SHCreateDirectoryExW(NULL, s_save_dir, NULL);
        for (int i = 0; i < (int)(sizeof(subs) / sizeof(subs[0])); i++) {
            swprintf_s(dir, MAX_PATH, L"%s\\%s", s_save_dir, subs[i]);
            SHCreateDirectoryExW(NULL, dir, NULL);
        }
        swprintf_s(image, MAX_PATH, L"%s\\%s", s_save_dir, XBOX_DISK_IMAGE_NAME);
        xbox_write_partition_table(image);
        /* The other partition devices, sized to the same geometry the table
         * above describes, so a title that asks the device how big it is gets
         * an answer consistent with the table it just read. Marked sparse
         * first: the cache partitions are 750 MB each and none of that is
         * touched until something writes to it. */
        {
            static const ULONGLONG part_sectors[6] = {
                0,             /* 0: whole disk, sized below      */
                0x00465400ull, /* 1: E: game and saves            */
                0x000FA000ull, /* 2: C: system                    */
                0x00177000ull, /* 3: X: cache                     */
                0x00177000ull, /* 4: Y: cache                     */
                0x00177000ull, /* 5: Z: cache                     */
            };
            for (int p = 1; p <= 5; p++) {
                HANDLE h;
                DWORD ret;
                LARGE_INTEGER end;

                swprintf_s(image, MAX_PATH, L"%s\\Partition%d.img",
                           s_save_dir, p);
                h = CreateFileW(image, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h == INVALID_HANDLE_VALUE)
                    continue;
                DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0,
                                &ret, NULL);
                end.QuadPart = (LONGLONG)(part_sectors[p] * 512ull);
                if (SetFilePointerEx(h, end, NULL, FILE_BEGIN))
                    SetEndOfFile(h);
                CloseHandle(h);
            }
        }
    }

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%S, save=%S", s_game_dir, s_save_dir);
    fprintf(stderr, "[PATH] saves in %ls (%s)\n", s_save_dir, save_source);
    fflush(stderr);
    if (strcmp(save_source, "RECOMP_SAVE_DIR") != 0)
        migrate_saves();
}

/* The host path the last translation produced.
 *
 * A caller that wants to act on the file a title just opened -- playing an FMV
 * the host can decode itself, say -- has the guest path but not the host one,
 * and re-deriving it would duplicate every rule above. */
/* Thread-local: several threads open files at once, and a plain static let one
 * thread's translation overwrite another's between the translate and the read.
 * That showed up as the FMV trigger firing on roughly two runs in three. */
static __declspec(thread) wchar_t s_last_host_path[MAX_PATH];

static void xbox_remember_host_path(const wchar_t *p)
{
    if (p) wcsncpy_s(s_last_host_path, MAX_PATH, p, _TRUNCATE);
}

const wchar_t *xbox_LastHostPath(void)
{
    return s_last_host_path;
}


BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char*  remainder = NULL;
    const WCHAR* base_dir  = NULL;
    const char*  sub_dir   = NULL;
    char         sub_buf[64];   /* cache_sub_dir */
    int          skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    {
        char linked[512];
        if (resolve_symlink(xbox_path, linked, sizeof(linked)))
            return xbox_translate_path(linked, host_path_buf, buf_size);
    }

    if (xbox_partition_device_path(xbox_path, host_path_buf, buf_size)) {
        fprintf(stderr, "  [PATH] %s -> partition image\n", xbox_path);
        fflush(stderr);
        return TRUE;
    }

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save ? s_save_dir : s_game_dir;
            sub_dir   = cache_sub_dir(s_rules[i].sub_win, '\\', sub_buf, sizeof sub_buf);
            goto translate;
        }
    }

    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    MultiByteToWideChar(CP_ACP, 0, xbox_path, -1, host_path_buf, buf_size);
    return TRUE;

translate:
    fprintf(stderr, "  [PATH] %s\n", xbox_path);
    fflush(stderr);
    xbox_watch_note_path(xbox_path);   /* RECOMP_WATCH_ARM_ON */
    {
        WCHAR remainder_wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, remainder, -1, remainder_wide, MAX_PATH);

        for (WCHAR* p = remainder_wide; *p; p++) {
            if (*p == L'/') *p = L'\\';
        }

        if (sub_dir) {
            WCHAR sub_wide[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, sub_dir, -1, sub_wide, MAX_PATH);
            swprintf_s(host_path_buf, buf_size, L"%s%s\\%s", base_dir, sub_wide, remainder_wide);

            WCHAR dir_path[MAX_PATH];
            swprintf_s(dir_path, MAX_PATH, L"%s%s", base_dir, sub_wide);
            CreateDirectoryW(s_save_dir, NULL);
            SHCreateDirectoryExW(NULL, dir_path, NULL);   /* Cache\<title id> is two deep */
        } else {
            swprintf_s(host_path_buf, buf_size, L"%s\\%s", base_dir, remainder_wide);
        }

        /* An empty remainder means the title opened the device itself, so
         * the join above leaves a trailing separator -- which CreateFileW
         * rejects even with FILE_FLAG_BACKUP_SEMANTICS, turning an existing
         * directory into STATUS_OBJECT_PATH_NOT_FOUND. */
        {
            size_t n = wcslen(host_path_buf);
            while (n > 1 && (host_path_buf[n - 1] == L'\\'
                             || host_path_buf[n - 1] == L'/'))
                host_path_buf[--n] = L'\0';
        }

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %S", xbox_path, host_path_buf);
        xbox_remember_host_path(host_path_buf);
        return TRUE;
    }
}

/* ======================================================================== */
#else /* !_WIN32  -- POSIX / Linux */
/* ======================================================================== */

#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

static char s_game_dir[MAX_PATH];
static char s_save_dir[MAX_PATH];
static BOOL s_initialized = FALSE;

/* Strip a single trailing '/' (but never the root '/'). */
static void strip_trailing_slash(char* s)
{
    size_t len = strlen(s);
    if (len > 1 && s[len - 1] == '/')
        s[len - 1] = '\0';
}

/* Recursively create a directory and all missing parents. */
static void mkdir_p(const char* path)
{
    char tmp[MAX_PATH];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return;
    memcpy(tmp, path, len + 1);

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    if (game_dir) {
        snprintf(s_game_dir, sizeof(s_game_dir), "%s", game_dir);
    } else {
        /* Neutral default: CWD/"game", the layout game projects use. */
        char cwd[MAX_PATH];
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), ".");
        snprintf(s_game_dir, sizeof(s_game_dir), "%s/game", cwd);
    }

    /* RECOMP_SAVE_DIR moves T:, U:, Z: and the partition images out of the
     * per-user default. The default is shared by every title and by the
     * player's own games, so a scripted or test run that does not set this
     * reads and writes the player's real saves -- which is what happened on
     * 30 Sep 2026, and what also made those runs depend on each other. A
     * caller that passes a save_dir of its own still wins. */
    const char *save_source = save_dir ? "caller" : "default";
    if (!save_dir) {
        const char *env = getenv("RECOMP_SAVE_DIR");
        if (env && env[0]) {
            save_dir = env;
            save_source = "RECOMP_SAVE_DIR";
        }
    }

    if (save_dir) {
        snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);
    } else {
        /* XDG base-directory spec: $XDG_DATA_HOME or ~/.local/share */
        const char* xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0]) {
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/xboxrecomp", xdg);
        } else {
            const char* home = getenv("HOME");
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/.local/share/xboxrecomp",
                     (home && home[0]) ? home : ".");
        }
    }

    strip_trailing_slash(s_game_dir);
    strip_trailing_slash(s_save_dir);

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%s, save=%s",
             s_game_dir, s_save_dir);
    fprintf(stderr, "[PATH] saves in %s (%s)\n", s_save_dir, save_source);
    fflush(stderr);
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char* remainder = NULL;
    const char* base_dir  = NULL;
    const char* sub_dir   = NULL;
    char        sub_buf[64];   /* cache_sub_dir */
    int         skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    {
        char linked[512];
        if (resolve_symlink(xbox_path, linked, sizeof(linked)))
            return xbox_translate_path(linked, host_path_buf, buf_size);
    }

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save ? s_save_dir : s_game_dir;
            sub_dir   = cache_sub_dir(s_rules[i].sub_posix, '/', sub_buf, sizeof sub_buf);
            goto translate;
        }
    }

    /* Unrecognized path: pass through, just normalize separators. */
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    snprintf(host_path_buf, buf_size, "%s", xbox_path);
    for (char* p = host_path_buf; *p; p++)
        if (*p == '\\') *p = '/';
    return TRUE;

translate:
    {
        char remainder_posix[MAX_PATH];
        snprintf(remainder_posix, sizeof(remainder_posix), "%s", remainder);

        /* Xbox paths use backslashes -> POSIX slashes. */
        for (char* p = remainder_posix; *p; p++)
            if (*p == '\\') *p = '/';

        if (sub_dir) {
            snprintf(host_path_buf, buf_size, "%s%s/%s",
                     base_dir, sub_dir, remainder_posix);

            /* Ensure the save directory tree exists. */
            char dir_path[MAX_PATH];
            snprintf(dir_path, sizeof(dir_path), "%s%s", base_dir, sub_dir);
            mkdir_p(dir_path);
        } else {
            snprintf(host_path_buf, buf_size, "%s/%s", base_dir, remainder_posix);
        }

        {
            size_t n = strlen(host_path_buf);
            while (n > 1 && host_path_buf[n - 1] == '/')
                host_path_buf[--n] = '\0';
        }

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %s", xbox_path, host_path_buf);
        return TRUE;
    }
}

#endif /* _WIN32 */
