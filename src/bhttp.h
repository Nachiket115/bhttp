#ifndef BHTTP_H
#define BHTTP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>    /* strcasecmp */
#include <arpa/inet.h>

/* ── protocol constants ─────────────────────────────────────────────── */

#define BHTTP_VERSION   0x01

#define FRAME_REQUEST   0x01
#define FRAME_RESPONSE  0x02

#define FRAME_HDR_SIZE  8
#define MAX_PAYLOAD     (16 * 1024 * 1024)   /* 16 MB cap on payload */
#define MAX_HEADERS     64

/*
 * Predefined header IDs — saves sending common names on the wire.
 * ID 0 means "custom name follows", 1–10 map to this table.
 */
static const char *predefined_headers[] = {
    NULL,               /* 0 = custom */
    "Content-Type",     /* 1 */
    "Content-Length",    /* 2 */
    "Host",             /* 3 */
    "Connection",       /* 4 */
    "Date",             /* 5 */
    "Server",           /* 6 */
    "User-Agent",       /* 7 */
    "Accept",           /* 8 */
    "Cache-Control",    /* 9 */
    "Last-Modified"     /* 10 */
};
#define NUM_PREDEFINED  10

/* ── frame header ────────────────────────────────────────────────────
 *
 * 8 bytes, all fields byte-aligned, multi-byte fields big-endian:
 *
 *   offset  size  field
 *   0       1     version       (0x01)
 *   1       1     type          (0x01=REQ, 0x02=RESP)
 *   2       1     flags         (reserved, send 0x00)
 *   3       1     header_count
 *   4       4     length        (uint32, payload size after header)
 */
struct bhttp_frame {
    uint8_t  version;
    uint8_t  type;
    uint8_t  flags;
    uint8_t  header_count;
    uint32_t length;        /* host byte order in memory */
};

/* pack frame header into 8-byte wire format (big-endian length) */
static void frame_pack(const struct bhttp_frame *f, uint8_t buf[FRAME_HDR_SIZE])
{
    buf[0] = f->version;
    buf[1] = f->type;
    buf[2] = f->flags;
    buf[3] = f->header_count;
    uint32_t net_len = htonl(f->length);
    memcpy(buf + 4, &net_len, 4);
}

/*
 * Unpack 8 bytes into a frame header.
 * Returns 0 on success, -1 if version is wrong or length exceeds cap.
 * Does NOT reject unknown frame types — that's the caller's job
 * (forward-compat rule: skip unknown types, don't error).
 */
static int frame_unpack(const uint8_t buf[FRAME_HDR_SIZE], struct bhttp_frame *f)
{
    f->version = buf[0];
    f->type    = buf[1];
    f->flags   = buf[2];
    f->header_count = buf[3];

    uint32_t net_len;
    memcpy(&net_len, buf + 4, 4);
    f->length = ntohl(net_len);

    if (f->version != BHTTP_VERSION)
        return -1;
    if (f->length > MAX_PAYLOAD)
        return -1;

    return 0;
}

/* ── I/O helpers ─────────────────────────────────────────────────────
 *
 * Sockets can short-read/write, so we loop. These are the only two
 * places that call read()/write() on the socket fd.
 */

/* read exactly `count` bytes; returns count on success, 0 on clean EOF, -1 on error */
static ssize_t read_exact(int fd, void *buf, size_t count)
{
    size_t total = 0;
    while (total < count) {
        ssize_t n = read(fd, (char *)buf + total, count - total);
        if (n < 0)  return -1;
        if (n == 0) return 0;   /* peer closed */
        total += n;
    }
    return (ssize_t)total;
}

/* write all `count` bytes; returns count on success, -1 on error */
static ssize_t write_all(int fd, const void *buf, size_t count)
{
    size_t total = 0;
    while (total < count) {
        ssize_t n = write(fd, (const char *)buf + total, count - total);
        if (n <= 0) return -1;
        total += n;
    }
    return (ssize_t)total;
}

/* ── hexdump ─────────────────────────────────────────────────────────
 *
 * 16 bytes per line, hex on the left, printable ASCII on the right.
 * Output goes to stderr so it doesn't mix with the response body on stdout.
 */
static void hexdump(const char *label, const uint8_t *data, size_t len)
{
    fprintf(stderr, "--- %s (%zu bytes) ---\n", label, len);
    for (size_t i = 0; i < len; i += 16) {
        fprintf(stderr, "  %04zx  ", i);

        /* hex columns */
        for (size_t j = 0; j < 16; j++) {
            if (i + j < len)
                fprintf(stderr, "%02x ", data[i + j]);
            else
                fprintf(stderr, "   ");
            if (j == 7) fprintf(stderr, " ");
        }

        /* ascii sidebar */
        fprintf(stderr, " |");
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = data[i + j];
            fprintf(stderr, "%c", isprint(c) ? c : '.');
        }
        fprintf(stderr, "|\n");
    }
    fprintf(stderr, "---\n");
}

/* ── MIME type from file extension ───────────────────────────────────
 *
 * Tiny hardcoded table — covers the test files we ship, falls back
 * to octet-stream for anything else.
 */
static const char *mime_from_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html";
    if (strcmp(dot, ".txt") == 0)  return "text/plain";
    if (strcmp(dot, ".css") == 0)  return "text/css";
    if (strcmp(dot, ".js") == 0)   return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";
    return "application/octet-stream";
}

/* ── path safety ─────────────────────────────────────────────────────
 *
 * Reject any path with ".." to prevent directory traversal out of
 * the server's root. Intentionally strict — blocks edge cases like
 * a file literally named "weird..name" but that's fine for this project.
 */
static int path_is_safe(const char *path)
{
    if (strstr(path, "..") != NULL) return 0;
    return 1;
}

/* look up predefined header ID for a given name (case-insensitive) */
static uint8_t predefined_id(const char *name)
{
    for (int i = 1; i <= NUM_PREDEFINED; i++) {
        if (strcasecmp(predefined_headers[i], name) == 0)
            return (uint8_t)i;
    }
    return 0;   /* not predefined — caller must send name on the wire */
}

#endif /* BHTTP_H */
