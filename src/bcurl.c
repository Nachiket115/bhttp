/*
 * bcurl — binary HTTP client
 *
 * Usage: ./bcurl [-v] host:port/path [path2 ...]
 *
 * Sends one REQUEST per path, all on a single TCP connection (keepalive).
 * -v hexdumps every frame to stderr.
 * Exits non-zero if any response is 4xx/5xx.
 */

#include "bhttp.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>

/*
 * Build and send a REQUEST frame for the given path.
 * No request headers — just path_len + path.
 */
static int send_request(int fd, const char *path, int verbose)
{
    size_t raw_len = strlen(path);
    /* ponytail: paths > 255 bytes reject early; protocol path_len is uint8_t */
    if (raw_len > 255) {
        fprintf(stderr, "error: path exceeds 255 bytes\n");
        return -1;
    }
    uint8_t path_len = (uint8_t)raw_len;

    struct bhttp_frame f = {
        .version      = BHTTP_VERSION,
        .type         = FRAME_REQUEST,
        .flags        = 0x00,
        .header_count = 0,
        .length       = 1 + path_len   /* path_len(1) + path bytes */
    };

    uint32_t total = FRAME_HDR_SIZE + f.length;
    uint8_t *buf = malloc(total);
    if (!buf) return -1;

    frame_pack(&f, buf);
    buf[FRAME_HDR_SIZE] = path_len;
    memcpy(buf + FRAME_HDR_SIZE + 1, path, path_len);

    if (verbose)
        hexdump("request", buf, total);

    int ret = (write_all(fd, buf, total) < 0) ? -1 : 0;
    free(buf);
    return ret;
}

/*
 * Read one RESPONSE frame (skipping any unknown frame types per
 * forward-compat rule). Writes the body to stdout.
 * Returns the HTTP-like status code, or -1 on error.
 */
static int read_response(int fd, int verbose)
{
    uint8_t hdr_buf[FRAME_HDR_SIZE];

    for (;;) {
        if (read_exact(fd, hdr_buf, FRAME_HDR_SIZE) <= 0)
            return -1;

        struct bhttp_frame f;
        if (frame_unpack(hdr_buf, &f) < 0)
            return -1;

        /* read full payload */
        uint8_t *payload = NULL;
        if (f.length > 0) {
            payload = malloc(f.length);
            if (!payload) return -1;
            if (read_exact(fd, payload, f.length) <= 0) {
                free(payload);
                return -1;
            }
        }

        /* hexdump the complete frame if verbose */
        if (verbose) {
            uint32_t frame_total = FRAME_HDR_SIZE + f.length;
            uint8_t *frame_buf = malloc(frame_total);
            if (frame_buf) {
                memcpy(frame_buf, hdr_buf, FRAME_HDR_SIZE);
                if (payload)
                    memcpy(frame_buf + FRAME_HDR_SIZE, payload, f.length);
                hexdump("response", frame_buf, frame_total);
                free(frame_buf);
            }
        }

        /* forward-compat: skip anything that isn't a RESPONSE */
        if (f.type != FRAME_RESPONSE) {
            free(payload);
            continue;
        }

        /* need at least 2 bytes for status */
        if (f.length < 2) {
            free(payload);
            return -1;
        }

        /* parse status */
        uint16_t net_status;
        memcpy(&net_status, payload, 2);
        uint16_t status = ntohs(net_status);

        /*
         * Walk past the headers to find the body.
         * Each header: id(1) [+ name_len(1) + name if id==0] + value_len(2) + value
         */
        uint32_t off = 2;
        for (uint8_t i = 0; i < f.header_count; i++) {
            if (off >= f.length) { free(payload); return -1; }
            uint8_t id = payload[off++];

            if (id == 0) {
                /* custom header — skip name_len + name */
                if (off >= f.length) { free(payload); return -1; }
                uint8_t name_len = payload[off++];
                off += name_len;
                if (off > f.length) { free(payload); return -1; }
            }

            /* value_len + value */
            if (off + 2 > f.length) { free(payload); return -1; }
            uint16_t vlen;
            memcpy(&vlen, payload + off, 2);
            vlen = ntohs(vlen);
            off += 2 + vlen;
            if (off > f.length) { free(payload); return -1; }
        }

        /* everything from off to end of payload is the body */
        uint32_t body_len = f.length - off;
        if (body_len > 0) {
            fwrite(payload + off, 1, body_len, stdout);
            fflush(stdout);
        }

        free(payload);
        return (int)status;
    }
}

/* parse "host:port/path" → host, port, first_path */
static int parse_url(const char *arg, char *host, size_t host_sz,
                     int *port, const char **path)
{
    const char *colon = strchr(arg, ':');
    if (!colon) return -1;

    size_t hlen = (size_t)(colon - arg);
    if (hlen == 0 || hlen >= host_sz) return -1;
    memcpy(host, arg, hlen);
    host[hlen] = '\0';

    const char *slash = strchr(colon + 1, '/');
    if (slash) {
        char pbuf[16];
        size_t plen = (size_t)(slash - colon - 1);
        if (plen == 0 || plen >= sizeof(pbuf)) return -1;
        memcpy(pbuf, colon + 1, plen);
        pbuf[plen] = '\0';
        *port = atoi(pbuf);
        *path = slash;          /* includes leading / */
    } else {
        *port = atoi(colon + 1);
        *path = "/";
    }
    return 0;
}

int main(int argc, char *argv[])
{
    int verbose = 0;
    int argi = 1;

    if (argi < argc && strcmp(argv[argi], "-v") == 0) {
        verbose = 1;
        argi++;
    }

    if (argi >= argc) {
        fprintf(stderr, "usage: %s [-v] host:port/path [path ...]\n", argv[0]);
        return 1;
    }

    /* first arg has the host:port and first path */
    char host[256];
    int port;
    const char *first_path;
    if (parse_url(argv[argi], host, sizeof(host), &port, &first_path) < 0) {
        fprintf(stderr, "bad url (expected host:port/path)\n");
        return 1;
    }

    /* resolve hostname and connect — one connection for all requests */
    struct hostent *he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "can't resolve: %s\n", host);
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)port)
    };
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    int exit_code = 0;

    /* send first path from the URL arg, then any additional path args */
    const char *paths[argc];
    int npaths = 0;
    paths[npaths++] = first_path;
    for (int i = argi + 1; i < argc; i++)
        paths[npaths++] = argv[i];

    for (int i = 0; i < npaths; i++) {
        fprintf(stderr, "> GET %s\n", paths[i]);

        if (send_request(fd, paths[i], verbose) < 0) {
            fprintf(stderr, "send failed\n");
            exit_code = 1;
            break;
        }

        int status = read_response(fd, verbose);
        if (status < 0) {
            fprintf(stderr, "read failed\n");
            exit_code = 1;
            break;
        }

        fprintf(stderr, "< %d\n", status);
        if (status >= 400)
            exit_code = 1;
    }

    close(fd);
    return exit_code;
}
