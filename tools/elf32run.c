/* elf32run: run an old dynamically linked i386 glibc tool (ARM SDT 2.51 armcc/armlink/armasm/armlib)
 * on an x86-64 kernel with no 32-bit userland. Freestanding: own syscalls, own tiny libc, own ELF loader.
 * Usage: elf32run <i386-elf> [args...]
 */
typedef unsigned int u32; typedef int i32; typedef unsigned short u16; typedef unsigned char u8;
typedef unsigned long long u64; typedef long long i64; typedef unsigned int size_t; typedef int ssize_t;
typedef __builtin_va_list va_list;
#define va_start __builtin_va_start
#define va_arg __builtin_va_arg
#define va_end __builtin_va_end
#define NULL ((void*)0)

/* ---------- syscalls (i386 int 0x80) ---------- */
static i32 sc(i32 n, i32 a, i32 b, i32 c, i32 d, i32 e, i32 f) {
    i32 r;
    __asm__ volatile("push %%ebp\n mov %7,%%ebp\n int $0x80\n pop %%ebp"
                     : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "m"(f) : "memory");
    return r;
}
#define SYS_exit_group 252
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_unlink 10
#define SYS_time 13
#define SYS_chmod 15
#define SYS_lseek 19
#define SYS_utime 30
#define SYS_mkdir 39
#define SYS_ioctl 54
#define SYS_mmap2 192
#define SYS_stat64 195
#define SYS_fstat64 197
#define SYS_getdents64 220
#define SYS_clock_gettime 265

static int errno_v;
int *__errno_location(void) { return &errno_v; }
static i32 chk(i32 r) { if (r < 0 && r > -4096) { errno_v = -r; return -1; } return r; }

void _exit(int c) { for (;;) sc(SYS_exit_group, c, 0,0,0,0,0); }

/* ---------- string / memory ---------- */
void *memcpy(void *d, const void *s, size_t n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n) {
    u8 *a = d; const u8 *b = s;
    if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void *memset(void *d, int c, size_t n) { u8 *a = d; while (n--) *a++ = (u8)c; return d; }
int memcmp(const void *x, const void *y, size_t n) { const u8 *a = x, *b = y; for (; n; n--, a++, b++) if (*a != *b) return *a - *b; return 0; }
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return p - s; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }
char *strncpy(char *d, const char *s, size_t n) { char *r = d; while (n && *s) { *d++ = *s++; n--; } while (n--) *d++ = 0; return r; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return (u8)*a - (u8)*b; }
int strncmp(const char *a, const char *b, size_t n) { for (; n; n--, a++, b++) { if (*a != *b || !*a) return (u8)*a - (u8)*b; } return 0; }
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; } }
char *strrchr(const char *s, int c) { const char *r = 0; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; } }
size_t strspn(const char *s, const char *a) { size_t n = 0; while (s[n] && strchr(a, s[n])) n++; return n; }
size_t strcspn(const char *s, const char *a) { size_t n = 0; while (s[n] && !strchr(a, s[n])) n++; return n; }
char *strpbrk(const char *s, const char *a) { s += strcspn(s, a); return *s ? (char *)s : 0; }
char *strstr(const char *h, const char *n) { size_t l = strlen(n); for (; *h; h++) if (!strncmp(h, n, l)) return (char *)h; return l ? 0 : (char *)h; }
char *strtok(char *s, const char *d) {
    static char *p;
    if (s) p = s;
    if (!p) return 0;
    p += strspn(p, d);
    if (!*p) { p = 0; return 0; }
    s = p; p += strcspn(p, d);
    if (*p) *p++ = 0; else p = 0;
    return s;
}

/* ---------- ctype ---------- */
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
int toupper(int c) { return islower(c) ? c - 32 : c; }

/* ---------- malloc (size classes + bump from mmap arenas, never unmapped) ---------- */
static u8 *arena, *arena_end;
static void *freel[32];
static void *raw_alloc(size_t n) {
    if (arena + n > arena_end) {
        size_t sz = n > (8u << 20) ? ((n + 0xfff) & ~0xfffu) : (8u << 20);
        i32 p = sc(SYS_mmap2, 0, sz, 3, 0x22, -1, 0);
        if (p < 0 && p > -4096) return 0;
        arena = (u8 *)p; arena_end = arena + sz;
    }
    void *r = arena; arena += n; return r;
}
static int cls(size_t n) { int c = 4; while ((1u << c) < n) c++; return c; }
void *malloc(size_t n) {
    if (n == 0) n = 1;
    int c = cls(n + 8);
    u32 *p;
    if (freel[c]) { p = freel[c]; freel[c] = *(void **)(p + 2); }
    else { p = raw_alloc(1u << c); if (!p) { errno_v = 12; return 0; } }
    p[0] = c; p[1] = n;
    return p + 2;
}
void free(void *v) { if (!v) return; u32 *p = (u32 *)v - 2; int c = p[0]; *(void **)(p + 2) = freel[c]; freel[c] = p; }
void *realloc(void *v, size_t n) {
    if (!v) return malloc(n);
    u32 *p = (u32 *)v - 2;
    if (n + 8 <= (1u << p[0])) { p[1] = n; return v; }
    void *r = malloc(n); if (!r) return 0;
    memcpy(r, v, p[1] < n ? p[1] : n); free(v); return r;
}
void *calloc(size_t a, size_t b) { void *r = malloc(a * b); if (r) memset(r, 0, a * b); return r; }

/* ---------- qsort (heapsort, no recursion) ---------- */
static void swapb(u8 *a, u8 *b, size_t n) { while (n--) { u8 t = *a; *a++ = *b; *b++ = t; } }
void qsort(void *base, size_t n, size_t w, int (*cmp)(const void *, const void *)) {
    u8 *b = base; size_t i, start, end, root, child;
    if (n < 2) return;
    for (start = n / 2; start-- > 0;) {
        root = start;
        while ((child = 2 * root + 1) < n) {
            if (child + 1 < n && cmp(b + child * w, b + (child + 1) * w) < 0) child++;
            if (cmp(b + root * w, b + child * w) < 0) { swapb(b + root * w, b + child * w, w); root = child; } else break;
        }
    }
    for (end = n - 1; end > 0; end--) {
        swapb(b, b + end * w, w);
        root = 0;
        while ((child = 2 * root + 1) < end) {
            if (child + 1 < end && cmp(b + child * w, b + (child + 1) * w) < 0) child++;
            if (cmp(b + root * w, b + child * w) < 0) { swapb(b + root * w, b + child * w, w); root = child; } else break;
        }
    }
    (void)i;
}

/* ---------- strtol ---------- */
static unsigned long strtoux(const char *s, char **end, int base, int *neg) {
    unsigned long v = 0; const char *p = s; *neg = 0;
    while (isspace(*p)) p++;
    if (*p == '-' || *p == '+') { *neg = *p == '-'; p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit(p[2])) { p += 2; base = 16; }
    else if (base == 0 && p[0] == '0') base = 8;
    else if (base == 0) base = 10;
    const char *q = p;
    for (;; p++) {
        int d = isdigit(*p) ? *p - '0' : islower(*p) ? *p - 'a' + 10 : isupper(*p) ? *p - 'A' + 10 : 99;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)(p == q ? s : p);
    return v;
}
long __strtol_internal(const char *s, char **e, int b, int g) { int n; unsigned long v = strtoux(s, e, b, &n); (void)g; return n ? -(long)v : (long)v; }
unsigned long __strtoul_internal(const char *s, char **e, int b, int g) { int n; unsigned long v = strtoux(s, e, b, &n); (void)g; return n ? -v : v; }
long strtol(const char *s, char **e, int b) { return __strtol_internal(s, e, b, 0); }
unsigned long strtoul(const char *s, char **e, int b) { return __strtoul_internal(s, e, b, 0); }
int atoi(const char *s) { return (int)strtol(s, 0, 10); }

/* ---------- stdio ---------- */
#define BUFSZ 65536
typedef struct FILE { int fd, flags, eof, err, unget; int rpos, rlen, wlen; u8 *buf; } FILE;
#define F_READ 1
#define F_WRITE 2
#define F_UNBUF 4
#define F_USED 8
static FILE files[64];
static u8 stdbufs[3][BUFSZ];
FILE *my_stdin = &files[0], *my_stdout = &files[1], *my_stderr = &files[2];

static int flushw(FILE *f) {
    int off = 0;
    while (off < f->wlen) {
        i32 r = chk(sc(SYS_write, f->fd, (i32)(f->buf + off), f->wlen - off, 0,0,0));
        if (r <= 0) { f->err = 1; f->wlen = 0; return -1; }
        off += r;
    }
    f->wlen = 0; return 0;
}
static void dropread(FILE *f) {
    if (f->rlen > f->rpos || f->unget >= 0) {
        int back = f->rlen - f->rpos + (f->unget >= 0);
        sc(SYS_lseek, f->fd, -back, 1, 0,0,0);
    }
    f->rpos = f->rlen = 0; f->unget = -1;
}
int fflush(FILE *f) {
    if (!f) { int i; for (i = 0; i < 64; i++) if (files[i].flags & F_USED) fflush(&files[i]); return 0; }
    if (f->wlen) return flushw(f);
    return 0;
}
FILE *fopen(const char *path, const char *mode) {
    int fl = 0, i, fd;
    if (mode[0] == 'r') fl = strchr(mode, '+') ? 2 : 0;
    else if (mode[0] == 'w') fl = (strchr(mode, '+') ? 2 : 1) | 0100 | 01000;
    else if (mode[0] == 'a') fl = (strchr(mode, '+') ? 2 : 1) | 0100 | 02000;
    fd = chk(sc(SYS_open, (i32)path, fl, 0666, 0,0,0));
    if (fd < 0) return 0;
    for (i = 3; i < 64; i++) if (!(files[i].flags & F_USED)) break;
    if (i == 64) { sc(SYS_close, fd, 0,0,0,0,0); errno_v = 24; return 0; }
    FILE *f = &files[i];
    memset(f, 0, sizeof *f);
    f->fd = fd; f->flags = F_USED | F_READ | F_WRITE; f->unget = -1;
    f->buf = malloc(BUFSZ);
    return f;
}
int fclose(FILE *f) { int r = fflush(f); sc(SYS_close, f->fd, 0,0,0,0,0); if (f >= &files[3]) { free(f->buf); f->flags = 0; } return r; }
static int fill(FILE *f) {
    if (f->wlen) flushw(f);
    i32 r = chk(sc(SYS_read, f->fd, (i32)f->buf, BUFSZ, 0,0,0));
    if (r <= 0) { if (r < 0) f->err = 1; else f->eof = 1; f->rpos = f->rlen = 0; return -1; }
    f->rpos = 0; f->rlen = r; return 0;
}
int fgetc(FILE *f) {
    if (f->unget >= 0) { int c = f->unget; f->unget = -1; return c; }
    if (f->rpos >= f->rlen && fill(f) < 0) return -1;
    return f->buf[f->rpos++];
}
int _IO_getc(FILE *f) { return fgetc(f); }
int getc(FILE *f) { return fgetc(f); }
int ungetc(int c, FILE *f) { if (c < 0) return -1; f->unget = c & 0xff; f->eof = 0; return c; }
size_t fread(void *p, size_t sz, size_t n, FILE *f) {
    size_t want = sz * n, got = 0; u8 *d = p;
    if (!want) return 0;
    if (f->unget >= 0) { d[got++] = (u8)f->unget; f->unget = -1; }
    while (got < want) {
        if (f->rpos < f->rlen) {
            size_t k = f->rlen - f->rpos; if (k > want - got) k = want - got;
            memcpy(d + got, f->buf + f->rpos, k); f->rpos += k; got += k;
        } else if (want - got >= BUFSZ) {
            if (f->wlen) flushw(f);
            i32 r = chk(sc(SYS_read, f->fd, (i32)(d + got), want - got, 0,0,0));
            if (r <= 0) { if (r < 0) f->err = 1; else f->eof = 1; break; }
            got += r;
        } else if (fill(f) < 0) break;
    }
    return got / sz;
}
size_t fwrite(const void *p, size_t sz, size_t n, FILE *f) {
    size_t want = sz * n, i; const u8 *s = p;
    if (f->rlen || f->unget >= 0) dropread(f);
    for (i = 0; i < want;) {
        size_t k = BUFSZ - f->wlen; if (k > want - i) k = want - i;
        memcpy(f->buf + f->wlen, s + i, k); f->wlen += k; i += k;
        if (f->wlen == BUFSZ && flushw(f) < 0) return i / sz;
    }
    if (f->flags & F_UNBUF) flushw(f);
    return sz ? want / sz : 0;
}
int fputc(int c, FILE *f) { u8 b = (u8)c; fwrite(&b, 1, 1, f); if (c == '\n' && f == my_stderr) fflush(f); return b; }
int _IO_putc(int c, FILE *f) { return fputc(c, f); }
int fputs(const char *s, FILE *f) { fwrite(s, 1, strlen(s), f); return 0; }
int feof(FILE *f) { return f->eof; }
int ferror(FILE *f) { return f->err; }
int fileno(FILE *f) { return f->fd; }
int setvbuf(FILE *f, char *b, int mode, size_t sz) { (void)b; (void)sz; if (mode == 2) f->flags |= F_UNBUF; return 0; }
int fseek(FILE *f, long off, int wh) {
    if (f->wlen) flushw(f);
    if (wh == 1) off -= (f->rlen - f->rpos) + (f->unget >= 0);
    f->rpos = f->rlen = 0; f->unget = -1; f->eof = 0;
    return chk(sc(SYS_lseek, f->fd, off, wh, 0,0,0)) < 0 ? -1 : 0;
}
long ftell(FILE *f) {
    i32 p = chk(sc(SYS_lseek, f->fd, 0, 1, 0,0,0));
    if (p < 0) return -1;
    return p + f->wlen - (f->rlen - f->rpos) - (f->unget >= 0);
}

/* 64-bit division helpers normally in libgcc */
u64 __udivmoddi4(u64 n, u64 d, u64 *rem) {
    u64 q = 0, r = 0; int i;
    for (i = 63; i >= 0; i--) { r = (r << 1) | ((n >> i) & 1); if (r >= d) { r -= d; q |= 1ULL << i; } }
    if (rem) *rem = r;
    return q;
}
u64 __udivdi3(u64 n, u64 d) { return __udivmoddi4(n, d, 0); }
u64 __umoddi3(u64 n, u64 d) { u64 r; __udivmoddi4(n, d, &r); return r; }

/* ---------- printf ---------- */
typedef struct { char *s; size_t n; FILE *f; } Out;
static void outc(Out *o, char c) { if (o->f) fputc(c, o->f); else o->s[o->n] = c; o->n++; }
static void outs(Out *o, const char *s, int n) { while (n-- > 0) outc(o, *s++); }
static void pad(Out *o, char c, int n) { while (n-- > 0) outc(o, c); }
static int fmt_float(char *buf, double v, int prec, char conv, int alt) {
    /* simple %f / %e / %g, good enough for diagnostics */
    char *p = buf; int i, e = 0;
    if (v != v) { strcpy(buf, "nan"); return 3; }
    if (v < 0) { *p++ = '-'; v = -v; }
    if (v > 1e308) { strcpy(p, "inf"); return (p - buf) + 3; }
    if (prec < 0) prec = 6;
    if (conv == 'g' || conv == 'G') {
        double t = v; int x = 0;
        if (t != 0) { while (t >= 10) { t /= 10; x++; } while (t < 1) { t *= 10; x--; } }
        if (prec == 0) prec = 1;
        if (x < -4 || x >= prec) { conv = conv == 'g' ? 'e' : 'E'; prec--; }
        else { conv = 'f'; prec = prec - 1 - x; }
        if (!alt) alt = -1; /* strip zeros */
    }
    if (conv == 'e' || conv == 'E') {
        if (v != 0) { while (v >= 10) { v /= 10; e++; } while (v < 1) { v *= 10; e--; } }
    }
    double r = 0.5; for (i = 0; i < prec; i++) r /= 10;
    v += r;
    if ((conv == 'e' || conv == 'E') && v >= 10) { v /= 10; e++; }
    /* integer part */
    double ip = 1; while (ip * 10 <= v) ip *= 10;
    for (; ip >= 1; ip /= 10) { int d = (int)(v / ip); if (d > 9) d = 9; *p++ = '0' + d; v -= d * ip; }
    if (prec > 0 || alt > 0) *p++ = '.';
    for (i = 0; i < prec; i++) { v *= 10; int d = (int)v; if (d > 9) d = 9; *p++ = '0' + d; v -= d; }
    if (alt < 0 && prec > 0) { while (p[-1] == '0') p--; if (p[-1] == '.') p--; }
    if (conv == 'e' || conv == 'E') {
        *p++ = conv; *p++ = e < 0 ? '-' : '+'; if (e < 0) e = -e;
        if (e >= 100) *p++ = '0' + e / 100;
        *p++ = '0' + (e / 10) % 10; *p++ = '0' + e % 10;
    }
    *p = 0; return p - buf;
}
static int vfmt(Out *o, const char *f, va_list ap) {
    for (; *f; f++) {
        if (*f != '%') { outc(o, *f); continue; }
        f++;
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0, w = 0, prec = -1, lng = 0;
        for (;; f++) {
            if (*f == '-') left = 1; else if (*f == '+') plus = 1; else if (*f == ' ') space = 1;
            else if (*f == '#') alt = 1; else if (*f == '0') zero = 1; else break;
        }
        if (*f == '*') { w = va_arg(ap, int); if (w < 0) { left = 1; w = -w; } f++; }
        else while (isdigit(*f)) w = w * 10 + *f++ - '0';
        if (*f == '.') { f++; prec = 0; if (*f == '*') { prec = va_arg(ap, int); f++; } else while (isdigit(*f)) prec = prec * 10 + *f++ - '0'; }
        for (;; f++) { if (*f == 'l') lng++; else if (*f == 'h' || *f == 'z' || *f == 't' || *f == 'j') ; else if (*f == 'L') lng = 3; else break; }
        char buf[400]; char *s = buf; int n = 0; const char *pre = ""; int c = *f;
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            u64 v; int neg = 0;
            if (c == 'p') { v = (u32)va_arg(ap, void *); alt = 1; c = 'x'; }
            else if (lng >= 2) { v = va_arg(ap, u64); if ((c == 'd' || c == 'i') && (i64)v < 0) { neg = 1; v = -(i64)v; } }
            else { u32 x = va_arg(ap, u32); if ((c == 'd' || c == 'i') && (i32)x < 0) { neg = 1; x = -(i32)x; } v = x; }
            int base = (c == 'x' || c == 'X') ? 16 : c == 'o' ? 8 : 10;
            const char *dg = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char tmp[32]; int k = 0;
            do { tmp[k++] = dg[v % base]; v /= base; } while (v);
            if (prec == 0 && k == 1 && tmp[0] == '0') k = 0;
            while (k < prec) tmp[k++] = '0';
            if (alt && base == 8 && (k == 0 || tmp[k - 1] != '0')) tmp[k++] = '0';
            for (n = 0; n < k; n++) buf[n] = tmp[k - 1 - n];
            if (neg) pre = "-"; else if (plus && base == 10 && c != 'u') pre = "+"; else if (space && base == 10 && c != 'u') pre = " ";
            if (alt && base == 16 && n && !(n == 1 && buf[0] == '0')) pre = c == 'X' ? "0X" : "0x";
            if (prec >= 0) zero = 0;
            break; }
        case 'c': buf[0] = (char)va_arg(ap, int); n = 1; zero = 0; break;
        case 's': s = va_arg(ap, char *); if (!s) s = "(null)"; n = strlen(s); if (prec >= 0 && n > prec) n = prec; zero = 0; break;
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': {
            double d = va_arg(ap, double);
            n = fmt_float(buf, d, prec, c == 'F' ? 'f' : c, alt);
            if (buf[0] == '-') { pre = "-"; s = buf + 1; n--; } else if (plus) pre = "+"; else if (space) pre = " ";
            break; }
        case 'n': *va_arg(ap, int *) = o->n; continue;
        case '%': buf[0] = '%'; n = 1; break;
        default: buf[0] = '%'; buf[1] = c; n = 2; if (!c) f--; break;
        }
        int pl = strlen(pre), tot = n + pl;
        if (!left && !zero) pad(o, ' ', w - tot);
        outs(o, pre, pl);
        if (!left && zero) pad(o, '0', w - tot);
        outs(o, s, n);
        if (left) pad(o, ' ', w - tot);
    }
    return o->n;
}
int vfprintf(FILE *f, const char *fmt, va_list ap) { Out o = { 0, 0, f }; int r = vfmt(&o, fmt, ap); if (f == my_stderr) fflush(f); return r; }
int vsprintf(char *s, const char *fmt, va_list ap) { Out o = { s, 0, 0 }; vfmt(&o, fmt, ap); s[o.n] = 0; return o.n; }
int fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r; }
int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(my_stdout, fmt, ap); va_end(ap); return r; }
int sprintf(char *s, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsprintf(s, fmt, ap); va_end(ap); return r; }

/* ---------- files, time, env, misc ---------- */
int remove(const char *p) { return chk(sc(SYS_unlink, (i32)p, 0,0,0,0,0)) < 0 ? -1 : 0; }
int chmod(const char *p, u32 m) { return chk(sc(SYS_chmod, (i32)p, m, 0,0,0,0)) < 0 ? -1 : 0; }
int utime(const char *p, void *t) { return chk(sc(SYS_utime, (i32)p, (i32)t, 0,0,0,0)) < 0 ? -1 : 0; }
int mkdir(const char *p, u32 m) { return chk(sc(SYS_mkdir, (i32)p, m, 0,0,0,0)) < 0 ? -1 : 0; }
int open(const char *p, int fl, int m) { return chk(sc(SYS_open, (i32)p, fl, m, 0,0,0)); }
int close(int fd) { return chk(sc(SYS_close, fd, 0,0,0,0,0)); }
ssize_t read(int fd, void *b, size_t n) { return chk(sc(SYS_read, fd, (i32)b, n, 0,0,0)); }
long lseek(int fd, long o, int w) { return chk(sc(SYS_lseek, fd, o, w, 0,0,0)); }
int isatty(int fd) { u8 t[64]; return sc(SYS_ioctl, fd, 0x5401, (i32)t, 0,0,0) == 0; }
long time(long *t) { long r = sc(SYS_time, 0, 0,0,0,0,0); if (t) *t = r; return r; }
long clock(void) { i32 ts[2]; sc(SYS_clock_gettime, 2, (i32)ts, 0,0,0,0); return ts[0] * 1000000 + ts[1] / 1000; }
char *ctime(const long *t) {
    static char b[32]; static const char *wd[] = {"Thu","Fri","Sat","Sun","Mon","Tue","Wed"};
    static const char *mo[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    long s = *t, days = s / 86400, rem = s % 86400;
    long z = days + 719468, era = z / 146097, doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    sprintf(b, "%s %s %2ld %02ld:%02ld:%02ld %ld\n", wd[days % 7], mo[m - 1], d, rem / 3600, rem / 60 % 60, rem % 60, y);
    return b;
}
static char **envp_g;
char *getenv(const char *n) {
    size_t l = strlen(n); char **e;
    for (e = envp_g; e && *e; e++) if (!strncmp(*e, n, l) && (*e)[l] == '=') return *e + l + 1;
    return 0;
}
int system(const char *c) { (void)c; return -1; }
void *__sysv_signal(int s, void *h) { (void)s; (void)h; return 0; }
void __register_frame_info(void *a, void *b) { (void)a; (void)b; }
void *__deregister_frame_info(void *a) { (void)a; return 0; }
void exit(int c) { fflush(0); _exit(c); }
void abort(void) { fflush(0); _exit(134); }

/* glibc i386 struct stat (version 3, 88 bytes) from kernel stat64 */
int __xstat(int ver, const char *path, u8 *g) {
    u8 k[96]; (void)ver;
    if (chk(sc(SYS_stat64, (i32)path, (i32)k, 0,0,0,0)) < 0) return -1;
    memset(g, 0, 88);
    memcpy(g + 0, k + 0, 8);      /* st_dev */
    memcpy(g + 12, k + 88, 4);    /* st_ino (low) */
    memcpy(g + 16, k + 16, 16);   /* mode nlink uid gid */
    memcpy(g + 32, k + 32, 8);    /* rdev */
    memcpy(g + 44, k + 44, 4);    /* size (low) */
    memcpy(g + 48, k + 52, 4);    /* blksize */
    memcpy(g + 52, k + 56, 4);    /* blocks (low) */
    memcpy(g + 56, k + 64, 24);   /* atim mtim ctim */
    return 0;
}

/* dirent: glibc i386 layout {u32 ino; i32 off; u16 reclen; u8 type; char name[256];} */
typedef struct { int fd, pos, len; u8 buf[4096]; u8 ent[268]; } DIR;
DIR *opendir(const char *p) {
    int fd = chk(sc(SYS_open, (i32)p, 0200000, 0, 0,0,0));
    if (fd < 0) return 0;
    DIR *d = malloc(sizeof(DIR)); d->fd = fd; d->pos = d->len = 0; return d;
}
void *readdir(DIR *d) {
    if (d->pos >= d->len) {
        i32 r = sc(SYS_getdents64, d->fd, (i32)d->buf, sizeof d->buf, 0,0,0);
        if (r <= 0) return 0;
        d->len = r; d->pos = 0;
    }
    u8 *e = d->buf + d->pos; u16 rl; memcpy(&rl, e + 16, 2);
    d->pos += rl;
    memset(d->ent, 0, sizeof d->ent);
    memcpy(d->ent, e, 4); memcpy(d->ent + 4, e + 8, 4);
    u16 grl = 268; memcpy(d->ent + 8, &grl, 2); d->ent[10] = e[18];
    strncpy((char *)d->ent + 11, (char *)e + 19, 255);
    return d->ent;
}
int closedir(DIR *d) { sc(SYS_close, d->fd, 0,0,0,0,0); free(d); return 0; }

/* setjmp/longjmp, glibc i386 jmp_buf layout: ebx esi edi ebp esp pc */
extern int __sigsetjmp(void *, int);
extern void longjmp(void *, int);
__asm__(".text\n.globl __sigsetjmp\n__sigsetjmp:\n"
        " mov 4(%esp),%eax\n mov %ebx,(%eax)\n mov %esi,4(%eax)\n mov %edi,8(%eax)\n mov %ebp,12(%eax)\n"
        " lea 4(%esp),%ecx\n mov %ecx,16(%eax)\n mov (%esp),%ecx\n mov %ecx,20(%eax)\n xor %eax,%eax\n ret\n"
        ".globl longjmp\nlongjmp:\n"
        " mov 4(%esp),%edx\n mov 8(%esp),%eax\n test %eax,%eax\n jnz 1f\n inc %eax\n1:\n"
        " mov (%edx),%ebx\n mov 4(%edx),%esi\n mov 8(%edx),%edi\n mov 12(%edx),%ebp\n mov 16(%edx),%esp\n jmp *20(%edx)\n");

typedef int (*MainFn)(int, char **, char **);
typedef void (*InitFn)(int, char **, char **);
int __libc_start_main(MainFn m, int argc, char **argv, InitFn init, void *fini, void *rtld, void *se) {
    (void)fini; (void)rtld; (void)se;
    envp_g = argv + argc + 1;
    if (init) init(argc, argv, envp_g);
    exit(m(argc, argv, envp_g));
    return 0;
}

/* ---------- symbol table ---------- */
#define S(n) { #n, (void *)n }
static struct { const char *name; void *addr; } syms[] = {
    S(_IO_getc), S(_IO_putc), S(__deregister_frame_info), S(__errno_location), S(__libc_start_main),
    S(__register_frame_info), S(__sigsetjmp), S(__strtol_internal), S(__strtoul_internal), S(__sysv_signal),
    S(__xstat), S(abort), S(chmod), S(clock), S(ctime), S(fclose), S(feof), S(ferror), S(fflush), S(fgetc),
    S(fileno), S(fopen), S(fprintf), S(fputc), S(fputs), S(fread), S(free), S(fseek), S(ftell), S(fwrite),
    S(getenv), S(isalnum), S(isalpha), S(isatty), S(isdigit), S(islower), S(isprint), S(isspace), S(isupper),
    S(isxdigit), S(isgraph), S(iscntrl), S(longjmp), S(malloc), S(memcpy), S(memmove), S(memset), S(memcmp),
    S(printf), S(qsort), S(realloc), S(calloc), S(remove), S(sprintf), S(strcat), S(strchr), S(strcmp), S(strcpy),
    S(strncmp), S(strncpy), S(strpbrk), S(strrchr), S(strspn), S(strstr), S(strtok), S(strlen), S(system), S(time),
    S(tolower), S(toupper), S(ungetc), S(utime), S(vfprintf), S(vsprintf), S(close), S(lseek), S(mkdir), S(open),
    S(read), S(setvbuf), S(exit), S(closedir), S(opendir), S(readdir), S(strtol), S(strtoul), S(atoi), S(getc),
};

static void die(const char *m, const char *a) { fputs("elf32run: ", my_stderr); fputs(m, my_stderr); if (a) fputs(a, my_stderr); fputc('\n', my_stderr); fflush(0); _exit(127); }

/* ---------- loader ---------- */
typedef struct { u8 id[16]; u16 type, mach; u32 ver, entry, phoff, shoff, flags; u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { u32 type, offset, vaddr, paddr, filesz, memsz, flags, align; } Phdr;
typedef struct { u32 name, value, size; u8 info, other; u16 shndx; } Sym;

static void preadf(int fd, void *b, u32 n, u32 off) {
    sc(SYS_lseek, fd, off, 0, 0,0,0);
    u8 *p = b;
    while (n) { i32 r = sc(SYS_read, fd, (i32)p, n, 0,0,0); if (r <= 0) die("short read", 0); p += r; n -= r; }
}

__attribute__((noreturn)) static void run(char **sp_argv, int argc, char **envp) {
    const char *path = sp_argv[0];
    int fd = chk(sc(SYS_open, (i32)path, 0, 0, 0,0,0));
    if (fd < 0) die("cannot open ", path);
    Ehdr eh; preadf(fd, &eh, sizeof eh, 0);
    if (memcmp(eh.id, "\177ELF", 4) || eh.id[4] != 1 || eh.mach != 3) die("not an i386 ELF: ", path);
    Phdr ph[16]; if (eh.phnum > 16) die("too many phdrs", 0);
    preadf(fd, ph, eh.phnum * sizeof(Phdr), eh.phoff);
    u32 lo = 0xffffffff, hi = 0, dyn = 0, i;
    for (i = 0; i < eh.phnum; i++) if (ph[i].type == 1) {
        if (ph[i].vaddr < lo) lo = ph[i].vaddr;
        if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
    } else if (ph[i].type == 2) dyn = ph[i].vaddr;
    lo &= ~0xfffu; hi = (hi + 0xfff) & ~0xfffu;
    i32 m = sc(SYS_mmap2, lo, hi - lo, 7, 0x32 /* PRIVATE|ANON|FIXED */, -1, 0);
    if ((u32)m != lo) die("mmap failed", 0);
    for (i = 0; i < eh.phnum; i++) if (ph[i].type == 1) preadf(fd, (void *)ph[i].vaddr, ph[i].filesz, ph[i].offset);
    sc(SYS_close, fd, 0,0,0,0,0);

    /* relocate */
    u32 *d = (u32 *)dyn, symtab = 0, strtab = 0, rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0;
    for (; d && d[0]; d += 2) switch (d[0]) {
        case 6: symtab = d[1]; break; case 5: strtab = d[1]; break;
        case 17: rel = d[1]; break; case 18: relsz = d[1]; break;
        case 23: jmprel = d[1]; break; case 2: pltrelsz = d[1]; break;
    }
    u32 pass;
    for (pass = 0; pass < 2; pass++) {
        u32 *r = (u32 *)(pass ? jmprel : rel), n = (pass ? pltrelsz : relsz) / 8;
        for (i = 0; r && i < n; i++) {
            u32 where = r[2 * i], type = r[2 * i + 1] & 0xff, si = r[2 * i + 1] >> 8;
            Sym *s = (Sym *)symtab + si;
            const char *nm = (const char *)strtab + s->name;
            u32 val = 0, k;
            if (type == 8) { *(u32 *)where += 0; continue; } /* RELATIVE: EXEC base 0 */
            if (!strcmp(nm, "stdout")) val = (u32)my_stdout;
            else if (!strcmp(nm, "stderr")) val = (u32)my_stderr;
            else if (!strcmp(nm, "stdin")) val = (u32)my_stdin;
            else if (!strcmp(nm, "__gmon_start__")) val = 0;
            else if (s->shndx != 0) val = s->value;
            else {
                for (k = 0; k < sizeof syms / sizeof syms[0]; k++) if (!strcmp(syms[k].name, nm)) { val = (u32)syms[k].addr; break; }
                if (k == sizeof syms / sizeof syms[0]) die("unresolved symbol ", nm);
            }
            if (type == 6 || type == 7) *(u32 *)where = val;
            else if (type == 5) *(u32 *)where = val;          /* COPY of stdio pointer */
            else if (type == 1) *(u32 *)where += val;
            else die("unsupported reloc", 0);
        }
    }

    /* new stack: argc argv... 0 envp... 0 auxv(0,0) */
    u32 nenv = 0; while (envp[nenv]) nenv++;
    u32 words = 1 + argc + 1 + nenv + 1 + 2 + 4;
    u32 *st = (u32 *)((u32)raw_alloc(1u << 20) + (1u << 20) - 64);
    st -= words; st = (u32 *)((u32)st & ~15u);
    u32 *p = st; *p++ = argc;
    for (i = 0; i < (u32)argc; i++) *p++ = (u32)sp_argv[i];
    *p++ = 0;
    for (i = 0; i < nenv; i++) *p++ = (u32)envp[i];
    *p++ = 0; *p++ = 0; *p++ = 0;
    __asm__ volatile("mov %0,%%esp\n xor %%edx,%%edx\n xor %%ebp,%%ebp\n jmp *%1" :: "r"(st), "r"(eh.entry) : "memory");
    __builtin_unreachable();
}

__attribute__((used)) void cmain(u32 *sp) {
    int argc = sp[0]; char **argv = (char **)(sp + 1); char **envp = argv + argc + 1;
    int i;
    for (i = 0; i < 3; i++) { files[i].fd = i; files[i].flags = F_USED | (i ? F_WRITE : F_READ); files[i].unget = -1; files[i].buf = stdbufs[i]; }
    files[2].flags |= F_UNBUF;
    envp_g = envp;
    if (argc < 2) die("usage: elf32run <prog> [args]", 0);
    run(argv + 1, argc - 1, envp);
}
__asm__(".text\n.globl _start\n_start:\n xor %ebp,%ebp\n mov %esp,%eax\n and $-16,%esp\n sub $12,%esp\n push %eax\n call cmain\n hlt\n");
