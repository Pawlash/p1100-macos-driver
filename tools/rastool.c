/*
 * rastool - test helper for CUPS raster files.
 *
 *   rastool info in.ras
 *   rastool pattern in.ras out.ras KIND [pages]   rewrite pixels, keep headers
 *       KIND: white black ramp levels noise mixed
 *   rastool set in.ras out.ras FIELD=VALUE ...    change header fields, keep pixels
 *       FIELD: mediatype mediaposition copies duplex tumble compression
 *              width height yres pagesize (WxH points)
 */
#include <cups/raster.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static void info(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); exit(1); }
    cups_raster_t *r = cupsRasterOpen(fd, CUPS_RASTER_READ);
    cups_page_header2_t h;
    int page = 0;
    while (cupsRasterReadHeader2(r, &h)) {
        page++;
        printf("page %d: %ux%u bpl=%u bpc=%u bpp=%u cs=%u order=%u res=%ux%u\n",
               page, h.cupsWidth, h.cupsHeight, h.cupsBytesPerLine, h.cupsBitsPerColor,
               h.cupsBitsPerPixel, h.cupsColorSpace, h.cupsColorOrder,
               h.HWResolution[0], h.HWResolution[1]);
        printf("  PageSize=%ux%u ImagingBB=%u,%u,%u,%u Margins=%u,%u cupsPageSize=%.2fx%.2f\n",
               h.PageSize[0], h.PageSize[1], h.ImagingBoundingBox[0], h.ImagingBoundingBox[1],
               h.ImagingBoundingBox[2], h.ImagingBoundingBox[3], h.Margins[0], h.Margins[1],
               h.cupsPageSize[0], h.cupsPageSize[1]);
        printf("  MediaType='%s' cupsMediaType=%u MediaPosition=%u NumCopies=%u Duplex=%u Tumble=%u"
               " cupsCompression=%u ManualFeed=%u cupsPageSizeName='%s' MediaClass='%s'\n",
               h.MediaType, h.cupsMediaType, h.MediaPosition, h.NumCopies, h.Duplex, h.Tumble,
               h.cupsCompression, h.ManualFeed, h.cupsPageSizeName, h.MediaClass);
        unsigned char *line = malloc(h.cupsBytesPerLine);
        unsigned long nonwhite = 0;
        for (unsigned y = 0; y < h.cupsHeight; y++) {
            cupsRasterReadPixels(r, line, h.cupsBytesPerLine);
            for (unsigned x = 0; x < h.cupsBytesPerLine; x++) nonwhite += line[x] != 0xff;
        }
        printf("  non-white bytes: %lu\n", nonwhite);
        free(line);
    }
    cupsRasterClose(r);
    close(fd);
}

static unsigned char pix(const char *kind, unsigned x, unsigned y, unsigned w, unsigned h, int page)
{
    if (!strcmp(kind, "white")) return 0xff;
    if (!strcmp(kind, "black")) return 0x00;
    if (!strcmp(kind, "ramp")) return (unsigned char)(255 - (x * 256 / w));
    if (!strcmp(kind, "levels")) {          /* 64 horizontal bands, each a gray level */
        unsigned band = y * 64 / h;
        return (unsigned char)(255 - band * 4 - (x * 4 / w));
    }
    if (!strcmp(kind, "noise")) return (unsigned char)rnd();
    if (!strcmp(kind, "mixed")) {           /* text-like blocks, ramps, noise, solid areas */
        unsigned zone = (y * 6 / h + page) % 6;
        switch (zone) {
        case 0: return ((x / 7 + y / 11) % 5 == 0 && (y % 40) < 28) ? 0 : 0xff;
        case 1: return (unsigned char)(255 - (x * 256 / w));
        case 2: return (unsigned char)((rnd() & 0x80) ? 0xff : rnd());
        case 3: return (x % 300 < 150) ? 0x00 : 0xff;
        case 4: return (unsigned char)((x ^ y) & 0xff);
        default: return (y % 3 == 0 || x % 5 == 0) ? 0x40 : 0xff;
        }
    }
    fprintf(stderr, "unknown kind %s\n", kind);
    exit(1);
}

static void apply_set(cups_page_header2_t *h, const char *kv)
{
    char key[64];
    const char *eq = strchr(kv, '=');
    if (!eq || eq - kv >= (long)sizeof(key)) { fprintf(stderr, "bad %s\n", kv); exit(1); }
    memcpy(key, kv, eq - kv); key[eq - kv] = 0;
    const char *v = eq + 1;
    unsigned n = (unsigned)strtoul(v, NULL, 10);
    if (!strcmp(key, "mediatype")) h->cupsMediaType = n;
    else if (!strcmp(key, "mediatypename")) strlcpy(h->MediaType, v, sizeof(h->MediaType));
    else if (!strcmp(key, "mediaposition")) h->MediaPosition = n;
    else if (!strcmp(key, "copies")) h->NumCopies = n;
    else if (!strcmp(key, "duplex")) h->Duplex = n;
    else if (!strcmp(key, "tumble")) h->Tumble = n;
    else if (!strcmp(key, "compression")) h->cupsCompression = n;
    else if (!strcmp(key, "width")) { h->cupsWidth = n; h->cupsBytesPerLine = n * h->cupsBitsPerPixel / 8; }
    else if (!strcmp(key, "height")) h->cupsHeight = n;
    else if (!strcmp(key, "yres")) h->HWResolution[1] = n;
    else if (!strcmp(key, "pagesize")) {
        unsigned a, b;
        if (sscanf(v, "%ux%u", &a, &b) != 2) { fprintf(stderr, "bad pagesize\n"); exit(1); }
        h->PageSize[0] = a; h->PageSize[1] = b;
        h->cupsPageSize[0] = (float)a; h->cupsPageSize[1] = (float)b;
    } else if (!strcmp(key, "pagesizename")) strlcpy(h->cupsPageSizeName, v, sizeof(h->cupsPageSizeName));
    else { fprintf(stderr, "unknown field %s\n", key); exit(1); }
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "info")) { info(argv[2]); return 0; }
    if (argc < 5 || (strcmp(argv[1], "pattern") && strcmp(argv[1], "set"))) {
        fprintf(stderr, "usage: see source\n");
        return 1;
    }
    int setmode = !strcmp(argv[1], "set");
    int ifd = open(argv[2], O_RDONLY);
    int ofd = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ifd < 0 || ofd < 0) { perror("open"); return 1; }
    cups_raster_t *in = cupsRasterOpen(ifd, CUPS_RASTER_READ);
    cups_raster_t *out = cupsRasterOpen(ofd, CUPS_RASTER_WRITE);
    int pages = (!setmode && argc > 5) ? atoi(argv[5]) : 0;
    cups_page_header2_t h;
    int page = 0;
    while (cupsRasterReadHeader2(in, &h)) {
        cups_page_header2_t oh = h;
        if (setmode)
            for (int i = 4; i < argc; i++) apply_set(&oh, argv[i]);
        unsigned char *line = calloc(1, oh.cupsBytesPerLine > h.cupsBytesPerLine ? oh.cupsBytesPerLine : h.cupsBytesPerLine);
        int reps = pages ? pages : 1;
        for (int rep = 0; rep < reps; rep++) {
            cupsRasterWriteHeader2(out, &oh);
            for (unsigned y = 0; y < oh.cupsHeight; y++) {
                if (setmode) {
                    if (y < h.cupsHeight) cupsRasterReadPixels(in, line, h.cupsBytesPerLine);
                    else memset(line, 0xff, oh.cupsBytesPerLine);
                    if (oh.cupsBytesPerLine > h.cupsBytesPerLine)
                        memset(line + h.cupsBytesPerLine, 0xff, oh.cupsBytesPerLine - h.cupsBytesPerLine);
                } else {
                    for (unsigned x = 0; x < oh.cupsWidth; x++)
                        line[x] = pix(argv[4], x, y, oh.cupsWidth, oh.cupsHeight, page + rep);
                }
                cupsRasterWritePixels(out, line, oh.cupsBytesPerLine);
            }
            if (!setmode && rep == 0)
                for (unsigned y = 0; y < h.cupsHeight; y++) cupsRasterReadPixels(in, line, h.cupsBytesPerLine);
        }
        if (setmode && oh.cupsHeight < h.cupsHeight)
            for (unsigned y = oh.cupsHeight; y < h.cupsHeight; y++) cupsRasterReadPixels(in, line, h.cupsBytesPerLine);
        free(line);
        page++;
    }
    cupsRasterClose(in);
    cupsRasterClose(out);
    close(ifd);
    close(ofd);
    return 0;
}
