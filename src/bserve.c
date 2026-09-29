/*
 * bserve — binary HTTP server
 *
 * Usage: ./bserve <root_dir> <port>
 *
 * Serves files from root_dir over the bhttp binary protocol.
 * One connection at a time, keeps it alive for multiple requests.
 */

#include "bhttp.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <errno.h>

/*
 * Build and send a complete RESPONSE frame.
 * Always includes Content-Type and Content-Length headers.
 * Returns 0 on success, -1 if the write fails.
 */
static int send_response(int fd, uint16_t status, const char *ctype,
                         const uint8_t *body, uint32_t body_len)
{
    char len_str[16];
    snprintf(len_str, sizeof(len_str), "%u", body_len);

    uint16_t ctype_vlen = (uint16_t)strlen(ctype);
    uint16_t clen_vlen  = (uint16_t)strlen(len_str);

    /*
     * payload = status(2)
     *         + Content-Type:  id(1) + value_len(2) + value
     *         + Content-Length: id(1) + value_len(2) + value
     *         + body
     */
    uint32_t headers_size = (1 + 2 + ctype_vlen) + (1 + 2 + clen_vlen);
    uint32_t payload_len  = 2 + headers_size + body_len;

    struct bhttp_frame f = {
        .version      = BHTTP_VERSION,
        .type         = FRAME_RESPONSE,
        .flags        = 0x00,
        .header_count = 2,
        .length       = payload_len
    };

    /* one big buffer = one write_all call, no partial-frame risk */
    uint32_t total = FRAME_HDR_SIZE + payload_len;
    uint8_t *buf = malloc(total);
    if (!buf) return -1;

    frame_pack(&f, buf);
    uint8_t *p = buf + FRAME_HDR_SIZE;

    /* status — 2 bytes big-endian */
    uint16_t net_status = htons(status);
    memcpy(p, &net_status, 2);
    p += 2;

    /* Content-Type (predefined ID 1) */
    *p++ = 1;
    uint16_t nv = htons(ctype_vlen);
    memcpy(p, &nv, 2); p += 2;
    memcpy(p, ctype, ctype_vlen); p += ctype_vlen;

    /* Content-Length (predefined ID 2) */
    *p++ = 2;
    nv = htons(clen_vlen);
    memcpy(p, &nv, 2); p += 2;
    memcpy(p, len_str, clen_vlen); p += clen_vlen;

    /* body */
    if (body_len > 0)
        memcpy(p, body, body_len);

    int ret = (write_all(fd, buf, total) < 0) ? -1 : 0;
    free(buf);
    return ret;
}

static int send_error(int fd, uint16_t status, const char *msg)
{
    return send_response(fd, status, "text/plain",
                         (const uint8_t *)msg, (uint32_t)strlen(msg));
}

/* slurp an entire file into a malloc'd buffer */
static uint8_t *read_file(const char *path, uint32_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz < 0 || (unsigned long)sz > MAX_PAYLOAD) {
        fclose(fp);
        return NULL;
    }

    /* empty file is valid — return a non-NULL pointer so caller
       doesn't confuse it with "file not found" */
    if (sz == 0) {
        fclose(fp);
        *out_len = 0;
        return malloc(1);
    }

    uint8_t *data = malloc((size_t)sz);
    if (!data) { fclose(fp); return NULL; }

    if (fread(data, 1, (size_t)sz, fp) != (size_t)sz) {
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_len = (uint32_t)sz;
    return data;
}

/*
 * Per-connection loop. Reads frames until the client disconnects
 * or we hit something unrecoverable (bad frame header = broken stream).
 */
static void handle_connection(int cfd, const char *root)
{
    uint8_t hdr_buf[FRAME_HDR_SIZE];

    for (;;) {
        ssize_t n = read_exact(cfd, hdr_buf, FRAME_HDR_SIZE);
        if (n <= 0) break;          /* EOF or error — done with this client */

        struct bhttp_frame f;
        if (frame_unpack(hdr_buf, &f) < 0) {
            /* bad version or insane length — stream is desynchronized */
            send_error(cfd, 400, "bad frame header\n");
            break;
        }

        /*
         * Forward-compat: unknown frame type → skip its payload, move on.
         * We don't know what it is, but we know how long it is.
         */
        if (f.type != FRAME_REQUEST) {
            if (f.length > 0) {
                uint8_t *skip = malloc(f.length);
                if (!skip) break;
                if (read_exact(cfd, skip, f.length) <= 0) {
                    free(skip);
                    break;
                }
                free(skip);
            }
            continue;
        }

        /* need at least 1 byte for path_len */
        if (f.length == 0) {
            send_error(cfd, 400, "empty request\n");
            continue;
        }

        /* read the full request payload */
        uint8_t *payload = malloc(f.length);
        if (!payload) break;

        if (read_exact(cfd, payload, f.length) <= 0) {
            free(payload);
            break;
        }

        /* parse: path_len(1) + path(path_len) + headers (we ignore them) */
        uint8_t path_len = payload[0];
        if (path_len == 0 || 1 + path_len > f.length) {
            free(payload);
            send_error(cfd, 400, "invalid path length\n");
            continue;
        }

        /* path buf is 256, path_len is uint8_t (max 255) — always fits */
        char path[256];
        memcpy(path, payload + 1, path_len);
        path[path_len] = '\0';
        free(payload);  /* done parsing — headers ignored for a simple file server */

        if (!path_is_safe(path)) {
            send_error(cfd, 400, "bad path\n");
            continue;
        }

        /* strip leading slash if present */
        const char *rel = path;
        if (rel[0] == '/') rel++;

        /* resolve against document root */
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", root, rel);

        uint32_t body_len = 0;
        uint8_t *body = read_file(full_path, &body_len);
        if (!body) {
            fprintf(stderr, "  %s -> 404\n", path);
            send_error(cfd, 404, "not found\n");
            continue;
        }

        fprintf(stderr, "  %s -> 200 (%u bytes)\n", path, body_len);
        send_response(cfd, 200, mime_from_ext(path), body, body_len);
        free(body);
    }

    close(cfd);
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <root_dir> <port>\n", argv[0]);
        return 1;
    }

    const char *root = argv[1];
    int port = atoi(argv[2]);

    /* SIGPIPE kills the process on broken connections — ignore it,
       we'll catch the write error from write_all instead */
    signal(SIGPIPE, SIG_IGN);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }

    /* so we can restart immediately without waiting for TIME_WAIT */
    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)port),
        .sin_addr   = { .s_addr = INADDR_ANY }
    };

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(sfd); return 1;
    }
    if (listen(sfd, 5) < 0) {
        perror("listen"); close(sfd); return 1;
    }

    fprintf(stderr, "bserve: listening on port %d, root=%s\n", port, root);

    /* ponytail: single-threaded accept loop, add fork() if concurrency needed */
    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) { perror("accept"); continue; }
        fprintf(stderr, "connection accepted\n");
        handle_connection(cfd, root);
        fprintf(stderr, "connection closed\n");
    }
}
