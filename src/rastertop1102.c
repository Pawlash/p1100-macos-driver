/*
 * rastertop1102 - native CUPS filter for the HP LaserJet Professional P1100 series
 * (P1102, P1102w, P1104, P1106, P1107, P1108, P1109).
 *
 * Drop-in replacement for HP's x86_64-only "rastertozjs": reads 8-bit grayscale CUPS
 * raster and writes the printer's ZjStream (ZJS) language, JBIG-compressed, 2 bits/pixel.
 * The output is byte-identical to HP's filter for the same input (see tests/), except:
 *   - manual duplex with an odd page count works (HP's filter crashes),
 *   - a one-page job with duplex selected is printed simplex instead of asking to flip,
 *   - "Duplex=..." without "sides=..." also triggers manual duplex,
 *   - control characters in the job title are replaced in the PJL JOB NAME,
 *   - printer status is reported in the queue (no paper, jam, ...) like HP's filter does,
 *     but without HP's status monitor app,
 *   - always 2 bits/pixel: HP switches to a 1-bit mode for even cupsCompression values,
 *     which applies the 2-bit screens as thresholds (garbage); the PPD never selects it.
 *
 * Usage (CUPS filter): rastertop1102 job user title copies options [file]
 */
#include <cups/cups.h>
#include <cups/raster.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "papers.h"
#include "screens.h" /* generated from HP's driver at build time, see tools/ */

/* ---------------------------------------------------------------------------------------
 * Small helpers
 */

typedef struct {
    uint8_t *p;
    size_t n, cap;
} buf_t;

static void die(const char *msg)
{
    fprintf(stderr, "ERROR: %s\n", msg);
    exit(1);
}

static void buf_reserve(buf_t *b, size_t extra)
{
    if (b->n + extra <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 65536;
    while (cap < b->n + extra)
        cap *= 2;
    uint8_t *p = realloc(b->p, cap);
    if (!p)
        die("out of memory");
    b->p = p;
    b->cap = cap;
}

static inline void buf_byte(buf_t *b, uint8_t v)
{
    if (b->n == b->cap)
        buf_reserve(b, 1);
    b->p[b->n++] = v;
}

static void xwrite(FILE *f, const void *p, size_t n)
{
    if (n && fwrite(p, 1, n, f) != n)
        die("write failed");
}

static void put_be32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v};
    xwrite(f, b, 4);
}

static void put_be16(FILE *f, uint16_t v)
{
    uint8_t b[2] = {v >> 8, v};
    xwrite(f, b, 2);
}

static volatile sig_atomic_t canceled;

static void on_sigterm(int sig)
{
    (void)sig;
    canceled = 1;
}

/* ---------------------------------------------------------------------------------------
 * JBIG (ITU-T T.82) encoder: single layer, sequential, 2-line template, TPBON, MX=0.
 * Faithful to the encoder in HP's filter (itself a QM coder identical to jbigkit's).
 */

static const uint16_t jbg_lsz[113] = {
    0x5a1d, 0x2586, 0x1114, 0x080b, 0x03d8, 0x01da, 0x00e5, 0x006f, 0x0036, 0x001a, 0x000d,
    0x0006, 0x0003, 0x0001, 0x5a7f, 0x3f25, 0x2cf2, 0x207c, 0x17b9, 0x1182, 0x0cef, 0x09a1,
    0x072f, 0x055c, 0x0406, 0x0303, 0x0240, 0x01b1, 0x0144, 0x00f5, 0x00b7, 0x008a, 0x0068,
    0x004e, 0x003b, 0x002c, 0x5ae1, 0x484c, 0x3a0d, 0x2ef1, 0x261f, 0x1f33, 0x19a8, 0x1518,
    0x1177, 0x0e74, 0x0bfb, 0x09f8, 0x0861, 0x0706, 0x05cd, 0x04de, 0x040f, 0x0363, 0x02d4,
    0x025c, 0x01f8, 0x01a4, 0x0160, 0x0125, 0x00f6, 0x00cb, 0x00ab, 0x008f, 0x5b12, 0x4d04,
    0x412c, 0x37d8, 0x2fe8, 0x293c, 0x2379, 0x1edf, 0x1aa9, 0x174e, 0x1424, 0x119c, 0x0f6b,
    0x0d51, 0x0bb6, 0x0a40, 0x5832, 0x4d1c, 0x438e, 0x3bdd, 0x34ee, 0x2eae, 0x299a, 0x2516,
    0x5570, 0x4ca9, 0x44d9, 0x3e22, 0x3824, 0x32b4, 0x2e17, 0x56a8, 0x4f46, 0x47e5, 0x41cf,
    0x3c3d, 0x375e, 0x5231, 0x4c0f, 0x4639, 0x415e, 0x5627, 0x50e7, 0x4b85, 0x5597, 0x504f,
    0x5a10, 0x5522, 0x59eb};

/* next state after LPS; bit 7 set = swap MPS */
static const uint8_t jbg_nlps[113] = {
    129, 14,  16,  18,  20,  23, 25,  28, 30,  33, 35, 9,   10, 12, 143, 36, 38,  39, 40,
    42,  43,  45,  46,  48,  49, 51,  52, 54,  56, 57, 59,  60, 62, 63,  32, 33,  165, 64,
    65,  67,  68,  69,  70,  72, 73,  74, 75,  77, 78, 79,  48, 50, 50,  51, 52,  53, 54,
    55,  56,  57,  58,  59,  61, 61,  193, 80, 81, 82, 83,  84, 86, 87,  87, 72,  72, 74,
    74,  75,  77,  77,  208, 88, 89,  90, 91,  92, 93, 86,  216, 95, 96, 97,  99, 99,  93,
    223, 101, 102, 103, 104, 99, 105, 106, 107, 103, 233, 108, 109, 110, 111, 238, 112, 240};

static const uint8_t jbg_nmps[113] = {
    1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 13, 15, 16, 17,  18,  19,  20,  21,  22,
    23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 9,  37, 38, 39,  40,  41,  42,  43,  44,
    45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,  62,  63,  32,  65,  66,
    67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 48, 81, 82, 83,  84,  85,  86,  87,  71,
    89, 90, 91, 92, 93, 94, 86, 96, 97, 98, 99, 100, 93, 102, 103, 104, 99, 106, 107, 103, 109,
    107, 111, 109, 111};

#define TPB2CX 0x195 /* context of the "line not typical" pseudo-pixel */

typedef struct {
    uint8_t st[1024], mps[1024];
    uint32_t c, a, sc;
    int ct;
    uint8_t buffer;
    int started;
    buf_t *out;
} qm_t;

static inline void qm_put(qm_t *q, uint8_t b, uint8_t fill)
{
    buf_t *o = q->out;
    if (q->started) {
        buf_byte(o, b);
        if (b == 0xff)
            buf_byte(o, 0x00);
    }
    q->started = 1;
    for (; q->sc; q->sc--) {
        buf_byte(o, fill);
        if (fill == 0xff)
            buf_byte(o, 0x00);
    }
}

static void qm_begin(qm_t *q)
{
    q->a = 0x10000;
    q->c = 0;
    q->ct = 11;
    q->sc = 0;
    q->buffer = 0;
    q->started = 0;
}

static inline void qm_renorm(qm_t *q)
{
    do {
        q->a <<= 1;
        q->c <<= 1;
        if (--q->ct == 0) {
            uint32_t temp = q->c >> 19;
            if (temp > 0xff) {
                qm_put(q, (uint8_t)(q->buffer + 1), 0x00);
                q->buffer = (uint8_t)temp;
            } else if (temp == 0xff) {
                q->sc++;
            } else {
                qm_put(q, q->buffer, 0xff);
                q->buffer = (uint8_t)temp;
            }
            q->c &= 0x7ffff;
            q->ct = 8;
        }
    } while (q->a <= 0x7fff);
}

static inline void qm_encode(qm_t *q, int cx, int pix)
{
    uint8_t ss = q->st[cx];
    uint32_t lsz = jbg_lsz[ss];
    q->a -= lsz;
    if (pix != q->mps[cx]) {
        if (lsz <= q->a) {
            q->c += q->a;
            q->a = lsz;
        }
        if (jbg_nlps[ss] & 0x80)
            q->mps[cx] ^= 1;
        q->st[cx] = jbg_nlps[ss] & 0x7f;
    } else {
        if (q->a > 0x7fff)
            return;
        if (lsz > q->a) {
            q->c += q->a;
            q->a = lsz;
        }
        q->st[cx] = jbg_nmps[ss];
    }
    qm_renorm(q);
}

static void qm_flush(qm_t *q)
{
    buf_t *o = q->out;
    uint32_t temp = (q->a - 1 + q->c) & 0xffff0000u;
    q->c = (temp < q->c) ? temp + 0x8000 : temp;
    q->c <<= q->ct;
    if (q->c > 0x7ffffff) {
        if (q->started) {
            buf_byte(o, (uint8_t)(q->buffer + 1));
            if ((uint8_t)(q->buffer + 1) == 0xff)
                buf_byte(o, 0x00);
        }
        if (q->c & 0x7fff800)
            for (; q->sc; q->sc--)
                buf_byte(o, 0x00);
    } else {
        if (q->started) {
            buf_byte(o, q->buffer);
            if (q->buffer == 0xff)
                buf_byte(o, 0x00);
        }
        for (; q->sc; q->sc--) {
            buf_byte(o, 0xff);
            buf_byte(o, 0x00);
        }
    }
    if (q->c & 0x7fff800) {
        uint8_t b = (q->c >> 19) & 0xff;
        buf_byte(o, b);
        if (b == 0xff)
            buf_byte(o, 0x00);
        if (q->c & 0x7f800) {
            b = (q->c >> 11) & 0xff;
            buf_byte(o, b);
            if (b == 0xff)
                buf_byte(o, 0x00);
        }
    }
}

/* Encode one line; returns 1 if it differed from the previous one (i.e. was coded). */
static int jbig_line(qm_t *q, const uint8_t *cur, const uint8_t *prev, int bpl, int differ_old)
{
    int differ = memcmp(cur, prev, bpl) != 0;
    qm_encode(q, TPB2CX, differ == differ_old);
    if (!differ)
        return 0;
    for (int c = 0; c < bpl; c++) {
        uint32_t p = (uint32_t)prev[c] << 8;
        uint32_t k = cur[c];
        if (c > 0) {
            p |= (uint32_t)prev[c - 1] << 16;
            k |= (uint32_t)cur[c - 1] << 8;
        }
        if (c + 1 < bpl)
            p |= prev[c + 1];
        for (int b = 0; b < 8; b++) {
            int cx = (int)(((k >> (8 - b)) & 0x00f) | ((p >> (9 - b)) & 0x3f0));
            qm_encode(q, cx, (cur[c] >> (7 - b)) & 1);
        }
    }
    return 1;
}

/* ---------------------------------------------------------------------------------------
 * ZjStream output
 */

enum {
    ZJT_START_DOC = 0, ZJT_END_DOC = 1, ZJT_START_PAGE = 2, ZJT_END_PAGE = 3,
    ZJT_JBIG_BIH = 4, ZJT_JBIG_BID = 5, ZJT_END_JBIG = 6, ZJT_PAUSE = 11
};

typedef struct {
    uint16_t id;
    uint32_t value;
} zitem_t;

static void zjs_record(FILE *f, uint32_t type, const zitem_t *it, int nit, const void *data,
                       size_t len)
{
    put_be32(f, (uint32_t)(16 + 12 * nit + len));
    put_be32(f, type);
    put_be32(f, (uint32_t)nit);
    put_be16(f, (uint16_t)(12 * nit));
    put_be16(f, 0x5a5a);
    for (int i = 0; i < nit; i++) {
        put_be32(f, 12);
        put_be16(f, it[i].id);
        uint8_t tp[2] = {1, 0}; /* type = UINT32, param = 0 */
        xwrite(f, tp, 2);
        put_be32(f, it[i].value);
    }
    xwrite(f, data, len);
}

#define BID_CHUNK 65536

/* ---------------------------------------------------------------------------------------
 * Job settings
 */

typedef struct {
    int econo;      /* EconoMode */
    int density;    /* 1..5 */
    int jam;        /* pjlJamRecovery */
    int duplex;     /* manual duplex requested */
    char title[256];
} job_t;

/* "*DefaultKEY: value" from the PPD, or NULL */
static const char *ppd_default(const char *ppdtext, const char *key, char *out, size_t outlen)
{
    if (!ppdtext)
        return NULL;
    char pat[96];
    snprintf(pat, sizeof(pat), "\n*Default%s:", key);
    const char *p = strstr(ppdtext, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t')
        p++;
    size_t n = strcspn(p, " \t\r\n");
    if (n >= outlen)
        n = outlen - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return out;
}

static char *read_file(const char *path)
{
    if (!path)
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    buf_t b = {0};
    buf_byte(&b, '\n');
    char tmp[16384];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        buf_reserve(&b, n);
        memcpy(b.p + b.n, tmp, n);
        b.n += n;
    }
    buf_byte(&b, 0);
    fclose(f);
    return (char *)b.p;
}

static int is_true(const char *v)
{
    return v && (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") ||
                 !strcmp(v, "1"));
}

static const char *opt_or_default(const char *key, int nopt, cups_option_t *opts,
                                  const char *ppdtext, char *tmp, size_t tmplen)
{
    const char *v = cupsGetOption(key, nopt, opts);
    if (v)
        return v;
    return ppd_default(ppdtext, key, tmp, tmplen);
}

static void job_init(job_t *job, const char *title, const char *options)
{
    cups_option_t *opts = NULL;
    int nopt = cupsParseOptions(options, 0, &opts);
    char *ppdtext = read_file(getenv("PPD"));
    char tmp[64];

    job->econo = is_true(opt_or_default("EconoMode", nopt, opts, ppdtext, tmp, sizeof(tmp)));
    job->jam = is_true(opt_or_default("pjlJamRecovery", nopt, opts, ppdtext, tmp, sizeof(tmp)));
    const char *d = opt_or_default("pjlDensity", nopt, opts, ppdtext, tmp, sizeof(tmp));
    job->density = (d && d[0] >= '1' && d[0] <= '5' && !d[1]) ? d[0] - '0' : 3;

    const char *sides = cupsGetOption("sides", nopt, opts);
    const char *dup = cupsGetOption("Duplex", nopt, opts);
    if (sides)
        job->duplex = !strncmp(sides, "two-sided", 9);
    else
        job->duplex = dup && strcasecmp(dup, "None") && strcasecmp(dup, "False");

    size_t i = 0;
    for (const unsigned char *s = (const unsigned char *)title; *s && i + 1 < sizeof(job->title);
         s++)
        job->title[i++] = (*s < 0x20 || *s == 0x7f) ? ' ' : (char)*s;
    job->title[i] = 0;

    cupsFreeOptions(nopt, opts);
    free(ppdtext);
}

static void write_job_start(FILE *f, const job_t *job)
{
    fprintf(f, "\033%%-12345X@PJL USTATUS JOB=ON\r\n@PJL USTATUS PAGE=ON\r\n"
               "@PJL USTATUS DEVICE=ON\r\n@PJL INFO STATUS\r\n");
    fprintf(f, "@PJL JOB NAME=%s\r\n", job->title);
    fprintf(f, "@PJL SET DENSITY = %d\r\n", job->density);
    fprintf(f, "@PJL SET JAMRECOVERY = %s\r\n", job->jam ? "AUTO" : "OFF");
    fprintf(f, "@PJL ENTER LANGUAGE=ZJS\r\n");
    xwrite(f, "JZJZ", 4);
    zitem_t it[] = {{0x01, 0}};
    zjs_record(f, ZJT_START_DOC, it, 1, NULL, 0);
}

static void write_job_end(FILE *f)
{
    zjs_record(f, ZJT_END_DOC, NULL, 0, NULL, 0);
    fprintf(f, "\033%%-12345X@PJL EOJ\r\n\033%%-12345X@PJL USTATUSOFF\r\n\033%%-12345X");
}

/* ---------------------------------------------------------------------------------------
 * Page rendering: screening + JBIG + ZJS page records
 */

typedef struct {
    unsigned width, height;   /* raster pixels; height rounded up to a multiple of 12 */
    unsigned raster_height;   /* lines actually present in the raster */
    unsigned bpl;             /* raster bytes per line */
    unsigned out_bpl;         /* bytes per line of the 2-bit image */
    const uint8_t *screen;
    zitem_t items[13];
} page_t;

static int paper_code(unsigned w, unsigned h)
{
    for (int i = 0; paper_codes[i].code; i++)
        if (paper_codes[i].w == (int)w && paper_codes[i].h == (int)h)
            return paper_codes[i].code;
    return 0;
}

static int page_setup(page_t *pg, const cups_page_header2_t *h, const job_t *job)
{
    if (h->cupsBitsPerColor != 8 || h->cupsBitsPerPixel != 8 || h->cupsColorOrder != 0) {
        fprintf(stderr, "ERROR: unsupported raster format (%u bpc, %u bpp, order %u)\n",
                h->cupsBitsPerColor, h->cupsBitsPerPixel, h->cupsColorOrder);
        return 0;
    }
    if (h->cupsWidth == 0 || h->cupsHeight == 0 || h->cupsBytesPerLine < h->cupsWidth)
        return 0;
    pg->width = h->cupsWidth;
    pg->raster_height = h->cupsHeight;
    pg->height = ((h->cupsHeight - 1) / 12 + 1) * 12;
    pg->bpl = h->cupsBytesPerLine;
    unsigned padded = (pg->width + 63) & ~63u;
    pg->out_bpl = padded / 4;

    /* the PPD encodes "FastRes 600" as cupsCompression 401 and "FastRes 1200" as 1 */
    unsigned vres = h->cupsCompression & 0x3ffe;
    if (vres == 0)
        vres = h->HWResolution[1];
    pg->screen = (vres == 400) ? screen_s10 : screen_r10;

    unsigned tray = h->MediaPosition ? h->MediaPosition : 7;
    zitem_t it[13] = {
        {0x03, (uint32_t)paper_code(h->PageSize[0], h->PageSize[1])},
        {0x04, h->NumCopies},
        {0x05, tray},
        {0x06, h->cupsMediaType},
        {0x07, 1},
        {0x08, h->HWResolution[0]},
        {0x09, vres},
        {0x0c, pg->width * 2},
        {0x0d, pg->height},
        {0x10, 2},
        {0x11, pg->width},
        {0x12, pg->height},
        {0x17, (uint32_t)job->econo},
    };
    memcpy(pg->items, it, sizeof(it));
    return 1;
}

static void screen_line(const page_t *pg, const uint8_t *in, unsigned row, uint8_t *out)
{
    const uint8_t *rp = pg->screen + 4 * (row % 12);
    memset(out, 0, pg->out_bpl);
    for (unsigned x = 0; x < pg->width; x++) {
        unsigned dark = 255u - in[x];
        if (dark)
            out[x >> 2] |= rp[dark * 48 + (x % 12) / 4] & (0xc0 >> (2 * (x & 3)));
    }
}

/*
 * Render one page from the raster (or a blank page if ras == NULL) and write its ZJS
 * records to f.  Returns 0 on fatal input errors.
 */
static int render_page(FILE *f, cups_raster_t *ras, const page_t *pg, int pageno)
{
    const unsigned L0 = 128;
    const unsigned obpl = pg->out_bpl;
    uint8_t *in = malloc(pg->bpl);
    /* two context lines (previous stripe) + one stripe */
    uint8_t *lines = calloc((size_t)(L0 + 2), obpl);
    qm_t *q = calloc(1, sizeof(qm_t));
    buf_t jb = {0};
    if (!in || !lines || !q)
        die("out of memory");
    q->out = &jb;

    zjs_record(f, ZJT_START_PAGE, pg->items, 13, NULL, 0);

    unsigned y = 0;
    int last_pct = -1;
    for (unsigned stripe = 0; y < pg->height; stripe++) {
        unsigned n = pg->height - y < L0 ? pg->height - y : L0;
        qm_begin(q);
        int differ_old = (stripe == 0 || memcmp(lines, lines + obpl, obpl) != 0) ? 1 : 0;
        for (unsigned i = 0; i < n; i++, y++) {
            uint8_t *cur = lines + (size_t)(i + 2) * obpl;
            if (ras && y < pg->raster_height && !canceled &&
                cupsRasterReadPixels(ras, in, pg->bpl) == pg->bpl) {
                screen_line(pg, in, pg->height - y, cur);
            } else {
                memset(cur, 0, obpl);
            }
            differ_old = jbig_line(q, cur, cur - obpl, (int)obpl, differ_old);
        }
        qm_flush(q);
        buf_byte(&jb, 0xff); /* ESC SDNORM */
        buf_byte(&jb, 0x02);
        memcpy(lines, lines + (size_t)n * obpl, 2 * (size_t)obpl);

        int pct = (int)(100.0 * y / pg->height);
        if (pct / 10 != last_pct / 10) {
            fprintf(stderr, "INFO: Printing page %d, %d%% complete...\n", pageno, pct);
            last_pct = pct;
        }
    }
    /* drain lines we did not need (raster taller than expected cannot happen, but a
       canceled job may leave some) */
    if (ras)
        while (y++ < pg->raster_height && cupsRasterReadPixels(ras, in, pg->bpl) == pg->bpl)
            ;

    uint8_t bih[20] = {0, 0, 1, 0};
    uint32_t xd = obpl * 8, yd = pg->height;
    bih[4] = xd >> 24; bih[5] = xd >> 16; bih[6] = xd >> 8; bih[7] = xd;
    bih[8] = yd >> 24; bih[9] = yd >> 16; bih[10] = yd >> 8; bih[11] = yd;
    bih[15] = L0;
    bih[16] = 0;    /* MX */
    bih[17] = 0;    /* MY */
    bih[18] = 0x03; /* order: HITOLO | SEQ */
    bih[19] = 0x5c; /* options: LRLTWO | TPDON | TPBON | DPON */
    zjs_record(f, ZJT_JBIG_BIH, NULL, 0, bih, sizeof(bih));

    size_t off = 0;
    while (jb.n - off > BID_CHUNK) {
        zjs_record(f, ZJT_JBIG_BID, NULL, 0, jb.p + off, BID_CHUNK);
        off += BID_CHUNK;
    }
    size_t rest = jb.n - off;
    if (rest) {
        size_t padded = (rest + 3) & ~(size_t)3;
        buf_reserve(&jb, padded - rest);
        memset(jb.p + jb.n, 0, padded - rest);
        zjs_record(f, ZJT_JBIG_BID, NULL, 0, jb.p + off, padded);
    }
    zjs_record(f, ZJT_END_JBIG, NULL, 0, NULL, 0);
    zjs_record(f, ZJT_END_PAGE, NULL, 0, NULL, 0);

    free(jb.p);
    free(q);
    free(lines);
    free(in);
    return 1;
}

/* ---------------------------------------------------------------------------------------
 * Manual duplex spooling
 */

typedef struct {
    long off, len;
} spooled_t;

static FILE *open_spool(void)
{
    const char *dir = getenv("TMPDIR");
    char path[1024];
    snprintf(path, sizeof(path), "%s/p1102XXXXXX", dir && *dir ? dir : "/tmp");
    int fd = mkstemp(path);
    if (fd < 0)
        return NULL;
    unlink(path);
    return fdopen(fd, "w+b");
}

static void copy_range(FILE *dst, FILE *src, long off, long len)
{
    char tmp[65536];
    if (fseek(src, off, SEEK_SET) != 0)
        die("spool seek failed");
    while (len > 0) {
        size_t n = fread(tmp, 1, len < (long)sizeof(tmp) ? (size_t)len : sizeof(tmp), src);
        if (n == 0)
            die("spool read failed");
        xwrite(dst, tmp, n);
        len -= (long)n;
    }
}

/* ---------------------------------------------------------------------------------------
 * Printer status (PJL USTATUS / INFO STATUS on the CUPS back channel, fd 3)
 *
 * Like HP's filter we keep stdout open after the last byte and listen until the printer
 * reports the end of the job, so problems (no paper, jam, open door, the manual duplex
 * pause) show up in the print queue instead of the job silently "completing".
 * Code ranges follow HP's filter (41100-41199 manual feed, 41002-41004 paper out,
 * 42000-42005 jam, 40120/40121/40131 manual duplex) and the HP PJL reference.
 */

#define BC_FD 3

typedef struct {
    int lo, hi;
    const char *reason;   /* IPP printer-state-reason without severity, NULL = none */
    const char *severity; /* "error" or "warning" */
    const char *en, *pl;
} status_code_t;

static const status_code_t status_codes[] = {
    {40120, 40121, "media-needed", "error",
     "Manual duplex: take the printed stack out of the output bin without turning it and "
     "put it back into the input tray, printed side down.",
     "Druk dwustronny: wyjmij zadrukowany stos z tacy odbiorczej, nie obracając go, i włóż "
     "z powrotem do podajnika zadrukowaną stroną w dół."},
    {40131, 40131, "media-needed", "error",
     "Manual duplex: take the printed stack out of the output bin without turning it and "
     "put it back into the input tray, printed side down.",
     "Druk dwustronny: wyjmij zadrukowany stos z tacy odbiorczej, nie obracając go, i włóż "
     "z powrotem do podajnika zadrukowaną stroną w dół."},
    {41100, 41199, "media-needed", "error",
     "Load paper into the input tray.",
     "Włóż papier do podajnika."},
    {41000, 41999, "media-empty", "error",
     "Out of paper or paper not picked up: reload the paper in the input tray.",
     "Brak papieru lub drukarka nie może pobrać kartki: włóż papier ponownie do podajnika."},
    {42000, 42999, "media-jam", "error",
     "Paper jam: open the cartridge door and remove the jammed paper.",
     "Zacięcie papieru: otwórz klapkę wkładu i wyjmij zaciętą kartkę."},
    {40022, 40022, "media-jam", "error",
     "Paper jam: open the cartridge door and remove the jammed paper.",
     "Zacięcie papieru: otwórz klapkę wkładu i wyjmij zaciętą kartkę."},
    {40021, 40021, "cover-open", "error",
     "The cartridge door is open or the toner cartridge is missing.",
     "Otwarta klapka albo brak wkładu z tonerem."},
    {40010, 40010, "toner-empty", "error",
     "Toner cartridge missing or not seated.",
     "Brak wkładu z tonerem albo jest źle włożony."},
    {40046, 40046, "toner-empty", "error",
     "Toner cartridge missing or not seated.",
     "Brak wkładu z tonerem albo jest źle włożony."},
    {40038, 40038, "toner-low", "warning", "Toner is low.", "Kończy się toner."},
    {10006, 10006, "toner-low", "warning", "Toner is low.", "Kończy się toner."},
    {40079, 40079, "offline", "report", "The printer is offline.", "Drukarka jest offline."},
    {50000, 59999, "other", "error",
     "Printer error %d: turn the printer off and on again.",
     "Błąd drukarki %d: wyłącz i włącz drukarkę."},
};

static struct {
    pthread_mutex_t m;
    pthread_cond_t cv;
    int active;          /* reader thread running */
    int closed;          /* back channel gone */
    int job_end;         /* printer reported end of job */
    int got_data;
    time_t last_data;
    const status_code_t *cur; /* current problem shown to the user */
    int stop;
    int polish;
} st = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0, NULL, 0, 0};

static void set_problem(const status_code_t *sc, int code)
{
    /* called with st.m held */
    if (sc == st.cur)
        return;
    if (st.cur)
        fprintf(stderr, "STATE: -%s-%s\n", st.cur->reason, st.cur->severity);
    st.cur = sc;
    if (sc) {
        fprintf(stderr, "STATE: +%s-%s\n", sc->reason, sc->severity);
        fprintf(stderr, "INFO: ");
        fprintf(stderr, st.polish ? sc->pl : sc->en, code);
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "INFO: %s\n", st.polish ? "Drukowanie..." : "Printing...");
    }
}

static void handle_code(int code)
{
    const status_code_t *sc = NULL;
    for (size_t i = 0; i < sizeof(status_codes) / sizeof(status_codes[0]); i++)
        if (code >= status_codes[i].lo && code <= status_codes[i].hi) {
            sc = &status_codes[i];
            break;
        }
    if (sc || (code >= 10000 && code < 11000)) /* 10xxx: ready / printing / warming up */
        set_problem(sc, code);
}

static void handle_message(char *msg)
{
    /* one PJL status message, without the trailing form feed */
    char dbg[512];
    size_t j = 0;
    for (const char *p = msg; *p && j + 4 < sizeof(dbg); p++)
        dbg[j++] = (*p == '\r' || *p == '\n') ? ' ' : *p;
    dbg[j] = 0;
    fprintf(stderr, "DEBUG: printer status: %s\n", dbg);

    if (strstr(msg, "USTATUS JOB")) {
        if (strstr(msg, "\nEND") || strstr(msg, "\rEND"))
            st.job_end = 1;
        return;
    }
    if (strstr(msg, "USTATUS PAGE")) {
        const char *p = strstr(msg, "PAGE");
        p = p ? strpbrk(p + 4, "0123456789") : NULL;
        if (p && !st.cur)
            fprintf(stderr, st.polish ? "INFO: Wydrukowano stronę %d\n" : "INFO: Printed page %d\n",
                    atoi(p));
        return;
    }
    const char *c = strstr(msg, "CODE=");
    if (c)
        handle_code(atoi(c + 5));
}

static void *status_reader(void *arg)
{
    (void)arg;
    char acc[16384];
    size_t n = 0;
    for (;;) {
        pthread_mutex_lock(&st.m);
        int stop = st.stop;
        pthread_mutex_unlock(&st.m);
        if (stop)
            break;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(BC_FD, &rf);
        struct timeval tv = {0, 300000};
        int s = select(BC_FD + 1, &rf, NULL, NULL, &tv);
        if (s < 0 && errno == EINTR)
            continue;
        if (s < 0)
            break;
        if (s == 0)
            continue;
        ssize_t r = read(BC_FD, acc + n, sizeof(acc) - 1 - n);
        if (r < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (r <= 0)
            break;
        n += (size_t)r;
        acc[n] = 0;
        pthread_mutex_lock(&st.m);
        st.got_data = 1;
        st.last_data = time(NULL);
        char *start = acc, *ff;
        while ((ff = memchr(start, '\f', (size_t)(acc + n - start)))) {
            *ff = 0;
            handle_message(start);
            start = ff + 1;
        }
        size_t rest = (size_t)(acc + n - start);
        if (rest == sizeof(acc) - 1)
            rest = 0; /* garbage without separators: drop it */
        memmove(acc, start, rest);
        n = rest;
        pthread_cond_broadcast(&st.cv);
        pthread_mutex_unlock(&st.m);
    }
    pthread_mutex_lock(&st.m);
    st.closed = 1;
    pthread_cond_broadcast(&st.cv);
    pthread_mutex_unlock(&st.m);
    return NULL;
}

static pthread_t status_thread;

/* Drop every printer-state-reason this filter can set. Runs at start (leftovers from a job
   that died) and from atexit(), so a problem never outlives the job that reported it,
   even when we exit through die() because the backend went away. */
static void clear_reasons(void)
{
    fprintf(stderr, "STATE: -media-needed-error,media-empty-error,media-jam-error,"
                    "cover-open-error,toner-empty-error,toner-low-warning,other-error,"
                    "offline-report\n");
}

static void status_start(void)
{
    const char *lang = getenv("LANG");
    st.polish = lang && !strncmp(lang, "pl", 2);
    clear_reasons();
    atexit(clear_reasons);
    struct stat sb;
    if (!getenv("DEVICE_URI") || fstat(BC_FD, &sb) != 0 ||
        !(S_ISFIFO(sb.st_mode) || S_ISSOCK(sb.st_mode)))
        return;
    st.active = pthread_create(&status_thread, NULL, status_reader, NULL) == 0;
}

/* After the job is sent: wait until the printer is done with it (or nothing is to come). */
static void status_finish(void)
{
    if (!st.active)
        return;
    time_t begin = time(NULL);
    pthread_mutex_lock(&st.m);
    for (;;) {
        time_t now = time(NULL);
        if (st.job_end || st.closed || canceled)
            break;
        if (!st.got_data && now - begin >= 5)
            break; /* printer does not talk to us */
        if (st.got_data && !st.cur && now - st.last_data >= 20)
            break; /* quiet and no problem pending: done */
        if (now - begin >= 3 * 3600)
            break;
        struct timespec ts = {now + 1, 0};
        pthread_cond_timedwait(&st.cv, &st.m, &ts);
    }
    if (st.cur && !canceled && !st.closed && !st.job_end)
        fprintf(stderr, "WARNING: gave up waiting for the printer\n");
    if (st.cur)
        fprintf(stderr, "STATE: -%s-%s\n", st.cur->reason, st.cur->severity);
    st.cur = NULL;
    st.stop = 1;
    pthread_mutex_unlock(&st.m);
    pthread_join(status_thread, NULL);
}

/* ---------------------------------------------------------------------------------------
 * main
 */

int main(int argc, char **argv)
{
    if (argc < 6 || argc > 7) {
        fprintf(stderr, "Usage: %s job-id user title copies options [file]\n", argv[0]);
        return 1;
    }
    setbuf(stderr, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    /* if the backend dies we want a write error (and a clean exit), not death by SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    int fd = 0;
    if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
        fprintf(stderr, "ERROR: unable to open %s: %s\n", argv[6], strerror(errno));
        return 1;
    }
    cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras)
        die("unable to read raster data");

    job_t job;
    job_init(&job, argv[3], argv[5]);
    status_start();

    FILE *out = stdout;
    FILE *spool = NULL;
    spooled_t *pages = NULL;
    int npages = 0, cap = 0;
    page_t pg, last = {0};
    cups_page_header2_t h;
    int ok = 1;

    while (!canceled && cupsRasterReadHeader2(ras, &h)) {
        if (!page_setup(&pg, &h, &job)) {
            ok = 0;
            break;
        }
        if (npages == 0) {
            write_job_start(out, &job);
            if (job.duplex && !(spool = open_spool())) {
                fprintf(stderr, "WARNING: cannot create spool file, printing one-sided\n");
                job.duplex = 0;
            }
        }
        FILE *dst = job.duplex ? spool : out;
        long start = job.duplex ? ftell(spool) : 0;
        render_page(dst, ras, &pg, npages + 1);
        if (job.duplex) {
            if (npages == cap) {
                cap = cap ? cap * 2 : 16;
                pages = realloc(pages, (size_t)cap * sizeof(*pages));
                if (!pages)
                    die("out of memory");
            }
            pages[npages].off = start;
            pages[npages].len = ftell(spool) - start;
        } else {
            fflush(out);
        }
        npages++;
        last = pg;
        fprintf(stderr, "PAGE: %d %u\n", npages, h.NumCopies ? h.NumCopies : 1);
    }

    if (npages > 0 && job.duplex) {
        fflush(spool);
        if (npages == 1) {
            /* nothing to put on the back: just print it */
            copy_range(out, spool, pages[0].off, pages[0].len);
        } else {
            if (npages & 1) {
                long start = ftell(spool);
                if (fseek(spool, 0, SEEK_END) == 0)
                    start = ftell(spool);
                render_page(spool, NULL, &last, npages + 1);
                fflush(spool);
                if (npages == cap)
                    pages = realloc(pages, (size_t)(cap + 1) * sizeof(*pages));
                pages[npages].off = start;
                pages[npages].len = ftell(spool) - start;
                npages++;
            }
            /* backs (even pages) last-to-first, then the printer pauses for the user to
               reload the stack, then the fronts (odd pages) first-to-last */
            for (int i = npages - 1; i >= 1; i -= 2)
                copy_range(out, spool, pages[i].off, pages[i].len);
            fprintf(stderr, "INFO: Manual duplex: when the printer pauses, put the printed "
                            "stack back in the tray printed side down and press the button\n");
            zjs_record(out, ZJT_PAUSE, NULL, 0, NULL, 0);
            for (int i = 0; i < npages; i += 2)
                copy_range(out, spool, pages[i].off, pages[i].len);
        }
        fclose(spool);
    }
    if (npages > 0)
        write_job_end(out);
    fflush(out);
    status_finish(); /* stdout stays open so the backend keeps relaying printer status */

    free(pages);
    cupsRasterClose(ras);
    if (fd)
        close(fd);
    if (canceled)
        fprintf(stderr, "INFO: Job canceled\n");
    return ok ? 0 : 1;
}
