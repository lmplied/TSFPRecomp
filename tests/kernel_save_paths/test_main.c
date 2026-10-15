#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The kernel library references the dispatch tables a title generates. */
void *recomp_lookup(ULONG a) { (void)a; abort(); }
void *recomp_lookup_manual(ULONG a) { (void)a; abort(); }

static char g_root[MAX_PATH], g_game[MAX_PATH], g_save[MAX_PATH];
static int g_fail;

static void put(const char *dir, const char *rel, const char *text)
{
    char path[MAX_PATH], *p;
    FILE *f;

    snprintf(path, sizeof path, "%s\\%s", dir, rel);
    for (p = path + strlen(dir) + 1; *p; p++)
        if (*p == '\\') {
            *p = '\0';
            CreateDirectoryA(path, NULL);
            *p = '\\';
        }
    f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

/* The file's contents, or "" when it cannot be read. */
static const char *get(const char *dir, const char *rel)
{
    static char buf[64];
    char path[MAX_PATH];
    FILE *f;
    size_t n;

    snprintf(path, sizeof path, "%s\\%s", dir, rel);
    buf[0] = '\0';
    f = fopen(path, "rb");
    if (f) {
        n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = '\0';
        fclose(f);
    }
    return buf;
}

static void expect_file(const char *dir, const char *rel, const char *text)
{
    const char *got = get(dir, rel);
    if (strcmp(got, text) != 0) {
        fprintf(stderr, "FAIL %s\\%s holds \"%s\", expected \"%s\"\n", dir, rel, got, text);
        g_fail = 1;
    }
}

/* xbox_path is translated to <base>\<rel>, compared without regard to case. */
static void expect_path(const char *xbox_path, const char *base, const char *rel)
{
    WCHAR got[MAX_PATH], want[MAX_PATH];
    char narrow[MAX_PATH];

    if (!xbox_translate_path(xbox_path, got, MAX_PATH)) {
        fprintf(stderr, "FAIL %s did not translate\n", xbox_path);
        g_fail = 1;
        return;
    }
    snprintf(narrow, sizeof narrow, "%s%s%s", base, *rel ? "\\" : "", rel);
    MultiByteToWideChar(CP_ACP, 0, narrow, -1, want, MAX_PATH);
    if (_wcsicmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s -> %ls, expected %ls\n", xbox_path, got, want);
        g_fail = 1;
    }
}

static void rm_tree(const char *dir)
{
    char pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                rm_tree(path);
            else
                DeleteFileA(path);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

int main(void)
{
    char tmp[MAX_PATH];

    GetTempPathA(MAX_PATH, tmp);
    snprintf(g_root, sizeof g_root, "%sxbr-save-paths-%lu", tmp, GetCurrentProcessId());
    snprintf(g_game, sizeof g_game, "%s\\game", g_root);
    snprintf(g_save, sizeof g_save, "%s\\save", g_root);
    CreateDirectoryA(g_root, NULL);
    CreateDirectoryA(g_game, NULL);
    CreateDirectoryA(g_save, NULL);

    /* Saves made before the fix, beside the game. */
    put(g_game, "UDATA\\45410066\\16D15BD8D404\\tsftrpft", "profile");
    put(g_game, "UDATA\\45410066\\TitleMeta.xbx", "meta");
    put(g_game, "TDATA\\45410066\\settings", "title data");
    /* A title that has saved since on the save side: its folder must win. */
    put(g_game, "UDATA\\4D530004\\slot", "old");
    put(g_save, "UDATA\\4D530004\\slot", "new");
    /* A disc file, which no rule here may move. */
    put(g_game, "pak\\chr.pak", "disc");

    /* A caller-chosen save folder: a player's, so the copy runs. */
    SetEnvironmentVariableA("RECOMP_SAVE_DIR", NULL);
    xbox_path_init(g_game, g_save);

    expect_file(g_save, "UDATA\\45410066\\16D15BD8D404\\tsftrpft", "profile");
    expect_file(g_save, "UDATA\\45410066\\TitleMeta.xbx", "meta");
    expect_file(g_save, "TDATA\\45410066\\settings", "title data");
    expect_file(g_save, "UDATA\\4D530004\\slot", "new");
    expect_file(g_game, "UDATA\\45410066\\16D15BD8D404\\tsftrpft", "profile");

    /* The device paths XAPI uses. Case as Future Perfect's log has it. */
    expect_path("\\Device\\Harddisk0\\partition1\\UDATA\\45410066\\16D15BD8D404\\SaveMeta.xbx",
                g_save, "UDATA\\45410066\\16D15BD8D404\\SaveMeta.xbx");
    expect_path("\\Device\\Harddisk0\\partition1\\UDATA\\45410066\\", g_save, "UDATA\\45410066");
    expect_path("\\Device\\Harddisk0\\partition1\\UDATA", g_save, "UDATA");
    expect_path("\\Device\\Harddisk0\\Partition1\\TDATA\\45410066\\$u\\contentmeta.xbx",
                g_save, "TDATA\\45410066\\$u\\contentmeta.xbx");
    expect_path("\\Device\\Harddisk0\\Partition1\\TDATA", g_save, "TDATA");
    /* The rest of E: stays with the game. */
    expect_path("\\Device\\Harddisk0\\Partition1\\pak\\chr.pak", g_game, "pak\\chr.pak");
    expect_path("D:\\pak\\chr.pak", g_game, "pak\\chr.pak");
    expect_path("\\Device\\Harddisk0\\Partition1\\", g_save, "Partition1.img");

    rm_tree(g_root);
    if (g_fail)
        return 1;
    puts("PASS: E:\\TDATA and E:\\UDATA go to the save folder, the rest of E: and D: to "
         "the game folder; saves beside the game are copied over once, never over newer ones");
    return 0;
}
