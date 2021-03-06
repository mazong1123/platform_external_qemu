/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>
#include <zlib.h>

/* Needed early for CONFIG_BSD etc. */
#include "config-host.h"

#ifndef _WIN32
#include <sys/times.h>
#include <sys/wait.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <netdb.h>
#include <sys/select.h>
#ifdef CONFIG_BSD
#include <sys/stat.h>
#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__)
#include <libutil.h>
#else
#include <util.h>
#endif
#ifdef __linux__
#include <pty.h>
#include <malloc.h>
#include <linux/rtc.h>
#endif
#endif
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <malloc.h>
#include <sys/timeb.h>
#include <mmsystem.h>
#define getopt_long_only getopt_long
#define memalign(align, size) malloc(size)
#endif

#include "qemu-common.h"
#include "hw/hw.h"
#include "hw/qdev.h"
#include "net/net.h"
#include "monitor/monitor.h"
#include "sysemu/sysemu.h"
#include "sysemu/char.h"
#include "sysemu/blockdev.h"
#include "block/block.h"
#include "audio/audio.h"
#include "migration/migration.h"
#include "migration/qemu-file.h"
#include "migration/vmstate.h"
#include "qemu/bitmap.h"
#include "qemu/iov.h"
#include "qemu/sockets.h"
#include "qemu/timer.h"
#include "qemu/queue.h"
#include "android/snapshot.h"


#define SELF_ANNOUNCE_ROUNDS 5

#ifndef ETH_P_RARP
#define ETH_P_RARP 0x8035
#endif
#define ARP_HTYPE_ETH 0x0001
#define ARP_PTYPE_IP 0x0800
#define ARP_OP_REQUEST_REV 0x3

static int announce_self_create(uint8_t *buf,
				uint8_t *mac_addr)
{
    /* Ethernet header. */
    memset(buf, 0xff, 6);         /* destination MAC addr */
    memcpy(buf + 6, mac_addr, 6); /* source MAC addr */
    *(uint16_t *)(buf + 12) = htons(ETH_P_RARP); /* ethertype */

    /* RARP header. */
    *(uint16_t *)(buf + 14) = htons(ARP_HTYPE_ETH); /* hardware addr space */
    *(uint16_t *)(buf + 16) = htons(ARP_PTYPE_IP); /* protocol addr space */
    *(buf + 18) = 6; /* hardware addr length (ethernet) */
    *(buf + 19) = 4; /* protocol addr length (IPv4) */
    *(uint16_t *)(buf + 20) = htons(ARP_OP_REQUEST_REV); /* opcode */
    memcpy(buf + 22, mac_addr, 6); /* source hw addr */
    memset(buf + 28, 0x00, 4);     /* source protocol addr */
    memcpy(buf + 32, mac_addr, 6); /* target hw addr */
    memset(buf + 38, 0x00, 4);     /* target protocol addr */

    /* Padding to get up to 60 bytes (ethernet min packet size, minus FCS). */
    memset(buf + 42, 0x00, 18);

    return 60; /* len (FCS will be added by hardware) */
}

static void qemu_announce_self_once(void *opaque)
{
    int i, len;
    VLANState *vlan;
    VLANClientState *vc;
    uint8_t buf[256];
    static int count = SELF_ANNOUNCE_ROUNDS;
    QEMUTimer *timer = *(QEMUTimer **)opaque;

    for (i = 0; i < MAX_NICS; i++) {
        if (!nd_table[i].used)
            continue;
        len = announce_self_create(buf, nd_table[i].macaddr);
        vlan = nd_table[i].vlan;
	for(vc = vlan->first_client; vc != NULL; vc = vc->next) {
            vc->receive(vc, buf, len);
        }
    }
    if (--count) {
        /* delay 50ms, 150ms, 250ms, ... */
        timer_mod(timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                       50 + (SELF_ANNOUNCE_ROUNDS - count - 1) * 100);
    } else {
	    timer_del(timer);
	    timer_free(timer);
    }
}

void qemu_announce_self(void)
{
	static QEMUTimer *timer;
	timer = timer_new_ms(QEMU_CLOCK_REALTIME, qemu_announce_self_once, &timer);
	qemu_announce_self_once(&timer);
}

/***********************************************************/
/* savevm/loadvm support */

void yield_until_fd_readable(int fd);

#define IO_BUF_SIZE 32768
#define MAX_IOV_SIZE MIN(IOV_MAX, 64)

struct QEMUFile {
    const QEMUFileOps *ops;
    void *opaque;

    int64_t bytes_xfer;
    int64_t xfer_limit;

    int64_t pos; /* start of buffer when writing, end of buffer
                    when reading */
    int buf_index;
    int buf_size; /* 0 when writing */
    uint8_t buf[IO_BUF_SIZE];

    struct iovec iov[MAX_IOV_SIZE];
    unsigned int iovcnt;

    int last_error;
};

typedef struct QEMUFileStdio
{
    FILE *stdio_file;
    QEMUFile *file;
} QEMUFileStdio;

typedef struct QEMUFileSocket
{
    int fd;
    QEMUFile *file;
} QEMUFileSocket;

static ssize_t socket_writev_buffer(void *opaque, struct iovec *iov, int iovcnt,
                                    int64_t pos)
{
    QEMUFileSocket *s = opaque;
    ssize_t len;
    ssize_t size = iov_size(iov, iovcnt);

    len = iov_send(s->fd, iov, iovcnt, 0, size);
    if (len < size) {
        len = -socket_error();
    }
    return len;
}

static int socket_get_fd(void *opaque)
{
    QEMUFileSocket *s = opaque;

    return s->fd;
}

static int socket_get_buffer(void *opaque, uint8_t *buf, int64_t pos, int size)
{
    QEMUFileSocket *s = opaque;
    ssize_t len;

    for (;;) {
        len = qemu_recv(s->fd, buf, size, 0);
        if (len != -1) {
            break;
        }
#ifndef CONFIG_ANDROID
        if (socket_error() == EAGAIN) {
            yield_until_fd_readable(s->fd);
        } else if (socket_error() != EINTR) {
            break;
        }
#else
       if (socket_error() != EINTR)
           break;
#endif
    }

    if (len == -1) {
        len = -socket_error();
    }
    return len;
}

static int file_socket_close(void *opaque)
{
    QEMUFileSocket *s = opaque;
    if (s->fd >= 0)
        socket_close(s->fd);
    g_free(s);
    return 0;
}

static int stdio_get_fd(void *opaque)
{
    QEMUFileStdio *s = opaque;

    return fileno(s->stdio_file);
}

static int stdio_put_buffer(void *opaque, const uint8_t *buf, int64_t pos, int size)
{
    QEMUFileStdio *s = opaque;
    return fwrite(buf, 1, size, s->stdio_file);
}

static int stdio_get_buffer(void *opaque, uint8_t *buf, int64_t pos, int size)
{
    QEMUFileStdio *s = opaque;
    FILE *fp = s->stdio_file;
    int bytes;

    for (;;) {
        clearerr(fp);
        bytes = fread(buf, 1, size, fp);
        if (bytes != 0 || !ferror(fp)) {
            break;
        }
#ifndef CONFIG_ANDROID
        if (errno == EAGAIN) {
            yield_until_fd_readable(fileno(fp));
        } else if (errno != EINTR) {
            break;
        }
#else
        if (errno != EINTR)
            break;
#endif
    }
    return bytes;
}

static int stdio_pclose(void *opaque)
{
    QEMUFileStdio *s = opaque;
    int ret;
    ret = pclose(s->stdio_file);
    if (ret == -1) {
        ret = -errno;
    } else if (!WIFEXITED(ret) || WEXITSTATUS(ret) != 0) {
        /* close succeeded, but non-zero exit code: */
        ret = -EIO; /* fake errno value */
    }
    g_free(s);
    return ret;
}

static int stdio_fclose(void *opaque)
{
    QEMUFileStdio *s = opaque;
    int ret = 0;

    if (s->file->ops->put_buffer || s->file->ops->writev_buffer) {
        int fd = fileno(s->stdio_file);
        struct stat st;

        ret = fstat(fd, &st);
        if (ret == 0 && S_ISREG(st.st_mode)) {
            /*
             * If the file handle is a regular file make sure the
             * data is flushed to disk before signaling success.
             */
            ret = fsync(fd);
            if (ret != 0) {
                ret = -errno;
                return ret;
            }
        }
    }
    if (fclose(s->stdio_file) == EOF) {
        ret = -errno;
    }
    g_free(s);
    return ret;
}

static const QEMUFileOps stdio_pipe_read_ops = {
    .get_fd =     stdio_get_fd,
    .get_buffer = stdio_get_buffer,
    .close =      stdio_pclose
};

static const QEMUFileOps stdio_pipe_write_ops = {
    .get_fd =     stdio_get_fd,
    .put_buffer = stdio_put_buffer,
    .close =      stdio_pclose
};

QEMUFile *qemu_popen_cmd(const char *command, const char *mode)
{
    FILE *stdio_file;
    QEMUFileStdio *s;

    if (mode == NULL || (mode[0] != 'r' && mode[0] != 'w') || mode[1] != 0) {
        fprintf(stderr, "qemu_popen: Argument validity check failed\n");
        return NULL;
    }

    stdio_file = popen(command, mode);
    if (stdio_file == NULL) {
        return NULL;
    }

    s = g_malloc0(sizeof(QEMUFileStdio));

    s->stdio_file = stdio_file;

    if(mode[0] == 'r') {
        s->file = qemu_fopen_ops(s, &stdio_pipe_read_ops);
    } else {
        s->file = qemu_fopen_ops(s, &stdio_pipe_write_ops);
    }
    return s->file;
}

static const QEMUFileOps stdio_file_read_ops = {
    .get_fd =     stdio_get_fd,
    .get_buffer = stdio_get_buffer,
    .close =      stdio_fclose
};

static const QEMUFileOps stdio_file_write_ops = {
    .get_fd =     stdio_get_fd,
    .put_buffer = stdio_put_buffer,
    .close =      stdio_fclose
};

static ssize_t unix_writev_buffer(void *opaque, struct iovec *iov, int iovcnt,
                                  int64_t pos)
{
    QEMUFileSocket *s = opaque;
    ssize_t len, offset;
    ssize_t size = iov_size(iov, iovcnt);
    ssize_t total = 0;

    assert(iovcnt > 0);
    offset = 0;
    while (size > 0) {
        /* Find the next start position; skip all full-sized vector elements  */
        while (offset >= (ssize_t)iov[0].iov_len) {
            offset -= iov[0].iov_len;
            iov++, iovcnt--;
        }

        /* skip `offset' bytes from the (now) first element, undo it on exit */
        assert(iovcnt > 0);
        iov[0].iov_base += offset;
        iov[0].iov_len -= offset;

        do {
            len = writev(s->fd, iov, iovcnt);
        } while (len == -1 && errno == EINTR);
        if (len == -1) {
            return -errno;
        }

        /* Undo the changes above */
        iov[0].iov_base -= offset;
        iov[0].iov_len += offset;

        /* Prepare for the next iteration */
        offset += len;
        total += len;
        size -= len;
    }

    return total;
}

static int unix_get_buffer(void *opaque, uint8_t *buf, int64_t pos, int size)
{
    QEMUFileSocket *s = opaque;
    ssize_t len;

    for (;;) {
        len = read(s->fd, buf, size);
        if (len != -1) {
            break;
        }
        if (errno == EAGAIN) {
            yield_until_fd_readable(s->fd);
        } else if (errno != EINTR) {
            break;
        }
    }

    if (len == -1) {
        len = -errno;
    }
    return len;
}

static int unix_close(void *opaque)
{
    QEMUFileSocket *s = opaque;
    close(s->fd);
    g_free(s);
    return 0;
}

static const QEMUFileOps unix_read_ops = {
    .get_fd =     socket_get_fd,
    .get_buffer = unix_get_buffer,
    .close =      unix_close
};

static const QEMUFileOps unix_write_ops = {
    .get_fd =     socket_get_fd,
    .writev_buffer = unix_writev_buffer,
    .close =      unix_close
};

QEMUFile *qemu_fdopen(int fd, const char *mode)
{
    QEMUFileSocket *s;

    if (mode == NULL ||
	(mode[0] != 'r' && mode[0] != 'w') ||
	mode[1] != 'b' || mode[2] != 0) {
        fprintf(stderr, "qemu_fdopen: Argument validity check failed\n");
        return NULL;
    }

    s = g_malloc0(sizeof(QEMUFileSocket));
    s->fd = fd;

    if(mode[0] == 'r') {
        s->file = qemu_fopen_ops(s, &unix_read_ops);
    } else {
        s->file = qemu_fopen_ops(s, &unix_write_ops);
    }
    return s->file;
}

static const QEMUFileOps socket_read_ops = {
    .get_fd =     socket_get_fd,
    .get_buffer = socket_get_buffer,
    .close = file_socket_close
};

static const QEMUFileOps socket_write_ops = {
    .get_fd =     socket_get_fd,
    .writev_buffer = socket_writev_buffer,
    .close = file_socket_close
};

bool qemu_file_mode_is_not_valid(const char *mode)
{
    if (mode == NULL ||
        (mode[0] != 'r' && mode[0] != 'w') ||
        mode[1] != 'b' || mode[2] != 0) {
        fprintf(stderr, "qemu_fopen: Argument validity check failed\n");
        return true;
    }

    return false;
}

QEMUFile *qemu_fopen_socket(int fd, const char *mode)
{
    QEMUFileSocket *s;

    if (qemu_file_mode_is_not_valid(mode)) {
        return NULL;
    }

    s = g_malloc0(sizeof(QEMUFileSocket));
    s->fd = fd;
    if (mode[0] == 'w') {
        qemu_set_block(s->fd);
        s->file = qemu_fopen_ops(s, &socket_write_ops);
    } else {
        s->file = qemu_fopen_ops(s, &socket_read_ops);
    }
    return s->file;
}

QEMUFile *qemu_fopen(const char *filename, const char *mode)
{
    QEMUFileStdio *s;

    if (qemu_file_mode_is_not_valid(mode)) {
        return NULL;
    }

    s = g_malloc0(sizeof(QEMUFileStdio));

    s->stdio_file = fopen(filename, mode);
    if (!s->stdio_file)
        goto fail;

    if(mode[0] == 'w') {
        s->file = qemu_fopen_ops(s, &stdio_file_write_ops);
    } else {
        s->file = qemu_fopen_ops(s, &stdio_file_read_ops);
    }
    return s->file;
fail:
    g_free(s);
    return NULL;
}

#ifndef CONFIG_ANDROID
// TODO(digit): Once bdrv_writev_vmstate() is implemented.
static ssize_t block_writev_buffer(void *opaque, struct iovec *iov, int iovcnt,
                                   int64_t pos)
{
    int ret;
    QEMUIOVector qiov;

    qemu_iovec_init_external(&qiov, iov, iovcnt);
    ret = bdrv_writev_vmstate(opaque, &qiov, pos);
    if (ret < 0) {
        return ret;
    }

    return qiov.size;
}
#endif

static int block_put_buffer(void *opaque, const uint8_t *buf,
                           int64_t pos, int size)
{
    bdrv_save_vmstate(opaque, buf, pos, size);
    return size;
}

static int block_get_buffer(void *opaque, uint8_t *buf, int64_t pos, int size)
{
    return bdrv_load_vmstate(opaque, buf, pos, size);
}

static int bdrv_fclose(void *opaque)
{
    // TODO(digit): bdrv_flush() should return error code.
    bdrv_flush(opaque);
    return 0;
}

static const QEMUFileOps bdrv_read_ops = {
    .get_buffer = block_get_buffer,
    .close =      bdrv_fclose
};

static const QEMUFileOps bdrv_write_ops = {
    .put_buffer     = block_put_buffer,
    //.writev_buffer  = block_writev_buffer,
    .close          = bdrv_fclose
};

static QEMUFile *qemu_fopen_bdrv(BlockDriverState *bs, int is_writable)
{
    if (is_writable)
        return qemu_fopen_ops(bs, &bdrv_write_ops);
    return qemu_fopen_ops(bs, &bdrv_read_ops);
}

QEMUFile *qemu_fopen_ops(void *opaque, const QEMUFileOps *ops)
{
    QEMUFile *f;

    f = g_malloc0(sizeof(QEMUFile));

    f->opaque = opaque;
    f->ops = ops;
    return f;
}

/*
 * Get last error for stream f
 *
 * Return negative error value if there has been an error on previous
 * operations, return 0 if no error happened.
 *
 */
int qemu_file_get_error(QEMUFile *f)
{
    return f->last_error;
}

void qemu_file_set_error(QEMUFile *f, int ret)
{
    if (f->last_error == 0) {
        f->last_error = ret;
    }
}

static inline bool qemu_file_is_writable(QEMUFile *f)
{
    return f->ops->writev_buffer || f->ops->put_buffer;
}

/**
 * Flushes QEMUFile buffer
 *
 * If there is writev_buffer QEMUFileOps it uses it otherwise uses
 * put_buffer ops.
 */
void qemu_fflush(QEMUFile *f)
{
    ssize_t ret = 0;

    if (!qemu_file_is_writable(f)) {
        return;
    }

    if (f->ops->writev_buffer) {
        if (f->iovcnt > 0) {
            ret = f->ops->writev_buffer(f->opaque, f->iov, f->iovcnt, f->pos);
        }
    } else {
        if (f->buf_index > 0) {
            ret = f->ops->put_buffer(f->opaque, f->buf, f->pos, f->buf_index);
        }
    }
    if (ret >= 0) {
        f->pos += ret;
    }
    f->buf_index = 0;
    f->iovcnt = 0;
    if (ret < 0) {
        qemu_file_set_error(f, ret);
    }
}

#ifndef CONFIG_ANDROID
// TODO(digit).
void ram_control_before_iterate(QEMUFile *f, uint64_t flags)
{
    int ret = 0;

    if (f->ops->before_ram_iterate) {
        ret = f->ops->before_ram_iterate(f, f->opaque, flags);
        if (ret < 0) {
            qemu_file_set_error(f, ret);
        }
    }
}

void ram_control_after_iterate(QEMUFile *f, uint64_t flags)
{
    int ret = 0;

    if (f->ops->after_ram_iterate) {
        ret = f->ops->after_ram_iterate(f, f->opaque, flags);
        if (ret < 0) {
            qemu_file_set_error(f, ret);
        }
    }
}

void ram_control_load_hook(QEMUFile *f, uint64_t flags)
{
    int ret = -EINVAL;

    if (f->ops->hook_ram_load) {
        ret = f->ops->hook_ram_load(f, f->opaque, flags);
        if (ret < 0) {
            qemu_file_set_error(f, ret);
        }
    } else {
        qemu_file_set_error(f, ret);
    }
}

size_t ram_control_save_page(QEMUFile *f, ram_addr_t block_offset,
                         ram_addr_t offset, size_t size, int *bytes_sent)
{
    if (f->ops->save_page) {
        int ret = f->ops->save_page(f, f->opaque, block_offset,
                                    offset, size, bytes_sent);

        if (ret != RAM_SAVE_CONTROL_DELAYED) {
            if (bytes_sent && *bytes_sent > 0) {
                qemu_update_position(f, *bytes_sent);
            } else if (ret < 0) {
                qemu_file_set_error(f, ret);
            }
        }

        return ret;
    }

    return RAM_SAVE_CONTROL_NOT_SUPP;
}
#endif  // !CONFIG_ANDROID

static void qemu_fill_buffer(QEMUFile *f)
{
    int len;
    int pending;

    assert(!qemu_file_is_writable(f));

    pending = f->buf_size - f->buf_index;
    if (pending > 0) {
        memmove(f->buf, f->buf + f->buf_index, pending);
    }
    f->buf_index = 0;
    f->buf_size = pending;

    len = f->ops->get_buffer(f->opaque, f->buf + pending, f->pos,
                        IO_BUF_SIZE - pending);
    if (len > 0) {
        f->buf_size += len;
        f->pos += len;
    } else if (len == 0) {
        qemu_file_set_error(f, -EIO);
    } else if (len != -EAGAIN)
        qemu_file_set_error(f, len);
}

int qemu_get_fd(QEMUFile *f)
{
    if (f->ops->get_fd) {
        return f->ops->get_fd(f->opaque);
    }
    return -1;
}

void qemu_update_position(QEMUFile *f, size_t size)
{
    f->pos += size;
}

/** Closes the file
 *
 * Returns negative error value if any error happened on previous operations or
 * while closing the file. Returns 0 or positive number on success.
 *
 * The meaning of return value on success depends on the specific backend
 * being used.
 */
int qemu_fclose(QEMUFile *f)
{
    int ret;
    qemu_fflush(f);
    ret = qemu_file_get_error(f);

    if (f->ops->close) {
        int ret2 = f->ops->close(f->opaque);
        if (ret >= 0) {
            ret = ret2;
        }
    }
    /* If any error was spotted before closing, we should report it
     * instead of the close() return value.
     */
    if (f->last_error) {
        ret = f->last_error;
    }
    g_free(f);
    return ret;
}

static void add_to_iovec(QEMUFile *f, const uint8_t *buf, int size)
{
    /* check for adjacent buffer and coalesce them */
    if (f->iovcnt > 0 && buf == f->iov[f->iovcnt - 1].iov_base +
        f->iov[f->iovcnt - 1].iov_len) {
        f->iov[f->iovcnt - 1].iov_len += size;
    } else {
        f->iov[f->iovcnt].iov_base = (uint8_t *)buf;
        f->iov[f->iovcnt++].iov_len = size;
    }

    if (f->iovcnt >= MAX_IOV_SIZE) {
        qemu_fflush(f);
    }
}

void qemu_put_buffer_async(QEMUFile *f, const uint8_t *buf, int size)
{
    if (!f->ops->writev_buffer) {
        qemu_put_buffer(f, buf, size);
        return;
    }

    if (f->last_error) {
        return;
    }

    f->bytes_xfer += size;
    add_to_iovec(f, buf, size);
}

void qemu_put_buffer(QEMUFile *f, const uint8_t *buf, int size)
{
    int l;

    if (f->last_error) {
        return;
    }

    while (size > 0) {
        l = IO_BUF_SIZE - f->buf_index;
        if (l > size)
            l = size;
        memcpy(f->buf + f->buf_index, buf, l);
        f->bytes_xfer += l;
        if (f->ops->writev_buffer) {
            add_to_iovec(f, f->buf + f->buf_index, l);
        }
        f->buf_index += l;
        if (f->buf_index == IO_BUF_SIZE) {
            qemu_fflush(f);
        }
        if (qemu_file_get_error(f)) {
            break;
        }
        buf += l;
        size -= l;
    }
}

void qemu_put_byte(QEMUFile *f, int v)
{
    if (f->last_error) {
        return;
    }

    f->buf[f->buf_index] = v;
    f->bytes_xfer++;
    if (f->ops->writev_buffer) {
        add_to_iovec(f, f->buf + f->buf_index, 1);
    }
    f->buf_index++;
    if (f->buf_index == IO_BUF_SIZE) {
        qemu_fflush(f);
    }
}

static void qemu_file_skip(QEMUFile *f, int size)
{
    if (f->buf_index + size <= f->buf_size) {
        f->buf_index += size;
    }
}

static int qemu_peek_buffer(QEMUFile *f, uint8_t *buf, int size, size_t offset)
{
    int pending;
    int index;

    assert(!qemu_file_is_writable(f));

    index = f->buf_index + offset;
    pending = f->buf_size - index;
    if (pending < size) {
        qemu_fill_buffer(f);
        index = f->buf_index + offset;
        pending = f->buf_size - index;
    }

    if (pending <= 0) {
        return 0;
    }
    if (size > pending) {
        size = pending;
    }

    memcpy(buf, f->buf + index, size);
    return size;
}

int qemu_get_buffer(QEMUFile *f, uint8_t *buf, int size)
{
    int pending = size;
    int done = 0;

    while (pending > 0) {
        int res;

        res = qemu_peek_buffer(f, buf, pending, 0);
        if (res == 0) {
            return done;
        }
        qemu_file_skip(f, res);
        buf += res;
        pending -= res;
        done += res;
    }
    return done;
}

static int qemu_peek_byte(QEMUFile *f, int offset)
{
    int index = f->buf_index + offset;

    assert(!qemu_file_is_writable(f));

    if (index >= f->buf_size) {
        qemu_fill_buffer(f);
        index = f->buf_index + offset;
        if (index >= f->buf_size) {
            return 0;
        }
    }
    return f->buf[index];
}

int qemu_get_byte(QEMUFile *f)
{
    int result;

    result = qemu_peek_byte(f, 0);
    qemu_file_skip(f, 1);
    return result;
}

#ifdef CONFIG_ANDROID
void qemu_put_string(QEMUFile *f, const char* str)
{
    /* We will encode NULL and the empty string in the same way */
    int  slen;
    if (str == NULL) {
        str = "";
    }
    slen = strlen(str);
    qemu_put_be32(f, slen);
    qemu_put_buffer(f, (const uint8_t*)str, slen);
}

char* qemu_get_string(QEMUFile *f)
{
    int slen = qemu_get_be32(f);
    char* str;
    if (slen == 0)
        return NULL;

    str = g_malloc(slen+1);
    if (qemu_get_buffer(f, (uint8_t*)str, slen) != slen) {
        g_free(str);
        return NULL;
    }
    str[slen] = '\0';
    return str;
}
#endif


int64_t qemu_ftell(QEMUFile *f)
{
    qemu_fflush(f);
    return f->pos;
}

int qemu_file_rate_limit(QEMUFile *f)
{
    if (qemu_file_get_error(f)) {
        return 1;
    }
    if (f->xfer_limit > 0 && f->bytes_xfer > f->xfer_limit) {
        return 1;
    }
    return 0;
}

int64_t qemu_file_get_rate_limit(QEMUFile *f)
{
    return f->xfer_limit;
}

void qemu_file_set_rate_limit(QEMUFile *f, int64_t limit)
{
    f->xfer_limit = limit;
}

void qemu_file_reset_rate_limit(QEMUFile *f)
{
    f->bytes_xfer = 0;
}

void qemu_put_be16(QEMUFile *f, unsigned int v)
{
    qemu_put_byte(f, v >> 8);
    qemu_put_byte(f, v);
}

void qemu_put_be32(QEMUFile *f, unsigned int v)
{
    qemu_put_byte(f, v >> 24);
    qemu_put_byte(f, v >> 16);
    qemu_put_byte(f, v >> 8);
    qemu_put_byte(f, v);
}

void qemu_put_be64(QEMUFile *f, uint64_t v)
{
    qemu_put_be32(f, v >> 32);
    qemu_put_be32(f, v);
}

unsigned int qemu_get_be16(QEMUFile *f)
{
    unsigned int v;
    v = qemu_get_byte(f) << 8;
    v |= qemu_get_byte(f);
    return v;
}

unsigned int qemu_get_be32(QEMUFile *f)
{
    unsigned int v;
    v = qemu_get_byte(f) << 24;
    v |= qemu_get_byte(f) << 16;
    v |= qemu_get_byte(f) << 8;
    v |= qemu_get_byte(f);
    return v;
}

uint64_t qemu_get_be64(QEMUFile *f)
{
    uint64_t v;
    v = (uint64_t)qemu_get_be32(f) << 32;
    v |= qemu_get_be32(f);
    return v;
}


void  qemu_put_struct(QEMUFile*  f, const QField*  fields, const void*  s)
{
    const QField*  qf = fields;

    /* Iterate over struct fields */
    while (qf->type != Q_FIELD_END) {
        uint8_t*  p = (uint8_t*)s + qf->offset;

        switch (qf->type) {
            case Q_FIELD_BYTE:
                qemu_put_byte(f, p[0]);
                break;
            case Q_FIELD_INT16:
                qemu_put_be16(f, ((uint16_t*)p)[0]);
                break;
            case Q_FIELD_INT32:
                qemu_put_be32(f, ((uint32_t*)p)[0]);
                break;
            case Q_FIELD_INT64:
                qemu_put_be64(f, ((uint64_t*)p)[0]);
                break;
            case Q_FIELD_BUFFER:
                if (qf[1].type != Q_FIELD_BUFFER_SIZE ||
                    qf[2].type != Q_FIELD_BUFFER_SIZE)
                {
                    fprintf(stderr, "%s: invalid QFIELD_BUFFER item passed as argument. aborting\n",
                            __FUNCTION__ );
                    exit(1);
                }
                else
                {
                    uint32_t  size = ((uint32_t)qf[1].offset << 16) | (uint32_t)qf[2].offset;

                    qemu_put_buffer(f, p, size);
                    qf += 2;
                }
                break;
            default:
                fprintf(stderr, "%s: invalid fields list passed as argument. aborting\n", __FUNCTION__);
                exit(1);
        }
        qf++;
    }
}

int   qemu_get_struct(QEMUFile*  f, const QField*  fields, void*  s)
{
    const QField*  qf = fields;

    /* Iterate over struct fields */
    while (qf->type != Q_FIELD_END) {
        uint8_t*  p = (uint8_t*)s + qf->offset;

        switch (qf->type) {
            case Q_FIELD_BYTE:
                p[0] = qemu_get_byte(f);
                break;
            case Q_FIELD_INT16:
                ((uint16_t*)p)[0] = qemu_get_be16(f);
                break;
            case Q_FIELD_INT32:
                ((uint32_t*)p)[0] = qemu_get_be32(f);
                break;
            case Q_FIELD_INT64:
                ((uint64_t*)p)[0] = qemu_get_be64(f);
                break;
            case Q_FIELD_BUFFER:
                if (qf[1].type != Q_FIELD_BUFFER_SIZE ||
                        qf[2].type != Q_FIELD_BUFFER_SIZE)
                {
                    fprintf(stderr, "%s: invalid QFIELD_BUFFER item passed as argument.\n",
                            __FUNCTION__ );
                    return -1;
                }
                else
                {
                    uint32_t  size = ((uint32_t)qf[1].offset << 16) | (uint32_t)qf[2].offset;
                    int       ret  = qemu_get_buffer(f, p, size);

                    if (ret != (int)size) {
                        fprintf(stderr, "%s: not enough bytes to load structure\n", __FUNCTION__);
                        return -1;
                    }
                    qf += 2;
                }
                break;
            default:
                fprintf(stderr, "%s: invalid fields list passed as argument. aborting\n", __FUNCTION__);
                exit(1);
        }
        qf++;
    }
    return 0;
}

/* write a float to file */
void qemu_put_float(QEMUFile *f, float v)
{
    uint8_t *bytes = (uint8_t*) &v;
    qemu_put_buffer(f, bytes, sizeof(float));
}

/* read a float from file */
float qemu_get_float(QEMUFile *f)
{
    uint8_t bytes[sizeof(float)];
    qemu_get_buffer(f, bytes, sizeof(float));

    return *((float*) bytes);
}

/* timer */

void timer_put(QEMUFile *f, QEMUTimer *ts)
{
    uint64_t expire_time;

    expire_time = timer_expire_time_ns(ts);
    qemu_put_be64(f, expire_time);
}

void timer_get(QEMUFile *f, QEMUTimer *ts)
{
    uint64_t expire_time;

    expire_time = qemu_get_be64(f);
    if (expire_time != (uint64_t)(-1)) {
        timer_mod_ns(ts, expire_time);
    } else {
        timer_del(ts);
    }
}


/* bool */

static int get_bool(QEMUFile *f, void *pv, size_t size)
{
    bool *v = pv;
    *v = qemu_get_byte(f);
    return 0;
}

static void put_bool(QEMUFile *f, void *pv, size_t size)
{
    bool *v = pv;
    qemu_put_byte(f, *v);
}

const VMStateInfo vmstate_info_bool = {
    .name = "bool",
    .get  = get_bool,
    .put  = put_bool,
};

/* 8 bit int */

static int get_int8(QEMUFile *f, void *pv, size_t size)
{
    int8_t *v = pv;
    qemu_get_s8s(f, v);
    return 0;
}

static void put_int8(QEMUFile *f, void *pv, size_t size)
{
    int8_t *v = pv;
    qemu_put_s8s(f, v);
}

const VMStateInfo vmstate_info_int8 = {
    .name = "int8",
    .get  = get_int8,
    .put  = put_int8,
};

/* 16 bit int */

static int get_int16(QEMUFile *f, void *pv, size_t size)
{
    int16_t *v = pv;
    qemu_get_sbe16s(f, v);
    return 0;
}

static void put_int16(QEMUFile *f, void *pv, size_t size)
{
    int16_t *v = pv;
    qemu_put_sbe16s(f, v);
}

const VMStateInfo vmstate_info_int16 = {
    .name = "int16",
    .get  = get_int16,
    .put  = put_int16,
};

/* 32 bit int */

static int get_int32(QEMUFile *f, void *pv, size_t size)
{
    int32_t *v = pv;
    qemu_get_sbe32s(f, v);
    return 0;
}

static void put_int32(QEMUFile *f, void *pv, size_t size)
{
    int32_t *v = pv;
    qemu_put_sbe32s(f, v);
}

const VMStateInfo vmstate_info_int32 = {
    .name = "int32",
    .get  = get_int32,
    .put  = put_int32,
};

/* 32 bit int. See that the received value is the same than the one
   in the field */

static int get_int32_equal(QEMUFile *f, void *pv, size_t size)
{
    int32_t *v = pv;
    int32_t v2;
    qemu_get_sbe32s(f, &v2);

    if (*v == v2)
        return 0;
    return -EINVAL;
}

const VMStateInfo vmstate_info_int32_equal = {
    .name = "int32 equal",
    .get  = get_int32_equal,
    .put  = put_int32,
};

/* 32 bit int. See that the received value is the less or the same
   than the one in the field */

static int get_int32_le(QEMUFile *f, void *pv, size_t size)
{
    int32_t *old = pv;
    int32_t new;
    qemu_get_sbe32s(f, &new);

    if (*old <= new)
        return 0;
    return -EINVAL;
}

const VMStateInfo vmstate_info_int32_le = {
    .name = "int32 equal",
    .get  = get_int32_le,
    .put  = put_int32,
};

/* 64 bit int */

static int get_int64(QEMUFile *f, void *pv, size_t size)
{
    int64_t *v = pv;
    qemu_get_sbe64s(f, v);
    return 0;
}

static void put_int64(QEMUFile *f, void *pv, size_t size)
{
    int64_t *v = pv;
    qemu_put_sbe64s(f, v);
}

const VMStateInfo vmstate_info_int64 = {
    .name = "int64",
    .get  = get_int64,
    .put  = put_int64,
};

/* 8 bit unsigned int */

static int get_uint8(QEMUFile *f, void *pv, size_t size)
{
    uint8_t *v = pv;
    qemu_get_8s(f, v);
    return 0;
}

static void put_uint8(QEMUFile *f, void *pv, size_t size)
{
    uint8_t *v = pv;
    qemu_put_8s(f, v);
}

const VMStateInfo vmstate_info_uint8 = {
    .name = "uint8",
    .get  = get_uint8,
    .put  = put_uint8,
};

/* 16 bit unsigned int */

static int get_uint16(QEMUFile *f, void *pv, size_t size)
{
    uint16_t *v = pv;
    qemu_get_be16s(f, v);
    return 0;
}

static void put_uint16(QEMUFile *f, void *pv, size_t size)
{
    uint16_t *v = pv;
    qemu_put_be16s(f, v);
}

const VMStateInfo vmstate_info_uint16 = {
    .name = "uint16",
    .get  = get_uint16,
    .put  = put_uint16,
};

/* 32 bit unsigned int */

static int get_uint32(QEMUFile *f, void *pv, size_t size)
{
    uint32_t *v = pv;
    qemu_get_be32s(f, v);
    return 0;
}

static void put_uint32(QEMUFile *f, void *pv, size_t size)
{
    uint32_t *v = pv;
    qemu_put_be32s(f, v);
}

const VMStateInfo vmstate_info_uint32 = {
    .name = "uint32",
    .get  = get_uint32,
    .put  = put_uint32,
};

/* 32 bit uint. See that the received value is the same than the one
   in the field */

static int get_uint32_equal(QEMUFile *f, void *pv, size_t size)
{
    uint32_t *v = pv;
    uint32_t v2;
    qemu_get_be32s(f, &v2);

    if (*v == v2) {
        return 0;
    }
    return -EINVAL;
}

const VMStateInfo vmstate_info_uint32_equal = {
    .name = "uint32 equal",
    .get  = get_uint32_equal,
    .put  = put_uint32,
};

/* 64 bit unsigned int */

static int get_uint64(QEMUFile *f, void *pv, size_t size)
{
    uint64_t *v = pv;
    qemu_get_be64s(f, v);
    return 0;
}

static void put_uint64(QEMUFile *f, void *pv, size_t size)
{
    uint64_t *v = pv;
    qemu_put_be64s(f, v);
}

const VMStateInfo vmstate_info_uint64 = {
    .name = "uint64",
    .get  = get_uint64,
    .put  = put_uint64,
};

/* 64 bit unsigned int. See that the received value is the same than the one
   in the field */

static int get_uint64_equal(QEMUFile *f, void *pv, size_t size)
{
    uint64_t *v = pv;
    uint64_t v2;
    qemu_get_be64s(f, &v2);

    if (*v == v2) {
        return 0;
    }
    return -EINVAL;
}

const VMStateInfo vmstate_info_uint64_equal = {
    .name = "int64 equal",
    .get  = get_uint64_equal,
    .put  = put_uint64,
};

/* 8 bit int. See that the received value is the same than the one
   in the field */

static int get_uint8_equal(QEMUFile *f, void *pv, size_t size)
{
    uint8_t *v = pv;
    uint8_t v2;
    qemu_get_8s(f, &v2);

    if (*v == v2)
        return 0;
    return -EINVAL;
}

const VMStateInfo vmstate_info_uint8_equal = {
    .name = "uint8 equal",
    .get  = get_uint8_equal,
    .put  = put_uint8,
};

/* 16 bit unsigned int int. See that the received value is the same than the one
   in the field */

static int get_uint16_equal(QEMUFile *f, void *pv, size_t size)
{
    uint16_t *v = pv;
    uint16_t v2;
    qemu_get_be16s(f, &v2);

    if (*v == v2)
        return 0;
    return -EINVAL;
}

const VMStateInfo vmstate_info_uint16_equal = {
    .name = "uint16 equal",
    .get  = get_uint16_equal,
    .put  = put_uint16,
};

/* floating point */

static int get_float64(QEMUFile *f, void *pv, size_t size)
{
    float64 *v = pv;

    *v = make_float64(qemu_get_be64(f));
    return 0;
}

static void put_float64(QEMUFile *f, void *pv, size_t size)
{
    uint64_t *v = pv;

    qemu_put_be64(f, float64_val(*v));
}

const VMStateInfo vmstate_info_float64 = {
    .name = "float64",
    .get  = get_float64,
    .put  = put_float64,
};

/* timers  */

static int get_timer(QEMUFile *f, void *pv, size_t size)
{
    QEMUTimer *v = pv;
    timer_get(f, v);
    return 0;
}

static void put_timer(QEMUFile *f, void *pv, size_t size)
{
    QEMUTimer *v = pv;
    timer_put(f, v);
}

const VMStateInfo vmstate_info_timer = {
    .name = "timer",
    .get  = get_timer,
    .put  = put_timer,
};

/* uint8_t buffers */

static int get_buffer(QEMUFile *f, void *pv, size_t size)
{
    uint8_t *v = pv;
    qemu_get_buffer(f, v, size);
    return 0;
}

static void put_buffer(QEMUFile *f, void *pv, size_t size)
{
    uint8_t *v = pv;
    qemu_put_buffer(f, v, size);
}

const VMStateInfo vmstate_info_buffer = {
    .name = "buffer",
    .get  = get_buffer,
    .put  = put_buffer,
};

/* unused buffers: space that was used for some fields that are
   not useful anymore */

static int get_unused_buffer(QEMUFile *f, void *pv, size_t size)
{
    uint8_t buf[1024];
    int block_len;

    while (size > 0) {
        block_len = MIN(sizeof(buf), size);
        size -= block_len;
        qemu_get_buffer(f, buf, block_len);
    }
   return 0;
}

static void put_unused_buffer(QEMUFile *f, void *pv, size_t size)
{
    static const uint8_t buf[1024];
    int block_len;

    while (size > 0) {
        block_len = MIN(sizeof(buf), size);
        size -= block_len;
        qemu_put_buffer(f, buf, block_len);
    }
}

const VMStateInfo vmstate_info_unused_buffer = {
    .name = "unused_buffer",
    .get  = get_unused_buffer,
    .put  = put_unused_buffer,
};

/* bitmaps (as defined by bitmap.h). Note that size here is the size
 * of the bitmap in bits. The on-the-wire format of a bitmap is 64
 * bit words with the bits in big endian order. The in-memory format
 * is an array of 'unsigned long', which may be either 32 or 64 bits.
 */
/* This is the number of 64 bit words sent over the wire */
#define BITS_TO_U64S(nr) DIV_ROUND_UP(nr, 64)
static int get_bitmap(QEMUFile *f, void *pv, size_t size)
{
    unsigned long *bmp = pv;
    int i, idx = 0;
    for (i = 0; i < (int)BITS_TO_U64S(size); i++) {
        uint64_t w = qemu_get_be64(f);
        bmp[idx++] = w;
        if (sizeof(unsigned long) == 4 && idx < BITS_TO_LONGS(size)) {
            bmp[idx++] = w >> 32;
        }
    }
    return 0;
}

static void put_bitmap(QEMUFile *f, void *pv, size_t size)
{
    unsigned long *bmp = pv;
    int i, idx = 0;
    for (i = 0; i < (int)BITS_TO_U64S(size); i++) {
        uint64_t w = bmp[idx++];
        if (sizeof(unsigned long) == 4 && idx < BITS_TO_LONGS(size)) {
            w |= ((uint64_t)bmp[idx++]) << 32;
        }
        qemu_put_be64(f, w);
    }
}

const VMStateInfo vmstate_info_bitmap = {
    .name = "bitmap",
    .get = get_bitmap,
    .put = put_bitmap,
};

typedef struct CompatEntry {
    char idstr[256];
    int instance_id;
} CompatEntry;

typedef struct SaveStateEntry {
    QTAILQ_ENTRY(SaveStateEntry) entry;
    char idstr[256];
    int instance_id;
    int alias_id;
    int version_id;
    int section_id;
    SaveVMHandlers *ops;
    const VMStateDescription *vmsd;
    void *opaque;
    CompatEntry *compat;
    int no_migrate;
    int is_ram;
} SaveStateEntry;


static QTAILQ_HEAD(savevm_handlers, SaveStateEntry) savevm_handlers =
    QTAILQ_HEAD_INITIALIZER(savevm_handlers);
static int global_section_id;

static int calculate_new_instance_id(const char *idstr)
{
    SaveStateEntry *se;
    int instance_id = 0;

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        if (strcmp(idstr, se->idstr) == 0
            && instance_id <= se->instance_id) {
            instance_id = se->instance_id + 1;
        }
    }
    return instance_id;
}

static int calculate_compat_instance_id(const char *idstr)
{
    SaveStateEntry *se;
    int instance_id = 0;

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        if (!se->compat)
            continue;

        if (strcmp(idstr, se->compat->idstr) == 0
            && instance_id <= se->compat->instance_id) {
            instance_id = se->compat->instance_id + 1;
        }
    }
    return instance_id;
}

/* TODO: Individual devices generally have very little idea about the rest
   of the system, so instance_id should be removed/replaced.
   Meanwhile pass -1 as instance_id if you do not already have a clearly
   distinguishing id for all instances of your device class. */
int register_savevm_live(DeviceState *dev,
                         const char *idstr,
                         int instance_id,
                         int version_id,
                         SaveVMHandlers *ops,
                         void *opaque)
{
    SaveStateEntry *se;

    se = g_malloc0(sizeof(SaveStateEntry));
    se->version_id = version_id;
    se->section_id = global_section_id++;
    se->ops = ops;
    se->opaque = opaque;
    se->vmsd = NULL;
    se->no_migrate = 0;
    /* if this is a live_savem then set is_ram */
    if (ops->save_live_state != NULL) {
        se->is_ram = 1;
    }

    if (dev) {
        char *id = qdev_get_dev_path(dev);
        if (id) {
            pstrcpy(se->idstr, sizeof(se->idstr), id);
            pstrcat(se->idstr, sizeof(se->idstr), "/");
            g_free(id);

            se->compat = g_malloc0(sizeof(CompatEntry));
            pstrcpy(se->compat->idstr, sizeof(se->compat->idstr), idstr);
            se->compat->instance_id = instance_id == -1 ?
                         calculate_compat_instance_id(idstr) : instance_id;
            instance_id = -1;
        }
    }
    pstrcat(se->idstr, sizeof(se->idstr), idstr);

    if (instance_id == -1) {
        se->instance_id = calculate_new_instance_id(se->idstr);
    } else {
        se->instance_id = instance_id;
    }
    assert(!se->compat || se->instance_id == 0);
    /* add at the end of list */
    QTAILQ_INSERT_TAIL(&savevm_handlers, se, entry);
    return 0;
}

int register_savevm(DeviceState *dev,
                    const char *idstr,
                    int instance_id,
                    int version_id,
                    SaveStateHandler *save_state,
                    LoadStateHandler *load_state,
                    void *opaque)
{
    SaveVMHandlers *ops = g_malloc0(sizeof(SaveVMHandlers));
    ops->save_state = save_state;
    ops->load_state = load_state;
    return register_savevm_live(dev, idstr, instance_id, version_id,
                                ops, opaque);
}

void unregister_savevm(DeviceState *dev, const char *idstr, void *opaque)
{
    SaveStateEntry *se, *new_se;
    char id[256] = "";

    if (dev) {
        char *path = qdev_get_dev_path(dev);
        if (path) {
            pstrcpy(id, sizeof(id), path);
            pstrcat(id, sizeof(id), "/");
            g_free(path);
        }
    }
    pstrcat(id, sizeof(id), idstr);

    QTAILQ_FOREACH_SAFE(se, &savevm_handlers, entry, new_se) {
        if (strcmp(se->idstr, id) == 0 && se->opaque == opaque) {
            QTAILQ_REMOVE(&savevm_handlers, se, entry);
            if (se->compat) {
                g_free(se->compat);
            }
            g_free(se->ops);
            g_free(se);
        }
    }
}

int vmstate_register_with_alias_id(DeviceState *dev, int instance_id,
                                   const VMStateDescription *vmsd,
                                   void *opaque, int alias_id,
                                   int required_for_version)
{
    SaveStateEntry *se;

    /* If this triggers, alias support can be dropped for the vmsd. */
    assert(alias_id == -1 || required_for_version >= vmsd->minimum_version_id);

    se = g_malloc0(sizeof(SaveStateEntry));
    se->version_id = vmsd->version_id;
    se->section_id = global_section_id++;
    se->opaque = opaque;
    se->vmsd = vmsd;
    se->alias_id = alias_id;
    se->no_migrate = vmsd->unmigratable;

    if (dev) {
        char *id = qdev_get_dev_path(dev);
        if (id) {
            pstrcpy(se->idstr, sizeof(se->idstr), id);
            pstrcat(se->idstr, sizeof(se->idstr), "/");
            g_free(id);

            se->compat = g_malloc0(sizeof(CompatEntry));
            pstrcpy(se->compat->idstr, sizeof(se->compat->idstr), vmsd->name);
            se->compat->instance_id = instance_id == -1 ?
                         calculate_compat_instance_id(vmsd->name) : instance_id;
            instance_id = -1;
        }
    }
    pstrcat(se->idstr, sizeof(se->idstr), vmsd->name);

    if (instance_id == -1) {
        se->instance_id = calculate_new_instance_id(se->idstr);
    } else {
        se->instance_id = instance_id;
    }
    assert(!se->compat || se->instance_id == 0);
    /* add at the end of list */
    QTAILQ_INSERT_TAIL(&savevm_handlers, se, entry);
    return 0;
}

void vmstate_unregister(DeviceState *dev, const VMStateDescription *vmsd,
                        void *opaque)
{
    SaveStateEntry *se, *new_se;

    QTAILQ_FOREACH_SAFE(se, &savevm_handlers, entry, new_se) {
        if (se->vmsd == vmsd && se->opaque == opaque) {
            QTAILQ_REMOVE(&savevm_handlers, se, entry);
            if (se->compat) {
                g_free(se->compat);
            }
            g_free(se);
        }
    }
}

static void vmstate_subsection_save(QEMUFile *f, const VMStateDescription *vmsd,
                                    void *opaque);
static int vmstate_subsection_load(QEMUFile *f, const VMStateDescription *vmsd,
                                   void *opaque);

int vmstate_load_state(QEMUFile *f, const VMStateDescription *vmsd,
                       void *opaque, int version_id)
{
    VMStateField *field = vmsd->fields;
    int ret;

    if (version_id > vmsd->version_id) {
        return -EINVAL;
    }
    if (version_id < vmsd->minimum_version_id_old) {
        return -EINVAL;
    }
    if  (version_id < vmsd->minimum_version_id) {
        return vmsd->load_state_old(f, opaque, version_id);
    }
    if (vmsd->pre_load) {
        int ret = vmsd->pre_load(opaque);
        if (ret)
            return ret;
    }
    while(field->name) {
        if ((field->field_exists &&
             field->field_exists(opaque, version_id)) ||
            (!field->field_exists &&
             field->version_id <= version_id)) {
            void *base_addr = opaque + field->offset;
            int i, n_elems = 1;
            int size = field->size;

            if (field->flags & VMS_VBUFFER) {
                size = *(int32_t *)(opaque+field->size_offset);
                if (field->flags & VMS_MULTIPLY) {
                    size *= field->size;
                }
            }
            if (field->flags & VMS_ARRAY) {
                n_elems = field->num;
            } else if (field->flags & VMS_VARRAY_INT32) {
                n_elems = *(int32_t *)(opaque+field->num_offset);
            } else if (field->flags & VMS_VARRAY_UINT32) {
                n_elems = *(uint32_t *)(opaque+field->num_offset);
            } else if (field->flags & VMS_VARRAY_UINT16) {
                n_elems = *(uint16_t *)(opaque+field->num_offset);
            } else if (field->flags & VMS_VARRAY_UINT8) {
                n_elems = *(uint8_t *)(opaque+field->num_offset);
            }
            if (field->flags & VMS_POINTER) {
                base_addr = *(void **)base_addr + field->start;
            }
            for (i = 0; i < n_elems; i++) {
                void *addr = base_addr + size * i;

                if (field->flags & VMS_ARRAY_OF_POINTER) {
                    addr = *(void **)addr;
                }
                if (field->flags & VMS_STRUCT) {
                    ret = vmstate_load_state(f, field->vmsd, addr, field->vmsd->version_id);
                } else {
                    ret = field->info->get(f, addr, size);

                }
                if (ret < 0) {
                    return ret;
                }
            }
        }
        field++;
    }
    ret = vmstate_subsection_load(f, vmsd, opaque);
    if (ret != 0) {
        return ret;
    }
    if (vmsd->post_load) {
        return vmsd->post_load(opaque, version_id);
    }
    return 0;
}

void vmstate_save_state(QEMUFile *f, const VMStateDescription *vmsd,
                        void *opaque)
{
    VMStateField *field = vmsd->fields;

    if (vmsd->pre_save) {
        vmsd->pre_save(opaque);
    }
    while(field->name) {
        if (!field->field_exists ||
            field->field_exists(opaque, vmsd->version_id)) {
            void *base_addr = opaque + field->offset;
            int i, n_elems = 1;
            int size = field->size;

            if (field->flags & VMS_VBUFFER) {
                size = *(int32_t *)(opaque+field->size_offset);
                if (field->flags & VMS_MULTIPLY) {
                    size *= field->size;
                }
            }
            if (field->flags & VMS_ARRAY) {
                n_elems = field->num;
            } else if (field->flags & VMS_VARRAY_INT32) {
                n_elems = *(int32_t *)(opaque+field->num_offset);
            } else if (field->flags & VMS_VARRAY_UINT32) {
                n_elems = *(uint32_t *)(opaque+field->num_offset);
            } else if (field->flags & VMS_VARRAY_UINT16) {
                n_elems = *(uint16_t *)(opaque+field->num_offset);
            } else if (field->flags & VMS_VARRAY_UINT8) {
                n_elems = *(uint8_t *)(opaque+field->num_offset);
            }
            if (field->flags & VMS_POINTER) {
                base_addr = *(void **)base_addr + field->start;
            }
            for (i = 0; i < n_elems; i++) {
                void *addr = base_addr + size * i;

                if (field->flags & VMS_ARRAY_OF_POINTER) {
                    addr = *(void **)addr;
                }
                if (field->flags & VMS_STRUCT) {
                    vmstate_save_state(f, field->vmsd, addr);
                } else {
                    field->info->put(f, addr, size);
                }
            }
        }
        field++;
    }
    vmstate_subsection_save(f, vmsd, opaque);
}

static int vmstate_load(QEMUFile *f, SaveStateEntry *se, int version_id)
{
    if (!se->vmsd) {         /* Old style */
        return se->ops->load_state(f, se->opaque, version_id);
    }
    return vmstate_load_state(f, se->vmsd, se->opaque, version_id);
}

static void vmstate_save(QEMUFile *f, SaveStateEntry *se)
{
    if (!se->vmsd) {         /* Old style */
        se->ops->save_state(f, se->opaque);
        return;
    }
    vmstate_save_state(f,se->vmsd, se->opaque);
}

#define QEMU_VM_FILE_MAGIC           0x5145564d
#define QEMU_VM_FILE_VERSION_COMPAT  0x00000002
#define QEMU_VM_FILE_VERSION         0x00000004

#define QEMU_VM_EOF                  0x00
#define QEMU_VM_SECTION_START        0x01
#define QEMU_VM_SECTION_PART         0x02
#define QEMU_VM_SECTION_END          0x03
#define QEMU_VM_SECTION_FULL         0x04
#define QEMU_VM_SUBSECTION           0x05

bool qemu_savevm_state_blocked(Error **errp)
{
    SaveStateEntry *se;

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        if (se->no_migrate) {
            error_set(errp, QERR_MIGRATION_NOT_SUPPORTED, se->idstr);
            return true;
        }
    }
    return false;
}

int qemu_savevm_state_begin(QEMUFile *f)
{
    SaveStateEntry *se;

    qemu_put_be32(f, QEMU_VM_FILE_MAGIC);
    qemu_put_be32(f, QEMU_VM_FILE_VERSION);

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        int len;

        if (!se->ops || !se->ops->save_live_state) {
            continue;
        }
#if 0
        if (se->ops && se->ops->is_active) {
            if (!se->ops->is_active(se->opaque)) {
                continue;
            }
        }
#endif
        /* Section type */
        qemu_put_byte(f, QEMU_VM_SECTION_START);
        qemu_put_be32(f, se->section_id);

        /* ID string */
        len = strlen(se->idstr);
        qemu_put_byte(f, len);
        qemu_put_buffer(f, (uint8_t *)se->idstr, len);

        qemu_put_be32(f, se->instance_id);
        qemu_put_be32(f, se->version_id);

        se->ops->save_live_state(f, QEMU_VM_SECTION_START, se->opaque);
    }

    return qemu_file_get_error(f);
}

int qemu_savevm_state_iterate(QEMUFile *f)
{
    SaveStateEntry *se;
    int ret = 1;

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        if (!se->ops || !se->ops->save_live_state) {
            continue;
        }
#if 0
        if (se->ops && se->ops->is_active) {
            if (!se->ops->is_active(se->opaque)) {
                continue;
            }
        }
#endif
        if (qemu_file_rate_limit(f)) {
            return 0;
        }
        //trace_savevm_section_start();
        /* Section type */
        qemu_put_byte(f, QEMU_VM_SECTION_PART);
        qemu_put_be32(f, se->section_id);

        ret &= !!se->ops->save_live_state(f, QEMU_VM_SECTION_PART, se->opaque);
    }

    if (ret)
        return 1;

    return qemu_file_get_error(f);
}

int qemu_savevm_state_complete(QEMUFile *f)
{
    SaveStateEntry *se;

    // cpu_synchronize_all_states();

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        if (!se->ops || se->ops->save_live_state == NULL) {
            continue;
        }
#if 0
        if (se->ops && se->ops->is_active) {
            if (!se->ops->is_active(se->opaque)) {
                continue;
            }
        }
#endif
        //trace_savevm_section_start();
        /* Section type */
        qemu_put_byte(f, QEMU_VM_SECTION_END);
        qemu_put_be32(f, se->section_id);

        se->ops->save_live_state(f, QEMU_VM_SECTION_END, se->opaque);
    }

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        int len;

        if ((!se->ops || !se->ops->save_state) && !se->vmsd) {
            continue;
        }

        /* Section type */
        qemu_put_byte(f, QEMU_VM_SECTION_FULL);
        qemu_put_be32(f, se->section_id);

        /* ID string */
        len = strlen(se->idstr);
        qemu_put_byte(f, len);
        qemu_put_buffer(f, (uint8_t *)se->idstr, len);

        qemu_put_be32(f, se->instance_id);
        qemu_put_be32(f, se->version_id);

        vmstate_save(f, se);
    }

    qemu_put_byte(f, QEMU_VM_EOF);
    qemu_fflush(f);

    return qemu_file_get_error(f);
}

int qemu_savevm_state(QEMUFile *f)
{
    int saved_vm_running;
    int ret;

    if (qemu_savevm_state_blocked(NULL)) {
        return -EINVAL;
    }

    saved_vm_running = vm_running;
    vm_stop(0);

    bdrv_flush_all();

    ret = qemu_savevm_state_begin(f);
    if (ret < 0)
        goto out;

    do {
        ret = qemu_savevm_state_iterate(f);
        if (ret < 0)
            goto out;
    } while (ret == 0);

    ret = qemu_savevm_state_complete(f);

out:
    ret = qemu_file_get_error(f);

    if (!ret && saved_vm_running)
        vm_start();

    return ret;
}

static SaveStateEntry *find_se(const char *idstr, int instance_id)
{
    SaveStateEntry *se;

    QTAILQ_FOREACH(se, &savevm_handlers, entry) {
        if (!strcmp(se->idstr, idstr) &&
            (instance_id == se->instance_id ||
             instance_id == se->alias_id))
            return se;
        /* Migrating from an older version? */
        if (strstr(se->idstr, idstr) && se->compat) {
            if (!strcmp(se->compat->idstr, idstr) &&
                (instance_id == se->compat->instance_id ||
                 instance_id == se->alias_id))
                return se;
        }
    }
    return NULL;
}

static const VMStateDescription *vmstate_get_subsection(const VMStateSubsection *sub, char *idstr)
{
    while(sub && sub->needed) {
        if (strcmp(idstr, sub->vmsd->name) == 0) {
            return sub->vmsd;
        }
        sub++;
    }
    return NULL;
}

static int vmstate_subsection_load(QEMUFile *f, const VMStateDescription *vmsd,
                                   void *opaque)
{
    while (qemu_peek_byte(f, 0) == QEMU_VM_SUBSECTION) {
        char idstr[256];
        int ret;
        uint8_t version_id, len, size;
        const VMStateDescription *sub_vmsd;

        len = qemu_peek_byte(f, 1);
        if (len < strlen(vmsd->name) + 1) {
            /* subsection name has be be "section_name/a" */
            return 0;
        }
        size = qemu_peek_buffer(f, (uint8_t *)idstr, len, 2);
        if (size != len) {
            return 0;
        }
        idstr[size] = 0;

        if (strncmp(vmsd->name, idstr, strlen(vmsd->name)) != 0) {
            /* it don't have a valid subsection name */
            return 0;
        }
        sub_vmsd = vmstate_get_subsection(vmsd->subsections, idstr);
        if (sub_vmsd == NULL) {
            return -ENOENT;
        }
        qemu_file_skip(f, 1); /* subsection */
        qemu_file_skip(f, 1); /* len */
        qemu_file_skip(f, len); /* idstr */
        version_id = qemu_get_be32(f);

        ret = vmstate_load_state(f, sub_vmsd, opaque, version_id);
        if (ret) {
            return ret;
        }
    }
    return 0;
}

static void vmstate_subsection_save(QEMUFile *f, const VMStateDescription *vmsd,
                                    void *opaque)
{
    const VMStateSubsection *sub = vmsd->subsections;

    while (sub && sub->needed) {
        if (sub->needed(opaque)) {
            const VMStateDescription *vmsd = sub->vmsd;
            uint8_t len;

            qemu_put_byte(f, QEMU_VM_SUBSECTION);
            len = strlen(vmsd->name);
            qemu_put_byte(f, len);
            qemu_put_buffer(f, (uint8_t *)vmsd->name, len);
            qemu_put_be32(f, vmsd->version_id);
            vmstate_save_state(f, vmsd, opaque);
        }
        sub++;
    }
}

typedef struct LoadStateEntry {
    QLIST_ENTRY(LoadStateEntry) entry;
    SaveStateEntry *se;
    int section_id;
    int version_id;
} LoadStateEntry;

int qemu_loadvm_state(QEMUFile *f)
{
    QLIST_HEAD(, LoadStateEntry) loadvm_handlers =
        QLIST_HEAD_INITIALIZER(loadvm_handlers);
    LoadStateEntry *le, *new_le;
    uint8_t section_type;
    unsigned int v;
    int ret;

    if (qemu_savevm_state_blocked(NULL)) {
        return -EINVAL;
    }

    v = qemu_get_be32(f);
    if (v != QEMU_VM_FILE_MAGIC)
        return -EINVAL;

    v = qemu_get_be32(f);
    if (v < QEMU_VM_FILE_VERSION) {
        fprintf(stderr, "Snapshot format %d is too old for this version of the emulator, please create a new one.\n", v);
        return -ENOTSUP;
    } else if (v > QEMU_VM_FILE_VERSION) {
        fprintf(stderr, "Snapshot format %d is more recent than the emulator, please update your Android SDK Tools.\n", v);
        return -ENOTSUP;
    }

    while ((section_type = qemu_get_byte(f)) != QEMU_VM_EOF) {
        uint32_t instance_id, version_id, section_id;
        SaveStateEntry *se;
        char idstr[257];
        int len;

        switch (section_type) {
        case QEMU_VM_SECTION_START:
        case QEMU_VM_SECTION_FULL:
            /* Read section start */
            section_id = qemu_get_be32(f);
            len = qemu_get_byte(f);
            qemu_get_buffer(f, (uint8_t *)idstr, len);
            idstr[len] = 0;
            instance_id = qemu_get_be32(f);
            version_id = qemu_get_be32(f);

            /* Find savevm section */
            se = find_se(idstr, instance_id);
            if (se == NULL) {
                fprintf(stderr, "Unknown savevm section or instance '%s' %d\n", idstr, instance_id);
                ret = -EINVAL;
                goto out;
            }

            /* Validate version */
            if (version_id > (uint32_t)se->version_id) {
                fprintf(stderr, "savevm: unsupported version %d for '%s' v%d\n",
                        version_id, idstr, se->version_id);
                ret = -EINVAL;
                goto out;
            }

            /* Add entry */
            le = g_malloc0(sizeof(*le));

            le->se = se;
            le->section_id = section_id;
            le->version_id = version_id;
            QLIST_INSERT_HEAD(&loadvm_handlers, le, entry);

            ret = vmstate_load(f, le->se, le->version_id);
            if (ret < 0) {
                fprintf(stderr, "qemu: warning: error while loading state for instance 0x%x of device '%s'\n",
                        instance_id, idstr);
                goto out;
            }
            break;
        case QEMU_VM_SECTION_PART:
        case QEMU_VM_SECTION_END:
            section_id = qemu_get_be32(f);

            QLIST_FOREACH(le, &loadvm_handlers, entry) {
                if ((uint32_t)le->section_id == section_id) {
                    break;
                }
            }
            if (le == NULL) {
                fprintf(stderr, "Unknown savevm section %d\n", section_id);
                ret = -EINVAL;
                goto out;
            }

            ret = vmstate_load(f, le->se, le->version_id);
            if (ret < 0) {
                fprintf(stderr, "qemu: warning: error while loading state section id %d\n",
                        section_id);
                goto out;
            }
            break;
        default:
            fprintf(stderr, "Unknown savevm section type %d\n", section_type);
            ret = -EINVAL;
            goto out;
        }
    }

    //cpu_synchronize_all_post_init();

    ret = 0;

out:
    QLIST_FOREACH_SAFE(le, &loadvm_handlers, entry, new_le) {
        QLIST_REMOVE(le, entry);
        g_free(le);
    }

    if (ret == 0) {
        ret = qemu_file_get_error(f);
    }

    return ret;
}
#if 0
static BlockDriverState *get_bs_snapshots(void)
{
    BlockDriverState *bs;
    int i;

    if (bs_snapshots)
        return bs_snapshots;
    for(i = 0; i <= nb_drives; i++) {
        bs = drives_table[i].bdrv;
        if (bdrv_can_snapshot(bs))
            goto ok;
    }
    return NULL;
 ok:
    bs_snapshots = bs;
    return bs;
}
#endif
static int bdrv_snapshot_find(BlockDriverState *bs, QEMUSnapshotInfo *sn_info,
                              const char *name)
{
    QEMUSnapshotInfo *sn_tab, *sn;
    int nb_sns, i, ret;

    ret = -ENOENT;
    nb_sns = bdrv_snapshot_list(bs, &sn_tab);
    if (nb_sns < 0)
        return ret;
    for(i = 0; i < nb_sns; i++) {
        sn = &sn_tab[i];
        if (!strcmp(sn->id_str, name) || !strcmp(sn->name, name)) {
            *sn_info = *sn;
            ret = 0;
            break;
        }
    }
    g_free(sn_tab);
    return ret;
}

void do_savevm(Monitor *err, const char *name)
{
    BlockDriverState *bs, *bs1;
    QEMUSnapshotInfo sn1, *sn = &sn1, old_sn1, *old_sn = &old_sn1;
    int must_delete, ret;
    BlockDriverInfo bdi1, *bdi = &bdi1;
    QEMUFile *f;
    int saved_vm_running;
    uint32_t vm_state_size;
#ifdef _WIN32
    struct _timeb tb;
#else
    struct timeval tv;
#endif

    bs = bdrv_snapshots();
    if (!bs) {
        monitor_printf(err, "No block device can accept snapshots\n");
        return;
    }

    /* ??? Should this occur after vm_stop?  */
    qemu_aio_flush();

    saved_vm_running = vm_running;
    vm_stop(0);

    must_delete = 0;
    if (name) {
        ret = bdrv_snapshot_find(bs, old_sn, name);
        if (ret >= 0) {
            must_delete = 1;
        }
    }
    memset(sn, 0, sizeof(*sn));
    if (must_delete) {
        pstrcpy(sn->name, sizeof(sn->name), old_sn->name);
        pstrcpy(sn->id_str, sizeof(sn->id_str), old_sn->id_str);
    } else {
        if (name)
            pstrcpy(sn->name, sizeof(sn->name), name);
    }

    /* fill auxiliary fields */
#ifdef _WIN32
    _ftime(&tb);
    sn->date_sec = tb.time;
    sn->date_nsec = tb.millitm * 1000000;
#else
    gettimeofday(&tv, NULL);
    sn->date_sec = tv.tv_sec;
    sn->date_nsec = tv.tv_usec * 1000;
#endif
    sn->vm_clock_nsec = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (bdrv_get_info(bs, bdi) < 0 || bdi->vm_state_offset <= 0) {
        monitor_printf(err, "Device %s does not support VM state snapshots\n",
                              bdrv_get_device_name(bs));
        goto the_end;
    }

    /* save the VM state */
    f = qemu_fopen_bdrv(bs, 1);
    if (!f) {
        monitor_printf(err, "Could not open VM state file\n");
        goto the_end;
    }
    ret = qemu_savevm_state(f);
    vm_state_size = qemu_ftell(f);
    qemu_fclose(f);
    if (ret < 0) {
        monitor_printf(err, "Error %d while writing VM\n", ret);
        goto the_end;
    }

    /* create the snapshots */

    bs1 = NULL;
    while ((bs1 = bdrv_next(bs1))) {
        if (bdrv_can_snapshot(bs1)) {
            if (must_delete) {
                ret = bdrv_snapshot_delete(bs1, old_sn->id_str);
                if (ret < 0) {
                    monitor_printf(err,
                                          "Error while deleting snapshot on '%s'\n",
                                          bdrv_get_device_name(bs1));
                }
            }
            /* Write VM state size only to the image that contains the state */
            sn->vm_state_size = (bs == bs1 ? vm_state_size : 0);
            ret = bdrv_snapshot_create(bs1, sn);
            if (ret < 0) {
                monitor_printf(err, "Error while creating snapshot on '%s'\n",
                                      bdrv_get_device_name(bs1));
            }
        }
    }

 the_end:
    if (saved_vm_running)
        vm_start();
}

void do_loadvm(Monitor *err, const char *name)
{
    BlockDriverState *bs, *bs1;
    BlockDriverInfo bdi1, *bdi = &bdi1;
    QEMUSnapshotInfo sn;
    QEMUFile *f;
    int ret;
    int saved_vm_running;

    bs = bdrv_snapshots();
    if (!bs) {
        monitor_printf(err, "No block device supports snapshots\n");
        return;
    }

    /* Flush all IO requests so they don't interfere with the new state.  */
    qemu_aio_flush();

    saved_vm_running = vm_running;
    vm_stop(0);

    bs1 = bs;
    do {
        if (bdrv_can_snapshot(bs1)) {
            ret = bdrv_snapshot_goto(bs1, name);
            if (ret < 0) {
                if (bs != bs1)
                    monitor_printf(err, "Warning: ");
                switch(ret) {
                case -ENOTSUP:
                    monitor_printf(err,
                                   "Snapshots not supported on device '%s'\n",
                                   bdrv_get_device_name(bs1));
                    break;
                case -ENOENT:
                    monitor_printf(err, "Could not find snapshot '%s' on "
                                   "device '%s'\n",
                                   name, bdrv_get_device_name(bs1));
                    break;
                default:
                    monitor_printf(err, "Error %d while activating snapshot on"
                                   " '%s'\n", ret, bdrv_get_device_name(bs1));
                    break;
                }
                /* fatal on snapshot block device */
                if (bs == bs1)
                    goto the_end;
            }
        }
    } while ((bs1 = bdrv_next(bs)));

    if (bdrv_get_info(bs, bdi) < 0 || bdi->vm_state_offset <= 0) {
        monitor_printf(err, "Device %s does not support VM state snapshots\n",
                       bdrv_get_device_name(bs));
        return;
    }

    /* Don't even try to load empty VM states */
    ret = bdrv_snapshot_find(bs, &sn, name);
    if ((ret >= 0) && (sn.vm_state_size == 0))
        goto the_end;

    /* restore the VM state */
    f = qemu_fopen_bdrv(bs, 0);
    if (!f) {
        monitor_printf(err, "Could not open VM state file\n");
        goto the_end;
    }
    ret = qemu_loadvm_state(f);
    qemu_fclose(f);
    if (ret < 0) {
        monitor_printf(err, "Error %d while loading VM state\n", ret);
    }
 the_end:
    if (saved_vm_running)
        vm_start();
}

void do_delvm(Monitor *err, const char *name)
{
    BlockDriverState *bs, *bs1;
    int ret;

    bs = bdrv_snapshots();
    if (!bs) {
        monitor_printf(err, "No block device supports snapshots\n");
        return;
    }

    bs1 = NULL;
    while ((bs1 = bdrv_next(bs1))) {
        if (bdrv_can_snapshot(bs1)) {
            ret = bdrv_snapshot_delete(bs1, name);
            if (ret < 0) {
                if (ret == -ENOTSUP)
                    monitor_printf(err,
                                          "Snapshots not supported on device '%s'\n",
                                          bdrv_get_device_name(bs1));
                else
                    monitor_printf(err, "Error %d while deleting snapshot on "
                                          "'%s'\n", ret, bdrv_get_device_name(bs1));
            }
        }
    }
}

void do_info_snapshots(Monitor* out, Monitor* err)
{
    BlockDriverState *bs, *bs1;
    QEMUSnapshotInfo *sn_tab, *sn;
    int nb_sns, i;
    char buf[256];

    bs = bdrv_snapshots();
    if (!bs) {
        monitor_printf(err, "No available block device supports snapshots\n");
        return;
    }
    monitor_printf(out, "Snapshot devices:");
    bs1 = NULL;
    while ((bs1 = bdrv_next(bs1))) {
        if (bdrv_can_snapshot(bs1)) {
            if (bs == bs1)
                monitor_printf(out, " %s", bdrv_get_device_name(bs1));
        }
    }
    monitor_printf(out, "\n");

    nb_sns = bdrv_snapshot_list(bs, &sn_tab);
    if (nb_sns < 0) {
        monitor_printf(err, "bdrv_snapshot_list: error %d\n", nb_sns);
        return;
    }
    monitor_printf(out, "Snapshot list (from %s):\n",
                   bdrv_get_device_name(bs));
    monitor_printf(out, "%s\n", bdrv_snapshot_dump(buf, sizeof(buf), NULL));
    for(i = 0; i < nb_sns; i++) {
        sn = &sn_tab[i];
        monitor_printf(out, "%s\n", bdrv_snapshot_dump(buf, sizeof(buf), sn));
    }
    g_free(sn_tab);
}
