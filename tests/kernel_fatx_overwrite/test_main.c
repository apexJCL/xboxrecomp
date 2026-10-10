/*
 * FATX overwrite: regrowing after an overwrite keeps the data.
 *
 * A title can build its z: texture cache as:
 *
 *   NtCreateFile(x.tmp, FILE_OVERWRITE_IF, opts 0x48)   no buffering
 *   set end of file (size rounded up to 512); write all of it; close
 *   NtCreateFile(x.tmp, FILE_OVERWRITE_IF, opts 0x60)
 *   query position; set end of file (true size); rename to x.dat; close
 *
 * FATX keeps no valid-data length, so the second open's trim exposes the
 * bytes already on disk. The host truncate (O_TRUNC, CREATE_ALWAYS) zero-
 * fills instead, and every cache file came out as zeros of the right length
 * (seen on Proton: a 1 MB game archive file, all zeros).
 *
 * Cross-platform: it calls the xbox_Nt* file functions directly, which are
 * the Win32 backend on Windows (and Proton) and the POSIX one elsewhere.
 */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#include <sys/stat.h>
#endif

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

#define SEP_S "/"
#if defined(_WIN32)
#undef SEP_S
#define SEP_S "\\"
#endif

static int failures;
static char g_cache[512];

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-60s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static NTSTATUS open_file(const char *path, ULONG disposition, ULONG options,
                          HANDLE *out)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;

    name.Length = (USHORT)strlen(path);
    name.MaximumLength = (USHORT)(name.Length + 1);
    name.Buffer = (PCHAR)path;
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = 0x40;
    *out = NULL;
    return xbox_NtCreateFile(out, 0xC0110000u /* R|W|DELETE|SYNC */, &oa, &ios,
                             NULL, 0x80, 0, disposition, options);
}

static NTSTATUS set_eof(HANDLE h, LONGLONG size)
{
    XBOX_FILE_END_OF_FILE_INFORMATION info;
    XBOX_IO_STATUS_BLOCK ios;
    info.EndOfFile.QuadPart = size;
    return xbox_NtSetInformationFile(h, &ios, &info, sizeof info,
                                     XboxFileEndOfFileInformation);
}

static NTSTATUS query_pos(HANDLE h)
{
    XBOX_FILE_POSITION_INFORMATION info;
    XBOX_IO_STATUS_BLOCK ios;
    return xbox_NtQueryInformationFile(h, &ios, &info, sizeof info,
                                       XboxFilePositionInformation);
}

static LONGLONG query_size(HANDLE h)
{
    XBOX_FILE_STANDARD_INFORMATION info;
    XBOX_IO_STATUS_BLOCK ios;
    if (xbox_NtQueryInformationFile(h, &ios, &info, sizeof info,
                                    XboxFileStandardInformation) != 0)
        return -1;
    return info.EndOfFile.QuadPart;
}

static NTSTATUS write_at(HANDLE h, const void *buf, ULONG len, LONGLONG off)
{
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER o;
    o.QuadPart = off;
    return xbox_NtWriteFile(h, NULL, NULL, NULL, &ios, (PVOID)buf, len, &o);
}

static NTSTATUS rename_to(HANDLE h, const char *path)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    name.Length = (USHORT)strlen(path);
    name.MaximumLength = (USHORT)(name.Length + 1);
    name.Buffer = (PCHAR)path;
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = 0x40;
    return xbox_RenameFileByHandle(h, &oa, TRUE);
}

/* Host-side read of <cache>/<name>; -1 if it does not exist. */
static long host_file(const char *name, unsigned char *buf, size_t cap)
{
    char p[700];
    FILE *f;
    long n;
    snprintf(p, sizeof p, "%s" SEP_S "%s", g_cache, name);
    f = fopen(p, "rb");
    if (!f) return -1;
    n = (long)fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static unsigned char pattern(unsigned i) { return (unsigned char)(i * 7 + 1); }

int main(int argc, char **argv)
{
    static unsigned char got[8192];
    unsigned char *buf;
    char root[512], d[160];
    HANDLE h;
    NTSTATUS st;
    long n;
    unsigned i;
    int same;

    /* argv[1]: write the report there (Proton drops a console exe's stdout) */
    if (argc > 1 && !freopen(argv[1], "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        snprintf(root, sizeof root, "%sfatxow-%lu", tmp,
                 (unsigned long)GetCurrentProcessId());
        if (!CreateDirectoryA(root, NULL)) return 2;
        snprintf(g_cache, sizeof g_cache, "%s\\Cache", root);
        CreateDirectoryA(g_cache, NULL);
        /* unbuffered I/O wants a sector-aligned buffer */
        buf = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
#else
    {
        char tmpl[] = "/tmp/fatxowXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(root, sizeof root, "%s", tmpl);
        snprintf(g_cache, sizeof g_cache, "%s/Cache", root);
        mkdir(g_cache, 0755);
        if (posix_memalign((void **)&buf, 4096, 4096) != 0) buf = NULL;
    }
#endif
    if (!buf) return 2;
    xbox_path_init(root, root);
    for (i = 0; i < 4096; i++) buf[i] = pattern(i);
    memcpy(buf, "IPK1", 4);

    printf("FATX overwrite (the texture-cache copy)\n");

    /* First pass: create, size, write unbuffered, close. */
    st = open_file("z:\\o.tmp", XBOX_FILE_OVERWRITE_IF, 0x48, &h);
    snprintf(d, sizeof d, "status 0x%08X", (unsigned)st);
    check(st == 0 && h, "create z:\\o.tmp (OVERWRITE_IF, no buffering)", d);
    if (st) goto done;
    check(set_eof(h, 4096) == 0, "set end of file to 4096", NULL);
    st = write_at(h, buf, 4096, 0);
    snprintf(d, sizeof d, "status 0x%08X", (unsigned)st);
    check(st == 0, "write 4096 bytes starting \"IPK1\"", d);
    xbox_NtClose(h);
    check(host_file("o.tmp", got, sizeof got) == 4096, "host file is 4096 bytes", NULL);

    /* Second pass: overwrite, position query, trim, rename, close. */
    st = open_file("z:\\o.tmp", XBOX_FILE_OVERWRITE_IF, 0x60, &h);
    check(st == 0 && h, "reopen with OVERWRITE_IF", NULL);
    check(query_pos(h) == 0, "position query (XAPI SetEndOfFile does one)", NULL);
    check(set_eof(h, 3000) == 0, "set end of file to the true size, 3000", NULL);
    st = rename_to(h, "z:\\o.dat");
    snprintf(d, sizeof d, "status 0x%08X", (unsigned)st);
    check(st == 0, "rename to z:\\o.dat", d);
    xbox_NtClose(h);
    n = host_file("o.dat", got, sizeof got);
    same = n == 3000;
    for (i = 0; same && i < 3000; i++)
        if (got[i] != (i < 4 ? (unsigned char)"IPK1"[i] : pattern(i))) same = 0;
    snprintf(d, sizeof d, "%ld bytes, first 0x%02X", n, n > 0 ? got[0] : 0);
    check(n == 3000, "o.dat is 3000 bytes", d);
    check(same, "the bytes below the new end survive", d);
    check(host_file("o.tmp", got, sizeof got) < 0, "o.tmp is gone", NULL);

    /* An overwrite followed by a write is a plain truncate. */
    st = open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, 0x60, &h);
    check(st == 0, "overwrite o.dat again", NULL);
    write_at(h, buf, 4, 0);
    xbox_NtClose(h);
    check(host_file("o.dat", got, sizeof got) == 4,
          "overwrite + 4-byte write leaves 4 bytes", NULL);

    /* A size query sees the overwrite. */
    st = open_file("z:\\o.dat", XBOX_FILE_OVERWRITE, 0x60, &h);
    check(st == 0, "FILE_OVERWRITE of an existing file opens", NULL);
    snprintf(d, sizeof d, "size %lld", (long long)query_size(h));
    check(query_size(h) == 0, "overwrite + size query reports 0", d);
    xbox_NtClose(h);

    /* An overwrite that is just closed leaves an empty file. */
    st = open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, 0x60, &h);
    write_at(h, buf, 16, 0);
    xbox_NtClose(h);
    st = open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, 0x60, &h);
    xbox_NtClose(h);
    check(host_file("o.dat", got, sizeof got) == 0,
          "overwrite + close leaves 0 bytes", NULL);

    /* An overwrite that regrows past the old end zero-fills only the tail. */
    st = open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, 0x60, &h);
    write_at(h, buf, 100, 0);
    xbox_NtClose(h);
    st = open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, 0x60, &h);
    set_eof(h, 200);
    xbox_NtClose(h);
    n = host_file("o.dat", got, sizeof got);
    same = n == 200;
    for (i = 0; same && i < 200; i++)
        if (got[i] != (i < 4 ? (unsigned char)"IPK1"[i] : i < 100 ? pattern(i) : 0))
            same = 0;
    check(same, "regrow past the old end: old bytes, then zeros", NULL);

    /* FILE_OVERWRITE of a missing file still fails. */
    st = open_file("z:\\missing.dat", XBOX_FILE_OVERWRITE, 0x60, &h);
    check(st != 0, "FILE_OVERWRITE of a missing file fails", NULL);

done:
    {
        char cmd[700];
#if defined(_WIN32)
        snprintf(cmd, sizeof cmd, "cmd /c rmdir /s /q \"%s\"", root);
#else
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
#endif
        (void)system(cmd);
    }
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
