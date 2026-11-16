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
 * Localized messages
 */

/* Messages shown in the print queue, in the language CUPS passes in LANG (see
   detect_lang()). A missing translation falls back to English.

   Every language uses one fixed term for each part of the printer, the same in every
   message, following HP's own wording where known (e.g. "maintaining the paper
   orientation" for the duplex reload):

         input tray           output bin         cartridge door           toner cartridge
   PL    podajnik             taca odbiorcza     drzwiczki kasety z t.    kaseta z tonerem
   DE    Zufuhrfach           Ausgabefach        Patronenklappe           Tonerpatrone
   FR    bac d'alimentation   bac de sortie      porte de la cartouche    cartouche de toner
   ES    bandeja de entrada   bandeja de salida  puerta del cartucho      cartucho de tóner
   IT    vassoio di alim.     scomparto uscita   sportello cartuccia      cartuccia del toner
   PT    bandeja de entrada   bandeja de saída   porta do cartucho        cartucho de toner
   NL    invoerlade           uitvoerbak         klep van de cartridge    tonercartridge
   RU    входной лоток        выходной лоток     дверца картриджа         картридж с тонером
   JA    給紙トレイ           排紙トレイ         カートリッジドア         トナーカートリッジ
   ZH-S  进纸盒               出纸槽             硒鼓仓门                 硒鼓
   ZH-T  進紙匣               出紙槽             碳粉匣門                 碳粉匣
   KO    입력 용지함          출력 용지함        카트리지 덮개            토너 카트리지

   Problems are "Topic: what to do." sentences; progress messages have no final period. */
enum { L_EN, L_PL, L_DE, L_FR, L_ES, L_IT, L_PT, L_NL, L_RU, L_JA, L_ZH_HANS, L_ZH_HANT,
       L_KO, NLANG };
typedef const char *msg_t[NLANG];

/* the P1100 series has no Continue button (the only button is power): closing the
   cartridge door is what makes the printer resume after the duplex pause */
static const msg_t msg_duplex = {
    [L_EN] = "Manual duplex: take the printed stack out of the output bin and, without changing "
             "its orientation, put it into the input tray printed side down. Then open and close "
             "the cartridge door to continue.",
    [L_PL] = "Druk dwustronny: wyjmij zadrukowany stos z tacy odbiorczej i, nie zmieniając jego "
             "ułożenia, włóż go do podajnika zadrukowaną stroną w dół. Potem otwórz i zamknij "
             "drzwiczki kasety z tonerem, żeby kontynuować.",
    [L_DE] = "Manueller Duplexdruck: Nehmen Sie den bedruckten Stapel aus dem Ausgabefach und "
             "legen Sie ihn, ohne seine Ausrichtung zu ändern, mit der bedruckten Seite nach unten "
             "in das Zufuhrfach. Öffnen und schließen Sie dann die Patronenklappe, um "
             "fortzufahren.",
    [L_FR] = "Recto verso manuel : retirez la pile imprimée du bac de sortie et, sans changer son "
             "orientation, placez-la dans le bac d'alimentation, face imprimée vers le bas. "
             "Ouvrez puis refermez ensuite la porte de la cartouche pour continuer.",
    [L_ES] = "Dúplex manual: saque la pila impresa de la bandeja de salida y, sin cambiar su "
             "orientación, colóquela en la bandeja de entrada con la cara impresa hacia abajo. "
             "Después abra y cierre la puerta del cartucho para continuar.",
    [L_IT] = "Fronte-retro manuale: estrai la pila stampata dallo scomparto di uscita e, senza "
             "cambiarne l'orientamento, inseriscila nel vassoio di alimentazione con il lato "
             "stampato rivolto verso il basso. Poi apri e chiudi lo sportello della cartuccia per "
             "continuare.",
    [L_PT] = "Duplex manual: retire a pilha impressa da bandeja de saída e, sem alterar a "
             "orientação, coloque-a na bandeja de entrada com o lado impresso para baixo. Em "
             "seguida, abra e feche a porta do cartucho para continuar.",
    [L_NL] = "Handmatig dubbelzijdig: haal de bedrukte stapel uit de uitvoerbak en leg hem, "
             "zonder de richting te veranderen, met de bedrukte kant naar beneden in de "
             "invoerlade. Open en sluit daarna de klep van de cartridge om verder te gaan.",
    [L_RU] = "Ручная двусторонняя печать: выньте отпечатанную стопку из выходного лотка и, не "
             "меняя её ориентацию, положите во входной лоток отпечатанной стороной вниз. Затем "
             "откройте и закройте дверцу картриджа, чтобы продолжить.",
    [L_JA] = "手動両面印刷: 印刷済みの用紙の束を排紙トレイから取り出し、向きを変えずに、印刷面を"
             "下にして給紙トレイにセットしてください。その後、カートリッジドアを開けて閉じると"
             "印刷が続行されます。",
    [L_ZH_HANS] = "手动双面打印：从出纸槽取出已打印的纸叠，保持纸张方向不变，请将打印面朝下放入"
                  "进纸盒。然后打开并关闭硒鼓仓门以继续打印。",
    [L_ZH_HANT] = "手動雙面列印：從出紙槽取出已列印的紙疊，保持紙張方向不變，請將列印面朝下放入"
                  "進紙匣。然後打開並關閉碳粉匣門以繼續列印。",
    [L_KO] = "수동 양면 인쇄: 출력 용지함에서 인쇄된 용지 묶음을 꺼내 방향을 바꾸지 말고 "
             "인쇄면이 아래로 향하게 하여 입력 용지함에 넣으십시오. 그런 다음 카트리지 덮개를 "
             "열었다 닫으면 인쇄가 계속됩니다.",
};

/* shown when a manual duplex job starts, before the printer pauses */
static const msg_t msg_duplex_hint = {
    [L_EN] = "Manual duplex: when the printer pauses, put the printed stack into the input tray "
             "without changing its orientation, printed side down. Then open and close the "
             "cartridge door.",
    [L_PL] = "Druk dwustronny: gdy drukarka się zatrzyma, włóż zadrukowany stos do podajnika, nie "
             "zmieniając jego ułożenia, zadrukowaną stroną w dół. Potem otwórz i zamknij "
             "drzwiczki kasety z tonerem.",
    [L_DE] = "Manueller Duplexdruck: Wenn der Drucker anhält, legen Sie den bedruckten Stapel, "
             "ohne seine Ausrichtung zu ändern, mit der bedruckten Seite nach unten in das "
             "Zufuhrfach. Öffnen und schließen Sie dann die Patronenklappe.",
    [L_FR] = "Recto verso manuel : quand l'imprimante s'arrête, placez la pile imprimée dans le bac "
             "d'alimentation sans changer son orientation, face imprimée vers le bas. Ouvrez puis "
             "refermez ensuite la porte de la cartouche.",
    [L_ES] = "Dúplex manual: cuando la impresora se detenga, coloque la pila impresa en la bandeja "
             "de entrada sin cambiar su orientación, con la cara impresa hacia abajo. Después abra "
             "y cierre la puerta del cartucho.",
    [L_IT] = "Fronte-retro manuale: quando la stampante si ferma, inserisci la pila stampata nel "
             "vassoio di alimentazione senza cambiarne l'orientamento, con il lato stampato "
             "rivolto verso il basso. Poi apri e chiudi lo sportello della cartuccia.",
    [L_PT] = "Duplex manual: quando a impressora pausar, coloque a pilha impressa na bandeja de "
             "entrada sem alterar a orientação, com o lado impresso para baixo. Em seguida, abra e "
             "feche a porta do cartucho.",
    [L_NL] = "Handmatig dubbelzijdig: leg de bedrukte stapel, wanneer de printer pauzeert, zonder "
             "de richting te veranderen met de bedrukte kant naar beneden in de invoerlade. Open "
             "en sluit daarna de klep van de cartridge.",
    [L_RU] = "Ручная двусторонняя печать: когда принтер остановится, положите отпечатанную стопку "
             "во входной лоток, не меняя её ориентацию, отпечатанной стороной вниз. Затем "
             "откройте и закройте дверцу картриджа.",
    [L_JA] = "手動両面印刷: プリンタが一時停止したら、印刷済みの用紙の束を向きを変えずに、"
             "印刷面を下にして給紙トレイにセットしてください。その後、カートリッジドアを開けて"
             "閉じてください。",
    [L_ZH_HANS] = "手动双面打印：打印机暂停后，保持纸张方向不变，请将已打印的纸叠打印面朝下放入"
                  "进纸盒。然后打开并关闭硒鼓仓门。",
    [L_ZH_HANT] = "手動雙面列印：印表機暫停後，保持紙張方向不變，請將已列印的紙疊列印面朝下放入"
                  "進紙匣。然後打開並關閉碳粉匣門。",
    [L_KO] = "수동 양면 인쇄: 프린터가 일시 정지하면 인쇄된 용지 묶음을 방향을 바꾸지 말고 "
             "인쇄면이 아래로 향하게 하여 입력 용지함에 넣으십시오. 그런 다음 카트리지 덮개를 "
             "열었다 닫으십시오.",
};

static const msg_t msg_load_paper = {
    [L_EN] = "Load paper into the input tray.",
    [L_PL] = "Włóż papier do podajnika.",
    [L_DE] = "Legen Sie Papier in das Zufuhrfach ein.",
    [L_FR] = "Chargez du papier dans le bac d'alimentation.",
    [L_ES] = "Cargue papel en la bandeja de entrada.",
    [L_IT] = "Carica la carta nel vassoio di alimentazione.",
    [L_PT] = "Coloque papel na bandeja de entrada.",
    [L_NL] = "Plaats papier in de invoerlade.",
    [L_RU] = "Положите бумагу во входной лоток.",
    [L_JA] = "給紙トレイに用紙をセットしてください。",
    [L_ZH_HANS] = "请在进纸盒中放入纸张。",
    [L_ZH_HANT] = "請在進紙匣中放入紙張。",
    [L_KO] = "입력 용지함에 용지를 넣으십시오.",
};

static const msg_t msg_out_of_paper = {
    [L_EN] = "Out of paper or the paper was not picked up: reload the paper in the input tray.",
    [L_PL] = "Brak papieru lub papier nie został pobrany: włóż papier ponownie do podajnika.",
    [L_DE] = "Kein Papier oder das Papier wurde nicht eingezogen: Legen Sie das Papier erneut in "
             "das Zufuhrfach ein.",
    [L_FR] = "Plus de papier ou le papier n'a pas été entraîné : rechargez le papier dans le bac "
             "d'alimentation.",
    [L_ES] = "Sin papel o el papel no se ha recogido: vuelva a cargar el papel en la bandeja de "
             "entrada.",
    [L_IT] = "Carta esaurita o non prelevata: ricarica la carta nel vassoio di alimentazione.",
    [L_PT] = "Sem papel ou o papel não foi puxado: recoloque o papel na bandeja de entrada.",
    [L_NL] = "Papier op of het papier wordt niet ingevoerd: plaats het papier opnieuw in de "
             "invoerlade.",
    [L_RU] = "Нет бумаги или бумага не захватывается: заново положите бумагу во входной лоток.",
    [L_JA] = "用紙切れ、または用紙が給紙されません: 給紙トレイに用紙をセットし直してください。",
    [L_ZH_HANS] = "缺纸或无法进纸：请将纸张重新放入进纸盒。",
    [L_ZH_HANT] = "缺紙或無法進紙：請將紙張重新放入進紙匣。",
    [L_KO] = "용지가 없거나 급지되지 않습니다: 입력 용지함에 용지를 다시 넣으십시오.",
};

static const msg_t msg_jam = {
    [L_EN] = "Paper jam: open the cartridge door and remove the jammed paper.",
    [L_PL] = "Zacięcie papieru: otwórz drzwiczki kasety z tonerem i wyjmij zacięty papier.",
    [L_DE] = "Papierstau: Öffnen Sie die Patronenklappe und entfernen Sie das gestaute Papier.",
    [L_FR] = "Bourrage papier : ouvrez la porte de la cartouche et retirez le papier coincé.",
    [L_ES] = "Atasco de papel: abra la puerta del cartucho y retire el papel atascado.",
    [L_IT] = "Inceppamento della carta: apri lo sportello della cartuccia e rimuovi la carta "
             "inceppata.",
    [L_PT] = "Atolamento de papel: abra a porta do cartucho e remova o papel atolado.",
    [L_NL] = "Papierstoring: open de klep van de cartridge en verwijder het vastgelopen papier.",
    [L_RU] = "Замятие бумаги: откройте дверцу картриджа и извлеките застрявшую бумагу.",
    [L_JA] = "紙詰まり: カートリッジドアを開けて、詰まった用紙を取り除いてください。",
    [L_ZH_HANS] = "卡纸：请打开硒鼓仓门并取出卡住的纸张。",
    [L_ZH_HANT] = "卡紙：請打開碳粉匣門並取出卡住的紙張。",
    [L_KO] = "용지 걸림: 카트리지 덮개를 열고 걸린 용지를 제거하십시오.",
};

static const msg_t msg_cover_open = {
    [L_EN] = "The cartridge door is open or the toner cartridge is missing.",
    [L_PL] = "Drzwiczki kasety z tonerem są otwarte albo brak kasety z tonerem.",
    [L_DE] = "Die Patronenklappe ist offen oder die Tonerpatrone fehlt.",
    [L_FR] = "La porte de la cartouche est ouverte ou la cartouche de toner est absente.",
    [L_ES] = "La puerta del cartucho está abierta o falta el cartucho de tóner.",
    [L_IT] = "Lo sportello della cartuccia è aperto o manca la cartuccia del toner.",
    [L_PT] = "A porta do cartucho está aberta ou o cartucho de toner não está instalado.",
    [L_NL] = "De klep van de cartridge is open of de tonercartridge ontbreekt.",
    [L_RU] = "Дверца картриджа открыта или картридж с тонером отсутствует.",
    [L_JA] = "カートリッジドアが開いているか、トナーカートリッジがありません。",
    [L_ZH_HANS] = "硒鼓仓门已打开或未安装硒鼓。",
    [L_ZH_HANT] = "碳粉匣門已打開或未安裝碳粉匣。",
    [L_KO] = "카트리지 덮개가 열려 있거나 토너 카트리지가 없습니다.",
};

static const msg_t msg_no_toner = {
    [L_EN] = "The toner cartridge is missing or not seated correctly.",
    [L_PL] = "Brak kasety z tonerem albo jest źle włożona.",
    [L_DE] = "Die Tonerpatrone fehlt oder ist nicht richtig eingesetzt.",
    [L_FR] = "La cartouche de toner est absente ou mal installée.",
    [L_ES] = "Falta el cartucho de tóner o no está bien colocado.",
    [L_IT] = "La cartuccia del toner manca o non è inserita correttamente.",
    [L_PT] = "O cartucho de toner não está instalado ou está mal encaixado.",
    [L_NL] = "De tonercartridge ontbreekt of zit niet goed.",
    [L_RU] = "Картридж с тонером отсутствует или установлен неправильно.",
    [L_JA] = "トナーカートリッジがないか、正しく取り付けられていません。",
    [L_ZH_HANS] = "未安装硒鼓或硒鼓未装好。",
    [L_ZH_HANT] = "未安裝碳粉匣或碳粉匣未裝好。",
    [L_KO] = "토너 카트리지가 없거나 제대로 장착되지 않았습니다.",
};

static const msg_t msg_toner_low = {
    [L_EN] = "The toner is low.",
    [L_PL] = "Kończy się toner.",
    [L_DE] = "Der Toner ist fast leer.",
    [L_FR] = "Le niveau de toner est bas.",
    [L_ES] = "Queda poco tóner.",
    [L_IT] = "Il toner sta per esaurirsi.",
    [L_PT] = "O toner está acabando.",
    [L_NL] = "De toner is bijna op.",
    [L_RU] = "Заканчивается тонер.",
    [L_JA] = "トナーが残り少なくなっています。",
    [L_ZH_HANS] = "碳粉不足。",
    [L_ZH_HANT] = "碳粉不足。",
    [L_KO] = "토너가 부족합니다.",
};

static const msg_t msg_offline = {
    [L_EN] = "The printer is offline.",
    [L_PL] = "Drukarka jest offline.",
    [L_DE] = "Der Drucker ist offline.",
    [L_FR] = "L'imprimante est hors ligne.",
    [L_ES] = "La impresora está fuera de línea.",
    [L_IT] = "La stampante è offline.",
    [L_PT] = "A impressora está offline.",
    [L_NL] = "De printer is offline.",
    [L_RU] = "Принтер не в сети.",
    [L_JA] = "プリンタがオフラインです。",
    [L_ZH_HANS] = "打印机处于脱机状态。",
    [L_ZH_HANT] = "印表機處於離線狀態。",
    [L_KO] = "프린터가 오프라인 상태입니다.",
};

/* printf format: %d is the printer's status code */
static const msg_t msg_error = {
    [L_EN] = "Printer error %d: turn the printer off and on again.",
    [L_PL] = "Błąd drukarki %d: wyłącz i włącz ponownie drukarkę.",
    [L_DE] = "Druckerfehler %d: Schalten Sie den Drucker aus und wieder ein.",
    [L_FR] = "Erreur d'imprimante %d : éteignez puis rallumez l'imprimante.",
    [L_ES] = "Error de impresora %d: apague la impresora y vuelva a encenderla.",
    [L_IT] = "Errore della stampante %d: spegni e riaccendi la stampante.",
    [L_PT] = "Erro da impressora %d: desligue e ligue novamente a impressora.",
    [L_NL] = "Printerfout %d: zet de printer uit en weer aan.",
    [L_RU] = "Ошибка принтера %d: выключите и снова включите принтер.",
    [L_JA] = "プリンタエラー %d: プリンタの電源を切って入れ直してください。",
    [L_ZH_HANS] = "打印机错误 %d：请关闭打印机电源后重新打开。",
    [L_ZH_HANT] = "印表機錯誤 %d：請關閉印表機電源後重新開啟。",
    [L_KO] = "프린터 오류 %d: 프린터를 껐다가 다시 켜십시오.",
};

static const msg_t msg_printing = {
    [L_EN] = "Printing...",
    [L_PL] = "Drukowanie...",
    [L_DE] = "Drucken...",
    [L_FR] = "Impression...",
    [L_ES] = "Imprimiendo...",
    [L_IT] = "Stampa in corso...",
    [L_PT] = "Imprimindo...",
    [L_NL] = "Bezig met afdrukken...",
    [L_RU] = "Печать...",
    [L_JA] = "印刷中...",
    [L_ZH_HANS] = "正在打印...",
    [L_ZH_HANT] = "正在列印...",
    [L_KO] = "인쇄 중...",
};

/* printf format: %d is the page number */
static const msg_t msg_printed_page = {
    [L_EN] = "Printed page %d",
    [L_PL] = "Wydrukowano stronę %d",
    [L_DE] = "Seite %d gedruckt",
    [L_FR] = "Page %d imprimée",
    [L_ES] = "Página %d impresa",
    [L_IT] = "Pagina %d stampata",
    [L_PT] = "Página %d impressa",
    [L_NL] = "Pagina %d afgedrukt",
    [L_RU] = "Напечатана страница %d",
    [L_JA] = "%d ページを印刷しました",
    [L_ZH_HANS] = "已打印第 %d 页",
    [L_ZH_HANT] = "已列印第 %d 頁",
    [L_KO] = "%d페이지를 인쇄했습니다",
};

/* printf format: page number, then percent; every translation keeps that order */
static const msg_t msg_page_progress = {
    [L_EN] = "Printing page %d, %d%% complete...",
    [L_PL] = "Drukowanie strony %d, ukończono %d%%...",
    [L_DE] = "Seite %d wird gedruckt, %d%% abgeschlossen...",
    [L_FR] = "Impression de la page %d, %d%% terminé...",
    [L_ES] = "Imprimiendo página %d, %d%% completado...",
    [L_IT] = "Stampa della pagina %d, %d%% completato...",
    [L_PT] = "Imprimindo página %d, %d%% concluído...",
    [L_NL] = "Bezig met afdrukken van pagina %d, %d%% voltooid...",
    [L_RU] = "Печать страницы %d, выполнено %d%%...",
    [L_JA] = "%d ページを印刷中、%d%% 完了...",
    [L_ZH_HANS] = "正在打印第 %d 页，已完成 %d%%...",
    [L_ZH_HANT] = "正在列印第 %d 頁，已完成 %d%%...",
    [L_KO] = "%d페이지 인쇄 중, %d%% 완료...",
};

static int ui_lang = L_EN; /* L_*, set by status_start() */

static const char *tr(const msg_t *m)
{
    const char *s = (*m)[ui_lang];
    return s ? s : (*m)[L_EN];
}

/* Language of the status messages from LANG, which CUPS sets from the job's
   natural language (e.g. "de_DE.UTF-8", "zh_TW.UTF-8", "pt-BR"). */
static int detect_lang(void)
{
    static const struct {
        const char *prefix;
        int lang;
    } map[] = {{"pl", L_PL}, {"de", L_DE}, {"fr", L_FR}, {"es", L_ES}, {"it", L_IT},
               {"pt", L_PT}, {"nl", L_NL}, {"ru", L_RU}, {"ja", L_JA}, {"ko", L_KO}};
    const char *lang = getenv("LANG");
    if (!lang || strlen(lang) < 2 || (lang[2] && lang[2] != '_' && lang[2] != '-' &&
                                      lang[2] != '.'))
        return L_EN;
    if (!strncasecmp(lang, "zh", 2)) {
        /* Traditional for Taiwan, Hong Kong, Macau and explicit Hant, else Simplified */
        static const char *const hant[] = {"hant", "tw", "hk", "mo"};
        for (const char *p = lang + 2; *p && *p != '.' && *p != '@'; p++) {
            if (*p != '_' && *p != '-')
                continue;
            for (size_t i = 0; i < sizeof(hant) / sizeof(hant[0]); i++) {
                size_t n = strlen(hant[i]);
                if (!strncasecmp(p + 1, hant[i], n) &&
                    (!p[1 + n] || strchr("_-.@", p[1 + n])))
                    return L_ZH_HANT;
            }
        }
        return L_ZH_HANS;
    }
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strncasecmp(lang, map[i].prefix, 2))
            return map[i].lang;
    return L_EN;
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
            fprintf(stderr, "INFO: ");
            fprintf(stderr, tr(&msg_page_progress), pageno, pct);
            fprintf(stderr, "\n");
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
    const msg_t *msg;
} status_code_t;

static const status_code_t status_codes[] = {
    {40120, 40121, "media-needed", "error", &msg_duplex},
    {40131, 40131, "media-needed", "error", &msg_duplex},
    {41100, 41199, "media-needed", "error", &msg_load_paper},
    {41000, 41999, "media-empty", "error", &msg_out_of_paper},
    {42000, 42999, "media-jam", "error", &msg_jam},
    {40022, 40022, "media-jam", "error", &msg_jam},
    {40021, 40021, "cover-open", "error", &msg_cover_open},
    {40010, 40010, "toner-empty", "error", &msg_no_toner},
    {40046, 40046, "toner-empty", "error", &msg_no_toner},
    {40038, 40038, "toner-low", "warning", &msg_toner_low},
    {10006, 10006, "toner-low", "warning", &msg_toner_low},
    {40079, 40079, "offline", "report", &msg_offline},
    {50000, 59999, "other", "error", &msg_error},
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
} st = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0, NULL, 0};

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
        fprintf(stderr, tr(sc->msg), code);
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "INFO: %s\n", tr(&msg_printing));
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
        if (p && !st.cur) {
            fprintf(stderr, "INFO: ");
            fprintf(stderr, tr(&msg_printed_page), atoi(p));
            fprintf(stderr, "\n");
        }
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
    ui_lang = detect_lang();
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
            fprintf(stderr, "INFO: %s\n", tr(&msg_duplex_hint));
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
