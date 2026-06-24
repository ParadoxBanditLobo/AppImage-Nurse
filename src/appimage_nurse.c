#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <elf.h>
#include <ctype.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define VERSION "0.9.20 Preview"
#define MAX_WALK_FILES 20000
#define MAX_ELF_SCAN 5000
#define MAX_DEP_LINES 8000
#define MAX_REPORT_LINE 4096

typedef struct {
    char **items;
    size_t len;
    size_t cap;
} StrList;

typedef struct {
    int major;
    int minor;
    int patch;
    char text[64];
    bool seen;
} VersionMax;

typedef struct {
    int ok;
    int warn;
    int fail;
} Counts;

typedef struct {
    char root[PATH_MAX];
    StrList all_files;
    StrList elf_files;
    StrList provided_names;
    StrList broken_links;
    StrList desktop_files;
    size_t walked;
} WalkCtx;

typedef struct {
    char name[256];
    char exec[1024];
    char icon[1024];
    char type[128];
    char categories[1024];
} DesktopInfo;

typedef struct {
    char path[PATH_MAX];
    char arch[128];
    StrList needed;
    bool is_dynamic;
    bool parsed;
    char parse_note[256];
} ElfInfo;

static Counts g_counts = {0,0,0};
static FILE *g_report = NULL;
static bool g_quiet = false;
static bool g_color = true;
static StrList g_session_history = {0};


#define C_RESET "\033[0m"
#define C_OK "\033[32m"
#define C_WARN "\033[33m"
#define C_FAIL "\033[31m"
#define C_INFO "\033[36m"
#define C_BOLD "\033[1m"

static const char *status_color(const char *status) {
    if (!g_color) return "";
    if (strcmp(status, "OK") == 0) return C_OK;
    if (strcmp(status, "WARN") == 0) return C_WARN;
    if (strcmp(status, "FAIL") == 0) return C_FAIL;
    return C_INFO;
}
static const char *color_reset(void) { return g_color ? C_RESET : ""; }
static void color_printf(const char *color, const char *fmt, ...) {
    va_list ap;
    if (g_color && color && *color) fputs(color, stdout);
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    if (g_color && color && *color) fputs(C_RESET, stdout);
}

static void list_init(StrList *l) { l->items = NULL; l->len = 0; l->cap = 0; }
static void list_free(StrList *l) {
    if (!l) return;
    for (size_t i = 0; i < l->len; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL; l->len = 0; l->cap = 0;
}
static bool list_contains(const StrList *l, const char *s) {
    if (!l || !s) return false;
    for (size_t i = 0; i < l->len; i++) if (strcmp(l->items[i], s) == 0) return true;
    return false;
}
static void list_add(StrList *l, const char *s) {
    if (!l || !s || !*s) return;
    if (list_contains(l, s)) return;
    if (l->len == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 16;
        char **ni = realloc(l->items, nc * sizeof(char*));
        if (!ni) return;
        l->items = ni; l->cap = nc;
    }
    l->items[l->len] = strdup(s);
    if (l->items[l->len]) l->len++;
}
static void list_add_allow_dupe(StrList *l, const char *s) {
    if (!l || !s || !*s) return;
    if (l->len == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 16;
        char **ni = realloc(l->items, nc * sizeof(char*));
        if (!ni) return;
        l->items = ni; l->cap = nc;
    }
    l->items[l->len] = strdup(s);
    if (l->items[l->len]) l->len++;
}

static void session_history_init(void) {
    if (!g_session_history.items) list_init(&g_session_history);
}

static void session_log(const char *fmt, ...) {
    session_history_init();
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char line[2300];
    snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d  %s",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec, msg);
    list_add_allow_dupe(&g_session_history, line);
}

static void show_session_history(void) {
    session_history_init();
    printf("Session history is temporary and is not saved unless you choose to save it.\n\n");
    if (g_session_history.len == 0) { printf("No session actions logged yet.\n"); return; }
    for (size_t i = 0; i < g_session_history.len; i++) printf("%zu. %s\n", i + 1, g_session_history.items[i]);
}

static int save_session_history(const char *path) {
    session_history_init();
    if (!path || !*path) return 1;
    FILE *f = fopen(path, "w");
    if (!f) { printf("Could not save session history: %s\n", strerror(errno)); return 1; }
    fprintf(f, "AppImage Nurse %s - Session History\n", VERSION);
    fprintf(f, "History is user-saved only; AppImage Nurse does not keep a permanent history log.\n\n");
    for (size_t i = 0; i < g_session_history.len; i++) fprintf(f, "%zu. %s\n", i + 1, g_session_history.items[i]);
    fclose(f);
    printf("Saved session history: %s\n", path);
    return 0;
}

static void clear_session_history(void) {
    list_free(&g_session_history);
    list_init(&g_session_history);
    printf("Cleared temporary session history.\n");
}



static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

static bool contains_case_insensitive(const char *s, const char *needle) {
    if (!s || !needle) return false;
    size_t n = strlen(needle);
    if (n == 0) return true;
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}

static void report_line(const char *fmt, ...) {
    char buf[MAX_REPORT_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!g_quiet) puts(buf);
    if (g_report) fprintf(g_report, "%s\n", buf);
}

static void status_line(const char *status, const char *fmt, ...) {
    char msg[MAX_REPORT_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (strcmp(status, "OK") == 0) g_counts.ok++;
    else if (strcmp(status, "WARN") == 0) g_counts.warn++;
    else if (strcmp(status, "FAIL") == 0) g_counts.fail++;
    if (!g_quiet) {
        printf("[");
        color_printf(status_color(status), "%s", status);
        printf("] %s\n", msg);
    }
    if (g_report) fprintf(g_report, "[%s] %s\n", status, msg);
}

static void summary_line(void) {
    if (!g_quiet) {
        printf("\nSummary: ");
        color_printf(C_OK, "%d OK", g_counts.ok);
        printf(", ");
        color_printf(g_counts.warn ? C_WARN : C_OK, "%d warning(s)", g_counts.warn);
        printf(", ");
        color_printf(g_counts.fail ? C_FAIL : C_OK, "%d failure(s)", g_counts.fail);
        printf(".\n");
    }
    if (g_report) fprintf(g_report, "\nSummary: %d OK, %d warning(s), %d failure(s).\n", g_counts.ok, g_counts.warn, g_counts.fail);
}

static bool path_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}
static bool is_dir_path(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static bool dir_is_empty(const char *p) {
    DIR *d = opendir(p);
    if (!d) return false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) { closedir(d); return false; }
    }
    closedir(d);
    return true;
}
static bool is_file_path(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
static bool is_exec_path(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR);
}

static void join_path(char *out, size_t outsz, const char *a, const char *b) {
    if (!a || !*a) snprintf(out, outsz, "%s", b ? b : "");
    else if (!b || !*b) snprintf(out, outsz, "%s", a);
    else if (a[strlen(a)-1] == '/') snprintf(out, outsz, "%s%s", a, b);
    else snprintf(out, outsz, "%s/%s", a, b);
}

static char *trim(char *s) {
    if (!s) return s;
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = 0;
    return s;
}


static int prompt_line(const char *label, char *out, size_t outsz);
static void parent_dir(const char *path, char *out, size_t outsz);
static int mkdir_p(const char *path, mode_t mode);

static bool ends_with_ci(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t ls = strlen(s), lf = strlen(suffix);
    if (ls < lf) return false;
    s += ls - lf;
    for (size_t i = 0; i < lf; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suffix[i])) return false;
    }
    return true;
}

static void normalize_appimage_name(const char *name, char *out, size_t outsz) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", name && *name ? name : "Output");
    char *t = trim(tmp);
    memmove(tmp, t, strlen(t) + 1);
    if (!tmp[0]) snprintf(tmp, sizeof(tmp), "Output");
    size_t n = strlen(tmp);
    if (ends_with_ci(tmp, ".AppImage")) {
        if (n >= 9) tmp[n - 9] = 0;
    }
    snprintf(out, outsz, "%s.AppImage", tmp);
}

static void normalize_appdir_name(const char *name, char *out, size_t outsz) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", name && *name ? name : "Output");
    char *t = trim(tmp);
    memmove(tmp, t, strlen(t) + 1);
    if (!tmp[0]) snprintf(tmp, sizeof(tmp), "Output");
    size_t n = strlen(tmp);
    if (ends_with_ci(tmp, ".AppDir")) {
        if (n >= 7) tmp[n - 7] = 0;
    }
    snprintf(out, outsz, "%s.AppDir", tmp);
}

static bool filename_has_extension(const char *name) {
    if (!name) return false;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    const char *dot = strrchr(base, '.');
    return dot && dot != base && dot[1] != 0;
}

static void normalize_text_name(const char *name, char *out, size_t outsz) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", name && *name ? name : "session-history");
    char *t = trim(tmp);
    memmove(tmp, t, strlen(t) + 1);
    if (!tmp[0]) snprintf(tmp, sizeof(tmp), "session-history");
    if (filename_has_extension(tmp)) snprintf(out, outsz, "%s", tmp);
    else snprintf(out, outsz, "%s.txt", tmp);
}

static int prompt_output_text_path(char *out, size_t outsz) {
    char folder[PATH_MAX], name[PATH_MAX], fixed[PATH_MAX];
    if (prompt_line("Destination folder", folder, sizeof(folder)) != 0) return 1;
    if (prompt_line("Text filename", name, sizeof(name)) != 0) return 1;
    normalize_text_name(name, fixed, sizeof(fixed));
    join_path(out, outsz, folder, fixed);
    printf("Using output text path: %s\n", out);
    return 0;
}

static int prompt_output_appimage_path(char *out, size_t outsz) {
    char folder[PATH_MAX], name[PATH_MAX], fixed[PATH_MAX];
    if (prompt_line("Output folder", folder, sizeof(folder)) != 0) return 1;
    if (prompt_line("AppImage filename", name, sizeof(name)) != 0) return 1;
    normalize_appimage_name(name, fixed, sizeof(fixed));
    join_path(out, outsz, folder, fixed);
    printf("Using output AppImage path: %s\n", out);
    return 0;
}

static int prompt_output_appdir_path(char *out, size_t outsz) {
    char folder[PATH_MAX], name[PATH_MAX], fixed[PATH_MAX];
    if (prompt_line("Parent folder", folder, sizeof(folder)) != 0) return 1;
    if (prompt_line("AppDir folder name", name, sizeof(name)) != 0) return 1;
    normalize_appdir_name(name, fixed, sizeof(fixed));
    join_path(out, outsz, folder, fixed);
    printf("Using output AppDir path: %s\n", out);
    return 0;
}

static const char *icon_ext_from_path(const char *p) {
    if (p && ends_with_ci(p, ".svg")) return "svg";
    if (p && ends_with_ci(p, ".xpm")) return "xpm";
    return "png";
}

static int write_placeholder_icon_xpm(const char *path) {
    char pd[PATH_MAX]; parent_dir(path, pd, sizeof(pd));
    if (mkdir_p(pd, 0755) != 0) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "/* XPM */\nstatic char * appimage_nurse_icon[] = {\n");
    fprintf(f, "\"32 32 2 1\",\n");
    fprintf(f, "\"r c #cc0000\",\n");
    fprintf(f, "\"w c #ffffff\",\n");
    for (int y = 0; y < 32; y++) {
        fputc('"', f);
        for (int x = 0; x < 32; x++) {
            int plus = ((x >= 14 && x <= 17 && y >= 7 && y <= 24) || (y >= 14 && y <= 17 && x >= 7 && x <= 24));
            fputc(plus ? 'w' : 'r', f);
        }
        fprintf(f, y == 31 ? "\"\n" : "\",\n");
    }
    fprintf(f, "};\n");
    fclose(f);
    chmod(path, 0644);
    return 0;
}

static void strip_quotes(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n-1] == '"') || (s[0] == '\'' && s[n-1] == '\''))) {
        memmove(s, s+1, n-2);
        s[n-2] = 0;
    }
}

static void first_exec_token(const char *exec_line, char *out, size_t outsz) {
    out[0] = 0;
    if (!exec_line) return;
    const char *s = exec_line;
    while (isspace((unsigned char)*s)) s++;
    size_t i = 0;
    char quote = 0;
    if (*s == '"' || *s == '\'') quote = *s++;
    while (*s && i + 1 < outsz) {
        if (quote) {
            if (*s == quote) break;
        } else {
            if (isspace((unsigned char)*s)) break;
        }
        if (*s == '%' && s[1]) break;
        out[i++] = *s++;
    }
    out[i] = 0;
    strip_quotes(out);
}

static bool read_file_prefix(const char *path, unsigned char *buf, size_t want, size_t *got) {
    *got = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t r = read(fd, buf, want);
    close(fd);
    if (r < 0) return false;
    *got = (size_t)r;
    return true;
}

static bool file_is_elf(const char *path) {
    unsigned char b[4]; size_t got = 0;
    if (!read_file_prefix(path, b, sizeof(b), &got) || got < 4) return false;
    return b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F';
}

static const char *machine_to_str(uint16_t m) {
    switch (m) {
        case EM_386: return "x86 32-bit";
        case EM_X86_64: return "x86_64";
        case EM_AARCH64: return "aarch64";
        case EM_ARM: return "arm";
        case EM_RISCV: return "riscv";
        case EM_PPC64: return "ppc64";
        case EM_PPC: return "ppc";
        default: return "unknown";
    }
}

static bool read_entire_file(const char *path, unsigned char **data, size_t *sz) {
    *data = NULL; *sz = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) { close(fd); return false; }
    if ((uint64_t)st.st_size > (uint64_t)256 * 1024 * 1024) { close(fd); return false; }
    size_t n = (size_t)st.st_size;
    unsigned char *buf = malloc(n ? n : 1);
    if (!buf) { close(fd); return false; }
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, buf + off, n - off);
        if (r <= 0) { free(buf); close(fd); return false; }
        off += (size_t)r;
    }
    close(fd);
    *data = buf; *sz = n;
    return true;
}

static bool vaddr_to_offset64(const Elf64_Phdr *ph, int phnum, uint64_t vaddr, uint64_t *off) {
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t start = ph[i].p_vaddr;
        uint64_t end = ph[i].p_vaddr + ph[i].p_filesz;
        if (vaddr >= start && vaddr < end) {
            *off = ph[i].p_offset + (vaddr - start);
            return true;
        }
    }
    return false;
}
static bool vaddr_to_offset32(const Elf32_Phdr *ph, int phnum, uint32_t vaddr, uint32_t *off) {
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint32_t start = ph[i].p_vaddr;
        uint32_t end = ph[i].p_vaddr + ph[i].p_filesz;
        if (vaddr >= start && vaddr < end) {
            *off = ph[i].p_offset + (vaddr - start);
            return true;
        }
    }
    return false;
}

static void parse_elf_needed(const char *path, ElfInfo *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->path, sizeof(out->path), "%s", path);
    list_init(&out->needed);
    unsigned char *data = NULL; size_t sz = 0;
    if (!read_entire_file(path, &data, &sz)) {
        snprintf(out->parse_note, sizeof(out->parse_note), "could not read file or file too large");
        return;
    }
    if (sz < EI_NIDENT || memcmp(data, ELFMAG, SELFMAG) != 0) {
        snprintf(out->parse_note, sizeof(out->parse_note), "not an ELF file");
        free(data); return;
    }
    if (data[EI_DATA] != ELFDATA2LSB) {
        snprintf(out->parse_note, sizeof(out->parse_note), "non-little-endian ELF not supported in this prototype");
        free(data); return;
    }
    if (data[EI_CLASS] == ELFCLASS64) {
        if (sz < sizeof(Elf64_Ehdr)) { free(data); return; }
        Elf64_Ehdr *eh = (Elf64_Ehdr*)data;
        snprintf(out->arch, sizeof(out->arch), "%s", machine_to_str(eh->e_machine));
        if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > sz) {
            snprintf(out->parse_note, sizeof(out->parse_note), "program headers out of range"); free(data); return;
        }
        Elf64_Phdr *ph = (Elf64_Phdr*)(data + eh->e_phoff);
        Elf64_Dyn *dyn = NULL; size_t dyn_count = 0;
        for (int i = 0; i < eh->e_phnum; i++) {
            if (ph[i].p_type == PT_DYNAMIC) {
                if (ph[i].p_offset + ph[i].p_filesz <= sz) {
                    dyn = (Elf64_Dyn*)(data + ph[i].p_offset);
                    dyn_count = ph[i].p_filesz / sizeof(Elf64_Dyn);
                    out->is_dynamic = true;
                }
                break;
            }
        }
        if (!dyn) { out->parsed = true; snprintf(out->parse_note, sizeof(out->parse_note), "static or no dynamic section"); free(data); return; }
        uint64_t strtab_vaddr = 0, strtab_off = 0, strsz = 0;
        for (size_t i = 0; i < dyn_count; i++) {
            if (dyn[i].d_tag == DT_STRTAB) strtab_vaddr = dyn[i].d_un.d_ptr;
            else if (dyn[i].d_tag == DT_STRSZ) strsz = dyn[i].d_un.d_val;
        }
        if (!strtab_vaddr || !vaddr_to_offset64(ph, eh->e_phnum, strtab_vaddr, &strtab_off) || strtab_off >= sz) {
            snprintf(out->parse_note, sizeof(out->parse_note), "could not locate dynamic string table"); free(data); return;
        }
        if (!strsz || strtab_off + strsz > sz) strsz = sz - strtab_off;
        char *strtab = (char*)(data + strtab_off);
        for (size_t i = 0; i < dyn_count; i++) {
            if (dyn[i].d_tag == DT_NEEDED) {
                uint64_t noff = dyn[i].d_un.d_val;
                if (noff < strsz) list_add(&out->needed, strtab + noff);
            }
        }
        out->parsed = true;
    } else if (data[EI_CLASS] == ELFCLASS32) {
        if (sz < sizeof(Elf32_Ehdr)) { free(data); return; }
        Elf32_Ehdr *eh = (Elf32_Ehdr*)data;
        snprintf(out->arch, sizeof(out->arch), "%s", machine_to_str(eh->e_machine));
        if ((uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf32_Phdr) > sz) {
            snprintf(out->parse_note, sizeof(out->parse_note), "program headers out of range"); free(data); return;
        }
        Elf32_Phdr *ph = (Elf32_Phdr*)(data + eh->e_phoff);
        Elf32_Dyn *dyn = NULL; size_t dyn_count = 0;
        for (int i = 0; i < eh->e_phnum; i++) {
            if (ph[i].p_type == PT_DYNAMIC) {
                if ((uint64_t)ph[i].p_offset + ph[i].p_filesz <= sz) {
                    dyn = (Elf32_Dyn*)(data + ph[i].p_offset);
                    dyn_count = ph[i].p_filesz / sizeof(Elf32_Dyn);
                    out->is_dynamic = true;
                }
                break;
            }
        }
        if (!dyn) { out->parsed = true; snprintf(out->parse_note, sizeof(out->parse_note), "static or no dynamic section"); free(data); return; }
        uint32_t strtab_vaddr = 0, strtab_off = 0, strsz = 0;
        for (size_t i = 0; i < dyn_count; i++) {
            if (dyn[i].d_tag == DT_STRTAB) strtab_vaddr = dyn[i].d_un.d_ptr;
            else if (dyn[i].d_tag == DT_STRSZ) strsz = dyn[i].d_un.d_val;
        }
        if (!strtab_vaddr || !vaddr_to_offset32(ph, eh->e_phnum, strtab_vaddr, &strtab_off) || strtab_off >= sz) {
            snprintf(out->parse_note, sizeof(out->parse_note), "could not locate dynamic string table"); free(data); return;
        }
        if (!strsz || (uint64_t)strtab_off + strsz > sz) strsz = (uint32_t)(sz - strtab_off);
        char *strtab = (char*)(data + strtab_off);
        for (size_t i = 0; i < dyn_count; i++) {
            if (dyn[i].d_tag == DT_NEEDED) {
                uint32_t noff = dyn[i].d_un.d_val;
                if (noff < strsz) list_add(&out->needed, strtab + noff);
            }
        }
        out->parsed = true;
    } else {
        snprintf(out->parse_note, sizeof(out->parse_note), "unknown ELF class");
    }
    free(data);
}

static bool is_system_lib(const char *name) {
    static const char *libs[] = {
        "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
        "ld-linux-x86-64.so.2", "ld-linux.so.2", "ld-linux-aarch64.so.1",
        "libresolv.so.2", "libnsl.so.1", "libutil.so.1", "libgcc_s.so.1",
        "libGL.so.1", "libEGL.so.1", "libGLESv2.so.2", "libvulkan.so.1", "libdrm.so.2",
        "libX11.so.6", "libxcb.so.1", "libXext.so.6", "libXrender.so.1", "libXrandr.so.2",
        "libXi.so.6", "libXcursor.so.1", "libXinerama.so.1", "libXfixes.so.3", "libwayland-client.so.0",
        NULL
    };
    for (int i = 0; libs[i]; i++) if (strcmp(name, libs[i]) == 0) return true;
    return false;
}

static void version_update(VersionMax *v, const char *token) {
    if (!token || !*token) return;
    const char *p = strchr(token, '_');
    if (!p) return;
    p++;
    int a = 0, b = 0, c = 0;
    sscanf(p, "%d.%d.%d", &a, &b, &c);
    if (!v->seen || a > v->major || (a == v->major && b > v->minor) || (a == v->major && b == v->minor && c > v->patch)) {
        v->seen = true; v->major = a; v->minor = b; v->patch = c;
        snprintf(v->text, sizeof(v->text), "%s", token);
    }
}

static void scan_versions_in_file(const char *path, VersionMax *glibc, VersionMax *glibcxx, VersionMax *cxxabi) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 256 * 1024 * 1024) { close(fd); return; }
    size_t n = (size_t)st.st_size;
    unsigned char *buf = malloc(n);
    if (!buf) { close(fd); return; }
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, buf + off, n - off);
        if (r <= 0) break;
        off += (size_t)r;
    }
    close(fd);
    if (off != n) { free(buf); return; }
    const char *prefixes[] = {"GLIBC_", "GLIBCXX_", "CXXABI_"};
    for (size_t i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++) {
            size_t plen = strlen(prefixes[k]);
            if (i + plen < n && memcmp(buf + i, prefixes[k], plen) == 0) {
                char tok[64]; size_t j = 0;
                while (i + j < n && j + 1 < sizeof(tok)) {
                    unsigned char ch = buf[i+j];
                    if (!(isalnum(ch) || ch == '_' || ch == '.')) break;
                    tok[j++] = (char)ch;
                }
                tok[j] = 0;
                if (j > plen) {
                    if (k == 0) version_update(glibc, tok);
                    else if (k == 1) version_update(glibcxx, tok);
                    else version_update(cxxabi, tok);
                }
            }
        }
    }
    free(buf);
}

static void walk_dir(WalkCtx *ctx, const char *dir) {
    if (ctx->walked > MAX_WALK_FILES) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char path[PATH_MAX]; join_path(path, sizeof(path), dir, ent->d_name);
        ctx->walked++;
        if (ctx->walked > MAX_WALK_FILES) break;
        struct stat lst;
        if (lstat(path, &lst) != 0) continue;
        list_add_allow_dupe(&ctx->all_files, path);
        if (S_ISLNK(lst.st_mode)) {
            struct stat target;
            if (stat(path, &target) != 0) list_add(&ctx->broken_links, path);
            continue;
        }
        if (S_ISDIR(lst.st_mode)) {
            walk_dir(ctx, path);
            continue;
        }
        if (!S_ISREG(lst.st_mode)) continue;
        const char *bn = base_name(path);
        if (ends_with(bn, ".desktop")) list_add(&ctx->desktop_files, path);
        if (strstr(bn, ".so") != NULL || ends_with(bn, ".so")) list_add(&ctx->provided_names, bn);
        if (file_is_elf(path)) {
            if (ctx->elf_files.len < MAX_ELF_SCAN) list_add(&ctx->elf_files, path);
            list_add(&ctx->provided_names, bn);
        }
    }
    closedir(d);
}

static void walk_init(WalkCtx *ctx, const char *root) {
    memset(ctx, 0, sizeof(*ctx));
    snprintf(ctx->root, sizeof(ctx->root), "%s", root);
    list_init(&ctx->all_files); list_init(&ctx->elf_files); list_init(&ctx->provided_names);
    list_init(&ctx->broken_links); list_init(&ctx->desktop_files);
    walk_dir(ctx, root);
}
static void walk_free(WalkCtx *ctx) {
    list_free(&ctx->all_files); list_free(&ctx->elf_files); list_free(&ctx->provided_names);
    list_free(&ctx->broken_links); list_free(&ctx->desktop_files);
}

static bool parse_desktop_file(const char *path, DesktopInfo *info) {
    memset(info, 0, sizeof(*info));
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[2048]; bool in_entry = false;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        if (*s == '[') {
            in_entry = strcmp(s, "[Desktop Entry]") == 0;
            continue;
        }
        if (!in_entry) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        if (strcmp(key, "Name") == 0) snprintf(info->name, sizeof(info->name), "%s", val);
        else if (strcmp(key, "Exec") == 0) snprintf(info->exec, sizeof(info->exec), "%s", val);
        else if (strcmp(key, "Icon") == 0) snprintf(info->icon, sizeof(info->icon), "%s", val);
        else if (strcmp(key, "Type") == 0) snprintf(info->type, sizeof(info->type), "%s", val);
        else if (strcmp(key, "Categories") == 0) snprintf(info->categories, sizeof(info->categories), "%s", val);
    }
    fclose(f);
    return true;
}

static bool find_file_by_name_or_icon(const char *root, const char *name, char *found, size_t foundsz) {
    if (!name || !*name) return false;
    char candidate[PATH_MAX];
    if (name[0] == '/') {
        if (path_exists(name)) { snprintf(found, foundsz, "%s", name); return true; }
        return false;
    }
    const char *exts[] = {"", ".png", ".svg", ".xpm", ".ico", NULL};
    const char *dirs[] = {"", "usr/share/pixmaps", "usr/share/icons", "usr/share/icons/hicolor", "usr/share/applications", "usr/bin", "usr/local/bin", NULL};
    for (int d = 0; dirs[d]; d++) {
        for (int e = 0; exts[e]; e++) {
            char nbuf[512]; snprintf(nbuf, sizeof(nbuf), "%s%s", name, exts[e]);
            if (*dirs[d]) { char mid[PATH_MAX]; join_path(mid, sizeof(mid), root, dirs[d]); join_path(candidate, sizeof(candidate), mid, nbuf); }
            else join_path(candidate, sizeof(candidate), root, nbuf);
            if (path_exists(candidate)) { snprintf(found, foundsz, "%s", candidate); return true; }
        }
    }
    DIR *dir = opendir(root);
    if (!dir) return false;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char p[PATH_MAX]; join_path(p, sizeof(p), root, ent->d_name);
        struct stat st; if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (find_file_by_name_or_icon(p, name, found, foundsz)) { closedir(dir); return true; }
        } else if (S_ISREG(st.st_mode)) {
            const char *bn = base_name(p);
            for (int e = 0; exts[e]; e++) {
                char nbuf[512]; snprintf(nbuf, sizeof(nbuf), "%s%s", name, exts[e]);
                if (strcmp(bn, nbuf) == 0) { snprintf(found, foundsz, "%s", p); closedir(dir); return true; }
            }
        }
    }
    closedir(dir);
    return false;
}


static bool find_icon_file(const char *root, const char *name, char *found, size_t foundsz) {
    if (!name || !*name) return false;
    if (name[0] == '/') {
        if (path_exists(name) && (ends_with(name, ".png") || ends_with(name, ".svg") || ends_with(name, ".xpm") || ends_with(name, ".ico"))) {
            snprintf(found, foundsz, "%s", name);
            return true;
        }
        return false;
    }
    const char *exts[] = {".png", ".svg", ".xpm", ".ico", NULL};
    const char *dirs[] = {"", "usr/share/pixmaps", "usr/share/icons", "usr/share/icons/hicolor", "usr/share/icons/hicolor/256x256/apps", NULL};
    char candidate[PATH_MAX];
    for (int d = 0; dirs[d]; d++) {
        for (int e = 0; exts[e]; e++) {
            char nbuf[512];
            if (ends_with(name, exts[e])) snprintf(nbuf, sizeof(nbuf), "%s", name);
            else snprintf(nbuf, sizeof(nbuf), "%s%s", name, exts[e]);
            if (*dirs[d]) { char mid[PATH_MAX]; join_path(mid, sizeof(mid), root, dirs[d]); join_path(candidate, sizeof(candidate), mid, nbuf); }
            else join_path(candidate, sizeof(candidate), root, nbuf);
            if (path_exists(candidate)) { snprintf(found, foundsz, "%s", candidate); return true; }
        }
    }
    DIR *dir = opendir(root);
    if (!dir) return false;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char pth[PATH_MAX]; join_path(pth, sizeof(pth), root, ent->d_name);
        struct stat st; if (stat(pth, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (find_icon_file(pth, name, found, foundsz)) { closedir(dir); return true; }
        } else if (S_ISREG(st.st_mode)) {
            const char *bn = base_name(pth);
            for (int e = 0; exts[e]; e++) {
                char nbuf[512];
                if (ends_with(name, exts[e])) snprintf(nbuf, sizeof(nbuf), "%s", name);
                else snprintf(nbuf, sizeof(nbuf), "%s%s", name, exts[e]);
                if (strcmp(bn, nbuf) == 0) { snprintf(found, foundsz, "%s", pth); closedir(dir); return true; }
            }
        }
    }
    closedir(dir);
    return false;
}

static bool resolve_exec_in_appdir(const char *root, const char *exec_token, char *found, size_t foundsz) {
    if (!exec_token || !*exec_token) return false;
    char clean[1024]; snprintf(clean, sizeof(clean), "%s", exec_token); strip_quotes(clean);
    if (clean[0] == '/') {
        char inside[PATH_MAX]; join_path(inside, sizeof(inside), root, clean + 1);
        if (path_exists(inside)) { snprintf(found, foundsz, "%s", inside); return true; }
        if (path_exists(clean)) { snprintf(found, foundsz, "%s", clean); return true; }
        return false;
    }
    char candidate[PATH_MAX];
    join_path(candidate, sizeof(candidate), root, clean);
    if (path_exists(candidate)) { snprintf(found, foundsz, "%s", candidate); return true; }
    char binroot[PATH_MAX]; join_path(binroot, sizeof(binroot), root, "usr/bin");
    join_path(candidate, sizeof(candidate), binroot, clean);
    if (path_exists(candidate)) { snprintf(found, foundsz, "%s", candidate); return true; }
    char localbin[PATH_MAX]; join_path(localbin, sizeof(localbin), root, "usr/local/bin");
    join_path(candidate, sizeof(candidate), localbin, clean);
    if (path_exists(candidate)) { snprintf(found, foundsz, "%s", candidate); return true; }
    return find_file_by_name_or_icon(root, clean, found, foundsz);
}

static void check_fuse(void) {
    report_line("\nFUSE check");
    if (path_exists("/dev/fuse")) {
        int fd = open("/dev/fuse", O_RDWR);
        if (fd >= 0) { status_line("OK", "/dev/fuse exists and can be opened by this user."); close(fd); }
        else status_line("WARN", "/dev/fuse exists, but this user could not open it: %s", strerror(errno));
    } else {
        status_line("WARN", "/dev/fuse was not found. Some AppImages may need FUSE or extraction fallback.");
    }
}

static void check_basic_file(const char *path) {
    report_line("\nFile check");
    struct stat st;
    if (stat(path, &st) != 0) { status_line("FAIL", "Cannot stat path: %s", strerror(errno)); return; }
    if (S_ISREG(st.st_mode)) status_line("OK", "Path is a regular file.");
    else status_line("FAIL", "Path is not a regular file.");
    if (st.st_mode & S_IXUSR) status_line("OK", "Executable bit is set for owner.");
    else status_line("WARN", "Executable bit is not set. Run: chmod +x '%s'", path);
    unsigned char b[64]; size_t got = 0;
    if (read_file_prefix(path, b, sizeof(b), &got) && got >= 4 && memcmp(b, ELFMAG, SELFMAG) == 0) {
        const char *cls = got > EI_CLASS && b[EI_CLASS] == ELFCLASS64 ? "ELF 64-bit" : (b[EI_CLASS] == ELFCLASS32 ? "ELF 32-bit" : "ELF unknown-class");
        uint16_t mach = 0;
        if (got >= 20) memcpy(&mach, b + 18, sizeof(mach));
        status_line("OK", "File looks like an %s executable/container, architecture: %s.", cls, machine_to_str(mach));
    } else {
        status_line("WARN", "File does not look like an ELF executable. It may not be an AppImage.");
    }
    if (contains_case_insensitive(base_name(path), ".appimage")) status_line("OK", "Filename looks like an AppImage.");
    else status_line("WARN", "Filename does not end in .AppImage; that is not fatal, but it may confuse users.");
}

static void diagnose_appimage_file(const char *path) {
    report_line("AppImage Nurse %s", VERSION);
    report_line("Target: %s", path);
    check_basic_file(path);
    check_fuse();
    report_line("\nAppImage-specific notes");
    status_line("OK", "Use the Extract tab/command for deeper AppDir checks: appimage-nurse extract <AppImage> <output-dir>");
    status_line("WARN", "Outer-file scans are rough. Extracting and diagnosing the AppDir is more reliable.");
    VersionMax glibc = {0}, glibcxx = {0}, cxxabi = {0};
    scan_versions_in_file(path, &glibc, &glibcxx, &cxxabi);
    report_line("\nVersion-string scan of container file");
    if (glibc.seen) status_line("WARN", "Found possible highest GLIBC requirement string: %s. For AppImage files this is only a rough scan.", glibc.text);
    else status_line("OK", "No GLIBC_* strings found in the outer file scan.");
    if (glibcxx.seen) status_line("WARN", "Found possible highest GLIBCXX requirement string: %s.", glibcxx.text);
    if (cxxabi.seen) status_line("WARN", "Found possible highest CXXABI requirement string: %s.", cxxabi.text);
    report_line("\nBest next step");
    report_line("- Extract this AppImage, then diagnose the extracted AppDir.");
    report_line("- In the tab UI: Extract -> Diagnose extracted folder.");
}

static void diagnose_dependencies(WalkCtx *ctx, VersionMax *glibc, VersionMax *glibcxx, VersionMax *cxxabi) {
    report_line("\nELF / dependency scan");
    if (ctx->elf_files.len == 0) {
        status_line("WARN", "No ELF files found in this AppDir. If this is a script app, dependency scanning is limited.");
        return;
    }
    status_line("OK", "Found %zu ELF file(s) to inspect.", ctx->elf_files.len);
    StrList missing; list_init(&missing);
    StrList systemish; list_init(&systemish);
    StrList bundled; list_init(&bundled);
    size_t dep_lines = 0;
    for (size_t i = 0; i < ctx->elf_files.len && i < MAX_ELF_SCAN; i++) {
        const char *p = ctx->elf_files.items[i];
        scan_versions_in_file(p, glibc, glibcxx, cxxabi);
        ElfInfo ei; parse_elf_needed(p, &ei);
        if (!ei.parsed) {
            status_line("WARN", "Could not fully parse ELF: %s (%s)", p, ei.parse_note[0] ? ei.parse_note : "unknown reason");
            list_free(&ei.needed); continue;
        }
        if (ei.needed.len == 0) { list_free(&ei.needed); continue; }
        if (dep_lines < 25) report_line("- %s [%s]", p, ei.arch[0] ? ei.arch : "unknown arch");
        for (size_t j = 0; j < ei.needed.len; j++) {
            const char *need = ei.needed.items[j];
            if (list_contains(&ctx->provided_names, need)) {
                list_add(&bundled, need);
                if (dep_lines < 25) report_line("    bundled: %s", need);
            } else if (is_system_lib(need)) {
                list_add(&systemish, need);
                if (dep_lines < 25) report_line("    host/system: %s", need);
            } else {
                list_add(&missing, need);
                if (dep_lines < 25) report_line("    not bundled: %s", need);
            }
            dep_lines++;
        }
        list_free(&ei.needed);
    }
    if (dep_lines >= 25) report_line("  ... dependency listing truncated in screen output, summary below is still complete.");
    if (missing.len == 0) status_line("OK", "No obvious non-system DT_NEEDED libraries were missing from the AppDir.");
    else {
        status_line("WARN", "%zu library name(s) were referenced but not found bundled in the AppDir:", missing.len);
        for (size_t i = 0; i < missing.len; i++) report_line("  - %s", missing.items[i]);
    }
    if (systemish.len) {
        report_line("\nHost/system libraries detected. These are often intentionally not bundled:");
        for (size_t i = 0; i < systemish.len && i < 40; i++) report_line("  - %s", systemish.items[i]);
    }
    list_free(&missing); list_free(&systemish); list_free(&bundled);
}


static bool ctx_any_path_contains(WalkCtx *ctx, const char *needle) {
    for (size_t i = 0; i < ctx->all_files.len; i++) if (contains_case_insensitive(ctx->all_files.items[i], needle)) return true;
    return false;
}
static bool ctx_any_base_contains(WalkCtx *ctx, const char *needle) {
    for (size_t i = 0; i < ctx->all_files.len; i++) if (contains_case_insensitive(base_name(ctx->all_files.items[i]), needle)) return true;
    return false;
}
static bool ctx_any_base_ends(WalkCtx *ctx, const char *suffix) {
    for (size_t i = 0; i < ctx->all_files.len; i++) if (ends_with(base_name(ctx->all_files.items[i]), suffix)) return true;
    return false;
}

static void diagnose_frameworks(WalkCtx *ctx) {
    report_line("\nFramework-specific checks");
    bool detected = false;

    bool qt = ctx_any_base_contains(ctx, "libQt5") || ctx_any_base_contains(ctx, "libQt6") || ctx_any_path_contains(ctx, "PySide") || ctx_any_path_contains(ctx, "PyQt");
    if (qt) {
        detected = true;
        status_line("OK", "Qt/PySide/PyQt indicators found.");
        if (ctx_any_path_contains(ctx, "platforms/libqxcb.so") || ctx_any_path_contains(ctx, "plugins/platforms/libqxcb.so"))
            status_line("OK", "Qt xcb platform plugin appears to be bundled.");
        else
            status_line("WARN", "Qt app detected, but platforms/libqxcb.so was not found. Many Qt apps fail without a platform plugin.");
        if (ctx_any_path_contains(ctx, "imageformats/")) status_line("OK", "Qt image plugin folder appears to exist.");
        else status_line("WARN", "Qt imageformats plugin folder was not found. Icons/images may fail for some Qt apps.");
    }

    bool gtk = ctx_any_base_contains(ctx, "libgtk-3") || ctx_any_base_contains(ctx, "libgtk-4") || ctx_any_path_contains(ctx, "glib-2.0/schemas");
    if (gtk) {
        detected = true;
        status_line("OK", "GTK indicators found.");
        if (ctx_any_path_contains(ctx, "glib-2.0/schemas")) status_line("OK", "GSettings schema folder appears to exist.");
        else status_line("WARN", "GTK app detected, but glib-2.0/schemas was not found. Settings-dependent apps may fail.");
        if (ctx_any_path_contains(ctx, "share/icons") || ctx_any_path_contains(ctx, "share/themes")) status_line("OK", "GTK icon/theme resources appear to exist.");
        else status_line("WARN", "GTK icon/theme resources were not obvious. UI icons may be missing.");
    }

    bool electron = ctx_any_path_contains(ctx, "resources/app.asar") || ctx_any_base_contains(ctx, "chrome-sandbox") || ctx_any_base_contains(ctx, "electron");
    if (electron) {
        detected = true;
        status_line("OK", "Electron-style app indicators found.");
        if (ctx_any_path_contains(ctx, "resources/app.asar")) status_line("OK", "resources/app.asar appears to exist.");
        else status_line("WARN", "Electron indicator found, but resources/app.asar was not obvious.");
        if (ctx_any_base_contains(ctx, "chrome-sandbox")) status_line("WARN", "chrome-sandbox exists. If launch fails, sandbox permissions may need review or --no-sandbox may be used by the app.");
        else status_line("WARN", "Electron apps sometimes fail from sandbox setup; no chrome-sandbox file was obvious.");
    }

    bool sdl = ctx_any_base_contains(ctx, "libSDL2") || ctx_any_base_contains(ctx, "libSDL3") || ctx_any_base_contains(ctx, "libopenal") || ctx_any_base_contains(ctx, "liballegro");
    if (sdl) {
        detected = true;
        status_line("OK", "Game/audio framework indicators found: SDL/OpenAL/Allegro-like libraries.");
        if (ctx_any_path_contains(ctx, "share") || ctx_any_path_contains(ctx, "assets") || ctx_any_path_contains(ctx, "data")) status_line("OK", "Asset/data folders appear to exist.");
        else status_line("WARN", "Game framework detected, but no obvious assets/data folder was found.");
    }

    bool godot = ctx_any_base_ends(ctx, ".pck") || ctx_any_base_contains(ctx, "godot") || ctx_any_base_contains(ctx, "redot");
    if (godot) {
        detected = true;
        status_line("OK", "Godot/Redot-style indicators found.");
        if (ctx_any_base_ends(ctx, ".pck")) status_line("OK", "A .pck data file appears to exist.");
        else status_line("WARN", "Godot/Redot indicator found, but no .pck file was obvious. Exported projects often need the .pck beside the executable.");
    }

    if (!detected) status_line("OK", "No framework-specific problems detected by the current heuristics.");
}

static void diagnose_appdir(const char *root) {
    report_line("AppImage Nurse %s", VERSION);
    report_line("Target AppDir: %s", root);
    report_line("\nAppDir structure check");
    if (!is_dir_path(root)) { status_line("FAIL", "Target is not a directory."); return; }
    status_line("OK", "Target is a directory.");
    char apprun[PATH_MAX]; join_path(apprun, sizeof(apprun), root, "AppRun");
    if (path_exists(apprun)) {
        if (is_exec_path(apprun)) status_line("OK", "AppRun exists and is executable.");
        else status_line("WARN", "AppRun exists but is not executable. Run: chmod +x '%s'", apprun);
    } else status_line("FAIL", "AppRun is missing at AppDir root.");

    WalkCtx ctx; walk_init(&ctx, root);
    status_line("OK", "Scanned %zu filesystem entries.", ctx.walked);
    if (ctx.walked > MAX_WALK_FILES) status_line("WARN", "Scan stopped early after %d entries.", MAX_WALK_FILES);
    if (ctx.broken_links.len == 0) status_line("OK", "No broken symlinks found.");
    else {
        status_line("WARN", "Found %zu broken symlink(s):", ctx.broken_links.len);
        for (size_t i = 0; i < ctx.broken_links.len && i < 25; i++) report_line("  - %s", ctx.broken_links.items[i]);
    }

    report_line("\nDesktop file check");
    if (ctx.desktop_files.len == 0) {
        status_line("FAIL", "No .desktop file found in the AppDir.");
    } else {
        status_line("OK", "Found %zu .desktop file(s).", ctx.desktop_files.len);
        DesktopInfo di;
        if (parse_desktop_file(ctx.desktop_files.items[0], &di)) {
            report_line("- Using: %s", ctx.desktop_files.items[0]);
            if (di.name[0]) status_line("OK", "Desktop Name: %s", di.name);
            else status_line("WARN", "Desktop file has no Name= field.");
            if (strcmp(di.type, "Application") == 0) status_line("OK", "Desktop Type=Application.");
            else status_line("WARN", "Desktop Type is '%s'; usually should be Application.", di.type[0] ? di.type : "missing");
            if (di.exec[0]) {
                char tok[1024], found[PATH_MAX]; first_exec_token(di.exec, tok, sizeof(tok));
                if (tok[0] && resolve_exec_in_appdir(root, tok, found, sizeof(found))) {
                    if (is_exec_path(found)) status_line("OK", "Exec target resolves and is executable: %s", found);
                    else status_line("WARN", "Exec target resolves but may not be executable: %s", found);
                } else status_line("WARN", "Could not resolve Exec= target inside AppDir: %s", di.exec);
            } else status_line("FAIL", "Desktop file has no Exec= field.");
            if (di.icon[0]) {
                char found[PATH_MAX];
                if (find_icon_file(root, di.icon, found, sizeof(found))) status_line("OK", "Icon seems to exist: %s", found);
                else status_line("WARN", "Could not find icon referenced by Icon=%s", di.icon);
            } else status_line("WARN", "Desktop file has no Icon= field.");
        } else status_line("WARN", "Could not parse first .desktop file.");
    }

    VersionMax glibc = {0}, glibcxx = {0}, cxxabi = {0};
    diagnose_dependencies(&ctx, &glibc, &glibcxx, &cxxabi);
    diagnose_frameworks(&ctx);
    report_line("\nCompatibility hints");
    if (glibc.seen) status_line("WARN", "Highest GLIBC_* string found: %s. If this is newer than your target distro, rebuild on an older base.", glibc.text);
    else status_line("OK", "No GLIBC_* version strings found in scanned ELF files.");
    if (glibcxx.seen) status_line("WARN", "Highest GLIBCXX_* string found: %s. This may require a newer libstdc++ or bundling a compatible one.", glibcxx.text);
    if (cxxabi.seen) status_line("WARN", "Highest CXXABI_* string found: %s.", cxxabi.text);
    report_line("\nHuman explanation");
    report_line("- FAIL means the AppDir/AppImage is probably broken.");
    report_line("- WARN means it may work on your machine but fail elsewhere.");
    report_line("- GLIBC warnings are not fixable by simple repacking; rebuild in an older compatibility environment.");
    walk_free(&ctx);
}


static int appimage_repack(const char *appdir, const char *outfile);
static int create_appdir_skeleton(const char *appdir, const char *appname, const char *execpath, const char *iconpath, const char *assetspath);
static int write_placeholder_icon_xpm(const char *path);
static void normalize_appimage_name(const char *name, char *out, size_t outsz);
static void normalize_appdir_name(const char *name, char *out, size_t outsz);
static int prompt_output_appimage_path(char *out, size_t outsz);
static int prompt_output_appdir_path(char *out, size_t outsz);
static int prompt_output_text_path(char *out, size_t outsz);

static int prompt_line(const char *label, char *out, size_t outsz) {
    printf("%s: ", label);
    fflush(stdout);
    if (!fgets(out, outsz, stdin)) return 1;
    char *t = trim(out);
    memmove(out, t, strlen(t) + 1);
    return out[0] ? 0 : 1;
}

static int mkdir_p(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len-1] == '/') tmp[len-1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

static void parent_dir(const char *path, char *out, size_t outsz) {
    snprintf(out, outsz, "%s", path);
    char *slash = strrchr(out, '/');
    if (!slash) snprintf(out, outsz, ".");
    else if (slash == out) out[1] = 0;
    else *slash = 0;
}

static int copy_file_simple(const char *src, const char *dst, mode_t mode) {
    char pd[PATH_MAX]; parent_dir(dst, pd, sizeof(pd));
    if (mkdir_p(pd, 0755) != 0) return -1;
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode : 0644);
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    for (;;) {
        ssize_t r = read(in, buf, sizeof(buf));
        if (r == 0) break;
        if (r < 0) { close(in); close(out); return -1; }
        char *p = buf;
        ssize_t left = r;
        while (left > 0) {
            ssize_t w = write(out, p, left);
            if (w <= 0) { close(in); close(out); return -1; }
            p += w; left -= w;
        }
    }
    close(in); close(out);
    chmod(dst, mode ? mode : 0644);
    return 0;
}

static int copy_tree_simple(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        if (mkdir_p(dst, st.st_mode & 0777 ? st.st_mode & 0777 : 0755) != 0) return -1;
        DIR *d = opendir(src);
        if (!d) return -1;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            char s2[PATH_MAX], d2[PATH_MAX];
            join_path(s2, sizeof(s2), src, ent->d_name);
            join_path(d2, sizeof(d2), dst, ent->d_name);
            if (copy_tree_simple(s2, d2) != 0) { closedir(d); return -1; }
        }
        closedir(d);
        return 0;
    }
    if (S_ISLNK(st.st_mode)) {
        char linkbuf[PATH_MAX];
        ssize_t n = readlink(src, linkbuf, sizeof(linkbuf)-1);
        if (n < 0) return -1;
        linkbuf[n] = 0;
        char pd[PATH_MAX]; parent_dir(dst, pd, sizeof(pd));
        if (mkdir_p(pd, 0755) != 0) return -1;
        unlink(dst);
        return symlink(linkbuf, dst);
    }
    if (S_ISREG(st.st_mode)) return copy_file_simple(src, dst, st.st_mode & 0777);
    return 0;
}

static void nurse_config_dir(char *out, size_t outsz) {
    const char *home = getenv("HOME");
    if (!home || !*home) snprintf(out, outsz, ".appimage-nurse");
    else snprintf(out, outsz, "%s/.config/appimage-nurse", home);
}

static void nurse_tools_conf(char *out, size_t outsz) {
    char dir[PATH_MAX]; nurse_config_dir(dir, sizeof(dir));
    join_path(out, outsz, dir, "tools.conf");
}


static void nurse_settings_conf(char *out, size_t outsz) {
    char dir[PATH_MAX]; nurse_config_dir(dir, sizeof(dir));
    join_path(out, outsz, dir, "config.conf");
}

static void nurse_policy_conf(const char *which, char *out, size_t outsz) {
    char dir[PATH_MAX]; nurse_config_dir(dir, sizeof(dir));
    if (which && strcmp(which, "allow") == 0) join_path(out, outsz, dir, "bundle-allow.conf");
    else join_path(out, outsz, dir, "bundle-skip.conf");
}

static int ensure_config_dir(void) {
    char dir[PATH_MAX]; nurse_config_dir(dir, sizeof(dir));
    return mkdir_p(dir, 0755);
}

static bool config_get_value(const char *key, char *out, size_t outsz) {
    char conf[PATH_MAX]; nurse_settings_conf(conf, sizeof(conf));
    FILE *f = fopen(conf, "r");
    if (!f) return false;
    char line[4096]; bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char *t = trim(line);
        if (!*t || *t == '#') continue;
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        if (strcmp(trim(t), key) == 0) {
            snprintf(out, outsz, "%s", trim(eq + 1));
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static int config_set_value(const char *key, const char *value) {
    if (!key || !*key || !value) return 1;
    if (ensure_config_dir() != 0) { printf("Could not create config folder: %s\n", strerror(errno)); return 1; }
    char conf[PATH_MAX]; nurse_settings_conf(conf, sizeof(conf));
    StrList lines; list_init(&lines);
    FILE *f = fopen(conf, "r");
    if (f) {
        char line[4096];
        while (fgets(line, sizeof(line), f)) {
            char copy[4096]; snprintf(copy, sizeof(copy), "%s", line);
            char *t = trim(copy); char *eq = strchr(t, '='); bool skip = false;
            if (eq) { *eq = 0; if (strcmp(trim(t), key) == 0) skip = true; }
            if (!skip) list_add_allow_dupe(&lines, trim(line));
        }
        fclose(f);
    }
    f = fopen(conf, "w");
    if (!f) { list_free(&lines); printf("Could not write config: %s\n", strerror(errno)); return 1; }
    fprintf(f, "# AppImage Nurse settings. Only practical preferences are saved here.\n");
    for (size_t i = 0; i < lines.len; i++) if (lines.items[i][0]) fprintf(f, "%s\n", lines.items[i]);
    fprintf(f, "%s=%s\n", key, value);
    fclose(f);
    list_free(&lines);
    printf("Saved config: %s=%s\n", key, value);
    return 0;
}

static void config_show(void) {
    char conf[PATH_MAX]; nurse_settings_conf(conf, sizeof(conf));
    printf("Config file: %s\n", conf);
    printf("Only settings are persistent. Session history is temporary unless you save it yourself.\n\n");
    FILE *f = fopen(conf, "r");
    if (!f) { printf("No config file yet.\n"); return; }
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *t = trim(line);
        if (*t && *t != '#') printf("%s\n", t);
    }
    fclose(f);
}

static bool policy_file_contains(const char *which, const char *lib) {
    if (!lib || !*lib) return false;
    char conf[PATH_MAX]; nurse_policy_conf(which, conf, sizeof(conf));
    FILE *f = fopen(conf, "r");
    if (!f) return false;
    char line[1024]; bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char *t = trim(line);
        if (!*t || *t == '#') continue;
        if (strcmp(t, lib) == 0) { found = true; break; }
    }
    fclose(f);
    return found;
}

static int policy_add_library(const char *which, const char *lib) {
    if (!which || !lib || !*lib) return 1;
    if (ensure_config_dir() != 0) { printf("Could not create config folder: %s\n", strerror(errno)); return 1; }
    if (policy_file_contains(which, lib)) { printf("Already listed in %s policy: %s\n", which, lib); return 0; }
    char conf[PATH_MAX]; nurse_policy_conf(which, conf, sizeof(conf));
    FILE *f = fopen(conf, "a");
    if (!f) { printf("Could not write policy file: %s\n", strerror(errno)); return 1; }
    fprintf(f, "%s\n", lib);
    fclose(f);
    printf("Added to %s policy: %s\n", which, lib);
    return 0;
}

static void policy_show(void) {
    const char *names[] = {"allow", "skip", NULL};
    for (int i = 0; names[i]; i++) {
        char conf[PATH_MAX]; nurse_policy_conf(names[i], conf, sizeof(conf));
        printf("%s policy: %s\n", names[i], conf);
        FILE *f = fopen(conf, "r");
        if (!f) { printf("  (empty)\n"); continue; }
        char line[1024]; int count = 0;
        while (fgets(line, sizeof(line), f)) { char *t = trim(line); if (*t && *t != '#') { printf("  %s\n", t); count++; } }
        fclose(f);
        if (!count) printf("  (empty)\n");
    }
}


static bool tool_name_match(const char *requested, const char *configured) {
    if (!requested || !configured) return false;
    if (strcmp(requested, configured) == 0) return true;
    if (strcmp(requested, "linuxdeploy-x86_64.AppImage") == 0 && strcmp(configured, "linuxdeploy") == 0) return true;
    if (strcmp(requested, "linuxdeploy") == 0 && strcmp(configured, "linuxdeploy-x86_64.AppImage") == 0) return true;
    if (strcmp(requested, "appimagetool-x86_64.AppImage") == 0 && strcmp(configured, "appimagetool") == 0) return true;
    if (strcmp(requested, "appimagetool") == 0 && strcmp(configured, "appimagetool-x86_64.AppImage") == 0) return true;
    return false;
}

static bool find_tool_override(const char *exe, char *out, size_t outsz) {
    char conf[PATH_MAX]; nurse_tools_conf(conf, sizeof(conf));
    FILE *f = fopen(conf, "r");
    if (!f) return false;
    char line[PATH_MAX * 2];
    bool ok = false;
    while (fgets(line, sizeof(line), f)) {
        char *t = trim(line);
        if (!*t || *t == '#') continue;
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        char *name = trim(t);
        char *path = trim(eq + 1);
        if (tool_name_match(exe, name) && is_exec_path(path)) {
            snprintf(out, outsz, "%s", path);
            ok = true;
            break;
        }
    }
    fclose(f);
    return ok;
}

static bool find_in_path_raw(const char *exe, char *out, size_t outsz) {
    const char *path = getenv("PATH");
    if (!path) return false;
    char *dup = strdup(path);
    if (!dup) return false;
    char *save = NULL;
    for (char *tok = strtok_r(dup, ":", &save); tok; tok = strtok_r(NULL, ":", &save)) {
        char cand[PATH_MAX]; join_path(cand, sizeof(cand), tok[0] ? tok : ".", exe);
        if (is_exec_path(cand)) { snprintf(out, outsz, "%s", cand); free(dup); return true; }
    }
    free(dup);
    return false;
}

static bool find_in_path(const char *exe, char *out, size_t outsz) {
    if (find_tool_override(exe, out, outsz)) return true;
    return find_in_path_raw(exe, out, outsz);
}

static int save_tool_override(const char *name, const char *path) {
    if (!name || !*name || !path || !*path) return 1;
    if (!is_exec_path(path)) {
        printf("Tool path is not executable: %s\n", path);
        return 1;
    }
    char dir[PATH_MAX], conf[PATH_MAX];
    nurse_config_dir(dir, sizeof(dir));
    nurse_tools_conf(conf, sizeof(conf));
    if (mkdir_p(dir, 0755) != 0) { printf("Could not create config folder: %s\n", strerror(errno)); return 1; }

    StrList lines; list_init(&lines);
    FILE *f = fopen(conf, "r");
    if (f) {
        char line[PATH_MAX * 2];
        while (fgets(line, sizeof(line), f)) {
            char copy[PATH_MAX * 2]; snprintf(copy, sizeof(copy), "%s", line);
            char *t = trim(copy);
            char *eq = strchr(t, '=');
            bool skip = false;
            if (eq) { *eq = 0; if (tool_name_match(name, trim(t))) skip = true; }
            if (!skip) list_add_allow_dupe(&lines, trim(line));
        }
        fclose(f);
    }
    f = fopen(conf, "w");
    if (!f) { list_free(&lines); printf("Could not write config: %s\n", strerror(errno)); return 1; }
    fprintf(f, "# AppImage Nurse tool paths. These override PATH lookup when executable.\n");
    for (size_t i = 0; i < lines.len; i++) if (lines.items[i][0]) fprintf(f, "%s\n", lines.items[i]);
    fprintf(f, "%s=%s\n", name, path);
    fclose(f);
    list_free(&lines);
    printf("Saved tool path: %s -> %s\n", name, path);
    return 0;
}

static void print_tool_overrides(void) {
    char conf[PATH_MAX]; nurse_tools_conf(conf, sizeof(conf));
    printf("Tool path config: %s\n", conf);
    FILE *f = fopen(conf, "r");
    if (!f) { printf("No saved tool paths yet. PATH lookup will be used.\n"); return; }
    char line[PATH_MAX * 2];
    while (fgets(line, sizeof(line), f)) {
        char *t = trim(line);
        if (*t && *t != '#') printf("%s\n", t);
    }
    fclose(f);
}


static int run_child_wait(const char *cwd, const char *logfile, char *const argv[]);
static int generate_text_file(const char *path, const char *text, mode_t mode);
static int patch_rpath_appdir(const char *appdir);
static void shell_quote_single(const char *in, char *out, size_t outsz);

static bool find_file_recursive_limit(const char *dir, const char *name, int depth, int *visited, char *out, size_t outsz) {
    if (depth < 0 || *visited > 20000) return false;
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char p[PATH_MAX]; join_path(p, sizeof(p), dir, ent->d_name);
        (*visited)++;
        struct stat st; if (lstat(p, &st) != 0) continue;
        if (S_ISREG(st.st_mode) && strcmp(ent->d_name, name) == 0) { snprintf(out, outsz, "%s", p); closedir(d); return true; }
        if (S_ISDIR(st.st_mode) && depth > 0) {
            if (find_file_recursive_limit(p, name, depth - 1, visited, out, outsz)) { closedir(d); return true; }
        }
    }
    closedir(d);
    return false;
}

static bool locate_system_library(const char *name, char *out, size_t outsz) {
    const char *dirs[] = {"/lib", "/usr/lib", "/lib64", "/usr/lib64", "/usr/local/lib", NULL};
    for (int i = 0; dirs[i]; i++) {
        int visited = 0;
        if (find_file_recursive_limit(dirs[i], name, 4, &visited, out, outsz)) return true;
    }
    return false;
}

static void collect_missing_deps(WalkCtx *ctx, StrList *missing) {
    for (size_t i = 0; i < ctx->elf_files.len && i < MAX_ELF_SCAN; i++) {
        ElfInfo ei; parse_elf_needed(ctx->elf_files.items[i], &ei);
        if (!ei.parsed) { list_free(&ei.needed); continue; }
        for (size_t j = 0; j < ei.needed.len; j++) {
            const char *need = ei.needed.items[j];
            if (!list_contains(&ctx->provided_names, need) && !policy_file_contains("skip", need) && (!is_system_lib(need) || policy_file_contains("allow", need))) list_add(missing, need);
        }
        list_free(&ei.needed);
    }
}

static int ensure_basic_apprun(const char *appdir) {
    char apprun[PATH_MAX]; join_path(apprun, sizeof(apprun), appdir, "AppRun");
    if (path_exists(apprun)) return 0;
    WalkCtx ctx; walk_init(&ctx, appdir);
    int rc = 1;
    if (ctx.desktop_files.len > 0) {
        DesktopInfo di;
        if (parse_desktop_file(ctx.desktop_files.items[0], &di) && di.exec[0]) {
            char tok[1024]; first_exec_token(di.exec, tok, sizeof(tok));
            if (tok[0]) {
                char script[4096];
                snprintf(script, sizeof(script),
                    "#!/bin/sh\nHERE=\"$(dirname \"$(readlink -f \"$0\")\")\"\nexport PATH=\"$HERE/usr/bin:$PATH\"\nexport LD_LIBRARY_PATH=\"$HERE/usr/lib:$HERE/usr/lib64:$HERE/lib:$HERE/lib64:${LD_LIBRARY_PATH}\"\nexec \"$HERE/usr/bin/%s\" \"$@\"\n", tok);
                rc = generate_text_file(apprun, script, 0755);
            }
        }
    }
    walk_free(&ctx);
    return rc;
}



static bool locate_system_library_ldconfig(const char *name, char *out, size_t outsz) {
    if (!name || !*name) return false;
    FILE *fp = popen("ldconfig -p 2>/dev/null", "r");
    if (!fp) return false;
    char line[4096];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        char *t = trim(line);
        size_t n = strlen(name);
        if (strncmp(t, name, n) != 0) continue;
        char *arrow = strstr(t, "=>");
        if (!arrow) continue;
        arrow += 2;
        arrow = trim(arrow);
        if (*arrow && path_exists(arrow)) {
            snprintf(out, outsz, "%s", arrow);
            found = true;
            break;
        }
    }
    pclose(fp);
    return found;
}

static bool locate_library_smart(const char *name, char *out, size_t outsz) {
    if (locate_system_library_ldconfig(name, out, outsz)) return true;
    return locate_system_library(name, out, outsz);
}

static bool should_never_bundle(const char *name) {
    if (!name) return true;
    if (policy_file_contains("allow", name)) return false;
    if (policy_file_contains("skip", name)) return true;
    if (is_system_lib(name)) return true;
    if (strncmp(name, "ld-linux", 8) == 0) return true;
    if (strncmp(name, "libc.so", 7) == 0) return true;
    if (strncmp(name, "libpthread.so", 13) == 0) return true;
    if (strncmp(name, "libdl.so", 8) == 0) return true;
    if (strncmp(name, "libm.so", 7) == 0) return true;
    if (strncmp(name, "librt.so", 8) == 0) return true;
    if (strncmp(name, "libGL.so", 8) == 0) return true;
    if (strncmp(name, "libEGL.so", 9) == 0) return true;
    if (strncmp(name, "libvulkan.so", 12) == 0) return true;
    if (strncmp(name, "libdrm.so", 9) == 0) return true;
    return false;
}

static int copy_library_preserving_name(const char *src, const char *libname, const char *dst_dir) {
    char dst[PATH_MAX];
    join_path(dst, sizeof(dst), dst_dir, libname);
    if (path_exists(dst)) return 0;
    struct stat st;
    if (stat(src, &st) != 0) return -1;
    return copy_file_simple(src, dst, st.st_mode & 0777 ? st.st_mode & 0777 : 0644);
}

static int inject_env_wrapper(const char *appdir) {
    char apprun[PATH_MAX], backup[PATH_MAX];
    join_path(apprun, sizeof(apprun), appdir, "AppRun");
    join_path(backup, sizeof(backup), appdir, ".AppRun.appimage-nurse-original");
    if (!path_exists(apprun)) {
        return ensure_basic_apprun(appdir);
    }
    if (!path_exists(backup)) {
        if (rename(apprun, backup) != 0) {
            printf("Could not wrap AppRun for portable environment: %s\n", strerror(errno));
            return 1;
        }
    }
    const char *script =
        "#!/bin/sh\n"
        "HERE=\"$(dirname \"$(readlink -f \"$0\")\")\"\n"
        "export PATH=\"$HERE/usr/bin:$HERE/bin:${PATH}\"\n"
        "export LD_LIBRARY_PATH=\"$HERE/usr/lib:$HERE/usr/lib64:$HERE/lib:$HERE/lib64:${LD_LIBRARY_PATH}\"\n"
        "export QT_PLUGIN_PATH=\"$HERE/usr/plugins:$HERE/usr/lib/qt6/plugins:$HERE/usr/lib/qt5/plugins:${QT_PLUGIN_PATH}\"\n"
        "export QML2_IMPORT_PATH=\"$HERE/usr/qml:$HERE/usr/lib/qt6/qml:$HERE/usr/lib/qt5/qml:${QML2_IMPORT_PATH}\"\n"
        "export GSETTINGS_SCHEMA_DIR=\"$HERE/usr/share/glib-2.0/schemas${GSETTINGS_SCHEMA_DIR:+:$GSETTINGS_SCHEMA_DIR}\"\n"
        "export XDG_DATA_DIRS=\"$HERE/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}\"\n"
        "exec \"$HERE/.AppRun.appimage-nurse-original\" \"$@\"\n";
    if (generate_text_file(apprun, script, 0755) != 0) return 1;
    printf("Portable environment wrapper installed around AppRun. Original saved as .AppRun.appimage-nurse-original\n");
    return 0;
}

static bool find_first_existing_dir(const char **dirs, char *out, size_t outsz) {
    for (int i = 0; dirs[i]; i++) {
        if (is_dir_path(dirs[i])) { snprintf(out, outsz, "%s", dirs[i]); return true; }
    }
    return false;
}

static int copy_framework_dir_if_present(const char *src, const char *dst, const char *label) {
    if (!is_dir_path(src)) return 0;
    if (path_exists(dst)) { printf("Framework support: %s already present: %s\n", label, dst); return 0; }
    if (copy_tree_simple(src, dst) == 0) { printf("Framework support: copied %s: %s -> %s\n", label, src, dst); return 1; }
    printf("Framework support: could not copy %s from %s\n", label, src);
    return -1;
}

static int partial_framework_bundle(const char *appdir) {
    if (!is_dir_path(appdir)) { printf("AppDir not found: %s\n", appdir); return 1; }
    session_log("Framework bundle/check: %s", appdir);
    WalkCtx ctx; walk_init(&ctx, appdir);
    int actions = 0, warnings = 0;
    printf("Partial framework bundling/checks\n");
    printf("This is best-effort. It copies common runtime/plugin folders when safe, and prints rebuild advice when not safe.\n\n");

    bool qt6 = ctx_any_base_contains(&ctx, "libQt6") || ctx_any_path_contains(&ctx, "PySide6") || ctx_any_path_contains(&ctx, "PyQt6");
    bool qt5 = ctx_any_base_contains(&ctx, "libQt5") || ctx_any_path_contains(&ctx, "PySide2") || ctx_any_path_contains(&ctx, "PyQt5");
    if (qt6 || qt5) {
        printf("Detected Qt/PySide/PyQt indicators.\n");
        const char *qt6_plugins[] = { "/usr/lib/x86_64-linux-gnu/qt6/plugins", "/usr/lib64/qt6/plugins", "/usr/lib/qt6/plugins", NULL };
        const char *qt5_plugins[] = { "/usr/lib/x86_64-linux-gnu/qt5/plugins", "/usr/lib64/qt5/plugins", "/usr/lib/qt5/plugins", NULL };
        char srcroot[PATH_MAX];
        if (find_first_existing_dir(qt6 ? qt6_plugins : qt5_plugins, srcroot, sizeof(srcroot))) {
            char src[PATH_MAX], dst[PATH_MAX];
            join_path(src, sizeof(src), srcroot, "platforms"); join_path(dst, sizeof(dst), appdir, "usr/plugins/platforms"); actions += copy_framework_dir_if_present(src, dst, "Qt platforms");
            join_path(src, sizeof(src), srcroot, "imageformats"); join_path(dst, sizeof(dst), appdir, "usr/plugins/imageformats"); actions += copy_framework_dir_if_present(src, dst, "Qt imageformats");
            join_path(src, sizeof(src), srcroot, "xcbglintegrations"); join_path(dst, sizeof(dst), appdir, "usr/plugins/xcbglintegrations"); actions += copy_framework_dir_if_present(src, dst, "Qt xcbglintegrations");
            join_path(src, sizeof(src), srcroot, "platformthemes"); join_path(dst, sizeof(dst), appdir, "usr/plugins/platformthemes"); actions += copy_framework_dir_if_present(src, dst, "Qt platformthemes");
            join_path(src, sizeof(src), srcroot, "tls"); join_path(dst, sizeof(dst), appdir, "usr/plugins/tls"); actions += copy_framework_dir_if_present(src, dst, "Qt tls");
        } else {
            printf("WARN: Qt plugin root not found on this host. Install Qt runtime plugins or use linuxdeploy/appimage-builder.\n"); warnings++;
        }
    }

    bool gtk = ctx_any_base_contains(&ctx, "libgtk-3") || ctx_any_base_contains(&ctx, "libgtk-4") || ctx_any_path_contains(&ctx, "glib-2.0/schemas");
    if (gtk) {
        printf("Detected GTK indicators.\n");
        char schemas[PATH_MAX]; join_path(schemas, sizeof(schemas), appdir, "usr/share/glib-2.0/schemas");
        if (is_dir_path(schemas)) {
            char glibcompile[PATH_MAX];
            if (find_in_path("glib-compile-schemas", glibcompile, sizeof(glibcompile))) {
                char *args[] = { glibcompile, schemas, NULL };
                int code = run_child_wait(NULL, NULL, args);
                if (code == 0) { printf("Compiled GSettings schemas in AppDir.\n"); actions++; }
                else { printf("WARN: glib-compile-schemas failed.\n"); warnings++; }
            } else { printf("WARN: glib-compile-schemas not found; schemas were not compiled.\n"); warnings++; }
        } else {
            printf("WARN: GTK app has no bundled GSettings schemas folder. Copying host schemas blindly is not safe; rebuild or bundle with linuxdeploy/appimage-builder.\n"); warnings++;
        }
    }

    bool electron = ctx_any_path_contains(&ctx, "resources/app.asar") || ctx_any_base_contains(&ctx, "chrome-sandbox") || ctx_any_base_contains(&ctx, "electron");
    if (electron) {
        printf("Detected Electron indicators.\n");
        printf("WARN: Electron sandbox fixes are distro-sensitive. This tool does not set setuid automatically. Prefer official Electron packaging or review chrome-sandbox/no-sandbox behavior manually.\n");
        warnings++;
    }

    bool game = ctx_any_base_contains(&ctx, "libSDL2") || ctx_any_base_contains(&ctx, "libSDL3") || ctx_any_base_contains(&ctx, "libopenal") || ctx_any_base_contains(&ctx, "libraylib") || ctx_any_base_contains(&ctx, "liballegro");
    if (game) {
        printf("Detected SDL/Raylib/OpenAL/Allegro-style indicators.\n");
        printf("Game framework support mostly comes from dependency bundling plus asset path checks. Avoid bundling graphics-driver libraries like libGL/libvulkan.\n");
    }

    bool godot = ctx_any_base_ends(&ctx, ".pck") || ctx_any_base_contains(&ctx, "godot") || ctx_any_base_contains(&ctx, "redot");
    if (godot) {
        printf("Detected Godot/Redot indicators.\n");
        if (!ctx_any_base_ends(&ctx, ".pck")) { printf("WARN: no .pck was obvious. Godot/Redot exports often need the .pck beside the executable.\n"); warnings++; }
    }

    bool python = ctx_any_path_contains(&ctx, "site-packages") || ctx_any_base_contains(&ctx, "python3") || ctx_any_path_contains(&ctx, "PySide") || ctx_any_path_contains(&ctx, "PyQt");
    if (python) {
        printf("Detected Python/PySide/PyQt indicators.\n");
        printf("Python support is partial: this can help with Qt plugins/env vars, but a missing interpreter or incomplete venv usually needs rebuilding the AppDir.\n");
    }

    inject_env_wrapper(appdir);
    walk_free(&ctx);
    printf("Framework pass complete: %d action(s), %d warning(s).\n", actions, warnings);
    return warnings ? 1 : 0;
}

static int smart_bundle_dependencies(const char *appdir) {
    if (!is_dir_path(appdir)) { printf("AppDir not found: %s\n", appdir); return 1; }
    int total_copied = 0, unresolved = 0;
    session_log("Smart bundle: %s", appdir);
    printf("Smart dependency bundling\n");
    printf("This attempts safe bundling only. It will not bundle glibc, GPU drivers, Vulkan/OpenGL drivers, or other core host libraries.\n\n");
    for (int pass = 1; pass <= 4; pass++) {
        WalkCtx ctx; walk_init(&ctx, appdir);
        StrList missing; list_init(&missing); collect_missing_deps(&ctx, &missing);
        if (missing.len == 0) {
            printf("Pass %d: no obvious missing non-system libraries.\n", pass);
            list_free(&missing); walk_free(&ctx); break;
        }
        printf("Pass %d: %zu missing library name(s) to try.\n", pass, missing.len);
        char libdst[PATH_MAX]; join_path(libdst, sizeof(libdst), appdir, "usr/lib"); mkdir_p(libdst, 0755);
        int copied_this_pass = 0;
        unresolved = 0;
        for (size_t i = 0; i < missing.len; i++) {
            const char *need = missing.items[i];
            if (should_never_bundle(need)) { printf("  skipped core/host library: %s\n", need); continue; }
            char src[PATH_MAX];
            if (locate_library_smart(need, src, sizeof(src))) {
                if (copy_library_preserving_name(src, need, libdst) == 0) { printf("  copied: %s from %s\n", need, src); copied_this_pass++; total_copied++; }
                else { printf("  failed to copy: %s from %s\n", need, src); unresolved++; }
            } else { printf("  not found on host: %s\n", need); unresolved++; }
        }
        list_free(&missing); walk_free(&ctx);
        if (copied_this_pass == 0) break;
    }
    inject_env_wrapper(appdir);
    patch_rpath_appdir(appdir);
    partial_framework_bundle(appdir);
    printf("\nSmart bundle complete: %d file(s) copied, %d unresolved item(s) in last pass.\n", total_copied, unresolved);
    printf("Next: run Diagnose and Test. If you see GLIBC_* too-new warnings, use compat-build; bundling will not fix that.\n");
    return unresolved ? 1 : 0;
}

static int compat_build_pipeline(const char *project_dir, const char *build_cmd, const char *appdir, const char *out_appimage, const char *base) {
    if (!project_dir || !*project_dir || !is_dir_path(project_dir)) { printf("Project folder not found: %s\n", project_dir ? project_dir : ""); return 1; }
    if (!build_cmd || !*build_cmd) { printf("Build command is required. Example: 'make AppDir' or './build.sh'\n"); return 2; }
    if (!appdir || !*appdir || !out_appimage || !*out_appimage) { printf("AppDir path and output AppImage path are required.\n"); return 2; }
    if (!base || !*base) base = "debian:bullseye-slim";
    char engine[PATH_MAX];
    if (!find_in_path("podman", engine, sizeof(engine)) && !find_in_path("docker", engine, sizeof(engine))) {
        printf("Neither podman nor docker was found. I can generate the pipeline files, but cannot run them.\n");
    }
    char work[PATH_MAX]; join_path(work, sizeof(work), project_dir, ".appimage-nurse-compat-build");
    mkdir_p(work, 0755);
    char cfile[PATH_MAX], runfile[PATH_MAX], inside[PATH_MAX];
    join_path(cfile, sizeof(cfile), work, "Containerfile");
    join_path(runfile, sizeof(runfile), work, "run-compat-build.sh");
    join_path(inside, sizeof(inside), work, "inside-container.sh");
    char buildq[8192], appq[PATH_MAX*2], outq[PATH_MAX*2], baseq[512];
    shell_quote_single(build_cmd, buildq, sizeof(buildq));
    shell_quote_single(appdir, appq, sizeof(appq));
    shell_quote_single(out_appimage, outq, sizeof(outq));
    shell_quote_single(base, baseq, sizeof(baseq));
    char ctext[12000];
    snprintf(ctext, sizeof(ctext),
        "FROM %s\n"
        "ENV DEBIAN_FRONTEND=noninteractive\n"
        "RUN apt-get update && apt-get install -y --no-install-recommends \\\n"
        "    ca-certificates wget curl file binutils desktop-file-utils patchelf squashfs-tools fuse libfuse2 \\\n"
        "    build-essential make cmake pkg-config git python3 python3-pip \\\n"
        "    libglib2.0-bin \\\n"
        "    && rm -rf /var/lib/apt/lists/*\n"
        "WORKDIR /work\n"
        "CMD [\"/bin/sh\", \".appimage-nurse-compat-build/inside-container.sh\"]\n", base);
    char itext[16000];
    snprintf(itext, sizeof(itext),
        "#!/bin/sh\nset -eu\n"
        "BUILD_CMD=%s\nAPPDIR=%s\nOUT_APPIMAGE=%s\n"
        "echo '[1/4] Running build command inside older base...'\n"
        "sh -lc \"$BUILD_CMD\"\n"
        "echo '[2/4] Checking AppDir...'\n"
        "if [ ! -d \"$APPDIR\" ]; then echo \"AppDir not found after build: $APPDIR\"; exit 1; fi\n"
        "if [ -e \"$APPDIR/AppRun\" ]; then chmod +x \"$APPDIR/AppRun\" || true; fi\n"
        "echo '[3/4] Finding appimagetool...'\n"
        "APPIMAGETOOL=${APPIMAGETOOL:-}\n"
        "if [ -z \"$APPIMAGETOOL\" ] && command -v appimagetool >/dev/null 2>&1; then APPIMAGETOOL=appimagetool; fi\n"
        "if [ -z \"$APPIMAGETOOL\" ] && [ -x /work/appimagetool-x86_64.AppImage ]; then APPIMAGETOOL=/work/appimagetool-x86_64.AppImage; fi\n"
        "if [ -z \"$APPIMAGETOOL\" ]; then\n"
        "  echo 'Downloading appimagetool because it was not found...'\n"
        "  wget -q -O /tmp/appimagetool 'https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage' || { echo 'Could not download appimagetool. Put appimagetool-x86_64.AppImage in the project folder and retry.'; exit 1; }\n"
        "  chmod +x /tmp/appimagetool\n"
        "  APPIMAGETOOL=/tmp/appimagetool\n"
        "fi\n"
        "echo '[4/4] Repacking AppImage...'\n"
        "mkdir -p \"$(dirname \"$OUT_APPIMAGE\")\"\n"
        "ARCH=x86_64 \"$APPIMAGETOOL\" \"$APPDIR\" \"$OUT_APPIMAGE\"\n"
        "echo \"Done: $OUT_APPIMAGE\"\n", buildq, appq, outq);
    char rtext[12000];
    snprintf(rtext, sizeof(rtext),
        "#!/bin/sh\nset -eu\n"
        "ENGINE=${ENGINE:-podman}\n"
        "if ! command -v \"$ENGINE\" >/dev/null 2>&1; then ENGINE=docker; fi\n"
        "if ! command -v \"$ENGINE\" >/dev/null 2>&1; then echo 'Install podman or docker first.'; exit 1; fi\n"
        "cd \"$(dirname \"$0\")/..\"\n"
        "echo \"Using engine: $ENGINE\"\n"
        "\"$ENGINE\" build -t appimage-nurse-compat -f .appimage-nurse-compat-build/Containerfile .\n"
        "\"$ENGINE\" run --rm -it -v \"$PWD:/work\" -w /work appimage-nurse-compat\n");
    if (generate_text_file(cfile, ctext, 0644) != 0 || generate_text_file(inside, itext, 0755) != 0 || generate_text_file(runfile, rtext, 0755) != 0) {
        printf("Could not write compatibility pipeline files.\n"); return 1;
    }
    printf("Generated older-distro build pipeline in: %s\n", work);
    printf("Base image: %s\n", base);
    printf("Build command: %s\n", build_cmd);
    if (!find_in_path("podman", engine, sizeof(engine)) && !find_in_path("docker", engine, sizeof(engine))) {
        printf("Run later: %s\n", runfile);
        return 2;
    }
    printf("Running compatibility build now. This can take a while.\n");
    char *args[] = { runfile, NULL };
    return run_child_wait(project_dir, NULL, args);
}

static int bundle_dependencies(const char *appdir) {
    if (!is_dir_path(appdir)) { printf("AppDir not found: %s\n", appdir); return 1; }
    WalkCtx ctx; walk_init(&ctx, appdir);
    StrList missing; list_init(&missing); collect_missing_deps(&ctx, &missing);
    if (missing.len == 0) { printf("No obvious non-system missing libraries found.\n"); list_free(&missing); walk_free(&ctx); return 0; }
    char libdst[PATH_MAX]; join_path(libdst, sizeof(libdst), appdir, "usr/lib"); mkdir_p(libdst, 0755);
    int copied = 0, not_found = 0;
    session_log("Conservative bundle: %s", appdir);
    printf("Conservative bundling: copying only non-core libraries found on this host.\n");
    for (size_t i = 0; i < missing.len; i++) {
        char src[PATH_MAX], dst[PATH_MAX];
        if (locate_system_library(missing.items[i], src, sizeof(src))) {
            join_path(dst, sizeof(dst), libdst, missing.items[i]);
            if (copy_file_simple(src, dst, 0644) == 0) { printf("Copied: %s -> %s\n", src, dst); copied++; }
            else { printf("Could not copy: %s\n", src); not_found++; }
        } else { printf("Not found on host: %s\n", missing.items[i]); not_found++; }
    }
    ensure_basic_apprun(appdir);
    printf("Bundle pass complete: %d copied, %d not found/failed.\n", copied, not_found);
    printf("Next: run Diagnose again. If GLIBC is too new, rebuild on an older base; bundling will not fix that.\n");
    list_free(&missing); walk_free(&ctx);
    return not_found ? 1 : 0;
}

static int patch_rpath_appdir(const char *appdir) {
    if (!is_dir_path(appdir)) { printf("AppDir not found: %s\n", appdir); return 1; }
    ensure_basic_apprun(appdir);
    char tool[PATH_MAX];
    if (!find_in_path("patchelf", tool, sizeof(tool))) {
        printf("patchelf was not found in PATH.\n");
        printf("Fallback done: AppRun uses LD_LIBRARY_PATH when AppRun can be generated/found.\n");
        printf("Install patchelf for true ELF RUNPATH patching.\n");
        return 2;
    }
    WalkCtx ctx; walk_init(&ctx, appdir);
    const char *rpath = "$ORIGIN:$ORIGIN/../lib:$ORIGIN/../../lib:$ORIGIN/../lib64:$ORIGIN/../../lib64";
    int patched = 0, failed = 0;
    for (size_t i = 0; i < ctx.elf_files.len && i < MAX_ELF_SCAN; i++) {
        char *args[] = { tool, "--force-rpath", "--set-rpath", (char*)rpath, ctx.elf_files.items[i], NULL };
        int code = run_child_wait(NULL, NULL, args);
        if (code == 0) patched++; else failed++;
    }
    walk_free(&ctx);
    printf("RPATH patch pass complete: %d patched, %d failed.\n", patched, failed);
    return failed ? 1 : 0;
}

static int linuxdeploy_bundle(const char *appdir, const char *outfile) {
    if (!is_dir_path(appdir)) { printf("AppDir not found: %s\n", appdir); return 1; }
    char tool[PATH_MAX];
    if (!find_in_path("linuxdeploy", tool, sizeof(tool)) && !find_in_path("linuxdeploy-x86_64.AppImage", tool, sizeof(tool))) {
        printf("linuxdeploy was not found in PATH.\n");
        printf("This tab is wired in but expects linuxdeploy to be installed by the user.\n");
        return 2;
    }
    printf("Running linuxdeploy integration. This may bundle libraries and produce an AppImage if the AppImage plugin is present.\n");
    char *args[] = { tool, "--appdir", (char*)appdir, "--output", "appimage", NULL };
    int code = run_child_wait(NULL, NULL, args);
    if (outfile && *outfile) printf("If linuxdeploy created a differently named .AppImage, rename it to: %s\n", outfile);
    return code;
}

static bool get_first_desktop_info(const char *appdir, DesktopInfo *di) {
    WalkCtx ctx; walk_init(&ctx, appdir);
    bool ok = false;
    if (ctx.desktop_files.len > 0) ok = parse_desktop_file(ctx.desktop_files.items[0], di);
    walk_free(&ctx);
    return ok;
}

static int generate_appimage_builder_recipe(const char *appdir, const char *outpath) {
    if (!is_dir_path(appdir)) { printf("AppDir not found: %s\n", appdir); return 1; }
    DesktopInfo di; memset(&di, 0, sizeof(di)); get_first_desktop_info(appdir, &di);
    const char *name = di.name[0] ? di.name : base_name(appdir);
    const char *execv = di.exec[0] ? di.exec : "AppRun";
    char yaml[8192];
    snprintf(yaml, sizeof(yaml),
        "# Generated by AppImage Nurse. Review before use.\n"
        "version: 1\n"
        "AppDir:\n"
        "  path: %s\n"
        "  app_info:\n"
        "    id: %s\n"
        "    name: %s\n"
        "    icon: %s\n"
        "    version: latest\n"
        "    exec: %s\n"
        "    exec_args: \"$@\"\n"
        "  runtime:\n"
        "    env:\n"
        "      LD_LIBRARY_PATH: \"$APPDIR/usr/lib:$APPDIR/usr/lib64:$LD_LIBRARY_PATH\"\n"
        "AppImage:\n"
        "  arch: x86_64\n"
        "  update-information: guess\n",
        appdir, name, name, di.icon[0] ? di.icon : "application", execv);
    if (generate_text_file(outpath, yaml, 0644) != 0) { printf("Could not write recipe: %s\n", strerror(errno)); return 1; }
    printf("Generated appimage-builder recipe: %s\n", outpath);
    return 0;
}

static int generate_compat_template(const char *outdir, const char *base) {
    if (!base || !*base) base = "debian:bullseye-slim";
    if (mkdir_p(outdir, 0755) != 0) { printf("Could not create folder: %s\n", strerror(errno)); return 1; }
    char cfile[PATH_MAX], script[PATH_MAX]; join_path(cfile, sizeof(cfile), outdir, "Containerfile"); join_path(script, sizeof(script), outdir, "build-inside-container.sh");
    char ctext[8192];
    snprintf(ctext, sizeof(ctext),
        "FROM %s\n"
        "RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates file binutils patchelf wget desktop-file-utils \\\n"
        "    && rm -rf /var/lib/apt/lists/*\n"
        "WORKDIR /work\n"
        "# Copy your source/project here, build inside this older base, then package the AppDir.\n"
        "CMD [\"/bin/sh\"]\n", base);
    char stext[8192];
    snprintf(stext, sizeof(stext),
        "#!/bin/sh\nset -eu\nENGINE=${ENGINE:-podman}\nif ! command -v \"$ENGINE\" >/dev/null 2>&1; then ENGINE=docker; fi\nif ! command -v \"$ENGINE\" >/dev/null 2>&1; then echo 'Install podman or docker first.'; exit 1; fi\n\"$ENGINE\" build -t appimage-nurse-compat -f Containerfile .\n\"$ENGINE\" run --rm -it -v \"$PWD:/work\" appimage-nurse-compat\n");
    if (generate_text_file(cfile, ctext, 0644) != 0 || generate_text_file(script, stext, 0755) != 0) return 1;
    printf("Generated compatibility build template in: %s\n", outdir);
    printf("Default base: %s\n", base);
    return 0;
}


static void shell_quote_single(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    if (outsz == 0) return;
    out[j++] = '\'';
    for (size_t i = 0; in && in[i] && j + 5 < outsz; i++) {
        if (in[i] == '\'') {
            out[j++] = '\''; out[j++] = '\\'; out[j++] = '\''; out[j++] = '\'';
        } else out[j++] = in[i];
    }
    if (j + 1 < outsz) out[j++] = '\'';
    out[j < outsz ? j : outsz - 1] = 0;
}

static int appstream_validate(const char *appdir) {
    if (!is_dir_path(appdir)) { printf("AppDir not found: %s\n", appdir); return 1; }
    WalkCtx ctx; walk_init(&ctx, appdir);
    StrList meta; list_init(&meta);
    for (size_t i = 0; i < ctx.all_files.len; i++) {
        const char *p = ctx.all_files.items[i];
        if ((strstr(p, "/usr/share/metainfo/") || strstr(p, "/usr/share/appdata/")) &&
            (ends_with(p, ".metainfo.xml") || ends_with(p, ".appdata.xml") || ends_with(p, ".xml"))) {
            list_add(&meta, p);
        }
    }
    printf("AppStream metadata check\n");
    if (meta.len == 0) {
        printf("[%sWARN%s] No AppStream metadata found under usr/share/metainfo or usr/share/appdata.\n", status_color("WARN"), color_reset());
        printf("This is not fatal for a runnable AppImage, but app stores/software centers may not show rich metadata.\n");
        list_free(&meta); walk_free(&ctx); return 1;
    }
    char appstreamcli[PATH_MAX];
    bool have_cli = find_in_path("appstreamcli", appstreamcli, sizeof(appstreamcli));
    int bad = 0;
    for (size_t i = 0; i < meta.len; i++) {
        printf("Found metadata: %s\n", meta.items[i]);
        if (have_cli) {
            char *args[] = { appstreamcli, "validate", meta.items[i], NULL };
            int code = run_child_wait(NULL, NULL, args);
            if (code == 0) printf("[%sOK%s] appstreamcli accepted this file.\n", status_color("OK"), color_reset());
            else { printf("[%sWARN%s] appstreamcli reported issue(s), exit code %d.\n", status_color("WARN"), color_reset(), code); bad++; }
        } else {
            unsigned char *data = NULL; size_t sz = 0;
            if (read_entire_file(meta.items[i], &data, &sz)) {
                bool has_component = memmem(data, sz, "<component", 10) != NULL;
                bool has_id = memmem(data, sz, "<id>", 4) != NULL;
                bool has_name = memmem(data, sz, "<name>", 6) != NULL;
                if (has_component && has_id && has_name) printf("[%sOK%s] Basic metadata tags found. Install appstreamcli for strict validation.\n", status_color("OK"), color_reset());
                else { printf("[%sWARN%s] Metadata exists, but basic tags look incomplete. Install appstreamcli for strict validation.\n", status_color("WARN"), color_reset()); bad++; }
                free(data);
            } else { printf("[%sWARN%s] Could not read metadata file.\n", status_color("WARN"), color_reset()); bad++; }
        }
    }
    list_free(&meta); walk_free(&ctx);
    return bad ? 1 : 0;
}

static int update_metadata_check(const char *appimage) {
    if (!is_file_path(appimage)) { printf("AppImage file not found: %s\n", appimage); return 1; }
    struct stat st; if (stat(appimage, &st) == 0 && !(st.st_mode & S_IXUSR)) chmod(appimage, st.st_mode | S_IXUSR);
    printf("AppImage update metadata check\n");
    printf("Running: %s --appimage-updateinformation\n", appimage);
    char *args[] = { (char*)appimage, "--appimage-updateinformation", NULL };
    int code = run_child_wait(NULL, NULL, args);
    if (code == 0) printf("[%sOK%s] Runtime returned update information above.\n", status_color("OK"), color_reset());
    else {
        printf("[%sWARN%s] No update information was returned, or this AppImage runtime does not support the query. Exit code %d.\n", status_color("WARN"), color_reset(), code);
        printf("This is optional. It only matters if the AppImage is meant to support built-in updates.\n");
    }
    return code == 0 ? 0 : 1;
}

static int signature_check(const char *appimage) {
    if (!is_file_path(appimage)) { printf("AppImage file not found: %s\n", appimage); return 1; }
    struct stat st; if (stat(appimage, &st) == 0 && !(st.st_mode & S_IXUSR)) chmod(appimage, st.st_mode | S_IXUSR);
    printf("AppImage signature check\n");
    printf("Running: %s --appimage-signature\n", appimage);
    char *args[] = { (char*)appimage, "--appimage-signature", NULL };
    int code = run_child_wait(NULL, NULL, args);
    if (code == 0) printf("[%sOK%s] Signature information was returned above.\n", status_color("OK"), color_reset());
    else {
        printf("[%sWARN%s] No embedded signature info was returned, or this runtime does not support the query. Exit code %d.\n", status_color("WARN"), color_reset(), code);
        printf("Unsigned AppImages can still run. Signatures are optional release/distribution polish.\n");
    }
    char gpg[PATH_MAX];
    if (find_in_path("gpg", gpg, sizeof(gpg))) printf("[%sOK%s] gpg is available for detached/manual signature workflows: %s\n", status_color("OK"), color_reset(), gpg);
    else printf("[%sWARN%s] gpg not found; only runtime signature query was attempted.\n", status_color("WARN"), color_reset());
    return code == 0 ? 0 : 1;
}

static int cross_distro_test(const char *target, const char *image, const char *logfile) {
    if (!target || !*target) { printf("Target is required.\n"); return 1; }
    if (!image || !*image) image = "debian:bullseye-slim";
    char engine[PATH_MAX];
    if (!find_in_path("podman", engine, sizeof(engine)) && !find_in_path("docker", engine, sizeof(engine))) {
        printf("podman/docker was not found. Install one of them for cross-distro testing.\n");
        return 2;
    }
    char real[PATH_MAX];
    if (!realpath(target, real)) { printf("Cannot resolve target: %s\n", strerror(errno)); return 1; }
    char parent[PATH_MAX]; parent_dir(real, parent, sizeof(parent));
    char base[PATH_MAX]; snprintf(base, sizeof(base), "%s", base_name(real));
    char vol[PATH_MAX + 32]; snprintf(vol, sizeof(vol), "%s:/work:ro", parent);
    char qbase[PATH_MAX * 2], cmd[4096]; shell_quote_single(base, qbase, sizeof(qbase));
    struct stat st; stat(real, &st);
    if (S_ISDIR(st.st_mode)) {
        snprintf(cmd, sizeof(cmd), "chmod +x /work/%s/AppRun 2>/dev/null || true; /work/%s/AppRun >/tmp/appimage-nurse-cross.log 2>&1; code=$?; cat /tmp/appimage-nurse-cross.log; exit $code", qbase, qbase);
    } else {
        snprintf(cmd, sizeof(cmd), "chmod +x /work/%s 2>/dev/null || true; /work/%s --appimage-version >/tmp/appimage-nurse-cross.log 2>&1; code=$?; if [ $code -ne 0 ]; then /work/%s --appimage-help >/tmp/appimage-nurse-cross.log 2>&1; code=$?; fi; cat /tmp/appimage-nurse-cross.log; exit $code", qbase, qbase, qbase);
    }
    printf("Cross-distro smoke test\n");
    printf("Engine: %s\nImage: %s\nMounted parent: %s\n", engine, image, parent);
    char *args[] = { engine, "run", "--rm", "-v", vol, "-w", "/work", (char*)image, "/bin/sh", "-lc", cmd, NULL };
    int code = run_child_wait(NULL, logfile && *logfile ? logfile : NULL, args);
    if (logfile && *logfile) printf("Log saved to: %s\n", logfile);
    if (code == 0) printf("[%sOK%s] Container smoke test exited cleanly.\n", status_color("OK"), color_reset());
    else printf("[%sWARN%s] Container smoke test exited with code %d. GUI apps can fail in containers even when packaging is mostly fine.\n", status_color("WARN"), color_reset(), code);
    return code == 0 ? 0 : 1;
}

static int package_self_appimage(const char *outfile) {
    if (!outfile || !*outfile) { printf("Output .AppImage path is required.\n"); return 1; }
    char tool[PATH_MAX];
    if (!find_in_path("appimagetool", tool, sizeof(tool))) {
        printf("appimagetool was not found in PATH. Install it first, then retry.\n");
        return 2;
    }
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) { printf("Could not resolve this executable from /proc/self/exe.\n"); return 1; }
    self[n] = 0;
    char tmp[PATH_MAX]; snprintf(tmp, sizeof(tmp), "/tmp/appimage-nurse-self-%ld.AppDir", (long)getpid());
    if (path_exists(tmp)) { printf("Temporary folder already exists: %s\n", tmp); return 1; }
    printf("Creating temporary AppDir: %s\n", tmp);
    int rc = create_appdir_skeleton(tmp, "AppImage Nurse", self, "", "");
    if (rc != 0) return rc;
    printf("Packaging self with appimagetool...\n");
    rc = appimage_repack(tmp, outfile);
    if (rc == 0) printf("Created self AppImage: %s\n", outfile);
    else printf("Self packaging failed. Temporary AppDir left at: %s\n", tmp);
    return rc;
}

static void check_tools(void) {
    const char *tools[] = {"appimagetool", "linuxdeploy", "appimage-builder", "patchelf", "appstreamcli", "gpg", "podman", "docker", "ldconfig", "glib-compile-schemas", "file", NULL};
    printf("External tool check\n");
    for (int i = 0; tools[i]; i++) {
        char path[PATH_MAX];
        if (find_in_path(tools[i], path, sizeof(path))) printf("[%sOK%s] %s -> %s\n", status_color("OK"), color_reset(), tools[i], path);
        else printf("[%sWARN%s] %s not found\n", status_color("WARN"), color_reset(), tools[i]);
    }
}

static int run_child_wait(const char *cwd, const char *logfile, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (cwd && *cwd) chdir(cwd);
        if (logfile && *logfile) {
            int fd = open(logfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
}

static int rm_rf(const char *path) {
    struct stat st;
    if (!path || !*path) return -1;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            char child[PATH_MAX]; join_path(child, sizeof(child), path, ent->d_name);
            if (rm_rf(child) != 0) { closedir(d); return -1; }
        }
        closedir(d);
        return rmdir(path);
    }
    return unlink(path);
}


static int copy_with_backup_path(const char *appdir, char *backup, size_t backup_sz) {
    if (!is_dir_path(appdir)) { printf("AppDir not found: %s\n", appdir); return 1; }
    char parent[PATH_MAX]; parent_dir(appdir, parent, sizeof(parent));
    const char *base = base_name(appdir);
    time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
    char name[PATH_MAX];
    snprintf(name, sizeof(name), "%s.nurse-backup-%04d%02d%02d-%02d%02d%02d", base,
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    join_path(backup, backup_sz, parent, name);
    if (copy_tree_simple(appdir, backup) != 0) { printf("Could not create backup copy.\n"); return 1; }
    printf("Backup created beside AppDir: %s\n", backup);
    session_log("Backup created: %s", backup);
    return 0;
}

static int backup_appdir(const char *appdir) {
    char backup[PATH_MAX];
    return copy_with_backup_path(appdir, backup, sizeof(backup));
}

static int restore_appdir_from_backup(const char *backup, const char *target) {
    if (!is_dir_path(backup)) { printf("Backup folder not found: %s\n", backup); return 1; }
    if (!target || !*target) { printf("Target AppDir path is required.\n"); return 1; }
    printf("Restoring backup to: %s\n", target);
    if (path_exists(target)) {
        char safety[PATH_MAX];
        if (copy_with_backup_path(target, safety, sizeof(safety)) != 0) {
            printf("Could not make safety backup of current target. Restore cancelled.\n");
            return 1;
        }
        if (rm_rf(target) != 0) { printf("Could not remove current target: %s\n", strerror(errno)); return 1; }
    }
    if (copy_tree_simple(backup, target) != 0) { printf("Restore failed.\n"); return 1; }
    printf("Restore complete.\n");
    session_log("Restored backup %s to %s", backup, target);
    return 0;
}

static void explain_error_text(const char *text) {
    if (!text || !*text) { printf("No error text provided.\n"); return; }
    int hits = 0;
#define EXPLAIN_IF(pat, msg) do { if (strstr(text, pat)) { printf("- Pattern: %s\n  %s\n", pat, msg); hits++; } } while (0)
    EXPLAIN_IF("GLIBC_", "Likely built against a newer glibc than the target system provides. Repacking usually will not fix this; rebuild in an older compatibility environment.");
    EXPLAIN_IF("GLIBCXX_", "The C++ runtime/libstdc++ version may be too new or missing. Try bundling libstdc++ carefully or rebuild on an older base.");
    EXPLAIN_IF("CXXABI_", "The C++ ABI runtime is newer than the target provides. Bundling libstdc++ may help, but older-base rebuild is safer.");
    EXPLAIN_IF("libfuse.so.2", "FUSE 2 compatibility is missing on this system. Install libfuse2/libfuse2t64 or use extract/RAM mode.");
    EXPLAIN_IF("No such file or directory", "Often means the Exec target, interpreter, or a needed file path is missing. Check AppRun, .desktop Exec=, and bundled interpreters.");
    EXPLAIN_IF("Permission denied", "A file is likely missing executable permission. Try Repair or chmod +x on AppRun/main executable.");
    EXPLAIN_IF("Exec format error", "The binary architecture likely does not match this system, or the file is not a valid executable.");
    EXPLAIN_IF("error while loading shared libraries", "A shared library is missing or not found. Try Diagnose, Smart Bundle, and RPATH/AppRun environment wrapper.");
    EXPLAIN_IF("cannot open shared object file", "A shared library could not be found by the dynamic linker. Try Smart Bundle and RPATH/AppRun environment wrapper.");
    EXPLAIN_IF("xcb", "Qt xcb/platform plugin or one of its dependencies may be missing. Try framework bundle or linuxdeploy.");
    EXPLAIN_IF("Could not load the Qt platform plugin", "Qt platform plugins are missing or not discoverable. Check usr/plugins/platforms/libqxcb.so and QT_PLUGIN_PATH.");
    EXPLAIN_IF("chrome-sandbox", "Electron sandbox setup may be wrong. Review chrome-sandbox permissions or the app's no-sandbox behavior.");
    EXPLAIN_IF("AppRun", "The AppRun entry point may be missing, not executable, or pointing to the wrong target. Try Repair and Diagnose.");
    EXPLAIN_IF("desktop", "The .desktop file may be missing or invalid. Check Name, Exec, Icon, Type, and Categories fields.");
#undef EXPLAIN_IF
    if (!hits) printf("No known error pattern matched. Save a report/log and inspect the raw error text.\n");
}

static int explain_error_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("Could not open log: %s\n", strerror(errno)); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    if (n < 0) n = 0;
    if (n > 1024 * 1024) n = 1024 * 1024;
    char *buf = calloc(1, (size_t)n + 1);
    if (!buf) { fclose(f); return 1; }
    fread(buf, 1, (size_t)n, f);
    fclose(f);
    explain_error_text(buf);
    free(buf);
    return 0;
}

static int make_unique_dir(const char *base, const char *prefix, char *out, size_t outsz) {
    if (!base || !*base) base = ".";
    if (!prefix || !*prefix) prefix = "appimage-nurse-";
    snprintf(out, outsz, "%s/%sXXXXXX", base, prefix);
    char *made = mkdtemp(out);
    return made ? 0 : -1;
}

static int appimage_extract_internal(const char *appimage, const char *outdir, const char *temp_base) {
    char real[PATH_MAX];
    if (!realpath(appimage, real)) {
        printf("Cannot resolve AppImage path: %s\n", strerror(errno));
        return 1;
    }
    struct stat st;
    session_log("Extract AppImage: %s -> %s", appimage, outdir);
    if (stat(real, &st) != 0 || !S_ISREG(st.st_mode)) {
        printf("Not a regular AppImage file: %s\n", real);
        return 1;
    }
    if (!(st.st_mode & S_IXUSR)) {
        printf("Executable bit is missing. Setting owner executable bit.\n");
        chmod(real, st.st_mode | S_IXUSR);
    }

    char parent[PATH_MAX]; parent_dir(outdir, parent, sizeof(parent));
    if (mkdir_p(parent, 0755) != 0) {
        printf("Could not create output parent folder: %s\n", strerror(errno));
        return 1;
    }

    if (path_exists(outdir)) {
        if (is_dir_path(outdir) && dir_is_empty(outdir)) {
            printf("Output folder already exists but is empty; using it.\n");
            if (rmdir(outdir) != 0) { printf("Could not prepare empty output folder: %s\n", strerror(errno)); return 1; }
        } else {
            printf("Output path already exists and is not empty: %s\n", outdir);
            return 1;
        }
    }

    char tmp[PATH_MAX];
    const char *base = (temp_base && *temp_base) ? temp_base : parent;
    if (!is_dir_path(base)) {
        printf("Temporary base folder is not available: %s\n", base);
        return 1;
    }
    if (make_unique_dir(base, ".appimage-nurse-extract-", tmp, sizeof(tmp)) != 0) {
        printf("Could not create unique temp extraction folder in %s: %s\n", base, strerror(errno));
        return 1;
    }

    char *args[] = { real, "--appimage-extract", NULL };
    printf("Extracting with: %s --appimage-extract\n", real);
    printf("Temporary extraction folder: %s\n", tmp);
    int code = run_child_wait(tmp, NULL, args);
    if (code != 0) {
        printf("Extraction command exited with code %d.\n", code);
        printf("Temp folder left at: %s\n", tmp);
        return code;
    }
    char extracted[PATH_MAX]; join_path(extracted, sizeof(extracted), tmp, "squashfs-root");
    if (!is_dir_path(extracted)) {
        printf("Expected squashfs-root was not created. Temp folder left at: %s\n", tmp);
        return 1;
    }
    if (rename(extracted, outdir) != 0) {
        printf("Could not move extracted AppDir to %s: %s\n", outdir, strerror(errno));
        printf("Extracted folder remains at: %s\n", extracted);
        return 1;
    }
    rm_rf(tmp);
    printf("Extracted AppDir: %s\n", outdir);
    return 0;
}

static int appimage_extract(const char *appimage, const char *outdir) {
    return appimage_extract_internal(appimage, outdir, NULL);
}

static int appimage_repack(const char *appdir, const char *outfile) {
    if (!is_dir_path(appdir)) {
        printf("AppDir not found: %s\n", appdir);
        return 1;
    }
    if (!outfile || !*outfile) {
        printf("Output .AppImage path is required.\n");
        return 1;
    }
    if (is_dir_path(outfile)) {
        printf("Output path is a folder. Please include the AppImage filename, for example: %s/Repacked.AppImage\n", outfile);
        return 1;
    }
    if (!ends_with(outfile, ".AppImage")) {
        printf("Warning: output path does not end in .AppImage. Continuing anyway.\n");
    }
    char tool[PATH_MAX];
    if (!find_in_path("appimagetool", tool, sizeof(tool))) {
        printf("appimagetool was not found in PATH.\n");
        printf("Repacking is wired in, but not bundled in this no-dependency build.\n");
        printf("Install appimagetool, then rerun: appimage-nurse repack <AppDir> <Output.AppImage>\n");
        return 2;
    }
    char *args[] = { tool, (char*)appdir, (char*)outfile, NULL };
    printf("Repacking with: %s %s %s\n", tool, appdir, outfile);
    int code = run_child_wait(NULL, NULL, args);
    if (code == 0) printf("Created AppImage: %s\n", outfile);
    else printf("appimagetool exited with code %d.\n", code);
    return code;
}

static int generate_text_file(const char *path, const char *text, mode_t mode) {
    char pd[PATH_MAX]; parent_dir(path, pd, sizeof(pd));
    if (mkdir_p(pd, 0755) != 0) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    size_t n = strlen(text), off = 0;
    while (off < n) {
        ssize_t w = write(fd, text + off, n - off);
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }
    close(fd);
    chmod(path, mode);
    return 0;
}

static void sanitize_id(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c)) out[j++] = (char)tolower(c);
        else if (c == '-' || c == '_' || c == '.') out[j++] = (char)c;
        else if (isspace(c)) out[j++] = '-';
    }
    out[j] = 0;
    if (!out[0]) snprintf(out, outsz, "myapp");
}

static int create_appdir_skeleton(const char *appdir, const char *appname, const char *execpath, const char *iconpath, const char *assetspath) {
    if (path_exists(appdir)) {
        printf("Output AppDir already exists: %s\n", appdir);
        return 1;
    }
    char id[256]; sanitize_id(appname, id, sizeof(id));
    char usrbin[PATH_MAX], usrshareapps[PATH_MAX], usricons[PATH_MAX], usrshare[PATH_MAX];
    join_path(usrbin, sizeof(usrbin), appdir, "usr/bin");
    join_path(usrshareapps, sizeof(usrshareapps), appdir, "usr/share/applications");
    join_path(usricons, sizeof(usricons), appdir, "usr/share/icons/hicolor/256x256/apps");
    join_path(usrshare, sizeof(usrshare), appdir, "usr/share");
    if (mkdir_p(usrbin, 0755) != 0 || mkdir_p(usrshareapps, 0755) != 0 || mkdir_p(usricons, 0755) != 0) {
        printf("Could not create AppDir folders: %s\n", strerror(errno));
        return 1;
    }
    char dstexe[PATH_MAX]; join_path(dstexe, sizeof(dstexe), usrbin, id);
    if (execpath && *execpath) {
        if (copy_file_simple(execpath, dstexe, 0755) != 0) {
            printf("Could not copy executable: %s\n", strerror(errno));
            return 1;
        }
    } else {
        const char *demo = "#!/bin/sh\necho 'Replace usr/bin/myapp with your real executable.'\nread -r dummy 2>/dev/null || true\n";
        snprintf(dstexe, sizeof(dstexe), "%s/usr/bin/myapp", appdir);
        if (generate_text_file(dstexe, demo, 0755) != 0) return 1;
        snprintf(id, sizeof(id), "myapp");
    }
    const char *iconext = icon_ext_from_path(iconpath);
    char iconname[512]; snprintf(iconname, sizeof(iconname), "%s.%s", id, iconext);
    char dsticon[PATH_MAX]; join_path(dsticon, sizeof(dsticon), usricons, iconname);
    char rooticon[PATH_MAX]; snprintf(rooticon, sizeof(rooticon), "%s/%s.%s", appdir, id, iconext);
    if (iconpath && *iconpath && is_file_path(iconpath)) {
        copy_file_simple(iconpath, dsticon, 0644);
        copy_file_simple(iconpath, rooticon, 0644);
    } else {
        snprintf(iconname, sizeof(iconname), "%s.xpm", id);
        join_path(dsticon, sizeof(dsticon), usricons, iconname);
        snprintf(rooticon, sizeof(rooticon), "%s/%s.xpm", appdir, id);
        write_placeholder_icon_xpm(dsticon);
        write_placeholder_icon_xpm(rooticon);
        printf("No icon supplied; generated placeholder red square/white plus icon.\n");
    }
    if (assetspath && *assetspath && is_dir_path(assetspath)) {
        char assetdst[PATH_MAX]; join_path(assetdst, sizeof(assetdst), usrshare, id);
        if (copy_tree_simple(assetspath, assetdst) != 0) printf("Warning: could not copy all assets.\n");
    }
    char desktop[PATH_MAX], apprun[PATH_MAX], desktop2[PATH_MAX];
    snprintf(desktop, sizeof(desktop), "%s/%s.desktop", appdir, id);
    char desktop_file_name[512]; snprintf(desktop_file_name, sizeof(desktop_file_name), "%s.desktop", id); join_path(desktop2, sizeof(desktop2), usrshareapps, desktop_file_name);
    snprintf(apprun, sizeof(apprun), "%s/AppRun", appdir);
    char desktop_text[4096];
    snprintf(desktop_text, sizeof(desktop_text),
        "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\nIcon=%s\nCategories=Utility;\nTerminal=false\n",
        appname && *appname ? appname : "My App", id, id);
    char apprun_text[4096];
    snprintf(apprun_text, sizeof(apprun_text),
        "#!/bin/sh\nHERE=\"$(dirname \"$(readlink -f \"$0\")\")\"\nexport PATH=\"$HERE/usr/bin:$PATH\"\nexport LD_LIBRARY_PATH=\"$HERE/usr/lib:$HERE/usr/lib64:${LD_LIBRARY_PATH}\"\nexec \"$HERE/usr/bin/%s\" \"$@\"\n", id);
    if (generate_text_file(desktop, desktop_text, 0644) != 0) return 1;
    copy_file_simple(desktop, desktop2, 0644);
    if (generate_text_file(apprun, apprun_text, 0755) != 0) return 1;
    printf("Created AppDir skeleton: %s\n", appdir);
    printf("Next: run Diagnose on it, then Repack when appimagetool is available.\n");
    return 0;
}


static void print_appdir_tree_hint(const char *id) {
    const char *name = (id && *id) ? id : "myapp";
    printf("Suggested AppDir layout:\n");
    printf("%s.AppDir/\n", name);
    printf("|-- AppRun                         launcher script\n");
    printf("|-- %s.desktop                 desktop launcher metadata\n", name);
    printf("`-- usr/\n");
    printf("    |-- bin/%s                  main executable\n", name);
    printf("    |-- lib/                       bundled libraries, when needed\n");
    printf("    `-- share/\n");
    printf("        |-- applications/%s.desktop\n", name);
    printf("        |-- icons/hicolor/256x256/apps/%s.[png/svg/xpm]\n", name);
    printf("        |-- metainfo/%s.appdata.xml\n", name);
    printf("        `-- %s/                    assets/data folder\n", name);
}

static void xml_escape_small(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    if (!in) in = "";
    for (size_t i = 0; in[i] && j + 8 < outsz; i++) {
        char c = in[i];
        const char *rep = NULL;
        if (c == '&') rep = "&amp;";
        else if (c == '<') rep = "&lt;";
        else if (c == '>') rep = "&gt;";
        else if (c == '"') rep = "&quot;";
        else if (c == '\'') rep = "&apos;";
        if (rep) { size_t n = strlen(rep); memcpy(out + j, rep, n); j += n; }
        else out[j++] = c;
    }
    out[j] = 0;
}

static int create_appdir_guided_values(const char *appdir, const char *appname, const char *appid_in, const char *execpath, const char *iconpath, const char *assetspath, const char *libspath, const char *category, const char *version, bool terminal) {
    if (!appdir || !*appdir || !appname || !*appname) {
        printf("AppDir path and app name are required.\n");
        return 1;
    }
    if (path_exists(appdir)) {
        if (is_dir_path(appdir) && dir_is_empty(appdir)) {
            printf("Output folder already exists but is empty; using it.\n");
        } else {
            printf("Output AppDir already exists and is not empty: %s\n", appdir);
            return 1;
        }
    }
    char id[256];
    if (appid_in && *appid_in) sanitize_id(appid_in, id, sizeof(id));
    else sanitize_id(appname, id, sizeof(id));
    const char *cat = (category && *category) ? category : "Utility";
    const char *ver = (version && *version) ? version : "0.1.0";

    char usrbin[PATH_MAX], usrlib[PATH_MAX], usrshare[PATH_MAX], usrshareapps[PATH_MAX], usricons[PATH_MAX], metainfo[PATH_MAX], docdir[PATH_MAX];
    join_path(usrbin, sizeof(usrbin), appdir, "usr/bin");
    join_path(usrlib, sizeof(usrlib), appdir, "usr/lib");
    join_path(usrshare, sizeof(usrshare), appdir, "usr/share");
    join_path(usrshareapps, sizeof(usrshareapps), appdir, "usr/share/applications");
    join_path(usricons, sizeof(usricons), appdir, "usr/share/icons/hicolor/256x256/apps");
    join_path(metainfo, sizeof(metainfo), appdir, "usr/share/metainfo");
    char docbase[PATH_MAX]; join_path(docbase, sizeof(docbase), appdir, "usr/share/doc"); join_path(docdir, sizeof(docdir), docbase, id);
    if (mkdir_p(usrbin, 0755) != 0 || mkdir_p(usrlib, 0755) != 0 || mkdir_p(usrshareapps, 0755) != 0 || mkdir_p(usricons, 0755) != 0 || mkdir_p(metainfo, 0755) != 0 || mkdir_p(docdir, 0755) != 0) {
        printf("Could not create AppDir folders: %s\n", strerror(errno));
        return 1;
    }

    char dstexe[PATH_MAX]; join_path(dstexe, sizeof(dstexe), usrbin, id);
    if (execpath && *execpath) {
        if (!is_file_path(execpath)) { printf("Executable path is not a file: %s\n", execpath); return 1; }
        if (copy_file_simple(execpath, dstexe, 0755) != 0) { printf("Could not copy executable: %s\n", strerror(errno)); return 1; }
    } else {
        char placeholder[1024];
        snprintf(placeholder, sizeof(placeholder), "#!/bin/sh\necho 'Placeholder for %s. Replace usr/bin/%s with your real executable.'\nread -r dummy 2>/dev/null || true\n", appname, id);
        if (generate_text_file(dstexe, placeholder, 0755) != 0) return 1;
    }

    const char *iconext = icon_ext_from_path(iconpath);
    char iconname[512]; snprintf(iconname, sizeof(iconname), "%s.%s", id, iconext);
    char dsticon[PATH_MAX]; join_path(dsticon, sizeof(dsticon), usricons, iconname);
    char rooticon[PATH_MAX]; snprintf(rooticon, sizeof(rooticon), "%s/%s.%s", appdir, id, iconext);
    if (iconpath && *iconpath) {
        if (!is_file_path(iconpath)) {
            printf("Warning: icon path is not a file; generating placeholder icon instead.\n");
            snprintf(iconname, sizeof(iconname), "%s.xpm", id);
            join_path(dsticon, sizeof(dsticon), usricons, iconname);
            snprintf(rooticon, sizeof(rooticon), "%s/%s.xpm", appdir, id);
            write_placeholder_icon_xpm(dsticon);
            write_placeholder_icon_xpm(rooticon);
        } else {
            copy_file_simple(iconpath, dsticon, 0644);
            copy_file_simple(iconpath, rooticon, 0644);
        }
    } else {
        snprintf(iconname, sizeof(iconname), "%s.xpm", id);
        join_path(dsticon, sizeof(dsticon), usricons, iconname);
        snprintf(rooticon, sizeof(rooticon), "%s/%s.xpm", appdir, id);
        write_placeholder_icon_xpm(dsticon);
        write_placeholder_icon_xpm(rooticon);
        printf("No icon supplied; generated placeholder red square/white plus icon.\n");
    }
    if (assetspath && *assetspath) {
        if (!is_dir_path(assetspath)) printf("Warning: assets path is not a folder; skipping assets.\n");
        else { char assetdst[PATH_MAX]; join_path(assetdst, sizeof(assetdst), usrshare, id); if (copy_tree_simple(assetspath, assetdst) != 0) printf("Warning: could not copy all assets.\n"); }
    }
    if (libspath && *libspath) {
        if (!is_dir_path(libspath)) printf("Warning: library path is not a folder; skipping libraries.\n");
        else if (copy_tree_simple(libspath, usrlib) != 0) printf("Warning: could not copy all libraries.\n");
    }

    char desktop_root[PATH_MAX], desktop_share[PATH_MAX], appdata[PATH_MAX], readme[PATH_MAX], apprun[PATH_MAX];
    char desktop_file[512]; snprintf(desktop_file, sizeof(desktop_file), "%s.desktop", id);
    join_path(desktop_root, sizeof(desktop_root), appdir, desktop_file);
    join_path(desktop_share, sizeof(desktop_share), usrshareapps, desktop_file);
    char appdata_name[512]; snprintf(appdata_name, sizeof(appdata_name), "%s.appdata.xml", id); join_path(appdata, sizeof(appdata), metainfo, appdata_name);
    join_path(readme, sizeof(readme), docdir, "README-AppDir.txt");
    join_path(apprun, sizeof(apprun), appdir, "AppRun");

    char desktop_text[4096];
    snprintf(desktop_text, sizeof(desktop_text),
        "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\nIcon=%s\nCategories=%s;\nTerminal=%s\n",
        appname, id, id, cat, terminal ? "true" : "false");
    generate_text_file(desktop_root, desktop_text, 0644);
    copy_file_simple(desktop_root, desktop_share, 0644);

    char name_xml[1024], id_xml[512], cat_xml[512];
    xml_escape_small(appname, name_xml, sizeof(name_xml));
    xml_escape_small(id, id_xml, sizeof(id_xml));
    xml_escape_small(cat, cat_xml, sizeof(cat_xml));
    char datebuf[32] = "1970-01-01";
    time_t now = time(NULL); struct tm *tm = localtime(&now); if (tm) strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", tm);
    char appdata_text[8192];
    snprintf(appdata_text, sizeof(appdata_text),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<component type=\"desktop-application\">\n"
        "  <id>%s</id>\n"
        "  <name>%s</name>\n"
        "  <summary>%s packaged as an AppImage</summary>\n"
        "  <metadata_license>CC0-1.0</metadata_license>\n"
        "  <project_license>NOASSERTION</project_license>\n"
        "  <launchable type=\"desktop-id\">%s.desktop</launchable>\n"
        "  <categories><category>%s</category></categories>\n"
        "  <releases><release version=\"%s\" date=\"%s\"/></releases>\n"
        "</component>\n",
        id_xml, name_xml, name_xml, id_xml, cat_xml, ver, datebuf);
    generate_text_file(appdata, appdata_text, 0644);

    char apprun_text[4096];
    snprintf(apprun_text, sizeof(apprun_text),
        "#!/bin/sh\n"
        "HERE=\"$(dirname \"$(readlink -f \"$0\")\")\"\n"
        "export APPDIR=\"$HERE\"\n"
        "export PATH=\"$HERE/usr/bin:$PATH\"\n"
        "export LD_LIBRARY_PATH=\"$HERE/usr/lib:$HERE/usr/lib64:${LD_LIBRARY_PATH}\"\n"
        "export XDG_DATA_DIRS=\"$HERE/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}\"\n"
        "export GSETTINGS_SCHEMA_DIR=\"$HERE/usr/share/glib-2.0/schemas\"\n"
        "export QT_PLUGIN_PATH=\"$HERE/usr/plugins:$HERE/usr/lib/qt/plugins:$HERE/usr/lib/qt6/plugins:$HERE/usr/lib/qt5/plugins:${QT_PLUGIN_PATH}\"\n"
        "exec \"$HERE/usr/bin/%s\" \"$@\"\n", id);
    generate_text_file(apprun, apprun_text, 0755);

    char readme_text[4096];
    snprintf(readme_text, sizeof(readme_text),
        "AppDir generated by AppImage Nurse.\n\n"
        "Main executable: usr/bin/%s\n"
        "Desktop file: %s and usr/share/applications/%s\n"
        "Icon path: usr/share/icons/hicolor/256x256/apps/%s.[png/svg/xpm]\n"
        "Assets path: usr/share/%s/\n"
        "Libraries path: usr/lib/\n\n"
        "Next steps:\n"
        "1. Run Diagnose on this AppDir.\n"
        "2. Run Bundle/Patch if libraries or framework plugins are missing.\n"
        "3. Repack with appimagetool.\n", id, desktop_file, desktop_file, id, id);
    generate_text_file(readme, readme_text, 0644);

    printf("Created guided AppDir: %s\n", appdir);
    print_appdir_tree_hint(id);
    printf("Next: run Diagnose, then Bundle/Patch if needed, then Repack.\n");
    return 0;
}

static int guided_create_interactive(void) {
    char appdir[PATH_MAX], appname[512], appid[256], exe[PATH_MAX], icon[PATH_MAX], assets[PATH_MAX], libs[PATH_MAX], cat[256], ver[128], termbuf[32];
    memset(appdir,0,sizeof(appdir)); memset(appname,0,sizeof(appname)); memset(appid,0,sizeof(appid)); memset(exe,0,sizeof(exe)); memset(icon,0,sizeof(icon)); memset(assets,0,sizeof(assets)); memset(libs,0,sizeof(libs)); memset(cat,0,sizeof(cat)); memset(ver,0,sizeof(ver)); memset(termbuf,0,sizeof(termbuf));
    printf("Guided AppDir Builder\n");
    printf("This builds the folder layout and generates AppRun, desktop metadata, icon placement, and basic AppStream metadata.\n\n");
    print_appdir_tree_hint("myapp");
    printf("\n");
    if (prompt_output_appdir_path(appdir, sizeof(appdir)) != 0) return 1;
    if (prompt_line("App name", appname, sizeof(appname)) != 0) return 1;
    printf("App ID / executable name (blank to derive from app name): "); fflush(stdout); fgets(appid, sizeof(appid), stdin); memmove(appid, trim(appid), strlen(trim(appid)) + 1);
    printf("Main executable path (blank for placeholder): "); fflush(stdout); fgets(exe, sizeof(exe), stdin); memmove(exe, trim(exe), strlen(trim(exe)) + 1);
    printf("Icon path, png preferred (blank to skip): "); fflush(stdout); fgets(icon, sizeof(icon), stdin); memmove(icon, trim(icon), strlen(trim(icon)) + 1);
    printf("Assets folder (blank to skip): "); fflush(stdout); fgets(assets, sizeof(assets), stdin); memmove(assets, trim(assets), strlen(trim(assets)) + 1);
    printf("Libraries folder to copy into usr/lib (blank to skip): "); fflush(stdout); fgets(libs, sizeof(libs), stdin); memmove(libs, trim(libs), strlen(trim(libs)) + 1);
    printf("Desktop category (blank for Utility): "); fflush(stdout); fgets(cat, sizeof(cat), stdin); memmove(cat, trim(cat), strlen(trim(cat)) + 1);
    printf("App version for metadata (blank for 0.1.0): "); fflush(stdout); fgets(ver, sizeof(ver), stdin); memmove(ver, trim(ver), strlen(trim(ver)) + 1);
    printf("Terminal app? y/N: "); fflush(stdout); fgets(termbuf, sizeof(termbuf), stdin); memmove(termbuf, trim(termbuf), strlen(trim(termbuf)) + 1);
    bool terminal = (termbuf[0] == 'y' || termbuf[0] == 'Y');
    return create_appdir_guided_values(appdir, appname, appid, exe, icon, assets, libs, cat, ver, terminal);
}

static int repair_appdir(const char *appdir) {
    if (!is_dir_path(appdir)) {
        printf("AppDir not found: %s\n", appdir);
        return 1;
    }
    session_log("Safe repair: %s", appdir);
    int changes = 0;
    char apprun[PATH_MAX]; join_path(apprun, sizeof(apprun), appdir, "AppRun");
    if (path_exists(apprun)) {
        struct stat st;
        if (stat(apprun, &st) == 0 && !(st.st_mode & S_IXUSR)) {
            chmod(apprun, st.st_mode | S_IXUSR);
            printf("Fixed: made AppRun executable.\n"); changes++;
        }
    } else {
        WalkCtx ctx; walk_init(&ctx, appdir);
        if (ctx.desktop_files.len > 0) {
            DesktopInfo di;
            if (parse_desktop_file(ctx.desktop_files.items[0], &di) && di.exec[0]) {
                char tok[1024]; first_exec_token(di.exec, tok, sizeof(tok));
                char script[4096];
                snprintf(script, sizeof(script),
                    "#!/bin/sh\nHERE=\"$(dirname \"$(readlink -f \"$0\")\")\"\nexport PATH=\"$HERE/usr/bin:$PATH\"\nexport LD_LIBRARY_PATH=\"$HERE/usr/lib:$HERE/usr/lib64:${LD_LIBRARY_PATH}\"\nexec \"$HERE/usr/bin/%s\" \"$@\"\n", tok);
                if (tok[0] && generate_text_file(apprun, script, 0755) == 0) {
                    printf("Fixed: generated basic AppRun from desktop Exec=.\n"); changes++;
                }
            }
        }
        walk_free(&ctx);
    }
    WalkCtx ctx; walk_init(&ctx, appdir);
    for (size_t i = 0; i < ctx.desktop_files.len; i++) {
        DesktopInfo di;
        if (parse_desktop_file(ctx.desktop_files.items[i], &di) && di.exec[0]) {
            char tok[1024], found[PATH_MAX]; first_exec_token(di.exec, tok, sizeof(tok));
            if (resolve_exec_in_appdir(appdir, tok, found, sizeof(found))) {
                struct stat st;
                if (stat(found, &st) == 0 && S_ISREG(st.st_mode) && !(st.st_mode & S_IXUSR)) {
                    chmod(found, st.st_mode | S_IXUSR);
                    printf("Fixed: made Exec target executable: %s\n", found); changes++;
                }
            }
        }
    }
    walk_free(&ctx);
    if (!changes) printf("No simple automatic repairs were needed or possible. Run Diagnose for details.\n");
    else printf("Repair pass complete: %d change(s). Run Diagnose again.\n", changes);
    return 0;
}

static int test_launch(const char *target, const char *logfile) {
    char exe[PATH_MAX];
    struct stat st;
    session_log("Test launch: %s", target);
    if (stat(target, &st) != 0) { printf("Cannot access target: %s\n", strerror(errno)); return 1; }
    if (S_ISDIR(st.st_mode)) join_path(exe, sizeof(exe), target, "AppRun");
    else snprintf(exe, sizeof(exe), "%s", target);
    if (!is_exec_path(exe)) {
        printf("Launch target is not executable: %s\n", exe);
        return 1;
    }
    char *args[] = { exe, NULL };
    printf("Launching: %s\n", exe);
    if (logfile && *logfile) printf("Log file: %s\n", logfile);
    int code = run_child_wait(NULL, logfile, args);
    if (logfile && *logfile) {
        printf("Launch exited with code %d. Log: %s\n", code, logfile);
        if (code != 0) { printf("\nKnown error explanations from log:\n"); explain_error_file(logfile); }
    } else {
        printf("Launch exited with code %d.\n", code);
    }
    return code;
}

static void reset_counts(void) { g_counts.ok = 0; g_counts.warn = 0; g_counts.fail = 0; }

static void run_diagnose_target(const char *target) {
    reset_counts();
    struct stat st;
    if (stat(target, &st) != 0) {
        status_line("FAIL", "Cannot access target '%s': %s", target, strerror(errno));
        return;
    }
    if (S_ISDIR(st.st_mode)) diagnose_appdir(target);
    else if (S_ISREG(st.st_mode)) diagnose_appimage_file(target);
    else status_line("FAIL", "Target is neither a regular file nor a directory.");
    summary_line();
}

static int diagnose_to_report(const char *target, const char *report_path) {
    if (!report_path || !*report_path || strcmp(report_path, "-") == 0) {
        run_diagnose_target(target);
        return g_counts.fail ? 1 : 0;
    }
    g_report = fopen(report_path, "w");
    if (!g_report) { printf("Could not write report: %s\n", strerror(errno)); return 1; }
    run_diagnose_target(target);
    fclose(g_report); g_report = NULL;
    printf("\nSaved report: %s\n", report_path);
    return g_counts.fail ? 1 : 0;
}

static void press_enter(void) {
    printf("\nPress Enter to return to tabs...");
    fflush(stdout);
    char tmp[16]; fgets(tmp, sizeof(tmp), stdin);
}

static void clear_screen(void) {
    printf("\033[2J\033[H");
    fflush(stdout);
}

static void press_enter_message(const char *message) {
    printf("\n%s", message && *message ? message : "Press Enter to continue...");
    fflush(stdout);
    char tmp[32]; fgets(tmp, sizeof(tmp), stdin);
}

static void ram_pause(void) {
    press_enter_message("Press Enter to return to RAM menu...");
}


static bool is_back_input(const char *input) {
    if (!input) return false;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s", input);
    char *t = trim(tmp);
    for (char *q = t; *q; q++) *q = (char)tolower((unsigned char)*q);
    return strcmp(t, "0") == 0 || strcmp(t, "b") == 0 || strcmp(t, "back") == 0;
}

static int read_submenu_choice(const char *prompt) {
    char buf[64];
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, sizeof(buf), stdin)) return -1;
    if (is_back_input(buf)) return 0;
    return atoi(buf);
}

static int library_map(const char *appdir);
static int compatibility_score(const char *appdir);
static int repair_plan(const char *appdir, const char *outfile, bool apply);
static int compare_targets(const char *a, const char *b);
static void explain_errors_interactive(void);

static void ram_bundle_menu(const char *appdir) {
    for (;;) {
        clear_screen();
        printf("RAM Bundle/Patch tools for: %s\n", appdir);
        printf("1. Smart dependency + framework bundle\n");
        printf("2. Conservative dependency bundle\n");
        printf("3. Partial framework bundle/check\n");
        printf("4. Patch RUNPATH with patchelf\n");
        printf("5. Run linuxdeploy\n");
        printf("0. Back to RAM workspace\n");
        int c = read_submenu_choice("Choose: ");
        if (c < 0) return;
        bool did_action = true;
        if (c == 1) smart_bundle_dependencies(appdir);
        else if (c == 2) bundle_dependencies(appdir);
        else if (c == 3) partial_framework_bundle(appdir);
        else if (c == 4) patch_rpath_appdir(appdir);
        else if (c == 5) {
            char out[PATH_MAX];
            printf("Preferred output name, optional: "); fflush(stdout);
            fgets(out, sizeof(out), stdin); memmove(out, trim(out), strlen(trim(out)) + 1);
            linuxdeploy_bundle(appdir, out);
        } else if (c == 0) return;
        else { printf("Unknown option.\n"); }
        if (did_action) ram_pause();
    }
}

static void ram_repair_menu(const char *appdir) {
    for (;;) {
        clear_screen();
        printf("RAM Repair/Plan tools for: %s\n", appdir);
        printf("1. Apply simple safe repair\n");
        printf("2. Show repair plan\n");
        printf("3. Save repair plan to file\n");
        printf("4. Apply repair plan sequence\n");
        printf("5. Backup RAM AppDir beside workspace\n");
        printf("0. Back to RAM workspace\n");
        char out[PATH_MAX];
        int c = read_submenu_choice("Choose: ");
        if (c < 0) return;
        if (c == 1) repair_appdir(appdir);
        else if (c == 2) repair_plan(appdir, "-", false);
        else if (c == 3) { if (prompt_line("Output plan text file", out, sizeof(out)) == 0) repair_plan(appdir, out, false); else printf("Cancelled.\n"); }
        else if (c == 4) repair_plan(appdir, "-", true);
        else if (c == 5) backup_appdir(appdir);
        else if (c == 0) return;
        else printf("Unknown option.\n");
        ram_pause();
    }
}

static void ram_checks_menu(const char *appdir) {
    for (;;) {
        clear_screen();
        printf("RAM Checks/Analysis tools for: %s\n", appdir);
        printf("1. Compatibility score\n");
        printf("2. Library map\n");
        printf("3. AppStream metadata validation\n");
        printf("4. Cross-distro smoke test\n");
        printf("5. Compare RAM AppDir with another AppDir\n");
        printf("6. Explain error/log text\n");
        printf("0. Back to RAM workspace\n");
        char a[PATH_MAX], b[PATH_MAX];
        int c = read_submenu_choice("Choose: ");
        if (c < 0) return;
        if (c == 1) compatibility_score(appdir);
        else if (c == 2) library_map(appdir);
        else if (c == 3) appstream_validate(appdir);
        else if (c == 4) {
            printf("Container image (blank for debian:bullseye-slim): "); fflush(stdout);
            fgets(a, sizeof(a), stdin); memmove(a, trim(a), strlen(trim(a)) + 1);
            printf("Log file path (blank for screen output): "); fflush(stdout);
            fgets(b, sizeof(b), stdin); memmove(b, trim(b), strlen(trim(b)) + 1);
            cross_distro_test(appdir, *a ? a : "debian:bullseye-slim", b);
        } else if (c == 5) {
            if (prompt_line("Other AppDir path", a, sizeof(a)) == 0) compare_targets(appdir, a);
            else printf("Cancelled.\n");
        } else if (c == 6) {
            explain_errors_interactive();
        } else if (c == 0) return;
        else printf("Unknown option.\n");
        ram_pause();
    }
}


static void ram_first_aid_menu(const char *appdir) {
    for (;;) {
        clear_screen();
        printf("RAM First Aid for: %s\n", appdir);
        printf("This uses the already-extracted RAM AppDir. It will not ask you to extract again.\n\n");
        printf("1. It will not launch\n");
        printf("2. Inspect this AppDir\n");
        printf("3. Repair simple issues\n");
        printf("4. Improve compatibility\n");
        printf("5. Repack this RAM AppDir to disk\n");
        printf("6. Print or save a report\n");
        printf("0. Back to RAM workspace\n");
        char buf[64], out[PATH_MAX];
        int c = read_submenu_choice("Choose: ");
        if (c < 0) return;
        if (c == 1) {
            run_diagnose_target(appdir);
            printf("\nTest launch now? y/N: "); fflush(stdout);
            if (fgets(buf, sizeof(buf), stdin) && (buf[0] == 'y' || buf[0] == 'Y')) {
                printf("Log file path (blank for screen output): "); fflush(stdout);
                fgets(out, sizeof(out), stdin); memmove(out, trim(out), strlen(trim(out)) + 1);
                test_launch(appdir, out);
            }
        } else if (c == 2) {
            run_diagnose_target(appdir);
            printf("\nShow library map? y/N: "); fflush(stdout);
            if (fgets(buf, sizeof(buf), stdin) && (buf[0] == 'y' || buf[0] == 'Y')) library_map(appdir);
            printf("\nShow compatibility score? y/N: "); fflush(stdout);
            if (fgets(buf, sizeof(buf), stdin) && (buf[0] == 'y' || buf[0] == 'Y')) compatibility_score(appdir);
        } else if (c == 3) {
            repair_plan(appdir, "-", false);
            printf("\nApply safe repair now? y/N: "); fflush(stdout);
            if (fgets(buf, sizeof(buf), stdin) && (buf[0] == 'y' || buf[0] == 'Y')) repair_appdir(appdir);
        } else if (c == 4) {
            compatibility_score(appdir);
            printf("\nRun smart bundle/framework check/RPATH patch? y/N: "); fflush(stdout);
            if (fgets(buf, sizeof(buf), stdin) && (buf[0] == 'y' || buf[0] == 'Y')) {
                smart_bundle_dependencies(appdir);
                patch_rpath_appdir(appdir);
                compatibility_score(appdir);
            }
        } else if (c == 5) {
            if (prompt_output_appimage_path(out, sizeof(out)) == 0) appimage_repack(appdir, out);
            else printf("Cancelled.\n");
        } else if (c == 6) {
            printf("Report output path (blank to print only): "); fflush(stdout);
            fgets(out, sizeof(out), stdin); memmove(out, trim(out), strlen(trim(out)) + 1);
            diagnose_to_report(appdir, *out ? out : "-");
        } else if (c == 0) {
            return;
        } else {
            printf("Unknown RAM First Aid option.\n");
        }
        ram_pause();
    }
}

static int ram_workspace_session(const char *appimage) {
    const char *base = is_dir_path("/dev/shm") ? "/dev/shm" : "/tmp";
    char root[PATH_MAX];
    if (make_unique_dir(base, "appimage-nurse-ram-", root, sizeof(root)) != 0) {
        printf("Could not create RAM/temp workspace in %s: %s\n", base, strerror(errno));
        return 1;
    }
    char appdir[PATH_MAX]; join_path(appdir, sizeof(appdir), root, "AppDir");
    printf("RAM workspace: %s\n", root);
    if (strcmp(base, "/dev/shm") == 0) printf("Using /dev/shm, so this workspace is RAM-backed and will disappear after reboot.\n");
    else printf("/dev/shm was not available; using /tmp instead.\n");
    int rc = appimage_extract_internal(appimage, appdir, root);
    if (rc != 0) {
        printf("Extraction failed. Cleaning workspace: %s\n", root);
        rm_rf(root);
        return rc;
    }
    press_enter_message("Press Enter to open the RAM workspace menu...");
    for (;;) {
        clear_screen();
        printf("RAM workspace AppDir: %s\n", appdir);
        printf("0. First Aid for this RAM AppDir\n");
        printf("1. Diagnose\n");
        printf("2. Test launch\n");
        printf("3. Repair/Plan tools\n");
        printf("4. Bundle/Patch tools\n");
        printf("5. Checks/Analysis tools\n");
        printf("6. Report\n");
        printf("7. Repack to disk\n");
        printf("8. Save AppDir to disk\n");
        printf("9. Keep RAM workspace and return\n");
        printf("10. Clean and return\n");
        printf("Choose: "); fflush(stdout);
        char buf[64]; if (!fgets(buf, sizeof(buf), stdin)) break;
        int c = atoi(buf);
        if (c == 0 && trim(buf)[0] == '0') {
            ram_first_aid_menu(appdir);
        } else if (c == 1) {
            run_diagnose_target(appdir);
            ram_pause();
        } else if (c == 2) {
            char log[PATH_MAX]; printf("Log file path (blank for screen output): "); fflush(stdout); fgets(log, sizeof(log), stdin); memmove(log, trim(log), strlen(trim(log)) + 1);
            test_launch(appdir, log);
            ram_pause();
        } else if (c == 3) {
            ram_repair_menu(appdir);
        } else if (c == 4) {
            ram_bundle_menu(appdir);
        } else if (c == 5) {
            ram_checks_menu(appdir);
        } else if (c == 6) {
            char report[PATH_MAX];
            printf("Report output path (blank to print only): "); fflush(stdout);
            fgets(report, sizeof(report), stdin); memmove(report, trim(report), strlen(trim(report)) + 1);
            diagnose_to_report(appdir, *report ? report : "-");
            ram_pause();
        } else if (c == 7) {
            char out[PATH_MAX]; if (prompt_output_appimage_path(out, sizeof(out)) == 0) appimage_repack(appdir, out); else printf("Cancelled.\n");
            ram_pause();
        } else if (c == 8) {
            char out[PATH_MAX]; if (prompt_output_appdir_path(out, sizeof(out)) == 0) {
                if (path_exists(out)) printf("Output path already exists: %s\n", out);
                else if (copy_tree_simple(appdir, out) == 0) printf("Saved AppDir to: %s\n", out);
                else printf("Could not save AppDir: %s\n", strerror(errno));
            } else printf("Cancelled.\n");
            ram_pause();
        } else if (c == 9) {
            printf("Keeping workspace for now: %s\n", root);
            printf("It should disappear after reboot if it is in /dev/shm.\n");
            return 0;
        } else if (c == 10) {
            printf("Cleaning workspace: %s\n", root);
            rm_rf(root);
            return 0;
        } else {
            printf("Unknown option.\n");
            ram_pause();
        }
    }
    printf("Cleaning workspace: %s\n", root);
    rm_rf(root);
    return 0;
}

static const char *rel_from_root(const char *root, const char *path) {
    size_t n = strlen(root);
    if (strncmp(root, path, n) == 0) {
        const char *r = path + n;
        if (*r == '/') r++;
        return r;
    }
    return path;
}

static long long regular_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (long long)st.st_size;
}

static int compare_appdirs(const char *old_dir, const char *new_dir) {
    if (!is_dir_path(old_dir) || !is_dir_path(new_dir)) {
        printf("Both targets must be AppDir folders for deep folder comparison.\n");
        return 1;
    }
    WalkCtx a, b; walk_init(&a, old_dir); walk_init(&b, new_dir);
    int added = 0, removed = 0, changed = 0, same = 0;
    printf("Before/after AppDir comparison\n");
    printf("Old: %s\nNew: %s\n\n", old_dir, new_dir);

    for (size_t i = 0; i < a.all_files.len && i < 2000; i++) {
        const char *rel = rel_from_root(old_dir, a.all_files.items[i]);
        char other[PATH_MAX]; join_path(other, sizeof(other), new_dir, rel);
        if (!path_exists(other)) {
            printf("[%sREMOVED%s] %s\n", status_color("WARN"), color_reset(), rel);
            removed++;
            continue;
        }
        long long sa = regular_file_size(a.all_files.items[i]);
        long long sb = regular_file_size(other);
        if (sa >= 0 && sb >= 0 && sa != sb) {
            printf("[%sCHANGED%s] %s (%lld -> %lld bytes)\n", status_color("WARN"), color_reset(), rel, sa, sb);
            changed++;
        } else same++;
    }
    for (size_t i = 0; i < b.all_files.len && i < 2000; i++) {
        const char *rel = rel_from_root(new_dir, b.all_files.items[i]);
        char other[PATH_MAX]; join_path(other, sizeof(other), old_dir, rel);
        if (!path_exists(other)) {
            printf("[%sADDED%s] %s\n", status_color("OK"), color_reset(), rel);
            added++;
        }
    }
    printf("\nComparison summary: %d added, %d removed, %d changed-size, %d unchanged/unchecked.\n", added, removed, changed, same);
    walk_free(&a); walk_free(&b);
    return (removed || changed) ? 1 : 0;
}

static int compare_targets(const char *a, const char *b) {
    if (is_dir_path(a) && is_dir_path(b)) return compare_appdirs(a, b);
    if (is_file_path(a) && is_file_path(b)) {
        long long sa = regular_file_size(a), sb = regular_file_size(b);
        printf("AppImage/file comparison\n");
        printf("A: %s\nB: %s\n", a, b);
        printf("Size: %lld -> %lld bytes (%+lld)\n", sa, sb, sb - sa);
        printf("For deeper comparison, extract both AppImages and compare the AppDirs.\n");
        return sa == sb ? 0 : 1;
    }
    printf("Targets are different types or do not exist. For deep comparison, compare two AppDirs.\n");
    return 1;
}

static int library_map(const char *appdir) {
    if (!is_dir_path(appdir)) { printf("AppDir not found: %s\n", appdir); return 1; }
    WalkCtx ctx; walk_init(&ctx, appdir);
    printf("AppImage library map\n");
    printf("Legend: bundled = found inside AppDir, host = found on this system, missing = not found, skip = should usually stay host-provided.\n\n");
    for (size_t i = 0; i < ctx.elf_files.len && i < 200; i++) {
        const char *rel = rel_from_root(appdir, ctx.elf_files.items[i]);
        ElfInfo ei; parse_elf_needed(ctx.elf_files.items[i], &ei);
        printf("%s\n", rel);
        if (!ei.parsed || ei.needed.len == 0) {
            printf("  (no dynamic library list found)\n");
            list_free(&ei.needed);
            continue;
        }
        for (size_t j = 0; j < ei.needed.len; j++) {
            const char *need = ei.needed.items[j];
            if (list_contains(&ctx.provided_names, need)) printf("  |- [%s] %s\n", "bundled", need);
            else if (should_never_bundle(need)) printf("  |- [%s] %s\n", "skip", need);
            else {
                char found[PATH_MAX];
                if (locate_library_smart(need, found, sizeof(found))) printf("  |- [%s] %s -> %s\n", "host", need, found);
                else printf("  |- [%s] %s\n", "missing", need);
            }
        }
        list_free(&ei.needed);
    }
    walk_free(&ctx);
    return 0;
}

static int compatibility_score(const char *appdir) {
    if (!is_dir_path(appdir)) { printf("AppDir not found: %s\n", appdir); return 1; }
    int score = 100;
    WalkCtx ctx; walk_init(&ctx, appdir);
    StrList missing; list_init(&missing); collect_missing_deps(&ctx, &missing);
    VersionMax glibc = {0}, glibcxx = {0}, cxxabi = {0};
    for (size_t i = 0; i < ctx.elf_files.len && i < MAX_ELF_SCAN; i++) scan_versions_in_file(ctx.elf_files.items[i], &glibc, &glibcxx, &cxxabi);

    char apprun[PATH_MAX]; join_path(apprun, sizeof(apprun), appdir, "AppRun");
    if (!is_exec_path(apprun)) score -= 25;
    if (ctx.desktop_files.len == 0) score -= 10;
    if (ctx.broken_links.len) score -= (int)(ctx.broken_links.len > 5 ? 20 : ctx.broken_links.len * 4);
    if (missing.len) score -= (int)(missing.len > 10 ? 35 : missing.len * 4);
    if (glibc.seen && (glibc.major > 2 || (glibc.major == 2 && glibc.minor >= 36))) score -= 20;
    else if (glibc.seen && glibc.major == 2 && glibc.minor >= 32) score -= 10;
    if (score < 0) score = 0;

    const char *label = score >= 80 ? "Good" : (score >= 55 ? "Medium" : "Risky");
    const char *col = score >= 80 ? C_OK : (score >= 55 ? C_WARN : C_FAIL);
    printf("Compatibility estimate: "); color_printf(col, "%s (%d/100)", label, score); printf("\n");
    printf("This is a rough estimate, not a guarantee.\n\n");
    if (!is_exec_path(apprun)) printf("- AppRun is missing or not executable.\n");
    if (ctx.desktop_files.len == 0) printf("- No .desktop file found.\n");
    if (ctx.broken_links.len) printf("- Broken symlink(s): %zu\n", ctx.broken_links.len);
    if (missing.len) printf("- Missing non-core libraries: %zu\n", missing.len);
    if (glibc.seen) printf("- Highest GLIBC hint: %s\n", glibc.text);
    if (glibcxx.seen) printf("- Highest GLIBCXX hint: %s\n", glibcxx.text);
    printf("\nSuggested next step: %s\n", score >= 80 ? "test and repack" : (score >= 55 ? "run smart-bundle/repair, then diagnose again" : "consider compat-build/rebuild on an older base"));
    list_free(&missing); walk_free(&ctx);
    return score >= 55 ? 0 : 1;
}

static int repair_plan(const char *appdir, const char *outfile, bool apply) {
    if (!is_dir_path(appdir)) { printf("AppDir not found: %s\n", appdir); return 1; }
    FILE *out = NULL;
    if (outfile && *outfile && strcmp(outfile, "-") != 0) {
        out = fopen(outfile, "w");
        if (!out) { printf("Could not write repair plan: %s\n", strerror(errno)); return 1; }
    }
    FILE *old = g_report;
    g_report = out;
    printf("Repair plan for: %s\n", appdir);
    if (g_report) fprintf(g_report, "Repair plan for: %s\n", appdir);
    WalkCtx ctx; walk_init(&ctx, appdir);
    StrList missing; list_init(&missing); collect_missing_deps(&ctx, &missing);
    char apprun[PATH_MAX]; join_path(apprun, sizeof(apprun), appdir, "AppRun");
    int steps = 0;
#define PLAN_LINE(...) do { printf(__VA_ARGS__); printf("\n"); if (g_report) { fprintf(g_report, __VA_ARGS__); fprintf(g_report, "\n"); } } while(0)
    if (!path_exists(apprun)) { PLAN_LINE("1. Generate AppRun from desktop Exec= when possible."); steps++; }
    else if (!is_exec_path(apprun)) { PLAN_LINE("1. chmod +x AppRun"); steps++; }
    if (missing.len) { PLAN_LINE("%d. Try smart-bundle for %zu missing non-core libraries.", steps + 1, missing.len); steps++; }
    if (ctx.broken_links.len) { PLAN_LINE("%d. Review/remove/fix %zu broken symlink(s).", steps + 1, ctx.broken_links.len); steps++; }
    if (ctx.elf_files.len) { PLAN_LINE("%d. Patch RPATH/RUNPATH with patchelf, or use AppRun LD_LIBRARY_PATH wrapper.", steps + 1); steps++; }
    if (!steps) PLAN_LINE("No obvious safe repair steps found. Run Diagnose/Test for runtime errors.");
    PLAN_LINE("Final step: run Diagnose, Test, then Repack.");
#undef PLAN_LINE
    if (out) { fclose(out); printf("Saved repair plan: %s\n", outfile); }
    g_report = old;
    list_free(&missing); walk_free(&ctx);
    if (apply) {
        printf("\nApplying safe repair sequence: repair -> smart-bundle -> patch-rpath.\n");
        repair_appdir(appdir);
        smart_bundle_dependencies(appdir);
        patch_rpath_appdir(appdir);
    }
    return 0;
}

static int create_workspace(const char *folder, const char *source) {
    if (!folder || !*folder) return 1;
    if (mkdir_p(folder, 0755) != 0) { printf("Could not create workspace: %s\n", strerror(errno)); return 1; }
    const char *subs[] = {"source", "extracted", "reports", "logs", "recipes", "builds", NULL};
    for (int i = 0; subs[i]; i++) { char p[PATH_MAX]; join_path(p, sizeof(p), folder, subs[i]); mkdir_p(p, 0755); }
    char meta[PATH_MAX]; join_path(meta, sizeof(meta), folder, "workspace.nurse");
    char text[4096];
    snprintf(text, sizeof(text), "AppImage Nurse Workspace\nversion=%s\nsource=%s\n\nFolders:\nsource/    original AppImages or project files\nextracted/ extracted AppDirs\nreports/   diagnostic reports\nlogs/      launch/test logs\nrecipes/   generated recipes/templates\nbuilds/    repacked AppImages\n", VERSION, source && *source ? source : "");
    generate_text_file(meta, text, 0644);
    if (source && *source && path_exists(source)) {
        char dst[PATH_MAX]; join_path(dst, sizeof(dst), folder, "source"); join_path(dst, sizeof(dst), dst, base_name(source));
        if (is_file_path(source)) copy_file_simple(source, dst, 0644);
        else if (is_dir_path(source)) copy_tree_simple(source, dst);
    }
    printf("Created workspace: %s\n", folder);
    printf("Use it to keep source AppImages, extracted AppDirs, reports, logs, recipes, and repacked builds together.\n");
    return 0;
}

static void workspace_interactive(void) {
    char folder[PATH_MAX], source[PATH_MAX];
    printf("Workspace options:\n1. Create workspace\n2. Show workspace layout\n0. Back\n");
    int c = read_submenu_choice("Choose: ");
    if (c <= 0) return;
    if (c == 1) {
        if (prompt_line("Workspace folder", folder, sizeof(folder)) == 0) {
            printf("Source AppImage/project to copy in (blank to skip): "); fflush(stdout); fgets(source, sizeof(source), stdin); memmove(source, trim(source), strlen(trim(source)) + 1);
            create_workspace(folder, source);
        }
    } else if (c == 2) {
        printf("Workspace layout:\n  source/\n  extracted/\n  reports/\n  logs/\n  recipes/\n  builds/\n  workspace.nurse\n");
    } else printf("Unknown workspace option.\n");
}

static void tool_path_manager_interactive(void) {
    printf("Tool Path Manager\n");
    printf("Saved paths override PATH lookup. This is useful for downloaded AppImage tools that are not installed globally.\n\n");
    printf("1. Show resolved tools\n2. Show saved paths\n3. Set tool path\n0. Back\n");
    int c = read_submenu_choice("Choose: ");
    if (c <= 0) return;
    if (c == 1) check_tools();
    else if (c == 2) print_tool_overrides();
    else if (c == 3) {
        char name[128], path[PATH_MAX];
        printf("Tool name, e.g. appimagetool, linuxdeploy, patchelf: "); fflush(stdout); fgets(name, sizeof(name), stdin); memmove(name, trim(name), strlen(trim(name)) + 1);
        if (prompt_line("Executable path", path, sizeof(path)) == 0) save_tool_override(name, path);
    } else printf("Unknown tool option.\n");
}


static void session_menu_interactive(void) {
    printf("Session menu\n");
    printf("History is temporary and only lives during this run unless you save it.\n\n");
    printf("1. Show session history\n2. Save session history to file\n3. Clear session history\n0. Back\n");
    int c = read_submenu_choice("Choose: ");
    if (c <= 0) return;
    if (c == 1) show_session_history();
    else if (c == 2) { char out[PATH_MAX]; if (prompt_output_text_path(out, sizeof(out)) == 0) save_session_history(out); }
    else if (c == 3) clear_session_history();
    else printf("Unknown session option.\n");
}

static void library_policy_interactive(void) {
    printf("Library policy\n");
    printf("Use sparingly. Skip means never bundle. Allow means AppImage Nurse may try bundling a library normally treated as system/core.\n\n");
    printf("1. Show library policy\n2. Add library to skip list\n3. Add library to allow list\n0. Back\n");
    char lib[256];
    int c = read_submenu_choice("Choose: ");
    if (c <= 0) return;
    if (c == 1) policy_show();
    else if (c == 2) { if (prompt_line("Library name, e.g. libExample.so.1", lib, sizeof(lib)) == 0) policy_add_library("skip", lib); }
    else if (c == 3) { if (prompt_line("Library name, e.g. libExample.so.1", lib, sizeof(lib)) == 0) policy_add_library("allow", lib); }
    else printf("Unknown library policy option.\n");
}

static void config_interactive(void) {
    printf("Config\n");
    printf("Only practical settings are saved. History/workspace state is not silently logged.\n\n");
    printf("1. Show config\n2. Set color mode\n3. Set default output folder\n4. Set preferred temp mode\n0. Back\n");
    char val[PATH_MAX];
    int c = read_submenu_choice("Choose: ");
    if (c <= 0) return;
    if (c == 1) config_show();
    else if (c == 2) { printf("Color mode (auto/on/off): "); fflush(stdout); fgets(val, sizeof(val), stdin); memmove(val, trim(val), strlen(trim(val)) + 1); config_set_value("color", val); }
    else if (c == 3) { if (prompt_line("Default output folder", val, sizeof(val)) == 0) config_set_value("default_output_folder", val); }
    else if (c == 4) { printf("Preferred temp mode (ram/disk): "); fflush(stdout); fgets(val, sizeof(val), stdin); memmove(val, trim(val), strlen(trim(val)) + 1); config_set_value("preferred_temp_mode", val); }
    else printf("Unknown config option.\n");
}

static void explain_errors_interactive(void) {
    printf("Error explanations\n");
    printf("1. Explain a log file\n2. Paste/type one error line\n0. Back\n");
    char buf[4096];
    int c = read_submenu_choice("Choose: ");
    if (c <= 0) return;
    if (c == 1) { char path[PATH_MAX]; if (prompt_line("Log file path", path, sizeof(path)) == 0) explain_error_file(path); }
    else if (c == 2) { printf("Error text: "); fflush(stdout); if (fgets(buf, sizeof(buf), stdin)) explain_error_text(buf); }
    else printf("Unknown error explanation option.\n");
}

static void backup_restore_interactive(void) {
    printf("Backup/Undo\n");
    printf("Backups are only created when you choose them. They are stored beside the AppDir, not in a permanent history log.\n\n");
    printf("1. Create backup of AppDir\n2. Restore backup to AppDir\n0. Back\n");
    char a[PATH_MAX], b[PATH_MAX];
    int c = read_submenu_choice("Choose: ");
    if (c <= 0) return;
    if (c == 1) { if (prompt_line("AppDir path", a, sizeof(a)) == 0) backup_appdir(a); }
    else if (c == 2) { if (prompt_line("Backup folder", a, sizeof(a)) == 0 && prompt_line("Target AppDir path", b, sizeof(b)) == 0) restore_appdir_from_backup(a, b); }
    else printf("Unknown backup option.\n");
}

static void first_aid_presets(void) {
    char a[PATH_MAX], b[PATH_MAX], c[PATH_MAX];
    printf("First Aid Presets\n");
    printf("1. My AppImage will not launch\n");
    printf("2. Inspect an AppImage without leaving files behind\n");
    printf("3. Extract, repair simple issues, and repack\n");
    printf("4. Make a new AppImage folder\n");
    printf("5. Improve compatibility of an AppDir\n");
    printf("6. Compare two builds\n");
    printf("0. Back\n");
    char buf[64];
    int choice = read_submenu_choice("Choose: ");
    if (choice <= 0) return;
    if (choice == 1) {
        if (prompt_line("AppImage/AppDir path", a, sizeof(a)) == 0) {
            run_diagnose_target(a);
            printf("\nTest launch now? y/N: "); fflush(stdout); fgets(buf, sizeof(buf), stdin);
            if (buf[0] == 'y' || buf[0] == 'Y') test_launch(a, "");
        }
    } else if (choice == 2) {
        if (prompt_line("AppImage path", a, sizeof(a)) == 0) ram_workspace_session(a);
    } else if (choice == 3) {
        if (prompt_line("AppImage path", a, sizeof(a)) == 0 && prompt_output_appimage_path(b, sizeof(b)) == 0) {
            const char *base = is_dir_path("/dev/shm") ? "/dev/shm" : "/tmp";
            char root[PATH_MAX], appdir[PATH_MAX]; make_unique_dir(base, "appimage-nurse-flow-", root, sizeof(root)); join_path(appdir, sizeof(appdir), root, "AppDir");
            if (appimage_extract_internal(a, appdir, root) == 0) { repair_appdir(appdir); appimage_repack(appdir, b); }
            printf("Cleaning temporary workspace: %s\n", root); rm_rf(root);
        }
    } else if (choice == 4) guided_create_interactive();
    else if (choice == 5) {
        if (prompt_line("AppDir path", a, sizeof(a)) == 0) { compatibility_score(a); smart_bundle_dependencies(a); compatibility_score(a); }
    } else if (choice == 6) {
        if (prompt_line("Old AppImage/AppDir", a, sizeof(a)) == 0 && prompt_line("New AppImage/AppDir", b, sizeof(b)) == 0) compare_targets(a, b);
    } else printf("Unknown preset.\n");
}

static void print_tab_help(void) {
    if (g_color) printf(C_BOLD);
    printf("Tab Help\n");
    if (g_color) printf(C_RESET);
    printf("============================================================\n");
    printf("If a dependency is not mentioned for a tab, that tab does not need an extra backend tool.\n");
    printf("Some tested AppImages may still need their own runtime/system libraries.\n");
    printf("In submenus, choose 0 or type back to return before starting an action. Blank path prompts cancel that action.\n\n");
    printf("0. First Aid\n");
    printf("   Common guided workflows like will-not-launch, inspect without clutter, extract/repair/repack, make a new AppDir, improve compatibility, or compare builds. Dependencies depend on the chosen workflow.\n\n");
    printf("1. Diagnose\n");
    printf("   Checks an AppImage or extracted AppDir for common problems: AppRun, desktop file, icons, libraries, GLIBC hints, and broken symlinks.\n\n");
    printf("2. Bundle/Patch\n");
    printf("   Tries safe dependency copying, framework hints, RPATH patching, or linuxdeploy-assisted bundling. Dependencies: patchelf for RPATH, linuxdeploy for linuxdeploy mode.\n\n");
    printf("3. Extract\n");
    printf("   Extracts an AppImage into an editable AppDir folder, or opens a RAM workspace where First Aid, diagnose, repair, bundle, checks, report, test, save, and repack actions are available without disk clutter. The selected AppImage must support --appimage-extract.\n\n");
    printf("4. Repack\n");
    printf("   Turns an edited AppDir back into an AppImage. Dependency: appimagetool.\n\n");
    printf("5. Create\n");
    printf("   Quick Create makes a simple AppDir skeleton. Guided Builder shows the expected folder tree and generates AppRun, desktop metadata, icon placement, and basic AppStream metadata.\n\n");
    printf("6. Repair/Plan\n");
    printf("   Applies simple safe fixes or creates a repair plan you can save before changing anything. Dependencies: patchelf or backend tools only if you apply bundle/RPATH steps.\n\n");
    printf("7. Test\n");
    printf("   Launches an AppImage or AppDir and optionally saves a log. The tested app may need its own runtime/system dependencies.\n\n");
    printf("8. Recipes/Compat\n");
    printf("   Generates appimage-builder recipes, older-base container templates, runs an older-distro build pipeline, or creates a workspace. Dependencies: appimage-builder for recipes, Docker/Podman for compat builds, appimagetool for final packaging.\n\n");
    printf("9. Report\n");
    printf("   Shows the diagnosis in terminal or saves it to a text file for sharing.\n\n");
    printf("10. Checks\n");
    printf("   Runs release and analysis checks: container smoke tests, AppStream, update metadata, signatures, compatibility score, library map, and before/after comparisons. Dependencies vary: Docker/Podman, appstreamcli, gpg, or appimagetool depending on the option.\n\n");
    printf("11. Tools\n");
    printf("   Shows optional backend tools and lets you save tool paths when they are not installed globally.\n\n");
    printf("12. Session\n");
    printf("   Shows, saves, or clears the temporary in-memory session history. It is not kept after AppImage Nurse closes unless you save it.\n\n");
    printf("13. Help\n");
    printf("   Shows this quick guide for each tab.\n");
}

static void print_tabs(void) {
    printf("\033[2J\033[H");
    if (g_color) printf(C_BOLD);
    printf("AppImage Nurse %s\n", VERSION);
    if (g_color) printf(C_RESET);
    printf("============================================================\n");
    printf("[0 First Aid]\n");
    printf("[1 Diagnose] [2 Bundle/Patch] [3 Extract] [4 Repack]\n");
    printf("[5 Create]   [6 Repair/Plan]  [7 Test]    [8 Recipes/Compat]\n");
    printf("[9 Report]   [10 Checks]      [11 Tools]  [12 Session] [13 Help] [14 Quit]\n");
    printf("Inside submenus, choose 0 or type back to return before starting an action.\n");
    fflush(stdout);
}

static int interactive_tabs(void) {
    for (;;) {
        print_tabs();
        char choice[32];
        printf("Choose a tab: "); fflush(stdout);
        if (!fgets(choice, sizeof(choice), stdin)) return 0;
        int c = atoi(choice);
        if (c == 14 || tolower((unsigned char)choice[0]) == 'q') return 0;
        char a[PATH_MAX], b[PATH_MAX], cbuf[PATH_MAX], d[PATH_MAX], e[PATH_MAX];
        printf("\n");
        if (c == 0) {
            first_aid_presets();
            press_enter();
        } else if (c == 1) {
            if (prompt_line("AppImage/AppDir path", a, sizeof(a)) == 0) run_diagnose_target(a);
            press_enter();
        } else if (c == 2) {
            printf("Bundle/Patch options:\n1. Smart dependency + framework bundle\n2. Conservative dependency bundle\n3. Partial framework bundle/check\n4. Patch RUNPATH with patchelf\n5. Run linuxdeploy\n0. Back\n");
            int sub = read_submenu_choice("Choose: ");
            if (sub <= 0) continue;
            if (sub == 1) { if (prompt_line("AppDir path", a, sizeof(a)) == 0) smart_bundle_dependencies(a); }
            else if (sub == 2) { if (prompt_line("AppDir path", a, sizeof(a)) == 0) bundle_dependencies(a); }
            else if (sub == 3) { if (prompt_line("AppDir path", a, sizeof(a)) == 0) partial_framework_bundle(a); }
            else if (sub == 4) { if (prompt_line("AppDir path", a, sizeof(a)) == 0) patch_rpath_appdir(a); }
            else if (sub == 5) { if (prompt_line("AppDir path", a, sizeof(a)) == 0) { printf("Preferred output name, optional: "); fflush(stdout); fgets(b, sizeof(b), stdin); memmove(b, trim(b), strlen(trim(b)) + 1); linuxdeploy_bundle(a, b); } }
            else printf("Unknown Bundle/Patch option.\n");
            press_enter();
        } else if (c == 3) {
            printf("Extract options:\n1. Extract to saved AppDir on disk\n2. RAM workspace session (diagnose/test/repack, then clean)\n0. Back\n");
            int sub = read_submenu_choice("Choose: ");
            if (sub <= 0) continue;
            if (sub == 1) {
                if (prompt_line("AppImage path", a, sizeof(a)) == 0 && prompt_output_appdir_path(b, sizeof(b)) == 0) appimage_extract(a, b);
            } else if (sub == 2) {
                if (prompt_line("AppImage path", a, sizeof(a)) == 0) ram_workspace_session(a);
            } else printf("Unknown Extract option.\n");
            press_enter();
        } else if (c == 4) {
            if (prompt_line("AppDir path", a, sizeof(a)) == 0 && prompt_output_appimage_path(b, sizeof(b)) == 0) appimage_repack(a, b);
            press_enter();
        } else if (c == 5) {
            printf("Create options:\n1. Quick Create AppDir\n2. Guided AppDir Builder\n3. Show suggested AppDir tree\n0. Back\n");
            int sub = read_submenu_choice("Choose: ");
            if (sub <= 0) continue;
            if (sub == 1) {
                memset(a,0,sizeof(a)); memset(b,0,sizeof(b)); memset(cbuf,0,sizeof(cbuf)); memset(d,0,sizeof(d)); memset(e,0,sizeof(e));
                if (prompt_output_appdir_path(a, sizeof(a)) == 0 && prompt_line("App name", b, sizeof(b)) == 0) {
                    printf("Main executable path (blank for demo placeholder): "); fflush(stdout); fgets(cbuf, sizeof(cbuf), stdin); memmove(cbuf, trim(cbuf), strlen(trim(cbuf)) + 1);
                    printf("Icon path, png preferred (blank to skip): "); fflush(stdout); fgets(d, sizeof(d), stdin); memmove(d, trim(d), strlen(trim(d)) + 1);
                    printf("Assets folder (blank to skip): "); fflush(stdout); fgets(e, sizeof(e), stdin); memmove(e, trim(e), strlen(trim(e)) + 1);
                    create_appdir_skeleton(a, b, cbuf, d, e);
                }
            } else if (sub == 2) guided_create_interactive();
            else if (sub == 3) { char idbuf[256]; printf("App ID/name for example tree (blank for myapp): "); fflush(stdout); fgets(idbuf, sizeof(idbuf), stdin); memmove(idbuf, trim(idbuf), strlen(trim(idbuf)) + 1); print_appdir_tree_hint(idbuf[0] ? idbuf : "myapp"); }
            else printf("Unknown Create option.\n");
            press_enter();
        } else if (c == 6) {
            printf("Repair/Plan options:\n1. Apply simple safe repair\n2. Show repair plan\n3. Save repair plan to file\n4. Apply repair plan sequence\n5. Backup AppDir\n6. Restore backup\n0. Back\n");
            int sub = read_submenu_choice("Choose: ");
            if (sub <= 0) continue;
            if (sub == 1) { if (prompt_line("AppDir path", a, sizeof(a)) == 0) repair_appdir(a); }
            else if (sub == 2) { if (prompt_line("AppDir path", a, sizeof(a)) == 0) repair_plan(a, "-", false); }
            else if (sub == 3) { if (prompt_line("AppDir path", a, sizeof(a)) == 0 && prompt_line("Output plan text file", b, sizeof(b)) == 0) repair_plan(a, b, false); }
            else if (sub == 4) { if (prompt_line("AppDir path", a, sizeof(a)) == 0) { char ans[16]; printf("Create backup before applying? Y/n: "); fflush(stdout); fgets(ans, sizeof(ans), stdin); if (ans[0] != 'n' && ans[0] != 'N') backup_appdir(a); repair_plan(a, "-", true); } }
            else if (sub == 5) { if (prompt_line("AppDir path", a, sizeof(a)) == 0) backup_appdir(a); }
            else if (sub == 6) { if (prompt_line("Backup folder", a, sizeof(a)) == 0 && prompt_line("Target AppDir path", b, sizeof(b)) == 0) restore_appdir_from_backup(a, b); }
            else printf("Unknown Repair/Plan option.\n");
            press_enter();
        } else if (c == 7) {
            if (prompt_line("AppImage/AppDir path", a, sizeof(a)) == 0) {
                printf("Log file path (blank for screen output): "); fflush(stdout); fgets(b, sizeof(b), stdin); memmove(b, trim(b), strlen(trim(b)) + 1);
                test_launch(a, b);
            }
            press_enter();
        } else if (c == 8) {
            printf("Recipes/Compatibility options:\n1. Generate appimage-builder recipe\n2. Generate older-base container template\n3. Run older-distro build pipeline\n4. Workspace manager\n0. Back\n");
            int sub = read_submenu_choice("Choose: ");
            if (sub <= 0) continue;
            if (sub == 1) { if (prompt_line("AppDir path", a, sizeof(a)) == 0 && prompt_line("Output recipe .yml", b, sizeof(b)) == 0) generate_appimage_builder_recipe(a, b); }
            else if (sub == 2) { if (prompt_line("Output template folder", a, sizeof(a)) == 0) { printf("Base image (blank for debian:bullseye-slim): "); fflush(stdout); fgets(b, sizeof(b), stdin); memmove(b, trim(b), strlen(trim(b)) + 1); generate_compat_template(a, b); } }
            else if (sub == 3) {
                if (prompt_line("Project folder to mount", a, sizeof(a)) == 0) {
                    printf("Build command to run inside older distro, e.g. ./build.sh: "); fflush(stdout); fgets(b, sizeof(b), stdin); memmove(b, trim(b), strlen(trim(b)) + 1);
                    if (prompt_line("AppDir path inside project", cbuf, sizeof(cbuf)) == 0 && prompt_line("Output .AppImage path inside project", d, sizeof(d)) == 0) {
                        printf("Base image (blank for debian:bullseye-slim): "); fflush(stdout); fgets(e, sizeof(e), stdin); memmove(e, trim(e), strlen(trim(e)) + 1);
                        compat_build_pipeline(a, b, cbuf, d, e);
                    }
                }
            }
            else if (sub == 4) workspace_interactive();
            else printf("Unknown Recipes/Compat option.\n");
            press_enter();
        } else if (c == 9) {
            if (prompt_line("AppImage/AppDir path", a, sizeof(a)) == 0) {
                printf("Report output path (blank to show only): "); fflush(stdout);
                fgets(b, sizeof(b), stdin); memmove(b, trim(b), strlen(trim(b)) + 1);
                diagnose_to_report(a, b);
            }
            press_enter();
        } else if (c == 10) {
            printf("Extra checks/options:\n1. Cross-distro smoke test\n2. AppStream metadata validation\n3. AppImage update metadata check\n4. AppImage signature check\n5. Package AppImage Nurse itself as AppImage\n6. Compatibility score\n7. Library map\n8. Compare two builds\n9. Explain error/log text\n0. Back\n");
            int sub = read_submenu_choice("Choose: ");
            if (sub <= 0) continue;
            if (sub == 1) {
                if (prompt_line("AppImage/AppDir path", a, sizeof(a)) == 0) {
                    printf("Container image (blank for debian:bullseye-slim): "); fflush(stdout); fgets(b, sizeof(b), stdin); memmove(b, trim(b), strlen(trim(b)) + 1);
                    printf("Log file path (blank for screen output): "); fflush(stdout); fgets(d, sizeof(d), stdin); memmove(d, trim(d), strlen(trim(d)) + 1);
                    cross_distro_test(a, b, d);
                }
            } else if (sub == 2) { if (prompt_line("AppDir path", a, sizeof(a)) == 0) appstream_validate(a); }
            else if (sub == 3) { if (prompt_line("AppImage path", a, sizeof(a)) == 0) update_metadata_check(a); }
            else if (sub == 4) { if (prompt_line("AppImage path", a, sizeof(a)) == 0) signature_check(a); }
            else if (sub == 5) { if (prompt_output_appimage_path(a, sizeof(a)) == 0) package_self_appimage(a); }
            else if (sub == 6) { if (prompt_line("AppDir path", a, sizeof(a)) == 0) compatibility_score(a); }
            else if (sub == 7) { if (prompt_line("AppDir path", a, sizeof(a)) == 0) library_map(a); }
            else if (sub == 8) { if (prompt_line("Old AppImage/AppDir", a, sizeof(a)) == 0 && prompt_line("New AppImage/AppDir", b, sizeof(b)) == 0) compare_targets(a, b); }
            else if (sub == 9) explain_errors_interactive();
            else printf("Unknown Checks option.\n");
            press_enter();
        } else if (c == 11) {
            printf("Tools options:\n1. Check tools\n2. Tool path manager\n3. Config\n4. Library policy\n5. Backup/Undo\n0. Back\n");
            int sub = read_submenu_choice("Choose: ");
            if (sub <= 0) continue;
            if (sub == 1) check_tools();
            else if (sub == 2) tool_path_manager_interactive();
            else if (sub == 3) config_interactive();
            else if (sub == 4) library_policy_interactive();
            else if (sub == 5) backup_restore_interactive();
            else printf("Unknown Tools option.\n");
            press_enter();
        } else if (c == 12) {
            session_menu_interactive();
            press_enter();
        } else if (c == 13) {
            print_tab_help();
            press_enter();
        } else {
            printf("Unknown tab.\n"); press_enter();
        }
    }
}

static void usage(const char *argv0) {
    printf("AppImage Nurse %s\n", VERSION);
    printf("Usage:\n");
    printf("  %s                         # open tab UI\n", argv0);
    printf("  %s tabs                    # open tab UI\n", argv0);
    printf("  %s first-aid               # guided common workflows\n", argv0);
    printf("  %s diagnose <path>          # diagnose AppImage or AppDir\n", argv0);
    printf("  %s --report <out.txt|-> <path>\n", argv0);
    printf("  %s extract <AppImage> <out-AppDir>\n", argv0);
    printf("  %s ram-session <AppImage>      # extract to RAM workspace for diagnose/test/repack\n", argv0);
    printf("  %s repack <AppDir> <out.AppImage>\n", argv0);
    printf("  %s create <AppDir> <app-name> [executable] [icon] [assets-dir]\n", argv0);
    printf("  %s guided-create              # interactive guided AppDir builder\n", argv0);
    printf("  %s repair <AppDir>\n", argv0);
    printf("  %s repair-plan <AppDir> [out.txt|-] [--apply]\n", argv0);
    printf("  %s test <AppImage-or-AppDir> [log.txt]\n", argv0);
    printf("  %s smart-bundle <AppDir>    # smart deps + partial framework bundle\n", argv0);
    printf("  %s bundle <AppDir>          # conservative dependency copy to usr/lib\n", argv0);
    printf("  %s framework-bundle <AppDir>\n", argv0);
    printf("  %s patch-rpath <AppDir>     # uses patchelf if installed\n", argv0);
    printf("  %s linuxdeploy <AppDir> [preferred-output.AppImage]\n", argv0);
    printf("  %s recipe <AppDir> <out.yml>\n", argv0);
    printf("  %s compat-template <out-dir> [base-image]\n", argv0);
    printf("  %s compat-build <project-dir> <build-cmd> <AppDir-in-project> <out.AppImage-in-project> [base-image]\n", argv0);
    printf("  %s cross-test <path> [container-image] [log.txt]\n", argv0);
    printf("  %s compare <old> <new>\n", argv0);
    printf("  %s lib-map <AppDir>\n", argv0);
    printf("  %s compat-score <AppDir>\n", argv0);
    printf("  %s appstream <AppDir>\n", argv0);
    printf("  %s update-info <AppImage>\n", argv0);
    printf("  %s signature <AppImage>\n", argv0);
    printf("  %s self-appimage <out.AppImage>\n", argv0);
    printf("  %s workspace <folder> [source-AppImage-or-project]\n", argv0);
    printf("  %s tools\n", argv0);
    printf("  %s tool-path <tool-name> <executable-path>\n", argv0);
    printf("  %s tool-paths\n", argv0);
    printf("  %s config show|set <key> <value>\n", argv0);
    printf("  %s lib-policy show|allow|skip [libname]\n", argv0);
    printf("  %s backup <AppDir>\n", argv0);
    printf("  %s restore <backup-folder> <target-AppDir>\n", argv0);
    printf("  %s explain-log <logfile>\n", argv0);
    printf("  %s explain-text <error text>\n", argv0);
    printf("  %s tab-help\n", argv0);
    printf("  %s --no-color <command> ...\n", argv0);
    printf("  %s --help\n", argv0);
    printf("\nExamples:\n");
    printf("  %s diagnose MyApp.AppImage\n", argv0);
    printf("  %s smart-bundle MyApp.AppDir\n", argv0);
    printf("  %s bundle MyApp.AppDir\n", argv0);
    printf("  %s patch-rpath MyApp.AppDir\n", argv0);
    printf("  %s linuxdeploy MyApp.AppDir\n", argv0);
}

int main(int argc, char **argv) {
    const char *term = getenv("TERM");
    if (term && strcmp(term, "dumb") == 0) g_color = false;
    char color_pref[64];
    if (config_get_value("color", color_pref, sizeof(color_pref))) {
        if (strcmp(color_pref, "off") == 0) g_color = false;
        else if (strcmp(color_pref, "on") == 0) g_color = true;
    }
    session_history_init();
    if (argc > 1 && strcmp(argv[1], "--no-color") == 0) {
        g_color = false;
        argv++; argc--;
    }
    if (argc == 1) return interactive_tabs();
    if (strcmp(argv[1], "tabs") == 0 || strcmp(argv[1], "--tabs") == 0) return interactive_tabs();
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) { usage(argv[0]); return 0; }
    if (strcmp(argv[1], "--version") == 0) { puts(VERSION); return 0; }
    if (strcmp(argv[1], "tools") == 0 || strcmp(argv[1], "tool-check") == 0) { check_tools(); return 0; }
    if (strcmp(argv[1], "tool-paths") == 0) { print_tool_overrides(); return 0; }
    if (strcmp(argv[1], "tool-path") == 0) { if (argc < 4) { usage(argv[0]); return 2; } return save_tool_override(argv[2], argv[3]); }
    if (strcmp(argv[1], "first-aid") == 0 || strcmp(argv[1], "firstaid") == 0) { first_aid_presets(); return 0; }
    if (strcmp(argv[1], "config") == 0) {
        if (argc >= 3 && strcmp(argv[2], "show") == 0) { config_show(); return 0; }
        if (argc >= 5 && strcmp(argv[2], "set") == 0) return config_set_value(argv[3], argv[4]);
        usage(argv[0]); return 2;
    }
    if (strcmp(argv[1], "lib-policy") == 0 || strcmp(argv[1], "library-policy") == 0) {
        if (argc >= 3 && strcmp(argv[2], "show") == 0) { policy_show(); return 0; }
        if (argc >= 4 && strcmp(argv[2], "allow") == 0) return policy_add_library("allow", argv[3]);
        if (argc >= 4 && strcmp(argv[2], "skip") == 0) return policy_add_library("skip", argv[3]);
        usage(argv[0]); return 2;
    }
    if (strcmp(argv[1], "backup") == 0) { if (argc < 3) { usage(argv[0]); return 2; } return backup_appdir(argv[2]); }
    if (strcmp(argv[1], "restore") == 0) { if (argc < 4) { usage(argv[0]); return 2; } return restore_appdir_from_backup(argv[2], argv[3]); }
    if (strcmp(argv[1], "explain-log") == 0 || strcmp(argv[1], "explain") == 0) { if (argc < 3) { usage(argv[0]); return 2; } return explain_error_file(argv[2]); }
    if (strcmp(argv[1], "explain-text") == 0) { if (argc < 3) { usage(argv[0]); return 2; } explain_error_text(argv[2]); return 0; }
    if (strcmp(argv[1], "tab-help") == 0 || strcmp(argv[1], "tabs-help") == 0) { print_tab_help(); return 0; }
    if (strcmp(argv[1], "--report") == 0) {
        if (argc < 4) { usage(argv[0]); return 2; }
        bool old_color = g_color; g_color = false;
        int rc = diagnose_to_report(argv[3], argv[2]);
        g_color = old_color;
        return rc;
    }
    if (strcmp(argv[1], "diagnose") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        run_diagnose_target(argv[2]);
        return g_counts.fail ? 1 : 0;
    }
    if (strcmp(argv[1], "extract") == 0) {
        if (argc < 4) { usage(argv[0]); return 2; }
        return appimage_extract(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "ram-session") == 0 || strcmp(argv[1], "extract-ram") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        return ram_workspace_session(argv[2]);
    }
    if (strcmp(argv[1], "repack") == 0) {
        if (argc < 4) { usage(argv[0]); return 2; }
        return appimage_repack(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "create") == 0) {
        if (argc < 4) { usage(argv[0]); return 2; }
        const char *exe = argc > 4 ? argv[4] : "";
        const char *icon = argc > 5 ? argv[5] : "";
        const char *assets = argc > 6 ? argv[6] : "";
        return create_appdir_skeleton(argv[2], argv[3], exe, icon, assets);
    }
    if (strcmp(argv[1], "guided-create") == 0 || strcmp(argv[1], "create-guided") == 0) {
        return guided_create_interactive();
    }
    if (strcmp(argv[1], "repair") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        return repair_appdir(argv[2]);
    }
    if (strcmp(argv[1], "test") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        const char *logfile = argc > 3 ? argv[3] : NULL;
        return test_launch(argv[2], logfile);
    }
    if (strcmp(argv[1], "smart-bundle") == 0 || strcmp(argv[1], "auto-bundle") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        return smart_bundle_dependencies(argv[2]);
    }
    if (strcmp(argv[1], "framework-bundle") == 0 || strcmp(argv[1], "frameworks") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        return partial_framework_bundle(argv[2]);
    }
    if (strcmp(argv[1], "bundle") == 0 || strcmp(argv[1], "bundle-deps") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        return bundle_dependencies(argv[2]);
    }
    if (strcmp(argv[1], "patch-rpath") == 0 || strcmp(argv[1], "rpath") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        return patch_rpath_appdir(argv[2]);
    }
    if (strcmp(argv[1], "linuxdeploy") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        const char *out = argc > 3 ? argv[3] : "";
        return linuxdeploy_bundle(argv[2], out);
    }
    if (strcmp(argv[1], "recipe") == 0) {
        if (argc < 4) { usage(argv[0]); return 2; }
        return generate_appimage_builder_recipe(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "compat-template") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        const char *base = argc > 3 ? argv[3] : "";
        return generate_compat_template(argv[2], base);
    }
    if (strcmp(argv[1], "compat-build") == 0 || strcmp(argv[1], "old-build") == 0) {
        if (argc < 6) { usage(argv[0]); return 2; }
        const char *base = argc > 6 ? argv[6] : "";
        return compat_build_pipeline(argv[2], argv[3], argv[4], argv[5], base);
    }
    if (strcmp(argv[1], "cross-test") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        const char *image = argc > 3 ? argv[3] : "";
        const char *log = argc > 4 ? argv[4] : "";
        return cross_distro_test(argv[2], image, log);
    }
    if (strcmp(argv[1], "compare") == 0 || strcmp(argv[1], "diff") == 0) {
        if (argc < 4) { usage(argv[0]); return 2; }
        return compare_targets(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "lib-map") == 0 || strcmp(argv[1], "library-map") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        return library_map(argv[2]);
    }
    if (strcmp(argv[1], "compat-score") == 0 || strcmp(argv[1], "score") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        return compatibility_score(argv[2]);
    }
    if (strcmp(argv[1], "repair-plan") == 0 || strcmp(argv[1], "plan") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        const char *out = argc > 3 ? argv[3] : "-";
        bool apply = argc > 4 && strcmp(argv[4], "--apply") == 0;
        return repair_plan(argv[2], out, apply);
    }
    if (strcmp(argv[1], "workspace") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        const char *src = argc > 3 ? argv[3] : "";
        return create_workspace(argv[2], src);
    }
    if (strcmp(argv[1], "appstream") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        return appstream_validate(argv[2]);
    }
    if (strcmp(argv[1], "update-info") == 0 || strcmp(argv[1], "update-metadata") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        return update_metadata_check(argv[2]);
    }
    if (strcmp(argv[1], "signature") == 0 || strcmp(argv[1], "sign-check") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        return signature_check(argv[2]);
    }
    if (strcmp(argv[1], "self-appimage") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        return package_self_appimage(argv[2]);
    }

    /* Backward-compatible shorthand: appimage-nurse <path> */
    run_diagnose_target(argv[1]);
    return g_counts.fail ? 1 : 0;
}
