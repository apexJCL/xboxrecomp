/*
 * The cache partition in memory (RECOMP_STORAGE_RAM_CACHE).
 *
 * BLiNX 1 rewrites a ring of 300 z:\pelonNNN.rec files (1.9 MB each) at about
 * one a second; on disk that is about 7 GB of host writes an hour. With the
 * store on, z: and Partition3-5 live in memory: capped at the stock 750 MB,
 * seeded read-only from <hdd>/Cache, never written back.
 *
 * Covered: off until asked; seed reads; create, rewrite, seek and size; the
 * FATX overwrite semantics the disk backends keep; the cap (STATUS_DISK_FULL)
 * and the volume size; delete and delete-on-close; directories and patterns;
 * rename; Partition3 reaching the same files; and the host Cache untouched.
 * Then the review's cases: a GENERIC_WRITE handle writing at its offset (on
 * the store and on the host backend), duplicates, a double close, shrink and
 * regrow, a deleted seed staying deleted, seeds listed unopened, a restarted
 * scan and a listing buffer too small for its entry.
 *
 * Cross-platform: it calls the xbox_Nt* file functions directly.
 */
#include "kernel.h"
#include "kernel_ramfs.h"
#include "recomp_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <dirent.h>
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

#define ST_NOT_FOUND     0xC0000034u
#define ST_COLLISION     0xC0000035u
#define ST_END_OF_FILE   0xC0000011u
#define ST_DISK_FULL     0xC000007Fu
#define ST_NO_MORE_FILES 0x80000006u
#define ST_NOT_SAME_DEV  0xC00000D4u
#define ST_BUF_OVERFLOW  0x80000005u
#define ST_BUF_TOO_SMALL 0xC0000023u

/* What XAPI's CreateFile(GENERIC_READ | GENERIC_WRITE) passes once mapped:
 * FILE_GENERIC_READ | FILE_GENERIC_WRITE, so WRITE_DATA and APPEND_DATA. */
#define ACC_GENERIC_RW   0x0012019Fu
/* FILE_APPEND_DATA | SYNCHRONIZE: an append-only handle. */
#define ACC_APPEND_ONLY  0x00100004u

#define CAP (1024u * 1024u)   /* 64 clusters of 16 KB */
#define CLUSTER 16384u

static int failures;
static char g_cache[512];

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-60s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static void set_name(XBOX_ANSI_STRING *name, XBOX_OBJECT_ATTRIBUTES *oa,
                     const char *path, HANDLE root)
{
    name->Length = (USHORT)strlen(path);
    name->MaximumLength = (USHORT)(name->Length + 1);
    name->Buffer = (PCHAR)path;
    oa->RootDirectory = root;
    oa->ObjectName = name;
    oa->Attributes = 0x40;
}

static NTSTATUS open_at(HANDLE root, const char *path, ULONG disposition,
                        ULONG options, HANDLE *out, ULONG_PTR *info)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    NTSTATUS st;

    set_name(&name, &oa, path, root);
    *out = NULL;
    ios.Information = 0xFFFF;
    st = xbox_NtCreateFile(out, 0xC0110000u /* R|W|DELETE|SYNC */, &oa, &ios,
                           NULL, 0x80, 0, disposition, options);
    if (info) *info = ios.Information;
    return st;
}

static NTSTATUS open_access(const char *path, ACCESS_MASK access, ULONG disposition,
                            HANDLE *out)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;

    set_name(&name, &oa, path, NULL);
    *out = NULL;
    return xbox_NtCreateFile(out, access, &oa, &ios, NULL, 0x80, 0, disposition, 0x60);
}

static NTSTATUS open_file(const char *path, ULONG disposition, HANDLE *out)
{
    return open_at(NULL, path, disposition, 0x60, out, NULL);
}

static NTSTATUS set_eof(HANDLE h, LONGLONG size)
{
    XBOX_FILE_END_OF_FILE_INFORMATION info;
    XBOX_IO_STATUS_BLOCK ios;
    info.EndOfFile.QuadPart = size;
    return xbox_NtSetInformationFile(h, &ios, &info, sizeof info,
                                     XboxFileEndOfFileInformation);
}

static NTSTATUS set_pos(HANDLE h, LONGLONG pos)
{
    XBOX_FILE_POSITION_INFORMATION info;
    XBOX_IO_STATUS_BLOCK ios;
    info.CurrentByteOffset.QuadPart = pos;
    return xbox_NtSetInformationFile(h, &ios, &info, sizeof info,
                                     XboxFilePositionInformation);
}

static LONGLONG query_pos(HANDLE h)
{
    XBOX_FILE_POSITION_INFORMATION info;
    XBOX_IO_STATUS_BLOCK ios;
    if (xbox_NtQueryInformationFile(h, &ios, &info, sizeof info,
                                    XboxFilePositionInformation) != 0)
        return -1;
    return info.CurrentByteOffset.QuadPart;
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

/* off < 0: at the current position */
static NTSTATUS write_at(HANDLE h, const void *buf, ULONG len, LONGLONG off)
{
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER o;
    o.QuadPart = off;
    return xbox_NtWriteFile(h, NULL, NULL, NULL, &ios, (PVOID)buf, len,
                            off < 0 ? NULL : &o);
}

static NTSTATUS read_at(HANDLE h, void *buf, ULONG len, LONGLONG off, ULONG *got)
{
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER o;
    NTSTATUS st;
    o.QuadPart = off;
    ios.Information = 0;
    st = xbox_NtReadFile(h, NULL, NULL, NULL, &ios, buf, len, off < 0 ? NULL : &o);
    if (got) *got = (ULONG)ios.Information;
    return st;
}

static NTSTATUS rename_to(HANDLE h, const char *path, BOOLEAN replace)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    set_name(&name, &oa, path, NULL);
    return xbox_RenameFileByHandle(h, &oa, replace);
}

static int exists(const char *path)
{
    HANDLE h;
    if (open_file(path, XBOX_FILE_OPEN, &h) != 0) return 0;
    xbox_NtClose(h);
    return 1;
}

static unsigned char pattern(unsigned i) { return (unsigned char)(i * 7 + 1); }

static int matches(const unsigned char *b, unsigned from, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (b[i] != pattern(from + i)) return 0;
    return 1;
}

/* Host-side size of <cache>/<name>; -1 if it does not exist. */
static long host_size(const char *name)
{
    char p[700];
    FILE *f;
    long n;
    snprintf(p, sizeof p, "%s" SEP_S "%s", g_cache, name);
    if (!(f = fopen(p, "rb"))) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

static void host_write(const char *name, const unsigned char *buf, size_t n)
{
    char p[700];
    FILE *f;
    snprintf(p, sizeof p, "%s" SEP_S "%s", g_cache, name);
    if ((f = fopen(p, "wb")) != NULL) {
        fwrite(buf, 1, n, f);
        fclose(f);
    }
}

static void make_dir(const char *p)
{
#if defined(_WIN32)
    CreateDirectoryA(p, NULL);
#else
    mkdir(p, 0755);
#endif
}

/* Entries directly in the host Cache dir. */
static int host_entries(void)
{
    int n = 0;
#if defined(_WIN32)
    char p[700];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    snprintf(p, sizeof p, "%s\\*", g_cache);
    f = FindFirstFileA(p, &fd);
    if (f == INVALID_HANDLE_VALUE) return -1;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) n++;
    } while (FindNextFileA(f, &fd));
    FindClose(f);
#else
    DIR *d = opendir(g_cache);
    struct dirent *e;
    if (!d) return -1;
    while ((e = readdir(d)) != NULL)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    closedir(d);
#endif
    return n;
}

/* Lists <dir> with <mask>; returns the names joined by ',' in out. */
static NTSTATUS list_dir(const char *dir, const char *mask, char *out, size_t n)
{
    static ULONG info[256];
    PXBOX_FILE_DIRECTORY_INFORMATION e = (PXBOX_FILE_DIRECTORY_INFORMATION)info;
    XBOX_ANSI_STRING m;
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    NTSTATUS st;
    HANDLE h;
    int first = 1, guard = 0;

    out[0] = 0;
    set_name(&name, &oa, dir, NULL);
    /* what XAPI's FindFirstFile asks for: list, synchronous, shared */
    st = xbox_NtCreateFile(&h, 0x00100001u, &oa, &ios, NULL, 0, 3, XBOX_FILE_OPEN, 0x21);
    if (st) return st;
    m.Length = (USHORT)strlen(mask);
    m.MaximumLength = (USHORT)(m.Length + 1);
    m.Buffer = (PCHAR)mask;
    for (;;) {
        memset(info, 0, sizeof info);
        st = xbox_NtQueryDirectoryFile(h, NULL, NULL, NULL, &ios, e, sizeof info,
                                       XboxFileDirectoryInformation, &m, first);
        if (st || ++guard > 64) break;
        snprintf(out + strlen(out), n - strlen(out), "%s%.*s", first ? "" : ",",
                 (int)e->FileNameLength, e->FileName);
        first = 0;
    }
    xbox_NtClose(h);
    return st;
}

static void test_off_by_default(void)
{
    HANDLE h;
    printf("\nOff by default\n");
    check(!xbox_ramfs_active(), "unset: the store is off", NULL);
    /* The first file call fixed it off; turning it on is still allowed until
     * the store starts, which is what the enhance layer does at boot. */
    xbox_RamCacheSetCap(CAP);
    xbox_RamCacheEnable(1);
    check(xbox_ramfs_active(), "xbox_RamCacheEnable(1) before first use", NULL);
    check(open_file("z:\\seed.bin", XBOX_FILE_OPEN, &h) == 0, "seed.bin opens", NULL);
    xbox_NtClose(h);
    xbox_RamCacheEnable(0);
    check(xbox_ramfs_active(), "turning it off after use is ignored", NULL);
}

static void test_seed(const unsigned char *buf)
{
    static unsigned char got[4096];
    char d[160];
    HANDLE h;
    ULONG n = 0;
    NTSTATUS st;

    printf("\nSeeded from <hdd>/Cache\n");
    st = open_file("z:\\SEED.BIN", XBOX_FILE_OPEN, &h);
    snprintf(d, sizeof d, "status 0x%08X", (unsigned)st);
    check(st == 0, "z:\\SEED.BIN opens (names ignore case)", d);
    if (st) return;
    check(query_size(h) == 3000, "seed size 3000", NULL);
    st = read_at(h, got, sizeof got, 0, &n);
    check(st == 0 && n == 3000 && memcmp(got, buf, 3000) == 0,
          "seed bytes read back", NULL);
    xbox_NtClose(h);
    st = open_file("z:\\sub\\s2.txt", XBOX_FILE_OPEN, &h);
    check(st == 0, "seeded subdirectory file opens", NULL);
    if (!st) {
        st = read_at(h, got, 5, 0, &n);
        check(st == 0 && n == 5 && memcmp(got, "hello", 5) == 0, "its bytes", NULL);
        xbox_NtClose(h);
    }
    /* Rewrite the seed in memory; disk keeps its copy. */
    st = open_file("z:\\seed.bin", XBOX_FILE_OVERWRITE_IF, &h);
    write_at(h, "new", 3, 0);
    check(query_size(h) == 3, "overwritten seed is 3 bytes in memory", NULL);
    xbox_NtClose(h);
    check(host_size("seed.bin") == 3000, "host seed.bin still 3000 bytes", NULL);
}

static void test_file_io(const unsigned char *buf)
{
    static unsigned char got[65536];
    char d[160];
    HANDLE h, h2;
    ULONG_PTR info = 0;
    ULONG n = 0;
    NTSTATUS st;

    printf("\nCreate, rewrite, seek, size\n");
    st = open_at(NULL, "z:\\pelon000.rec", XBOX_FILE_OVERWRITE_IF, 0x60, &h, &info);
    snprintf(d, sizeof d, "status 0x%08X info %lu", (unsigned)st, (unsigned long)info);
    check(st == 0 && info == 2, "create z:\\pelon000.rec (CREATED)", d);
    if (st) return;
    st = write_at(h, buf, 65536, 0);
    check(st == 0, "write 64 KB", NULL);
    check(query_pos(h) == 65536, "position after the write is 64 KB", NULL);
    xbox_NtClose(h);
    check(host_size("pelon000.rec") < 0, "nothing on the host disk", NULL);

    st = open_at(NULL, "z:\\pelon000.rec", XBOX_FILE_OPEN_IF, 0x60, &h, &info);
    check(st == 0 && info == 1, "reopen (OPENED)", NULL);
    check(query_size(h) == 65536, "size 64 KB", NULL);
    check(set_pos(h, 1000) == 0 && query_pos(h) == 1000, "seek to 1000", NULL);
    st = read_at(h, got, 10, -1, &n);
    check(st == 0 && n == 10 && matches(got, 1000, 10), "read 10 at the position", NULL);
    check(query_pos(h) == 1010, "position moved to 1010", NULL);
    st = read_at(h, got, 100, 65500, &n);
    check(st == 0 && n == 36 && matches(got, 65500, 36), "short read at the end", NULL);
    st = read_at(h, got, 10, 65536, &n);
    check(st == ST_END_OF_FILE, "read at the end: STATUS_END_OF_FILE", NULL);
    write_at(h, "XY", 2, -1);   /* at the current position, past the read */
    check(query_pos(h) == 65538, "write at the position extends", NULL);
    check(query_size(h) == 65538, "size 65538", NULL);
    check(set_eof(h, 100) == 0 && query_size(h) == 100, "set end of file to 100", NULL);
    /* a sparse write: the gap reads as zeros */
    write_at(h, "Z", 1, 200);
    st = read_at(h, got, 201, 0, &n);
    check(st == 0 && n == 201 && matches(got, 0, 100) && got[150] == 0 && got[200] == 'Z',
          "write past the end zero-fills the gap", NULL);
    xbox_NtClose(h);

    /* A second handle sees the first's writes. */
    open_file("z:\\pelon000.rec", XBOX_FILE_OPEN, &h);
    open_file("z:\\pelon000.rec", XBOX_FILE_OPEN, &h2);
    write_at(h, "Q", 1, 0);
    read_at(h2, got, 1, 0, &n);
    check(got[0] == 'Q', "two handles share the file", NULL);
    xbox_NtClose(h);
    xbox_NtClose(h2);

    st = open_file("z:\\pelon000.rec", XBOX_FILE_CREATE, &h);
    check(st == ST_COLLISION, "FILE_CREATE of an existing file: collision", NULL);
    st = open_file("z:\\missing.rec", XBOX_FILE_OPEN, &h);
    check(st == ST_NOT_FOUND, "FILE_OPEN of a missing file: not found", NULL);
    st = open_file("z:\\missing.rec", XBOX_FILE_OVERWRITE, &h);
    check(st != 0, "FILE_OVERWRITE of a missing file fails", NULL);
}

/* The texture-cache copy, as tests/kernel_fatx_overwrite runs it on disk. */
static void test_fatx_overwrite(const unsigned char *buf)
{
    static unsigned char got[4096];
    HANDLE h;
    ULONG n = 0;
    int same;
    unsigned i;

    printf("\nFATX overwrite\n");
    open_file("z:\\o.tmp", XBOX_FILE_OVERWRITE_IF, &h);
    set_eof(h, 4096);
    write_at(h, buf, 4096, 0);
    xbox_NtClose(h);
    open_file("z:\\o.tmp", XBOX_FILE_OVERWRITE_IF, &h);
    check(query_pos(h) == 0, "position query after the overwrite", NULL);
    check(set_eof(h, 3000) == 0, "set end of file to 3000", NULL);
    check(rename_to(h, "z:\\o.dat", TRUE) == 0, "rename to z:\\o.dat", NULL);
    xbox_NtClose(h);
    check(!exists("z:\\o.tmp"), "o.tmp is gone", NULL);
    open_file("z:\\o.dat", XBOX_FILE_OPEN, &h);
    check(query_size(h) == 3000, "o.dat is 3000 bytes", NULL);
    read_at(h, got, 3000, 0, &n);
    check(n == 3000 && matches(got, 0, 3000), "the bytes below the new end survive", NULL);
    xbox_NtClose(h);

    open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, &h);
    write_at(h, buf, 4, 0);
    check(query_size(h) == 4, "overwrite + 4-byte write leaves 4 bytes", NULL);
    xbox_NtClose(h);
    open_file("z:\\o.dat", XBOX_FILE_OVERWRITE, &h);
    check(query_size(h) == 0, "overwrite + size query reports 0", NULL);
    xbox_NtClose(h);
    open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, &h);
    write_at(h, buf, 100, 0);
    xbox_NtClose(h);
    open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, &h);
    xbox_NtClose(h);
    open_file("z:\\o.dat", XBOX_FILE_OPEN, &h);
    check(query_size(h) == 0, "overwrite + close leaves 0 bytes", NULL);
    write_at(h, buf, 100, 0);
    xbox_NtClose(h);
    open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, &h);
    set_eof(h, 200);
    read_at(h, got, 200, 0, &n);
    same = n == 200;
    for (i = 0; same && i < 200; i++)
        if (got[i] != (i < 100 ? pattern(i) : 0)) same = 0;
    check(same, "regrow past the old end: old bytes, then zeros", NULL);
    xbox_NtClose(h);
}

/* The cap, the ring, and the volume size. */
static void test_cap(const unsigned char *buf)
{
    XBOX_FILE_FS_SIZE_INFORMATION fs;
    XBOX_IO_STATUS_BLOCK ios;
    char name[32], d[160];
    uint64_t used0, used;
    HANDLE h;
    NTSTATUS st;
    unsigned i;

    printf("\nThe cap\n");
    used0 = xbox_RamCacheUsed();
    open_file("z:\\big.rec", XBOX_FILE_OVERWRITE_IF, &h);
    st = set_eof(h, CAP + 1);
    snprintf(d, sizeof d, "status 0x%08X", (unsigned)st);
    check(st == ST_DISK_FULL, "end of file past the cap: STATUS_DISK_FULL", d);
    st = write_at(h, buf, 65536, (LONGLONG)CAP);
    check(st == ST_DISK_FULL, "write past the cap: STATUS_DISK_FULL", NULL);
    check(xbox_RamCacheUsed() == used0, "a refused write charges nothing", NULL);
    st = set_eof(h, (LONGLONG)(CAP - used0 - CLUSTER));
    check(st == 0, "a file up to the space left fits", NULL);
    st = xbox_NtQueryVolumeInformationFile(h, &ios, &fs, sizeof fs, XboxFileFsSizeInformation);
    snprintf(d, sizeof d, "total %lld avail %lld", (long long)fs.TotalAllocationUnits.QuadPart,
             (long long)fs.AvailableAllocationUnits.QuadPart);
    check(st == 0 && fs.TotalAllocationUnits.QuadPart == CAP / CLUSTER &&
          fs.AvailableAllocationUnits.QuadPart == 1 && fs.BytesPerSector == 512 &&
          fs.SectorsPerAllocationUnit == 32, "volume size: the cap, one cluster free", d);
    xbox_NtClose(h);
    open_file("z:\\big.rec", XBOX_FILE_OVERWRITE_IF, &h);
    xbox_NtClose(h);
    check(xbox_RamCacheUsed() == used0, "an overwrite gives the space back", NULL);

    /* BLiNX 1's ring: the same names rewritten over and over stay bounded. */
    for (i = 0; i < 200; i++) {
        snprintf(name, sizeof name, "z:\\pelon%03u.rec", 100 + i % 5);
        st = open_file(name, XBOX_FILE_OVERWRITE_IF, &h);
        if (st) break;
        if (write_at(h, buf, 65536, 0) != 0) st = 1;
        xbox_NtClose(h);
        if (st) break;
    }
    used = xbox_RamCacheUsed();
    snprintf(d, sizeof d, "%u rewrites, used %llu", i, (unsigned long long)used);
    check(i == 200 && used == used0 + 5 * 65536, "200 ring rewrites of 5 files: 5 files' worth", d);
    for (i = 0; i < 5; i++) {
        snprintf(name, sizeof name, "z:\\pelon%03u.rec", 100 + i);
        {
            XBOX_ANSI_STRING nm;
            XBOX_OBJECT_ATTRIBUTES oa;
            set_name(&nm, &oa, name, NULL);
            xbox_NtDeleteFile(&oa);
        }
    }
    check(xbox_RamCacheUsed() == used0, "deleting the ring gives the space back", NULL);
}

static void test_delete(void)
{
    XBOX_FILE_DISPOSITION_INFORMATION di;
    XBOX_IO_STATUS_BLOCK ios;
    unsigned char got[8];
    HANDLE h, h2;
    ULONG n = 0;

    printf("\nDelete\n");
    open_at(NULL, "z:\\doc.tmp", XBOX_FILE_CREATE, 0x60 | 0x1000, &h, NULL);
    write_at(h, "abc", 3, 0);
    check(exists("z:\\doc.tmp"), "delete-on-close file exists while open", NULL);
    xbox_NtClose(h);
    check(!exists("z:\\doc.tmp"), "and is gone after the close", NULL);

    open_file("z:\\disp.tmp", XBOX_FILE_CREATE, &h);
    write_at(h, "abc", 3, 0);
    open_file("z:\\disp.tmp", XBOX_FILE_OPEN, &h2);
    di.DeleteFile = TRUE;
    check(xbox_NtSetInformationFile(h, &ios, &di, sizeof di,
                                    XboxFileDispositionInformation) == 0,
          "set disposition delete", NULL);
    xbox_NtClose(h);
    check(!exists("z:\\disp.tmp"), "the name goes at once", NULL);
    check(read_at(h2, got, 3, 0, &n) == 0 && n == 3 && memcmp(got, "abc", 3) == 0,
          "an open handle still reads it", NULL);
    xbox_NtClose(h2);
}

static void test_dirs(void)
{
    char names[512], d[600];
    HANDLE h, dir;
    NTSTATUS st;

    printf("\nDirectories\n");
    st = open_at(NULL, "z:\\d", XBOX_FILE_CREATE, XBOX_FILE_DIRECTORY_FILE | 0x20, &dir, NULL);
    check(st == 0, "create directory z:\\d", NULL);
    if (st) return;
    open_file("z:\\d\\a.txt", XBOX_FILE_CREATE, &h);
    xbox_NtClose(h);
    open_file("z:\\d\\b.dat", XBOX_FILE_CREATE, &h);
    xbox_NtClose(h);
    /* relative to the directory handle */
    st = open_at(dir, "c.txt", XBOX_FILE_CREATE, 0x60, &h, NULL);
    check(st == 0, "create c.txt relative to the directory handle", NULL);
    xbox_NtClose(h);
    xbox_NtClose(dir);

    st = list_dir("z:\\d", "*", names, sizeof names);
    snprintf(d, sizeof d, "\"%s\" end 0x%08X", names, (unsigned)st);
    check(st == ST_NO_MORE_FILES && !strcmp(names, "a.txt,b.dat,c.txt"), "list *", d);
    list_dir("z:\\d", "*.txt", names, sizeof names);
    snprintf(d, sizeof d, "\"%s\"", names);
    check(!strcmp(names, "a.txt,c.txt"), "list *.txt", d);
    st = list_dir("z:\\d", "none*", names, sizeof names);
    check(st != 0 && names[0] == 0, "no match: nothing listed", NULL);
    st = open_file("z:\\nodir\\x.txt", XBOX_FILE_CREATE, &h);
    check(st != 0, "create in a missing directory fails", NULL);

    st = open_file("z:\\d\\c.txt", XBOX_FILE_OPEN, &h);
    check(rename_to(h, "z:\\d\\a.txt", FALSE) == ST_COLLISION,
          "rename onto an existing name without replace: collision", NULL);
    check(rename_to(h, "z:\\e.txt", FALSE) == 0, "rename out of the directory", NULL);
    xbox_NtClose(h);
    check(!exists("z:\\d\\c.txt") && exists("z:\\e.txt"), "moved", NULL);

    st = open_file("\\Device\\Harddisk0\\Partition3\\e.txt", XBOX_FILE_OPEN, &h);
    check(st == 0, "Partition3\\e.txt is the same store", NULL);
    if (!st) xbox_NtClose(h);

    st = open_file("z:\\d\\a.txt", XBOX_FILE_OPEN, &h);
    check(rename_to(h, "c:\\a.txt", TRUE) == ST_NOT_SAME_DEV,
          "rename to another drive: not the same device", NULL);
    xbox_NtClose(h);
}

/* A handle opened for read and write (GENERIC_WRITE, which holds APPEND_DATA)
 * writes at its offset; only an append-only one appends. `path` is on the
 * store or on the host backend. */
static void check_append(const char *path, const char *where)
{
    unsigned char got[8];
    char what[96];
    HANDLE h;
    ULONG n = 0;
    NTSTATUS st;

    st = open_access(path, ACC_GENERIC_RW, XBOX_FILE_OVERWRITE_IF, &h);
    snprintf(what, sizeof what, "%s: open with GENERIC_READ|WRITE", where);
    check(st == 0, what, NULL);
    if (st) return;
    write_at(h, "0123456789", 10, 0);
    write_at(h, "ab", 2, 3);
    read_at(h, got, 8, 0, &n);
    snprintf(what, sizeof what, "%s: GENERIC_WRITE writes at its offset", where);
    check(query_size(h) == 10 && n == 8 && memcmp(got, "012ab567", 8) == 0, what, NULL);
    xbox_NtClose(h);
    st = open_access(path, ACC_APPEND_ONLY, XBOX_FILE_OPEN, &h);
    if (!st) {
        write_at(h, "Z", 1, 0);
        xbox_NtClose(h);
    }
    open_file(path, XBOX_FILE_OPEN, &h);
    read_at(h, got, 1, 10, &n);
    snprintf(what, sizeof what, "%s: an append-only handle appends", where);
    check(st == 0 && query_size(h) == 11 && n == 1 && got[0] == 'Z', what, NULL);
    xbox_NtClose(h);
}

static void test_append(const char *host_path)
{
    printf("\nOffsets on a GENERIC_WRITE handle\n");
    check_append("z:\\gw.bin", "store");
    check_append(host_path, "host");
}

static void test_handles(void)
{
    unsigned char got[8];
    HANDLE h, d, d2;
    ULONG n = 0;

    printf("\nDuplicates and closes\n");
    open_file("z:\\dup.bin", XBOX_FILE_OVERWRITE_IF, &h);
    write_at(h, "abcdef", 6, 0);
    set_pos(h, 2);
    check(xbox_ramfs_duplicate(h, &d, 0) == 1 && d != NULL, "duplicate a store handle", NULL);
    read_at(d, got, 2, -1, &n);
    check(n == 2 && memcmp(got, "cd", 2) == 0 && query_pos(h) == 4,
          "the duplicate shares the position", NULL);
    check(xbox_ramfs_duplicate(d, &d2, 1) == 1 && d2 != NULL,
          "duplicate with close-source", NULL);
    check(xbox_ramfs_close(d) == 0, "the source is closed", NULL);
    check(query_pos(d2) == 4, "the new handle has the position", NULL);
    check(xbox_ramfs_close(h) == 1, "close the first handle", NULL);
    check(xbox_ramfs_close(h) == 0, "a second close is not the store's", NULL);
    read_at(d2, got, 2, 0, &n);
    check(n == 2 && memcmp(got, "ab", 2) == 0, "the file lives on in the last handle", NULL);
    xbox_NtClose(d2);
    check(exists("z:\\dup.bin"), "and keeps its name after the last close", NULL);

    printf("\nShrink, then regrow\n");
    open_file("z:\\dup.bin", XBOX_FILE_OPEN, &h);
    set_eof(h, 2);
    set_eof(h, 6);
    read_at(h, got, 6, 0, &n);
    check(n == 6 && memcmp(got, "ab\0\0\0\0", 6) == 0,
          "bytes cut off by a shrink read back as zero", NULL);
    xbox_NtClose(h);
}

static NTSTATUS query_one(HANDLE h, void *buf, ULONG len, BOOLEAN restart,
                          XBOX_IO_STATUS_BLOCK *ios)
{
    XBOX_ANSI_STRING m;
    m.Length = 1;
    m.MaximumLength = 2;
    m.Buffer = (PCHAR)"*";
    memset(buf, 0, len);
    return xbox_NtQueryDirectoryFile(h, NULL, NULL, NULL, ios, buf, len,
                                     XboxFileDirectoryInformation, &m, restart);
}

static void test_seed_listing(void)
{
    static ULONG info[256];
    PXBOX_FILE_DIRECTORY_INFORMATION e = (PXBOX_FILE_DIRECTORY_INFORMATION)info;
    ULONG header = (ULONG)((ULONG_PTR)&((PXBOX_FILE_DIRECTORY_INFORMATION)0)->FileName);
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    char names[512], d[600], first[64];
    HANDLE h;
    NTSTATUS st;

    printf("\nSeeds in listings and deletes\n");
    st = list_dir("z:\\sub", "*", names, sizeof names);
    snprintf(d, sizeof d, "\"%s\"", names);
    check(!strcmp(names, "never.dat,s2.txt") || !strcmp(names, "s2.txt,never.dat"),
          "a seed never opened is listed", d);
    st = list_dir("z:\\sub", "never*", names, sizeof names);
    check(!strcmp(names, "never.dat"), "and matched by a pattern", NULL);

    set_name(&name, &oa, "z:\\sub", NULL);
    st = xbox_NtCreateFile(&h, 0x00100001u, &oa, &ios, NULL, 0, 3, XBOX_FILE_OPEN, 0x21);
    if (st) return;
    query_one(h, info, sizeof info, TRUE, &ios);
    snprintf(first, sizeof first, "%.*s", (int)e->FileNameLength, e->FileName);
    query_one(h, info, sizeof info, FALSE, &ios);
    st = query_one(h, info, sizeof info, TRUE, &ios);
    check(st == 0 && e->FileNameLength == strlen(first)
          && !memcmp(e->FileName, first, strlen(first)),
          "a restarted scan starts from the first entry again", first);
    st = query_one(h, info, header - 1, TRUE, &ios);
    check(st == ST_BUF_TOO_SMALL && ios.Information == 0,
          "a buffer under the entry header: STATUS_BUFFER_TOO_SMALL", NULL);
    st = query_one(h, info, header + 2, FALSE, &ios);
    snprintf(d, sizeof d, "status 0x%08X info %lu", (unsigned)st,
             (unsigned long)ios.Information);
    check(st == ST_BUF_OVERFLOW && ios.Information == header + 2
          && e->FileNameLength == strlen(first) && !memcmp(e->FileName, first, 2),
          "a name cut short: the same entry, STATUS_BUFFER_OVERFLOW", d);
    st = query_one(h, info, sizeof info, FALSE, &ios);
    check(st == 0 && strncmp(e->FileName, first, e->FileNameLength) != 0,
          "and the cut entry is used up", NULL);
    xbox_NtClose(h);

    set_name(&name, &oa, "z:\\sub\\never.dat", NULL);
    check(xbox_NtDeleteFile(&oa) == 0, "delete a seed never opened", NULL);
    check(!exists("z:\\sub\\never.dat"), "it stays deleted", NULL);
    st = open_file("z:\\sub\\never.dat", XBOX_FILE_OPEN_IF, &h);
    check(st == 0 && query_size(h) == 0, "a new file of that name starts empty", NULL);
    if (!st) xbox_NtClose(h);
    check(host_size("sub" SEP_S "never.dat") == 4, "the host seed is untouched", NULL);
}

int main(int argc, char **argv)
{
    static unsigned char buf[65536];
    char root[512], p[700];
    unsigned i;

    /* argv[1]: write the report there (Proton drops a console exe's stdout) */
    if (argc > 1 && !freopen(argv[1], "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        snprintf(root, sizeof root, "%sramcache-%lu", tmp,
                 (unsigned long)GetCurrentProcessId());
        if (!CreateDirectoryA(root, NULL)) return 2;
    }
#else
    {
        char tmpl[] = "/tmp/ramcacheXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(root, sizeof root, "%s", tmpl);
    }
#endif
    snprintf(g_cache, sizeof g_cache, "%s" SEP_S "Cache", root);
    make_dir(g_cache);
    for (i = 0; i < sizeof buf; i++) buf[i] = pattern(i);
    host_write("seed.bin", buf, 3000);
    snprintf(p, sizeof p, "%s" SEP_S "sub", g_cache);
    make_dir(p);
    host_write("sub" SEP_S "s2.txt", (const unsigned char *)"hello", 5);
    host_write("sub" SEP_S "never.dat", (const unsigned char *)"seed", 4);
    snprintf(p, sizeof p, "%s" SEP_S "UDATA", root);
    make_dir(p);
    xbox_path_init(root, root);

    test_off_by_default();
    test_seed(buf);
    test_file_io(buf);
    test_fatx_overwrite(buf);
    test_cap(buf);
    test_delete();
    test_dirs();
    test_append("\\Device\\Harddisk0\\Partition1\\UDATA\\gw.bin");
    test_handles();
    test_seed_listing();

    printf("\nHost Cache\n");
    check(host_entries() == 2, "only the two seeded entries on disk", NULL);
    check(host_size("seed.bin") == 3000, "seed.bin unchanged in size", NULL);

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
