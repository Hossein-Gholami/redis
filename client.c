// stdlib
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
// system
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>


#define PORT 3490
// #define k_max_msg 4096
const size_t k_max_msg = 32 << 20;  // likely larger than the kernel buffer

// -------------------------------------------------------------------------------------
// HELPERS -----------------------------------------------------------------------------

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

// -------------------------------------------------------------------------------------
// HANDLERS ----------------------------------------------------------------------------

static int32_t read_full(int fd, uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) {
            return -1;  // error, or unexpected EOF
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t write_all(int fd, const uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) {
            return -1;  // error
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

// -------------------------------------------------------------------------------------
// VECTOR ------------------------------------------------------------------------------

// a growable byte buffer, replaces std::vector<uint8_t>
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} Buffer;

static void buf_init(Buffer *buf) {
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static void buf_free(Buffer *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static void buf_reserve(Buffer *buf, size_t new_cap) {
    if (new_cap > buf->cap) {
        uint8_t *new_data = (uint8_t *)realloc(buf->data, new_cap);
        if (!new_data) {
            die("realloc error");
        }
        buf->data = new_data;
        buf->cap = new_cap;
    }
}

static void buf_resize(Buffer *buf, size_t new_len) {
    buf_reserve(buf, new_len);
    buf->len = new_len;
}

// append to the back
static void buf_append(Buffer *buf, const uint8_t *data, size_t len) {
    if (buf->len + len > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap * 2 : 64;
        while (new_cap < buf->len + len) {
            new_cap *= 2;
        }
        buf_reserve(buf, new_cap);
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
}

// -------------------------------------------------------------------------------------
// QUERY -------------------------------------------------------------------------------

// the `query` function was simply splited into `send_req` and `read_res`.

// +------+-----+------+-----+------+-----+-----+------+
// | nstr | len | str1 | len | str2 | ... | len | strn |
// +------+-----+------+-----+------+-----+-----+------+
static int32_t send_req(int fd, char **cmd, size_t cmd_count) {
    uint32_t len = 4;  // the nstr field
    for (size_t i = 0; i < cmd_count; i++) {
        len += 4 + (uint32_t)strlen(cmd[i]);
    }
    if (len > k_max_msg) {
        return -1;
    }

    Buffer wbuf;
    buf_init(&wbuf);
    buf_append(&wbuf, (const uint8_t *)&len, 4);
    uint32_t n = (uint32_t)cmd_count;
    buf_append(&wbuf, (const uint8_t *)&n, 4);
    for (size_t i = 0; i < cmd_count; i++) {
        uint32_t p = (uint32_t)strlen(cmd[i]);
        buf_append(&wbuf, (const uint8_t *)&p, 4);
        buf_append(&wbuf, (const uint8_t *)cmd[i], p);
    }
    int32_t rv = write_all(fd, wbuf.data, wbuf.len);
    buf_free(&wbuf);
    return rv;
}

// +--------+---------+
// | status | data... |
// +--------+---------+
static int32_t read_res(int fd) {
    // 4 bytes header
    Buffer rbuf;
    buf_init(&rbuf);
    buf_resize(&rbuf, 4);
    errno = 0;
    int32_t err = read_full(fd, rbuf.data, 4);
    if (err) {
        if (errno == 0) {
            msg("EOF");
        } else {
            msg("read() error");
        }
        buf_free(&rbuf);
        return err;
    }

    uint32_t len = 0;
    memcpy(&len, rbuf.data, 4);
    if (len < 4) {
        msg("bad response");
        buf_free(&rbuf);
        return -1;
    }
    if (len > k_max_msg) {
        msg("too long");
        buf_free(&rbuf);
        return -1;
    }

    // reply body: 4-byte status + data
    buf_resize(&rbuf, 4 + len);
    err = read_full(fd, &rbuf.data[4], len);
    if (err) {
        msg("read() error");
        buf_free(&rbuf);
        return err;
    }

    uint32_t rescode = 0;
    memcpy(&rescode, &rbuf.data[4], 4);
    printf("server says: [%u] %.*s\n", rescode, (int)(len - 4), &rbuf.data[8]);
    buf_free(&rbuf);
    return 0;
}

// -------------------------------------------------------------------------------------
// MAIN --------------------------------------------------------------------------------

int main(int argc, char **argv) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(PORT);
    addr.sin_addr.s_addr = ntohl(INADDR_LOOPBACK);  // 127.0.0.1
    int rv = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("connect");
    }

    // the command is just the program's argv[1:], e.g. `./client get k`
    int32_t err = send_req(fd, argv + 1, (size_t)(argc - 1));
    if (err) {
        goto L_DONE;
    }
    err = read_res(fd);
    if (err) {
        goto L_DONE;
    }

L_DONE:
    close(fd);
    return 0;
}
