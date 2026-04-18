/*
 * PicoScope firmware extractor — LD_PRELOAD libusb shim.
 *
 * Intercepts the libusb calls made by the official Pico software when it
 * opens a PicoScope 2204A, captures the firmware blobs that are uploaded
 * to the device, and writes them to
 *     $PS2204A_FIRMWARE_DIR               (if set)
 *     $XDG_CONFIG_HOME/picoscope-libusb/firmware   (if set)
 *     $HOME/.config/picoscope-libusb/firmware      (fallback)
 *
 * Output files:
 *   fx2.bin        — packed [addr_be16][len_u8][data] chunks for the FX2 MCU
 *   fpga.bin       — concatenated FPGA bitstream (EP 0x06, large chunks)
 *   waveform.bin   — 8 KiB channel LUT (first small EP 0x06 chunk)
 *   stream_lut.bin — identical to waveform.bin (the 2204A reuses the same LUT)
 *   usb_trace.log  — human-readable protocol trace (unchanged from the tracer)
 *
 * Build:   see Makefile (produces libps_intercept.so)
 * Use:     LD_PRELOAD=./libps_intercept.so <any-pico-program>
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <libusb-1.0/libusb.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static FILE *logfile = NULL;
static int   packet_count = 0;
static struct timespec t_start;

/* FX2 packed buffer: we rebuild the same format the driver loads back.
 * The CPU halt/release markers (wValue=0xE600) are dropped: the driver
 * emits those itself. */
#define FX2_BUF_CAP   (1024 * 1024)
static uint8_t *fx2_buf = NULL;
static size_t   fx2_len = 0;
static int      fx2_chunks = 0;

/* FPGA bitstream: append every "large" (>= 1024 bytes) EP 0x06 OUT. */
#define FPGA_BUF_CAP  (4 * 1024 * 1024)
static uint8_t *fpga_buf = NULL;
static size_t   fpga_len = 0;

/* Waveform / stream LUT: first 8 KiB EP 0x06 transfer we see.
 * The 2204A uploads this blob twice (once per channel) and reuses the same
 * bytes as the SDK streaming LUT, so one captured copy covers both. */
static uint8_t *lut_buf = NULL;
static size_t   lut_len = 0;

/* Upstream libusb symbols. */
static int (*real_bulk)(libusb_device_handle *, unsigned char,
                        unsigned char *, int, int *, unsigned int) = NULL;
static int (*real_ctrl)(libusb_device_handle *, uint8_t, uint8_t,
                        uint16_t, uint16_t, unsigned char *,
                        uint16_t, unsigned int) = NULL;
static int (*real_submit)(struct libusb_transfer *) = NULL;
static int (*real_open)(libusb_device *, libusb_device_handle **) = NULL;
static void (*real_close)(libusb_device_handle *) = NULL;
static void *libusb_dl = NULL;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static long long now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)(t.tv_sec - t_start.tv_sec) * 1000000LL
         + ((long long)t.tv_nsec - (long long)t_start.tv_nsec) / 1000LL;
}

static void hex_dump(FILE *f, const char *prefix,
                     const unsigned char *data, int len) {
    if (!f || !data || len <= 0) return;
    fprintf(f, "%s (%d bytes): ", prefix, len);
    int limit = (len > 128) ? 128 : len;
    for (int i = 0; i < limit; i++) fprintf(f, "%02x ", data[i]);
    if (len > 128) fprintf(f, "...");
    fputc('\n', f);
}

/* Resolve the output directory. Same search order as the driver. */
static int resolve_output_dir(char *out, size_t out_size) {
    const char *env = getenv("PS2204A_FIRMWARE_DIR");
    if (env && *env) { snprintf(out, out_size, "%s", env); return 0; }

    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(out, out_size, "%s/picoscope-libusb/firmware", xdg);
        return 0;
    }

    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(out, out_size, "%s/.config/picoscope-libusb/firmware", home);
        return 0;
    }
    return -1;
}

/* mkdir -p for a single known-shape path. */
static int mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}

static int write_file(const char *dir, const char *name,
                      const uint8_t *buf, size_t len) {
    char path[1280];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[ps_intercept] cannot write %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    size_t n = fwrite(buf, 1, len, f);
    fclose(f);
    if (n != len) {
        fprintf(stderr, "[ps_intercept] short write on %s\n", path);
        return -1;
    }
    fprintf(stderr, "[ps_intercept] wrote %s (%zu bytes)\n", path, len);
    return 0;
}

static void resolve_symbols(void) {
    if (real_submit) return;
    real_submit = dlsym(RTLD_NEXT, "libusb_submit_transfer");
    if (!real_submit) {
        libusb_dl = dlopen("libusb-1.0.so.0", RTLD_NOW | RTLD_GLOBAL);
        if (libusb_dl) {
            real_submit = dlsym(libusb_dl, "libusb_submit_transfer");
            real_bulk   = dlsym(libusb_dl, "libusb_bulk_transfer");
            real_ctrl   = dlsym(libusb_dl, "libusb_control_transfer");
            real_open   = dlsym(libusb_dl, "libusb_open");
            real_close  = dlsym(libusb_dl, "libusb_close");
        }
    } else {
        real_bulk  = dlsym(RTLD_NEXT, "libusb_bulk_transfer");
        real_ctrl  = dlsym(RTLD_NEXT, "libusb_control_transfer");
        real_open  = dlsym(RTLD_NEXT, "libusb_open");
        real_close = dlsym(RTLD_NEXT, "libusb_close");
    }
}

/* ------------------------------------------------------------------ */
/* Capture logic                                                       */
/* ------------------------------------------------------------------ */

/* Record one FX2 control-transfer chunk: `addr` = wValue, `data` = payload.
 * Output is the packed format the driver expects:
 *     [addr_hi][addr_lo][len_u8][data[len]]+
 * The CPU-halt/release control chunks (wValue == 0xE600) are skipped — the
 * driver emits those itself. */
static void record_fx2_chunk(uint16_t addr, const uint8_t *data, int len) {
    if (addr == 0xE600) return;                   /* CPUCS marker */
    if (!data || len <= 0 || len > 255) return;
    if (fx2_len + 3 + (size_t)len > FX2_BUF_CAP) return;

    fx2_buf[fx2_len++] = (uint8_t)(addr >> 8);
    fx2_buf[fx2_len++] = (uint8_t)(addr & 0xff);
    fx2_buf[fx2_len++] = (uint8_t)len;
    memcpy(fx2_buf + fx2_len, data, (size_t)len);
    fx2_len += (size_t)len;
    fx2_chunks++;
}

/* Record an EP 0x06 bulk OUT. Large transfers (> 1 KiB) go to fpga.bin;
 * the first 8 KiB transfer (the channel waveform / stream LUT) goes to
 * lut_buf. */
static void record_ep06(const uint8_t *data, int len) {
    if (!data || len <= 0) return;

    if (len == 8192) {
        if (!lut_buf) {
            lut_buf = (uint8_t *)malloc(8192);
            if (lut_buf) { memcpy(lut_buf, data, 8192); lut_len = 8192; }
        }
        /* Drop every 8 KiB transfer from the FPGA stream — these are all
         * channel LUT uploads (one per enabled channel). */
        return;
    }
    if (len >= 1024) {
        if (fpga_len + (size_t)len <= FPGA_BUF_CAP) {
            memcpy(fpga_buf + fpga_len, data, (size_t)len);
            fpga_len += (size_t)len;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Constructors / destructors                                          */
/* ------------------------------------------------------------------ */

__attribute__((constructor))
static void init(void) {
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    fx2_buf  = (uint8_t *)malloc(FX2_BUF_CAP);
    fpga_buf = (uint8_t *)malloc(FPGA_BUF_CAP);
    if (!fx2_buf || !fpga_buf) {
        fprintf(stderr, "[ps_intercept] out of memory\n");
        _exit(1);
    }

    logfile = fopen("usb_trace.log", "w");
    if (logfile) {
        fprintf(logfile, "=== PicoScope USB Protocol Trace ===\n");
        fprintf(logfile, "Timestamp: %ld\n\n", time(NULL));
        fflush(logfile);
    }
    resolve_symbols();
    fprintf(stderr, "[ps_intercept] ready — run any Pico software now.\n");
}

__attribute__((destructor))
static void finish(void) {
    if (logfile) {
        fprintf(logfile, "\n=== end of trace (%d packets) ===\n",
                packet_count);
        fclose(logfile);
        logfile = NULL;
    }

    char dir[1024];
    if (resolve_output_dir(dir, sizeof(dir)) != 0) {
        fprintf(stderr, "[ps_intercept] cannot locate output directory "
                "(set PS2204A_FIRMWARE_DIR or HOME)\n");
        return;
    }
    mkdir_p(dir);

    fprintf(stderr,
            "[ps_intercept] captured: fx2=%zu B (%d chunks), fpga=%zu B, "
            "lut=%zu B\n", fx2_len, fx2_chunks, fpga_len, lut_len);

    if (fx2_len == 0 || fpga_len == 0 || lut_len == 0) {
        fprintf(stderr,
            "[ps_intercept] INCOMPLETE capture — one or more blobs are empty.\n"
            "  Did the scope actually connect? Try unplugging & replugging\n"
            "  before re-running under LD_PRELOAD.\n");
    }

    if (fx2_len)  write_file(dir, "fx2.bin",        fx2_buf,  fx2_len);
    if (fpga_len) write_file(dir, "fpga.bin",       fpga_buf, fpga_len);
    if (lut_len) {
        write_file(dir, "waveform.bin",   lut_buf, lut_len);
        write_file(dir, "stream_lut.bin", lut_buf, lut_len);
    }

    free(fx2_buf);  fx2_buf  = NULL;
    free(fpga_buf); fpga_buf = NULL;
    free(lut_buf);  lut_buf  = NULL;
    if (libusb_dl) { dlclose(libusb_dl); libusb_dl = NULL; }
}

/* ------------------------------------------------------------------ */
/* Intercepted entry points                                            */
/* ------------------------------------------------------------------ */

int libusb_submit_transfer(struct libusb_transfer *transfer) {
    if (!real_submit) resolve_symbols();
    if (!real_submit) return LIBUSB_ERROR_OTHER;

    unsigned char ep  = transfer->endpoint;
    int           len = transfer->length;
    bool          is_in = (ep & 0x80) != 0;

    if (!is_in && transfer->buffer && len > 0) {
        if (transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
            struct libusb_control_setup *s =
                (struct libusb_control_setup *)transfer->buffer;
            if (s->bRequest == 0xA0 && len > 8) {
                record_fx2_chunk(s->wValue, transfer->buffer + 8, len - 8);
            }
            if (logfile) {
                fprintf(logfile,
                        "\n[%04d] t=%lld ASYNC CTRL OUT reqType=0x%02x "
                        "bReq=0x%02x wVal=0x%04x wIdx=0x%04x wLen=%d\n",
                        packet_count, now_us(), s->bmRequestType, s->bRequest,
                        s->wValue, s->wIndex, s->wLength);
                if (len > 8) hex_dump(logfile, "  TX",
                                      transfer->buffer + 8, len - 8);
                fflush(logfile);
            }
        } else {
            if ((ep & 0x7f) == 0x06) record_ep06(transfer->buffer, len);
            if (logfile) {
                fprintf(logfile, "\n[%04d] t=%lld ASYNC BULK OUT EP 0x%02x "
                        "(len=%d)\n", packet_count, now_us(), ep, len);
                hex_dump(logfile, "  TX", transfer->buffer, len);
                fflush(logfile);
            }
        }
        packet_count++;
    }
    return real_submit(transfer);
}

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep,
                         unsigned char *data, int length,
                         int *actual_length, unsigned int timeout) {
    if (!real_bulk) resolve_symbols();
    if (!real_bulk) return -1;

    bool is_in = (ep & 0x80) != 0;
    if (!is_in && (ep & 0x7f) == 0x06) record_ep06(data, length);

    if (!is_in && logfile) {
        fprintf(logfile, "\n[%04d] t=%lld SYNC BULK OUT EP 0x%02x (len=%d)\n",
                packet_count, now_us(), ep, length);
        hex_dump(logfile, "  TX", data, length);
        fflush(logfile);
    }

    int r = real_bulk(h, ep, data, length, actual_length, timeout);

    if (is_in && logfile) {
        fprintf(logfile, "\n[%04d] t=%lld SYNC BULK IN  EP 0x%02x "
                "(actual=%d, ret=%d)\n", packet_count, now_us(), ep,
                actual_length ? *actual_length : 0, r);
        if (actual_length && *actual_length > 0)
            hex_dump(logfile, "  RX", data, *actual_length);
        fflush(logfile);
    }
    packet_count++;
    return r;
}

int libusb_control_transfer(libusb_device_handle *h, uint8_t rt,
                            uint8_t bReq, uint16_t wVal, uint16_t wIdx,
                            unsigned char *data, uint16_t wLen,
                            unsigned int timeout) {
    if (!real_ctrl) resolve_symbols();
    if (!real_ctrl) return -1;

    bool is_in = (rt & 0x80) != 0;
    if (!is_in && bReq == 0xA0 && wLen > 0 && data)
        record_fx2_chunk(wVal, data, wLen);

    if (logfile) {
        fprintf(logfile,
                "\n[%04d] t=%lld SYNC CTRL %s req=0x%02x val=0x%04x "
                "idx=0x%04x len=%d\n", packet_count, now_us(),
                is_in ? "IN " : "OUT", bReq, wVal, wIdx, wLen);
        if (!is_in && wLen > 0 && data) hex_dump(logfile, "  TX", data, wLen);
        fflush(logfile);
    }

    int r = real_ctrl(h, rt, bReq, wVal, wIdx, data, wLen, timeout);

    if (logfile && is_in && r > 0 && data) {
        hex_dump(logfile, "  RX", data, r);
        fflush(logfile);
    }
    packet_count++;
    return r;
}

int libusb_open(libusb_device *dev, libusb_device_handle **h) {
    if (!real_open) resolve_symbols();
    if (!real_open) return -1;
    int r = real_open(dev, h);
    if (logfile) { fprintf(logfile, "\n=== DEVICE OPENED (ret=%d) ===\n", r); fflush(logfile); }
    return r;
}

void libusb_close(libusb_device_handle *h) {
    if (!real_close) resolve_symbols();
    if (!real_close) return;
    if (logfile) { fprintf(logfile, "\n=== DEVICE CLOSED ===\n"); fflush(logfile); }
    real_close(h);
}
