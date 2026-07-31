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
static int32_t send_req(int fd, const uint8_t *text, size_t len) {
    if (len > k_max_msg) {
        return -1;
    }

    Buffer wbuf;
    buf_init(&wbuf);
    buf_append(&wbuf, (const uint8_t *)&len, 4);
    buf_append(&wbuf, text, len);
    int32_t rv = write_all(fd, wbuf.data, wbuf.len);
    buf_free(&wbuf);
    return rv;
}

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
    memcpy(&len, rbuf.data, 4);  // assume little endian
    if (len > k_max_msg) {
        msg("too long");
        buf_free(&rbuf);
        return -1;
    }

    // reply body
    buf_resize(&rbuf, 4 + len);
    err = read_full(fd, &rbuf.data[4], len);
    if (err) {
        msg("read() error");
        buf_free(&rbuf);
        return err;
    }

    // do something
    printf("len:%u data:%.*s\n", len, len < 100 ? len : 100, &rbuf.data[4]);
    buf_free(&rbuf);
    return 0;
}

// -------------------------------------------------------------------------------------
// MAIN --------------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

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

    // multiple pipelined requests
    char *big_msg = (char *)malloc(k_max_msg);
    memset(big_msg, 'z', k_max_msg);

    const char *query_list[5];
    size_t query_len[5];
    query_list[0] = "hello1";      query_len[0] = strlen("hello1");
    query_list[1] = "hello2";      query_len[1] = strlen("hello2");
    query_list[2] = "hello3";      query_len[2] = strlen("hello3");
    // a large message requires multiple event loop iterations
    query_list[3] = big_msg;       query_len[3] = k_max_msg;
    query_list[4] = "hello5";      query_len[4] = strlen("hello5");
    size_t query_count = 5;

    for (size_t i = 0; i < query_count; ++i) {
        int32_t err = send_req(fd, (const uint8_t *)query_list[i], query_len[i]);
        if (err) {
            goto L_DONE;
        }
    }
    for (size_t i = 0; i < query_count; ++i) {
        int32_t err = read_res(fd);
        if (err) {
            goto L_DONE;
        }
    }

L_DONE:
    free(big_msg);
    close(fd);
    return 0;
}