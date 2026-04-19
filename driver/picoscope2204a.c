/*
 * PicoScope 2204A libusb driver — implementation
 *
 * Reverse-engineered USB protocol for PS2204A:
 *   Cypress FX2 (CY7C68013A) + Xilinx FPGA, 8-bit ADC
 *   VID=0x0CE9, PID=0x1007
 *
 * All byte sequences verified against SDK USB traces (2026-02-10).
 */

#include "picoscope2204a.h"

#include <libusb-1.0/libusb.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#define PS_LOG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "ps2204a-drv", fmt, ##__VA_ARGS__)
#else
#define PS_LOG(fmt, ...) fprintf(stderr, "[ps2204a] " fmt "\n", ##__VA_ARGS__)
#endif

/* ========================================================================
 * Constants
 * ======================================================================== */

#define PICO_VID        0x0CE9
#define PICO_PID        0x1007

#define EP_CMD_OUT      0x01
#define EP_RESP_IN      0x81
#define EP_DATA_IN      0x82
#define EP_FW_OUT       0x06

#define FX2_VENDOR_REQ  0xA0
#define FX2_CPUCS_ADDR  0xE600

#define CMD_SIZE        64
#define DATA_BUF_SIZE   16384
#define FPGA_CHUNK_SIZE 32768

#define ADC_CENTER      128
#define ADC_HALF_RANGE  128.0f

#define TIMEOUT_CMD     5000
#define TIMEOUT_RESP    500
#define TIMEOUT_DATA    15000
#define TIMEOUT_FW      10000

#define MAX_SAMPLES_SINGLE  8064
#define MAX_SAMPLES_DUAL    3968

#define DEFAULT_RING_SIZE   1048576  /* 1M samples */

/* ========================================================================
 * Lookup tables (verified against SDK USB traces)
 * ======================================================================== */

/* PGA gain table: (bank, selector, flag_200mV) per range */
typedef struct { uint8_t bank, sel, f200; } pga_entry_t;

/* Hardware PGA configuration per range — verified by capturing the real
 * PicoSDK's cmd1 bytes (`85 07 97 ... b50 b51 b52`) for every enum value
 * and decoding them. The previous table in this driver was shifted by
 * one slot, which meant every range was programmed with the PGA of the
 * NEXT range (e.g. a user-requested 20V actually used the 10V PGA and
 * clipped at ~12V). That bug was masked by compensating (incorrect)
 * RANGE_MV values in the next table below. */
static const pga_entry_t PGA_TABLE[] = {
    /* PS_50MV=2  */ {1, 6, 0},
    /* PS_100MV=3 */ {1, 7, 0},
    /* PS_200MV=4 */ {1, 2, 1},
    /* PS_500MV=5 */ {1, 3, 0},
    /* PS_1V=6    */ {1, 1, 0},
    /* PS_2V=7    */ {0, 7, 0},
    /* PS_5V=8    */ {0, 2, 0},
    /* PS_10V=9   */ {0, 3, 0},
    /* PS_20V=10  */ {0, 1, 0},
};

/* Effective ADC full-scale range in mV, indexed by (ps_range_t - 2).
 * Values are the SDK nominal ranges — each range has its own dedicated
 * hardware PGA config, no digital scaling needed.
 * Fine per-unit gain correction is handled through dev->cal_gain[]. */
static const int RANGE_MV[] = {
    /*  50mV */    50,
    /* 100mV */   100,
    /* 200mV */   200,
    /* 500mV */   500,
    /*    1V */  1000,
    /*    2V */  2000,
    /*    5V */  5000,
    /*   10V */ 10000,
    /*   20V */ 20000
};

/* Timebase channel bytes for tb=0..10 */
static const uint8_t TB_CHAN[][2] = {
    {0x27, 0x2f}, /* tb=0  */
    {0x13, 0xa7}, /* tb=1  */
    {0x09, 0xe3}, /* tb=2  */
    {0x05, 0x01}, /* tb=3  */
    {0x02, 0x90}, /* tb=4  */
    {0x01, 0x57}, /* tb=5  */
    {0x00, 0xbb}, /* tb=6  */
    {0x00, 0x6d}, /* tb=7  */
    {0x00, 0x46}, /* tb=8  */
    {0x00, 0x33}, /* tb=9  */
    {0x00, 0x28}, /* tb=10 */
};

/* ========================================================================
 * Device structure
 * ======================================================================== */

struct ps2204a_device {
    libusb_context       *ctx;
    libusb_device_handle *handle;
    bool                  owns_ctx;  /* true if we created ctx */

    /* Channel configuration (stored locally, applied in capture cmd) */
    struct {
        bool          enabled;
        ps_coupling_t coupling;
        ps_range_t    range;
    } ch[2];

    int timebase;
    int n_samples;

    /* Trigger command (64 bytes, ready to send) */
    uint8_t trigger_cmd[CMD_SIZE];

    /* Streaming */
    pthread_t        stream_thread;
    volatile bool    streaming;      /* User intent: true while caller wants stream */
    bool             thread_started; /* True while stream_thread is joinable */
    pthread_mutex_t  stream_mutex;   /* Protects ring_* and stream_* stats */
    ps_stream_mode_t stream_mode;
    float           *ring_a;
    float           *ring_b;
    volatile size_t  ring_write_pos;
    size_t           ring_capacity;
    ps_stream_cb_t   stream_cb;
    void            *stream_user;
    void            *stream_internal; /* Private state for streaming thread */

    /* Streaming stats */
    uint64_t        stream_blocks;
    uint64_t        stream_samples_total;
    struct timespec stream_start;
    double          stream_last_block_ms;

    /* Signal generator state — persisted so that per-capture cmd2 doesn't
     * clobber user-set values (cmd2 embeds a siggen sub-command). */
    uint32_t siggen_freq_param;  /* freq_hz * 0x400, little-endian in cmd */
    uint8_t  siggen_wave;        /* ps_wave_t */

    /* Block-mode trigger state (encoded inside cmd1/cmd2 at capture time). */
    bool             trigger_armed;    /* true = armed, false = free-run */
    ps_channel_t     trigger_source;   /* PS_CHANNEL_A or PS_CHANNEL_B */
    int16_t          trigger_thr_sdk;  /* signed 16-bit SDK scale, ±32767 */
    ps_trigger_dir_t trigger_dir;      /* PS_RISING / PS_FALLING */
    int16_t          trigger_delay_pct;/* -100..+100, 0 = centred */
    uint8_t          trigger_hyst;     /* ADC counts, default 10 */
    ps_trigger_mode_t trigger_mode;    /* LEVEL / WINDOW / PWQ */
    int16_t          trigger_thr2_sdk; /* WINDOW upper threshold (SDK scale) */

    /* Signal generator sweep & offset / arbitrary LUT state. */
    uint32_t siggen_freq_stop_param;   /* BE uint32 for [18..21], max(start,stop) */
    uint32_t siggen_inc_param;         /* increment (inc_hz × 22906.368) for [26..29] */
    uint16_t siggen_dwell_samples;     /* dwell_s × 187500 for [30..31] */
    int32_t  siggen_offset_uv;         /* DC offset applied in LUT */
    uint8_t  siggen_duty_pct;          /* SQUARE duty cycle (0..100) */
    bool     siggen_use_arb;           /* use siggen_arb_lut instead of computing */
    int16_t  siggen_arb_lut[4096];
    /* True once the user has configured an active siggen output (set by
     * ps2204a_set_siggen*, cleared by disable). NOTE: on this hardware the
     * DAC is silenced for the lifetime of PS_STREAM_SDK regardless of what
     * LUT we upload (cmd1 buf_type=0x41 is a hardware multiplex — see
     * siggen_ep06_streaming_conflict.md). This flag is kept for callers
     * that still want to know a user siggen config is active. */
    bool     siggen_configured;
    uint32_t siggen_pkpk_uv;           /* remembered for LUT rebuild */

    /* Device info */
    char serial[16];
    char cal_date[16];

    /* Per-range calibration (index = ps_range_t - 2, i.e. 0..8 for
     * 50 mV .. 20 V). DC offset is subtracted from each sample; gain is a
     * multiplicative correction (1.0 = no correction). Default 0.0 / 1.0.
     * Populate manually via ps2204a_set_range_calibration() or let the
     * driver fit them automatically on ps2204a_calibrate_dc_offset(). */
    float cal_offset_mv[9];
    float cal_gain[9];
    uint8_t eeprom_raw[256];   /* 4 × 64-byte pages; read at open */

    /* Enhanced resolution: moving-average box filter with N = 4^extra_bits
     * taps. 0 = disabled (native 8-bit), up to 4 (12-bit effective). */
    int     res_extra_bits;

    /* ETS configuration (software reconstruction). 0 = disabled. */
    int ets_interleaves;   /* 2..20, effective-rate multiplier */
    int ets_cycles;        /* 1..32, captures per bin to average */

    /* Firmware blobs loaded at open-time from disk. See load_firmware(). */
    struct {
        uint8_t *fx2;         size_t fx2_len;          /* Packed chunks */
        uint8_t *fpga;        size_t fpga_len;         /* Xilinx bitstream */
        uint8_t *stream_lut;  size_t stream_lut_len;   /* SDK streaming LUT */
        uint8_t *waveform;    size_t waveform_len;     /* Channel waveform table */
    } fw;

    /* === SDK-streaming tunables ===
     * Read by sdk_streaming_thread at start-of-stream. All zero-initialised
     * on open, which gives the default behaviour:
     *   - sample_interval 1 µs → ticks 100
     *   - sample_count 10 000
     *   - auto_stop disabled (free-running)
     *
     * Setters: ps2204a_set_sdk_stream_interval_ns() and
     *          ps2204a_set_sdk_stream_auto_stop(). */
    uint32_t sdk_interval_ticks;   /* 0 = default (100 = 1 µs). Each tick = 10 ns. */
    uint64_t sdk_max_samples;      /* 0 = free-running (no auto_stop) */
    uint8_t  sdk_auto_stop;        /* mirrors (sdk_max_samples > 0); kept for clarity */
};

/* ========================================================================
 * Internal helpers
 * ======================================================================== */

static double timespec_ms(struct timespec *a, struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000.0 +
           (b->tv_nsec - a->tv_nsec) / 1e6;
}

/* ------------------------------------------------------------------ */
/* Firmware loading                                                    */
/* ------------------------------------------------------------------ */

/* Look up the directory that holds fx2.bin / fpga.bin / stream_lut.bin /
 * waveform.bin.
 * Search order:
 *   1. $PS2204A_FIRMWARE_DIR
 *   2. $XDG_CONFIG_HOME/picoscope-libusb/firmware
 *   3. $HOME/.config/picoscope-libusb/firmware
 *   4. /usr/local/share/picoscope-libusb/firmware
 *   5. /usr/share/picoscope-libusb/firmware
 * Returns 0 on success with `out` populated. Returns -1 if none exists. */
static int find_firmware_dir(char *out, size_t out_size)
{
    const char *candidates[5] = {0};
    char xdg_buf[512], home_buf[512];
    int n = 0;

    const char *env = getenv("PS2204A_FIRMWARE_DIR");
    if (env && *env) candidates[n++] = env;

    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(xdg_buf, sizeof(xdg_buf),
                 "%s/picoscope-libusb/firmware", xdg);
        candidates[n++] = xdg_buf;
    }

    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(home_buf, sizeof(home_buf),
                 "%s/.config/picoscope-libusb/firmware", home);
        candidates[n++] = home_buf;
    }

    candidates[n++] = "/usr/local/share/picoscope-libusb/firmware";
    candidates[n++] = "/usr/share/picoscope-libusb/firmware";

    for (int i = 0; i < n; i++) {
        struct stat st;
        if (stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(out, out_size, "%s", candidates[i]);
            return 0;
        }
    }
    return -1;
}

/* Slurp a file into a newly-malloc'd buffer. Caller owns the result.
 * Returns 0 on success. */
static int slurp_file(const char *path, uint8_t **out_buf, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    *out_buf = buf;
    *out_len = (size_t)sz;
    return 0;
}

static ps_status_t load_firmware(ps2204a_device_t *dev)
{
    char dir[512];
    if (find_firmware_dir(dir, sizeof(dir)) != 0) {
        fprintf(stderr,
            "\n[picoscope2204a] Firmware not found.\n"
            "  Looked for a directory containing fx2.bin / fpga.bin / stream_lut.bin / waveform.bin in:\n"
            "    $PS2204A_FIRMWARE_DIR\n"
            "    $XDG_CONFIG_HOME/picoscope-libusb/firmware\n"
            "    $HOME/.config/picoscope-libusb/firmware\n"
            "    /usr/local/share/picoscope-libusb/firmware\n"
            "    /usr/share/picoscope-libusb/firmware\n"
            "  Extract the firmware from your own device — see\n"
            "    docs/firmware-extraction.md\n");
        return PS_ERROR_FW;
    }

    char path[640];
    int rc;

    snprintf(path, sizeof(path), "%s/fx2.bin", dir);
    rc = slurp_file(path, &dev->fw.fx2, &dev->fw.fx2_len);
    if (rc != 0) {
        fprintf(stderr, "[picoscope2204a] Missing/unreadable: %s\n", path);
        return PS_ERROR_FW;
    }

    snprintf(path, sizeof(path), "%s/fpga.bin", dir);
    rc = slurp_file(path, &dev->fw.fpga, &dev->fw.fpga_len);
    if (rc != 0) {
        fprintf(stderr, "[picoscope2204a] Missing/unreadable: %s\n", path);
        return PS_ERROR_FW;
    }

    snprintf(path, sizeof(path), "%s/stream_lut.bin", dir);
    rc = slurp_file(path, &dev->fw.stream_lut, &dev->fw.stream_lut_len);
    if (rc != 0) {
        fprintf(stderr, "[picoscope2204a] Missing/unreadable: %s\n", path);
        return PS_ERROR_FW;
    }

    snprintf(path, sizeof(path), "%s/waveform.bin", dir);
    rc = slurp_file(path, &dev->fw.waveform, &dev->fw.waveform_len);
    if (rc != 0) {
        fprintf(stderr, "[picoscope2204a] Missing/unreadable: %s\n", path);
        return PS_ERROR_FW;
    }

    return PS_OK;
}

static void free_firmware(ps2204a_device_t *dev)
{
    free(dev->fw.fx2);        dev->fw.fx2 = NULL;        dev->fw.fx2_len = 0;
    free(dev->fw.fpga);       dev->fw.fpga = NULL;       dev->fw.fpga_len = 0;
    free(dev->fw.stream_lut); dev->fw.stream_lut = NULL; dev->fw.stream_lut_len = 0;
    free(dev->fw.waveform);   dev->fw.waveform = NULL;   dev->fw.waveform_len = 0;
}

static int send_cmd(ps2204a_device_t *dev, const uint8_t *data, int len)
{
    uint8_t buf[CMD_SIZE];
    int n = len < CMD_SIZE ? len : CMD_SIZE;
    memcpy(buf, data, n);
    if (n < CMD_SIZE) memset(buf + n, 0, CMD_SIZE - n);

    int transferred = 0;
    return libusb_bulk_transfer(dev->handle, EP_CMD_OUT, buf, CMD_SIZE,
                                &transferred, TIMEOUT_CMD);
}

static int read_resp(ps2204a_device_t *dev, uint8_t *buf, int size,
                     int timeout_ms)
{
    int transferred = 0;
    int r = libusb_bulk_transfer(dev->handle, EP_RESP_IN, buf, size,
                                 &transferred, timeout_ms);
    if (r == LIBUSB_ERROR_TIMEOUT) return 0;
    if (r < 0) return r;
    return transferred;
}

static int read_data(ps2204a_device_t *dev, uint8_t *buf, int size,
                     int timeout_ms)
{
    int transferred = 0;
    int r = libusb_bulk_transfer(dev->handle, EP_DATA_IN, buf, size,
                                 &transferred, timeout_ms);
    if (r == LIBUSB_ERROR_TIMEOUT) return 0;
    if (r < 0) return r;
    return transferred;
}

static void flush_buffers(ps2204a_device_t *dev)
{
    uint8_t buf[DATA_BUF_SIZE];
    for (int i = 0; i < 5; i++) {
        if (read_resp(dev, buf, CMD_SIZE, 50) <= 0) break;
    }
    for (int i = 0; i < 3; i++) {
        if (read_data(dev, buf, DATA_BUF_SIZE, 50) <= 0) break;
    }
}

/* Get PGA table entry (returns defaults for out-of-range) */
static pga_entry_t get_pga(ps_range_t range)
{
    int idx = (int)range - 2;
    if (idx >= 0 && idx < 9) return PGA_TABLE[idx];
    pga_entry_t def = {0, 7, 0};
    return def;
}

/* Get range in mV */
static int get_range_mv(ps_range_t range)
{
    int idx = (int)range - 2;
    if (idx >= 0 && idx < 9) return RANGE_MV[idx];
    return 5000;
}

/* Encode channel enable + coupling + PGA into 3 bytes for cmd1 */
static void encode_gain_bytes(ps2204a_device_t *dev,
                              uint8_t *b50, uint8_t *b51, uint8_t *b52)
{
    int a_en = dev->ch[0].enabled ? 1 : 0;
    int b_en = dev->ch[1].enabled ? 1 : 0;

    *b50 = 0x20 | (b_en << 1) | a_en;

    int a_dc = (dev->ch[0].coupling == PS_DC) ? 1 : 0;
    int b_dc = (dev->ch[1].coupling == PS_DC) ? 1 : 0;

    uint8_t a_bank, a_sel, a_200;
    uint8_t b_bank, b_sel, b_200;

    if (a_en) {
        pga_entry_t p = get_pga(dev->ch[0].range);
        a_bank = p.bank; a_sel = p.sel; a_200 = p.f200;
    } else {
        a_bank = 0; a_sel = 1; a_200 = 0;
    }

    if (b_en) {
        pga_entry_t p = get_pga(dev->ch[1].range);
        b_bank = p.bank; b_sel = p.sel; b_200 = p.f200;
    } else {
        b_bank = 0; b_sel = 1; b_200 = 0;
    }

    *b51 = (b_dc << 7) | (a_dc << 6) | (b_bank << 5) | (a_bank << 4)
         | (b_sel << 1) | b_200;
    *b52 = (a_sel << 5) | (a_200 << 4);
}

/* Get timebase-dependent channel config bytes */
static void get_tb_chan_bytes(int tb, uint8_t *hi, uint8_t *lo)
{
    if (tb >= 0 && tb <= 10) {
        *hi = TB_CHAN[tb][0];
        *lo = TB_CHAN[tb][1];
    } else if (tb > 10) {
        int val = 40;
        for (int i = 0; i < tb - 10; i++) {
            val >>= 1;
            if (val < 1) { val = 1; break; }
        }
        *hi = (val >> 8) & 0xFF;
        *lo = val & 0xFF;
    } else {
        *hi = 0x01; *lo = 0x57;
    }
}

/* ========================================================================
 * Command builders
 * ======================================================================== */

static void build_capture_cmd1(ps2204a_device_t *dev, uint8_t *cmd,
                               int n_samples, int timebase)
{
    uint8_t b50, b51, b52;
    encode_gain_bytes(dev, &b50, &b51, &b52);

    uint8_t chan_hi, chan_lo;
    get_tb_chan_bytes(timebase, &chan_hi, &chan_lo);

    int buf_val = (1 << timebase);
    if (buf_val > 0xFFFF) buf_val = 0xFFFF;
    uint8_t buf_hi = (buf_val >> 8) & 0xFF;
    uint8_t buf_lo = buf_val & 0xFF;

    int n = n_samples > 8192 ? 8192 : n_samples;

    /* Trigger delay: adjust the device's post-trigger sample count so the
     * hardware knows where to stop filling the 16 KB on-chip buffer relative
     * to the trigger event. This gives an approximate (trigger_position) =
     *    50 %   when delay_pct = 0
     *    → 0 %  when delay_pct = +100  (trigger near start of capture)
     *    → 100 %when delay_pct = -100  (trigger near end of capture)
     *
     * The current parser always extracts the last `n` samples of the 16 KB
     * buffer, so the effect of delay_pct is bounded by what that slicing
     * lets us see — fine for ±50 %, imperfect at the extremes (until the
     * parser grows a trigger-position-aware extraction). */
    int post_n = n;
    if (dev && dev->trigger_armed) {
        int dp = dev->trigger_delay_pct;
        if (dp < -100) dp = -100;
        if (dp >  100) dp = 100;
        post_n = (n * (100 + dp)) / 100;
        if (post_n < 0) post_n = 0;
        /* The on-chip buffer is 16 KB, so we can ask for up to ~16000 samples
         * after the trigger. Larger values get truncated at the device. */
        if (post_n > 16000) post_n = 16000;
    }
    uint8_t cnt_hi = (post_n >> 8) & 0xFF;
    uint8_t cnt_lo =  post_n       & 0xFF;

    /* Status-config byte after `85 05 95 00 08 00` tells the device which
     * trigger mode to use:
     *   0xff = free-run / auto-capture (no trigger wait)
     *   0x55 = armed LEVEL or WINDOW trigger — device stays in pending
     *          (0x33) state until the condition encoded in cmd2's
     *          `85 0c 86` sub-command is met, then → ready (0x3b).
     *   0x05 = Pulse-Width Qualifier enabled (LEVEL trigger gated by PWQ).
     *          Observed in SDK trace when ps2000SetPulseWidthQualifier is
     *          active; differs from 0x55 by the low nibble only. */
    uint8_t status_cfg;
    if (!dev || !dev->trigger_armed) {
        status_cfg = 0xff;
    } else if (dev->trigger_mode == PS_TRIGGER_PWQ) {
        status_cfg = 0x05;
    } else {
        status_cfg = 0x55;
    }

    memset(cmd, 0, CMD_SIZE);
    uint8_t tpl[] = {
        0x02,
        0x85, 0x08, 0x85, 0x00, 0x20, 0x00, 0x00, 0x00, cnt_hi, cnt_lo,
        0x85, 0x08, 0x93, 0x00, 0x20, 0x00, 0x00, 0x00, chan_hi, chan_lo,
        0x85, 0x08, 0x89, 0x00, 0x20, 0x00, 0x00, 0x00, buf_hi, buf_lo,
        0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01,
        0x85, 0x04, 0x9a, 0x00, 0x00, 0x00,
        0x85, 0x07, 0x97, 0x00, 0x14, 0x00, b50, b51, b52,
        0x85, 0x05, 0x95, 0x00, 0x08, 0x00, status_cfg, 0x00, 0x00, 0x00, 0x00
    };
    memcpy(cmd, tpl, sizeof(tpl));
}

static void build_capture_cmd2(ps2204a_device_t *dev, uint8_t *cmd)
{
    memset(cmd, 0, CMD_SIZE);
    static const uint8_t tpl[] = {
        0x02,
        0x85, 0x0c, 0x86, 0x00, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x05, 0x87, 0x00, 0x08, 0x00, 0x00,
        0x85, 0x0b, 0x90, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x08, 0x8a, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x01, 0x02,
        0x0c, 0x03, 0x0a, 0x00, 0x00,
        0x85, 0x04, 0x81, 0x00, 0x00, 0x00, 0x00
    };
    memcpy(cmd, tpl, sizeof(tpl));

    /* The `85 0c 86` sub-command at cmd[1..14] actually carries the
     * block-mode trigger config. Verified via USB trace of the real SDK:
     *
     *   No trigger (free-run, template default):
     *     cmd[7..14] = 01 00 00 00 00 00 00 00
     *     cmd[21]    = 0x00
     *   Trigger armed:
     *     cmd[7..10] = 00 ff 00 ff                   (fixed "trigger on" flag)
     *     cmd[11]    = 0x7f (rising) | 0x83 (falling)
     *     cmd[12]    = cmd[11] − 4
     *     cmd[13]    = 0x7d + round(thr_sdk16 / 288) mod 256
     *     cmd[14]    = cmd[13] − 10 mod 256          (hysteresis)
     *     cmd[21]    = 0x09 (rising) | 0x12 (falling)
     *
     * The threshold SDK-16-bit value is range-independent — the FPGA does
     * the volts↔counts scaling internally. Values derived empirically from
     * probing ps2000_set_trigger at 50mV..20V ranges × multiple thresholds. */
    if (dev && dev->trigger_armed) {
        int hyst = dev->trigger_hyst > 0 ? dev->trigger_hyst : 10;

        if (dev->trigger_mode == PS_TRIGGER_WINDOW) {
            /* WINDOW trigger layout (CH A verified from SDK trace,
             * trace_advtrig_window.log, 2026-04-18):
             *
             *   cmd[7..8]   = 00 ff           (armed flag, same as LEVEL)
             *   cmd[9..10]  = lower pair      (thr_lo, thr_lo − hyst)
             *   cmd[11..12] = 7f 7b           (direction markers, CH A enter)
             *   cmd[13..14] = upper pair      (thr_hi + hyst, thr_hi)
             *   cmd[21]     = 0x0d            (WINDOW/ENTER selector)
             *
             * Only ENTER direction (PS_RISING) has been traced; PS_FALLING
             * is treated the same for now — the real SDK's "EXIT window"
             * mode is not yet reverse-engineered.
             *
             * CH B layout is extrapolated from LEVEL (base 0x80 instead of
             * 0x7d, pair slots swapped) but not yet verified against a
             * trace — flagged in the comment and worth confirming. */
            int32_t thr_lo = (int32_t)dev->trigger_thr_sdk;
            int32_t thr_hi = (int32_t)dev->trigger_thr2_sdk;
            int32_t dlo = thr_lo >= 0 ? (thr_lo + 144) / 288 : (thr_lo - 144) / 288;
            int32_t dhi = thr_hi >= 0 ? (thr_hi + 144) / 288 : (thr_hi - 144) / 288;

            cmd[7]  = 0x00; cmd[8]  = 0xff;

            if (dev->trigger_source == PS_CHANNEL_B) {
                uint8_t lo_byte = (uint8_t)((0x80 + dlo) & 0xFF);
                uint8_t hi_byte = (uint8_t)((0x80 + dhi) & 0xFF);
                cmd[9]  = lo_byte;
                cmd[10] = (uint8_t)((lo_byte - hyst) & 0xFF);
                cmd[11] = 0x7d; cmd[12] = 0x79;
                cmd[13] = (uint8_t)((hi_byte + hyst) & 0xFF);
                cmd[14] = hi_byte;
            } else {
                uint8_t lo_byte = (uint8_t)((0x7d + dlo) & 0xFF);
                uint8_t hi_byte = (uint8_t)((0x7d + dhi) & 0xFF);
                cmd[9]  = lo_byte;
                cmd[10] = (uint8_t)((lo_byte - hyst) & 0xFF);
                cmd[11] = 0x7f; cmd[12] = 0x7b;
                cmd[13] = (uint8_t)((hi_byte + hyst) & 0xFF);
                cmd[14] = hi_byte;
            }
            cmd[21] = 0x0d;
            return;
        }

        /* LEVEL (and PWQ, which reuses the same edge encoding in cmd2):
         *
         *   Source = CH A: threshold pair at [13..14], direction markers
         *                   at [11..12] (always constant for direction).
         *   Source = CH B: threshold pair at [11..12], direction markers
         *                   at [13..14] (different constants than CH A).
         *
         *   Direction markers (delta +4 between byte and its companion):
         *     CH A rising  [11..12] = 7f 7b
         *     CH A falling [11..12] = 83 7f
         *     CH B rising  [13..14] = 7d 79
         *     CH B falling [13..14] = 81 7d
         *
         *   Threshold bytes (delta hyst, base ≈ 0x7d for CH A, 0x80 for CH B):
         *     threshold byte = base + round(thr_sdk16 / 288)
         *     for RISING:  threshold at higher slot, companion = threshold − hyst
         *     for FALLING: threshold at lower slot,  companion = threshold + hyst
         *
         *   cmd[21] = 0x09 rising / 0x12 falling (same for both channels).
         */
        int32_t thr = (int32_t)dev->trigger_thr_sdk;
        int32_t delta = thr >= 0 ? (thr + 144) / 288 : (thr - 144) / 288;

        cmd[7]  = 0x00; cmd[8]  = 0xff;
        cmd[9]  = 0x00; cmd[10] = 0xff;

        if (dev->trigger_source == PS_CHANNEL_B) {
            uint8_t thr_byte = (uint8_t)((0x80 + delta) & 0xFF);
            if (dev->trigger_dir == PS_FALLING) {
                cmd[11] = (uint8_t)((thr_byte + hyst) & 0xFF);
                cmd[12] = thr_byte;
                cmd[13] = 0x81; cmd[14] = 0x7d;
                cmd[21] = 0x12;
            } else {
                cmd[11] = thr_byte;
                cmd[12] = (uint8_t)((thr_byte - hyst) & 0xFF);
                cmd[13] = 0x7d; cmd[14] = 0x79;
                cmd[21] = 0x09;
            }
        } else {
            uint8_t thr_byte = (uint8_t)((0x7d + delta) & 0xFF);
            if (dev->trigger_dir == PS_FALLING) {
                cmd[11] = 0x83; cmd[12] = 0x7f;
                cmd[13] = (uint8_t)((thr_byte + hyst) & 0xFF);
                cmd[14] = thr_byte;
                cmd[21] = 0x12;
            } else {
                cmd[11] = 0x7f; cmd[12] = 0x7b;
                cmd[13] = thr_byte;
                cmd[14] = (uint8_t)((thr_byte - hyst) & 0xFF);
                cmd[21] = 0x09;
            }
        }
    }
}

static void build_block_trigger(uint8_t *cmd)
{
    memset(cmd, 0, CMD_SIZE);
    static const uint8_t tpl[] = {
        0x02, 0x07, 0x06, 0x00, 0x40, 0x00, 0x00, 0x02, 0x01, 0x00
    };
    memcpy(cmd, tpl, sizeof(tpl));
}

/* Native streaming cmd1: FPGA continuous mode.
 * Differences from block cmd1:
 *  - Channel bytes [19..20] fixed to (0x00, 0x06)
 *  - Buffer bytes [28..30] fixed to (0x0f, 0x42, 0x40)
 *  - Byte 37 = 0x41 (streaming flag) instead of 0x01
 *  - Sample count (bytes 9..10) = max_samples requested
 */
static void build_streaming_cmd1(ps2204a_device_t *dev, uint8_t *cmd,
                                 int n_samples)
{
    uint8_t b50, b51, b52;
    encode_gain_bytes(dev, &b50, &b51, &b52);

    int n = n_samples > 0xFFFF ? 0xFFFF : n_samples;
    uint8_t cnt_hi = (n >> 8) & 0xFF;
    uint8_t cnt_lo = n & 0xFF;

    memset(cmd, 0, CMD_SIZE);
    uint8_t tpl[] = {
        0x02,
        0x85, 0x08, 0x85, 0x00, 0x20, 0x00, 0x00, 0x00, cnt_hi, cnt_lo,
        0x85, 0x08, 0x93, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x06,
        0x85, 0x08, 0x89, 0x00, 0x20, 0x00, 0x00, 0x0f, 0x42, 0x40,
        0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x41,
        0x85, 0x04, 0x9a, 0x00, 0x00, 0x00,
        0x85, 0x07, 0x97, 0x00, 0x14, 0x00, b50, b51, b52,
        0x85, 0x05, 0x95, 0x00, 0x08, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00
    };
    memcpy(cmd, tpl, sizeof(tpl));
}

/* Native streaming cmd2. Uses Python driver's baseline values (matches
 * SDK streaming structure). Byte[47]=0x05 is the observed fast-mode value. */
static void build_streaming_cmd2(uint8_t *cmd)
{
    memset(cmd, 0, CMD_SIZE);
    static const uint8_t tpl[] = {
        0x02,
        0x85, 0x0c, 0x86, 0x00, 0x40, 0x00,
        0xb0, 0x19, 0xa6, 0x10, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x05, 0x87, 0x00, 0x08, 0x00, 0x00,
        0x85, 0x0b, 0x90, 0x00, 0x38, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x08, 0x8a, 0x00, 0x20, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x0b, 0x03, 0x05,
        0x00, 0x02, 0x0c, 0x03, 0x0a, 0x00, 0x00,
        0x85, 0x04, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    memcpy(cmd, tpl, sizeof(tpl));
}

/* Native streaming trigger (distinct from block trigger). */
static void build_stream_trigger(uint8_t *cmd)
{
    memset(cmd, 0, CMD_SIZE);
    static const uint8_t tpl[] = {
        0x02, 0x07, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00
    };
    memcpy(cmd, tpl, sizeof(tpl));
}

/* Native streaming stop (opcode 0x0a + follow-up 0x99). */
static void send_stream_stop(ps2204a_device_t *dev)
{
    static const uint8_t stop1[] = {
        0x02, 0x0a, 0x00, 0x85, 0x04, 0x99, 0x00, 0x00, 0x00, 0x0a
    };
    static const uint8_t stop2[] = {
        0x02, 0x85, 0x04, 0x99, 0x00, 0x00, 0x00, 0x0a
    };
    send_cmd(dev, stop1, sizeof(stop1));
    usleep(10000);
    send_cmd(dev, stop2, sizeof(stop2));
}

/* ========================================================================
 * FX2 Firmware Upload
 * ======================================================================== */

static ps_status_t upload_fx2(ps2204a_device_t *dev)
{
    int r;
    uint8_t halt = 0x01, run = 0x00;

    printf("  [FX2] Halting CPU...\n");
    r = libusb_control_transfer(dev->handle, 0x40, FX2_VENDOR_REQ,
                                FX2_CPUCS_ADDR, 0, &halt, 1, TIMEOUT_CMD);
    if (r < 0) {
        printf("  [FX2] Halt failed: %s\n", libusb_error_name(r));
        return PS_ERROR_FW;
    }
    usleep(10000);

    /* Parse and upload chunks from firmware data loaded at open() */
    const uint8_t *fx2 = dev->fw.fx2;
    const size_t fx2_size = dev->fw.fx2_len;
    printf("  [FX2] Uploading packed chunks (%zu bytes)...\n", fx2_size);
    size_t offset = 0;
    int chunks_sent = 0;

    while (offset + 3 <= fx2_size) {
        uint16_t addr = ((uint16_t)fx2[offset] << 8) | fx2[offset+1];
        uint8_t len = fx2[offset+2];
        offset += 3;

        if (offset + len > fx2_size) break;

        /* Skip CPUCS address — handled separately */
        if (addr == FX2_CPUCS_ADDR) {
            offset += len;
            continue;
        }

        r = libusb_control_transfer(dev->handle, 0x40, FX2_VENDOR_REQ,
                                    addr, 0,
                                    (uint8_t *)&fx2[offset],
                                    len, TIMEOUT_CMD);
        if (r < 0) {
            printf("  [FX2] Chunk @0x%04x failed: %s\n", addr,
                   libusb_error_name(r));
            return PS_ERROR_FW;
        }
        offset += len;
        chunks_sent++;
        usleep(1000);
    }
    printf("  [FX2] %d chunks uploaded\n", chunks_sent);

    /* Start CPU */
    printf("  [FX2] Starting CPU...\n");
    r = libusb_control_transfer(dev->handle, 0x40, FX2_VENDOR_REQ,
                                FX2_CPUCS_ADDR, 0, &run, 1, TIMEOUT_CMD);
    if (r < 0) return PS_ERROR_FW;

    return PS_OK;
}

/* Re-enumerate after FX2 upload: close old handle, find new device */
static ps_status_t fx2_reenumerate(ps2204a_device_t *dev)
{
    printf("  [FX2] Waiting for re-enumeration...\n");

    /* Close old handle */
    libusb_release_interface(dev->handle, 0);
    libusb_close(dev->handle);
    dev->handle = NULL;

    usleep(500000); /* 500ms for USB disconnect/reconnect */

    /* Find device again (may have new address) */
    for (int attempt = 0; attempt < 20; attempt++) {
        usleep(300000);
        dev->handle = libusb_open_device_with_vid_pid(dev->ctx,
                                                       PICO_VID, PICO_PID);
        if (dev->handle) {
            printf("  [FX2] Device found (attempt %d)\n", attempt + 1);

            /* Detach kernel driver if active */
            if (libusb_kernel_driver_active(dev->handle, 0) == 1) {
                libusb_detach_kernel_driver(dev->handle, 0);
            }
            int r = libusb_claim_interface(dev->handle, 0);
            if (r < 0) {
                printf("  [FX2] Claim failed: %s\n", libusb_error_name(r));
                return PS_ERROR_USB;
            }
            return PS_OK;
        }
    }

    printf("  [FX2] Device not found after re-enumeration\n");
    return PS_ERROR_USB;
}

/* ========================================================================
 * ADC Initialization
 * ======================================================================== */

static ps_status_t init_adc(ps2204a_device_t *dev)
{
    uint8_t resp[CMD_SIZE];

    /* ADC init sequence 1 */
    static const uint8_t adc1[] = {
        0x02, 0x81, 0x03, 0x80, 0x08, 0x18,
        0x81, 0x03, 0xb2, 0xff, 0x18,
        0x81, 0x03, 0xb0, 0x00, 0xf8,
        0x81, 0x03, 0xb5, 0xff, 0xf8
    };
    send_cmd(dev, adc1, sizeof(adc1));
    usleep(50000);

    /* ADC init sequence 2 */
    static const uint8_t adc2[] = {
        0x02, 0x81, 0x03, 0xb0, 0xff, 0x80, 0x0c, 0x03, 0x0a, 0x00, 0x00,
        0x81, 0x03, 0xb0, 0xff, 0x40, 0x0c, 0x03, 0x0a, 0x00, 0x00,
        0x81, 0x03, 0xb0, 0xff, 0x20, 0x0c, 0x03, 0x0a, 0x00, 0x00,
        0x81, 0x03, 0xb0, 0xff, 0x10, 0x0c, 0x03, 0x0a, 0x00, 0x00,
        0x81, 0x03, 0xb0, 0xff, 0x08, 0x0c, 0x03, 0x0a, 0x00, 0x01
    };
    send_cmd(dev, adc2, sizeof(adc2));
    usleep(100000);

    /* Wait for ACK */
    int n = read_resp(dev, resp, CMD_SIZE, 1000);
    if (n > 0 && resp[0] == 0x01) {
        printf("  [ADC] ACK received\n");
    } else {
        printf("  [ADC] ACK not received (got %d bytes)\n", n);
    }

    return PS_OK;
}

/* ========================================================================
 * Pre-FPGA Configuration
 * ======================================================================== */

static ps_status_t pre_fpga_config(ps2204a_device_t *dev)
{
    uint8_t resp[CMD_SIZE];
    int transferred;

    /* Write 0xe5 to FX2 registers 0x07-0x0c */
    static const uint8_t reg_cmd[] = {
        0x02,
        0x02, 0x02, 0x07, 0xe5,
        0x02, 0x02, 0x08, 0xe5,
        0x02, 0x02, 0x09, 0xe5,
        0x02, 0x02, 0x0a, 0xe5,
        0x02, 0x02, 0x0b, 0xe5,
        0x02, 0x02, 0x0c, 0xe5,
    };
    send_cmd(dev, reg_cmd, sizeof(reg_cmd));
    usleep(50000);
    read_resp(dev, resp, CMD_SIZE, 500);

    /* Single byte command (NOT padded to 64) */
    uint8_t one = 0x01;
    libusb_bulk_transfer(dev->handle, EP_CMD_OUT, &one, 1,
                         &transferred, TIMEOUT_CMD);
    usleep(50000);
    read_resp(dev, resp, CMD_SIZE, 500);

    /* Initial PGA/range config (20V) */
    static const uint8_t pga_cmd[] = {
        0x02, 0x85, 0x04, 0x99, 0x00, 0x00, 0x00, 0x0a
    };
    send_cmd(dev, pga_cmd, sizeof(pga_cmd));

    /* ADC register config */
    static const uint8_t adc_reg[] = {
        0x02, 0x81, 0x03, 0xb2, 0xff, 0x08
    };
    send_cmd(dev, adc_reg, sizeof(adc_reg));
    usleep(50000);

    return PS_OK;
}

/* ========================================================================
 * FPGA Firmware Upload
 * ======================================================================== */

static ps_status_t upload_fpga(ps2204a_device_t *dev)
{
    int transferred;

    /* Send FPGA upload command */
    static const uint8_t fpga_cmd[] = {0x04, 0x00, 0x95, 0x02, 0x00};
    libusb_bulk_transfer(dev->handle, EP_CMD_OUT, (uint8_t *)fpga_cmd,
                         sizeof(fpga_cmd), &transferred, TIMEOUT_CMD);
    usleep(50000);

    /* Upload FPGA firmware in 32KB chunks on EP 0x06 */
    const uint8_t *fpga = dev->fw.fpga;
    const size_t fpga_size = dev->fw.fpga_len;
    printf("  [FPGA] Uploading %zu bytes...", fpga_size);
    fflush(stdout);

    size_t offset = 0;
    while (offset < fpga_size) {
        size_t chunk = fpga_size - offset;
        if (chunk > FPGA_CHUNK_SIZE) chunk = FPGA_CHUNK_SIZE;

        int r = libusb_bulk_transfer(dev->handle, EP_FW_OUT,
                                     (uint8_t *)&fpga[offset],
                                     (int)chunk, &transferred,
                                     TIMEOUT_FW);
        if (r < 0) {
            printf("\n  [FPGA] Upload failed at offset %zu: %s\n",
                   offset, libusb_error_name(r));
            return PS_ERROR_FW;
        }
        offset += chunk;
        printf(".");
        fflush(stdout);
    }
    printf(" Done!\n");

    usleep(100000);
    return PS_OK;
}

/* ========================================================================
 * Post-FPGA Configuration
 * ======================================================================== */

static ps_status_t post_fpga_config(ps2204a_device_t *dev)
{
    uint8_t resp[CMD_SIZE];

    /* Post-FPGA config 1 */
    static const uint8_t cfg1[] = {
        0x02, 0x85, 0x04, 0x80, 0x00, 0x00, 0x00,
        0x81, 0x03, 0xb2, 0x00, 0x08
    };
    send_cmd(dev, cfg1, sizeof(cfg1));
    usleep(100000);

    /* Flush 3x with 300ms timeout */
    for (int i = 0; i < 3; i++) {
        read_resp(dev, resp, CMD_SIZE, 300);
    }

    /* Post-FPGA config 2 — triggers CAAC */
    static const uint8_t cfg2[] = {
        0x02, 0x85, 0x04, 0x80, 0x00, 0x00, 0x00,
        0x05, 0x04, 0x8f, 0x00, 0x10
    };
    send_cmd(dev, cfg2, sizeof(cfg2));
    usleep(300000);

    /* Wait for CAAC (0xCA 0xAC) */
    bool caac = false;
    for (int i = 0; i < 5; i++) {
        int n = read_resp(dev, resp, CMD_SIZE, 500);
        if (n >= 2 && resp[0] == 0xCA && resp[1] == 0xAC) {
            printf("  [FPGA] CAAC received — configured!\n");
            caac = true;
            break;
        }
    }
    if (!caac) {
        printf("  [FPGA] Warning: no CAAC response\n");
    }

    return PS_OK;
}

/* ========================================================================
 * Channel Setup (waveform upload)
 * ======================================================================== */

/* Async waveform transfer callback */
struct wf_ctx {
    bool done;
    bool success;
};

static void LIBUSB_CALL wf_callback(struct libusb_transfer *xfer)
{
    struct wf_ctx *ctx = (struct wf_ctx *)xfer->user_data;
    ctx->done = true;
    ctx->success = (xfer->status == LIBUSB_TRANSFER_COMPLETED);
}

static ps_status_t setup_channels(ps2204a_device_t *dev)
{
    uint8_t resp[CMD_SIZE];

    /* Exact SDK bytes for channel A setup — 64 bytes total. Verified
     * against SDK USB trace: `85 05 82` sub-command lives at position 42,
     * which means 10 bytes of padding follow the dwell field (positions
     * 32..41). Earlier versions of this array were 60 bytes and caused
     * the FPGA to ignore the EP 0x06 waveform pull — visible as
     * "Waveform A upload failed" at init. */
    static const uint8_t cmd_a[64] = {
        /* [0..17] header                              */
        0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00,
        0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
        /* [18..25] freq_start + freq_stop BE          */  0,0,0,0, 0,0,0,0,
        /* [26..27] increment                          */  0,0,
        /* [28..31] dwell                              */  0,0,0,0,
        /* [32..41] padding (10 bytes!)                */  0,0,0,0,0, 0,0,0,0,0,
        /* [42..48] GET_DATA sub-cmd                   */
        0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x00,
        /* [49..54]                                    */
        0x85, 0x04, 0x8b, 0x00, 0x00, 0x00,
        /* [55..63] 87 06 trailer                      */
        0x87, 0x06, 0x00, 0x20, 0x00, 0x00, 0x03, 0x01, 0x00
    };

    /* Exact SDK bytes for channel B setup (64 bytes; freq=1 kHz magic
     * pattern + sweep-init dwell bytes used by the SDK's open sequence). */
    static const uint8_t cmd_b[64] = {
        0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00,
        0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
        /* [18..25] freq_start + freq_stop = 0x015D8600 (SDK init pattern) */
        0x01, 0x5d, 0x86, 0x00, 0x01, 0x5d, 0x86, 0x00,
        /* [26..27] increment                          */  0x00, 0x00,
        /* [28..31] dwell                              */  0x59, 0x02, 0xdc, 0x6c,
        /* [32..41] padding (10 bytes)                 */  0,0,0,0,0, 0,0,0,0,0,
        /* [42..48] GET_DATA                           */
        0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x00,
        /* [49..54]                                    */
        0x85, 0x04, 0x8b, 0x00, 0x00, 0x00,
        /* [55..63] 87 06 trailer                      */
        0x87, 0x06, 0x00, 0x20, 0x00, 0x00, 0x03, 0x01, 0x00
    };

    static const uint8_t follow_up[] = {
        0x02, 0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01
    };

    /* Prepare async waveform transfer for channel A */
    struct wf_ctx wf_a = {false, false};
    struct libusb_transfer *xfer_a = libusb_alloc_transfer(0);
    if (!xfer_a) return PS_ERROR_ALLOC;

    uint8_t *wf_buf = dev->fw.waveform;
    libusb_fill_bulk_transfer(xfer_a, dev->handle, EP_FW_OUT,
                              wf_buf, 8192, wf_callback, &wf_a, 5000);

    int r = libusb_submit_transfer(xfer_a);
    if (r < 0) {
        printf("  [CH] Waveform A submit failed: %s\n", libusb_error_name(r));
        libusb_free_transfer(xfer_a);
        return PS_ERROR_USB;
    }

    /* Send channel A setup + follow-up */
    send_cmd(dev, cmd_a, sizeof(cmd_a));
    send_cmd(dev, follow_up, sizeof(follow_up));

    /* Wait for waveform A to complete */
    struct timeval tv = {2, 0};
    while (!wf_a.done) {
        libusb_handle_events_timeout(dev->ctx, &tv);
    }
    libusb_free_transfer(xfer_a);

    if (wf_a.success) {
        printf("  [CH] Waveform A uploaded OK\n");
    } else {
        printf("  [CH] Waveform A upload failed\n");
    }

    /* Drain ACKs before channel B */
    usleep(100000);
    for (int i = 0; i < 5; i++) {
        if (read_resp(dev, resp, CMD_SIZE, 100) <= 0) break;
    }

    /* Channel B: async waveform + setup */
    struct wf_ctx wf_b = {false, false};
    struct libusb_transfer *xfer_b = libusb_alloc_transfer(0);
    if (!xfer_b) return PS_ERROR_ALLOC;

    libusb_fill_bulk_transfer(xfer_b, dev->handle, EP_FW_OUT,
                              wf_buf, 8192, wf_callback, &wf_b, 5000);

    r = libusb_submit_transfer(xfer_b);
    if (r < 0) {
        printf("  [CH] Waveform B submit failed: %s\n", libusb_error_name(r));
        libusb_free_transfer(xfer_b);
        /* Channel B waveform failure is non-fatal (known PyUSB limitation) */
    } else {
        send_cmd(dev, cmd_b, sizeof(cmd_b));
        send_cmd(dev, follow_up, sizeof(follow_up));

        /* Wait for waveform B */
        while (!wf_b.done) {
            libusb_handle_events_timeout(dev->ctx, &tv);
        }
        libusb_free_transfer(xfer_b);

        if (wf_b.success) {
            printf("  [CH] Waveform B uploaded OK\n");
        } else {
            printf("  [CH] Waveform B upload failed (non-fatal)\n");
        }
    }

    /* Extra follow-up */
    send_cmd(dev, follow_up, sizeof(follow_up));
    usleep(100000);

    /* Drain all pending ACKs */
    for (int i = 0; i < 10; i++) {
        if (read_resp(dev, resp, CMD_SIZE, 100) <= 0) break;
    }

    /* Check status */
    static const uint8_t status_cmd[] = {0x02, 0x01, 0x01, 0x80};
    send_cmd(dev, status_cmd, sizeof(status_cmd));
    usleep(30000);
    int n = read_resp(dev, resp, CMD_SIZE, 500);
    if (n > 0) {
        printf("  [CH] Status: 0x%02x\n", resp[0]);
    }

    return PS_OK;
}

/* ========================================================================
 * Device Info
 * ======================================================================== */

static void read_device_info(ps2204a_device_t *dev)
{
    uint8_t resp[CMD_SIZE];

    static const uint8_t addrs[] = {0x00, 0x40, 0x80, 0xc0};

    for (int a = 0; a < 4; a++) {
        uint8_t info_req[] = {0x02, 0x83, 0x02, 0x50, addrs[a]};
        send_cmd(dev, info_req, sizeof(info_req));
        usleep(20000);
        read_resp(dev, resp, CMD_SIZE, TIMEOUT_CMD); /* ACK */

        uint8_t info_get[] = {0x02, 0x03, 0x02, 0x50, 0x40};
        send_cmd(dev, info_get, sizeof(info_get));
        usleep(30000);
        int n = read_resp(dev, resp, CMD_SIZE, 500);

        /* Stash the raw EEPROM bytes for downstream calibration work. */
        if (n > 0) {
            int copy = n > 64 ? 64 : n;
            memcpy(&dev->eeprom_raw[a * 64], resp, copy);
        }

        if (n >= 32 && addrs[a] == 0x00) {
            /* Look for serial (JOxxxxxxxx) */
            for (int i = 0; i < n - 1; i++) {
                if (resp[i] == 'J' && resp[i+1] == 'O') {
                    int len = 0;
                    while (i + len < n && len < 10 && resp[i+len] != 0) len++;
                    memcpy(dev->serial, &resp[i], len);
                    dev->serial[len] = '\0';
                    break;
                }
            }
            /* Look for calibration date */
            static const char *months[] = {
                "Jan","Feb","Mar","Apr","May","Jun",
                "Jul","Aug","Sep","Oct","Nov","Dec"
            };
            for (int m = 0; m < 12; m++) {
                for (int i = 2; i < n - 2; i++) {
                    if (memcmp(&resp[i], months[m], 3) == 0) {
                        int start = i - 2;
                        if (start < 0) start = 0;
                        int len = 7;
                        if (start + len > n) len = n - start;
                        memcpy(dev->cal_date, &resp[start], len);
                        dev->cal_date[len] = '\0';
                        goto found_date;
                    }
                }
            }
            found_date:;
        }
    }

    printf("  Serial: %s\n", dev->serial[0] ? dev->serial : "Unknown");
    printf("  Cal Date: %s\n", dev->cal_date[0] ? dev->cal_date : "Unknown");
}

/* ========================================================================
 * Waveform Parsing (8-bit ADC)
 * ======================================================================== */

/* Locate the valid (non-zero) data region in the circular sample buffer
 * after the 2-byte header. Returns the length of the valid segment and
 * sets *out_ptr to its start. The valid segment lies between the first
 * and last non-zero bytes; trailing zero runs come from an unfilled
 * circular buffer (when fewer samples than buffer capacity are captured).
 * If the buffer is entirely zero, input is at mid-scale — the whole
 * buffer is valid.
 */
static int find_valid_segment(const uint8_t *raw, int raw_len,
                              const uint8_t **out_ptr)
{
    if (raw_len <= 2) { *out_ptr = NULL; return 0; }
    const uint8_t *buf = raw + 2;
    int buf_len = raw_len - 2;

    int first_nz = -1, last_nz = -1;
    for (int i = 0; i < buf_len; i++) {
        if (buf[i] != 0) {
            if (first_nz < 0) first_nz = i;
            last_nz = i;
        }
    }

    if (first_nz < 0) {
        *out_ptr = buf;
        return buf_len;
    }

    int valid_len = last_nz - first_nz + 1;
    if (valid_len >= buf_len) {
        *out_ptr = buf;
        return buf_len;
    }
    *out_ptr = buf + first_nz;
    return valid_len;
}

/* Scale [start..start+n] uint8 samples into mV float array. */
static void scale_samples(const uint8_t *src, int n, float range_mv, float *out)
{
    float scale = range_mv / ADC_HALF_RANGE;
    for (int i = 0; i < n; i++) {
        out[i] = ((float)src[i] - ADC_CENTER) * scale;
    }
}

/* Single-channel parse: take the last n_samples from the valid segment. */
/* Extract `n_samples` from the valid buffer region with the trigger
 * positioned at `trigger_offset` bytes from the start (so pre_wanted =
 * trigger_offset, post_wanted = n_samples - trigger_offset). When
 * delay_pct = 0 the trigger sits at the middle of the returned data. */
static int parse_waveform_ex(const uint8_t *raw, int raw_len, int n_samples,
                             int post_captured, int delay_pct,
                             float range_mv, float *out)
{
    const uint8_t *valid;
    int valid_len = find_valid_segment(raw, raw_len, &valid);
    if (valid_len <= 0) return 0;

    /* Device-side post-trigger sample count (what cmd1 told it to capture
     * after trigger). Buffer layout: [older pre-trigger ...][trigger][...
     * post-trigger ...(end of valid)]. So trigger position in valid region
     * is at (valid_len - post_captured). */
    if (post_captured < 0) post_captured = 0;
    if (post_captured > valid_len) post_captured = valid_len;

    /* How many samples of our `n_samples` we want BEFORE the trigger. */
    int dp = delay_pct;
    if (dp < -100) dp = -100;
    if (dp >  100) dp =  100;
    int pre_wanted = (n_samples * (100 - dp)) / 200;
    /* Start of extraction in the valid region. */
    int start = (valid_len - post_captured) - pre_wanted;
    if (start < 0) start = 0;
    if (start + n_samples > valid_len) start = valid_len - n_samples;
    if (start < 0) start = 0;

    int copy_len = n_samples < valid_len ? n_samples : valid_len;
    scale_samples(valid + start, copy_len, range_mv, out);
    return copy_len;
}

static int parse_waveform(const uint8_t *raw, int raw_len, int n_samples,
                          float range_mv, float *out)
{
    /* Legacy path: no trigger info, take the tail. */
    return parse_waveform_ex(raw, raw_len, n_samples, n_samples, -100,
                             range_mv, out);
}

/* In-place centred moving-average of N taps with edge extension (repeat
 * first/last sample outside the array). Running-sum kernel, O(n). N must
 * be ≥ 1; N=1 is a no-op. */
static void apply_moving_average(float *buf, int n, int N)
{
    if (!buf || n <= 0 || N <= 1) return;
    if (N > n) N = n;

    const int half = N / 2;
    double sum = 0.0;
    /* Initial window: edge-extend the left side with buf[0], then fill
     * from buf[0..N-half-1]. */
    for (int i = 0; i < N; i++) {
        int src = i - half;
        if (src < 0) src = 0;
        if (src >= n) src = n - 1;
        sum += buf[src];
    }

    /* Scratch buffer — we can't write back while still reading later
     * window positions. Stack-allocate up to 64 KB, heap for larger. */
    float stack_tmp[2048];
    float *tmp = (n <= (int)(sizeof(stack_tmp)/sizeof(stack_tmp[0])))
                 ? stack_tmp
                 : (float *)malloc(n * sizeof(float));
    if (!tmp) return;  /* leave buf untouched on OOM */

    const double inv_N = 1.0 / (double)N;
    for (int i = 0; i < n; i++) {
        tmp[i] = (float)(sum * inv_N);

        /* Slide window: drop (i-half) sample, add (i+1-half+N-1) = i+1+half-1 */
        int drop = i - half;
        int add  = i + 1 - half + N - 1;  /* = i + N - half */
        if (drop < 0) drop = 0;
        if (drop >= n) drop = n - 1;
        if (add  < 0) add  = 0;
        if (add  >= n) add  = n - 1;
        sum += (double)buf[add] - (double)buf[drop];
    }

    memcpy(buf, tmp, n * sizeof(float));
    if (tmp != stack_tmp) free(tmp);
}

/* Convenience wrapper that reads extra_bits from the device state. */
static void apply_res_enhancement(ps2204a_device_t *dev, float *buf, int n)
{
    if (!dev || dev->res_extra_bits <= 0) return;
    int N = 1;
    for (int i = 0; i < dev->res_extra_bits; i++) N *= 4;
    apply_moving_average(buf, n, N);
}

/* Dual-channel parse: tail-interleaved layout.
 *
 * The device packs both channels into a single 16 KB buffer. The most
 * recent samples are at the TAIL, interleaved as B,A,B,A,... (2 bytes per
 * captured sample pair). Byte positions (relative to end): even = B, odd = A.
 *
 * Verified empirically 2026-04-16: cross-referencing the dual buffer
 * against single-channel A-only and B-only captures shows near-exact
 * matches on both mean and std, confirming this layout.
 *
 * Returns samples-per-channel written. Either out_a or out_b may be NULL. */
static int parse_waveform_dual(const uint8_t *raw, int raw_len, int n_samples,
                               float range_a_mv, float range_b_mv,
                               float *out_a, float *out_b)
{
    const uint8_t *valid;
    int valid_len = find_valid_segment(raw, raw_len, &valid);
    if (valid_len < 4) return 0;

    /* Two bytes per sample pair (B then A). Take the last 2*n bytes. */
    int pair_bytes = 2 * n_samples;
    if (pair_bytes > valid_len) {
        n_samples = valid_len / 2;
        pair_bytes = 2 * n_samples;
    }
    const uint8_t *tail = valid + (valid_len - pair_bytes);

    float scale_a = range_a_mv / ADC_HALF_RANGE;
    float scale_b = range_b_mv / ADC_HALF_RANGE;

    for (int i = 0; i < n_samples; i++) {
        int b_byte = tail[2 * i];
        int a_byte = tail[2 * i + 1];
        if (out_b) out_b[i] = ((float)b_byte - ADC_CENTER) * scale_b;
        if (out_a) out_a[i] = ((float)a_byte - ADC_CENTER) * scale_a;
    }
    return n_samples;
}

/* ========================================================================
 * Status Polling
 * ======================================================================== */

static int poll_status(ps2204a_device_t *dev, int timeout_ms)
{
    uint8_t resp[CMD_SIZE];
    static const uint8_t poll_cmd[] = {0x02, 0x01, 0x01, 0x80};

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int last_status = 0x00;

    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (timespec_ms(&start, &now) >= timeout_ms) break;

        send_cmd(dev, poll_cmd, sizeof(poll_cmd));
        int n = read_resp(dev, resp, CMD_SIZE, 200);
        if (n > 0) {
            last_status = resp[0];
            if (last_status == 0x3b) return 0x3b; /* Ready */
            if (last_status == 0x7b) return 0x7b; /* Error */
        }
        usleep(20000);
    }
    return last_status;
}

/* ========================================================================
 * Fast Block Capture (optimized, no flush between blocks)
 * ======================================================================== */

/* PS2204A_TIMING=1 in env → log per-phase timing for every fast block.
 * Writes to stderr; low enough volume to be useful at normal rates
 * (~50 lines/sec at 23 ms/block). */
static int fast_block_timing_env = -1;
static inline int fast_block_timing(void) {
    if (fast_block_timing_env < 0) {
        const char *e = getenv("PS2204A_TIMING");
        fast_block_timing_env = (e && *e && *e != '0') ? 1 : 0;
    }
    return fast_block_timing_env;
}
static inline long long mono_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000LL + (long long)t.tv_nsec / 1000LL;
}

/* Pre-poll wait in microseconds. The device enters 0x3b only after its
 * internal capture buffer is full; polling during the capture window
 * just queues 0x33 ACKs on EP 0x81 that we have to drain *after*
 * capture is already done (each drain = ~200 us of dead USB time).
 *
 * Measured on 2204A @ fast streaming: with tight polling we see ~100
 * iterations over ~22 ms; capture itself is only 10.3 ms. The extra
 * ~12 ms is drain overhead. Sleeping ~9 ms before the first poll
 * brings the loop down to 2–3 iterations and total block time to ~12 ms
 * (57% -> ~17% dead-time ratio on the wire).
 *
 * The caller passes expected_us; pass 0 to disable the pre-wait. */
static int fast_block_capture(ps2204a_device_t *dev,
                              const uint8_t *cmd1, const uint8_t *cmd2,
                              const uint8_t *trigger_cmd,
                              uint8_t *raw_out, int raw_size,
                              int expected_us)
{
    int transferred;
    uint8_t resp[CMD_SIZE];
    long long t_start = 0, t_cmds = 0, t_after_sleep = 0;
    long long t_poll_done = 0, t_trig = 0, t_data = 0;
    int poll_iters = 0;
    int timing = fast_block_timing();
    if (timing) t_start = mono_us();

    /* Send config commands */
    int r1 = libusb_bulk_transfer(dev->handle, EP_CMD_OUT, (uint8_t *)cmd1,
                                  CMD_SIZE, &transferred, 1000);
    int r2 = libusb_bulk_transfer(dev->handle, EP_CMD_OUT, (uint8_t *)cmd2,
                                  CMD_SIZE, &transferred, 1000);
    if (r1 < 0 || r2 < 0) return -1;
    if (timing) t_cmds = mono_us();

    /* Skip the bulk of the known capture window before starting to poll —
     * avoids filling the FX2 response queue with 0x33 ACKs. Margin = 1 ms
     * so we re-enter polling slightly before the device transitions to 0x3b. */
    if (expected_us > 1500) {
        struct timespec ts;
        ts.tv_sec  = (expected_us - 1000) / 1000000;
        ts.tv_nsec = ((expected_us - 1000) % 1000000) * 1000L;
        nanosleep(&ts, NULL);
    }
    if (timing) t_after_sleep = mono_us();

    /* Poll status until 0x3b (ready).
     * The FX2 queues responses on EP 0x81: ACKs from cmd1/cmd2 arrive first,
     * then status responses from poll commands. We read and discard non-status
     * responses. usleep(500) between iterations prevents overwhelming the FX2
     * (Python's PyUSB overhead provides ~1ms natural pacing). */
    static const uint8_t poll[] = {0x02, 0x01, 0x01, 0x80,
                                   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                   0,0,0,0,0,0,0,0,0,0,0,0};
    /* Poll for capture-complete. Each iteration's RX read waits up to 20 ms,
     * so 500 iterations = ~10 s worst-case wait. That covers timebases up
     * to tb=17 (one block at tb=17 is ~10.5 s). To support the full tb=0-23
     * range on single-shot/streaming, extend to 5000 iterations = ~100 s —
     * still bounded, but enough even for tb=20 (84 s blocks). */
    int status = 0;
    for (int i = 0; i < 5000; i++) {
        libusb_bulk_transfer(dev->handle, EP_CMD_OUT, (uint8_t *)poll,
                             CMD_SIZE, &transferred, 500);
        int n = 0;
        int r = libusb_bulk_transfer(dev->handle, EP_RESP_IN, resp,
                                     CMD_SIZE, &n, 20);
        if (timing) poll_iters++;
        if (r == 0 && n > 0) {
            if (resp[0] == 0x3b) { status = 0x3b; break; }
            if (resp[0] == 0x7b) return -2;
        }
        /* No sleep — USB round-trip (~0.5ms) provides natural pacing */
    }

    if (status != 0x3b) return -1;
    if (timing) t_poll_done = mono_us();

    /* Commit data transfer (same packet for free-run and armed trigger). */
    int rt = libusb_bulk_transfer(dev->handle, EP_CMD_OUT,
                                  (uint8_t *)trigger_cmd,
                                  CMD_SIZE, &transferred, 1000);
    if (rt < 0) return -1;
    if (timing) t_trig = mono_us();

    int n = 0;
    int r = libusb_bulk_transfer(dev->handle, EP_DATA_IN, raw_out,
                                 raw_size, &n, 2000);
    if (r == LIBUSB_ERROR_TIMEOUT) return 0;
    if (r < 0) return -1;
    if (timing) {
        t_data = mono_us();
        fprintf(stderr,
                "[ps2204a timing] cmds=%lld sleep=%lld poll=%lld(%d it) trig=%lld data=%lld total=%lld us (bytes=%d)\n",
                t_cmds - t_start,
                t_after_sleep - t_cmds,
                t_poll_done - t_after_sleep, poll_iters,
                t_trig - t_poll_done,
                t_data - t_trig,
                t_data - t_start, n);
    }
    return n;
}

/* ========================================================================
 * Streaming Thread
 * ======================================================================== */

/* Write a float block into a ring buffer, handling wrap. */
static void ring_write(float *ring, size_t cap, size_t wp,
                       const float *src, int n)
{
    if (!ring || !src || n <= 0) return;
    size_t idx = wp % cap;
    if (idx + (size_t)n <= cap) {
        memcpy(&ring[idx], src, n * sizeof(float));
    } else {
        size_t first = cap - idx;
        memcpy(&ring[idx], src, first * sizeof(float));
        memcpy(&ring[0], &src[first], (n - first) * sizeof(float));
    }
}

static void *fast_streaming_thread(void *arg)
{
    ps2204a_device_t *dev = (ps2204a_device_t *)arg;

    int tb = dev->timebase;
    if (tb < 0) tb = 0;
    if (tb > 23) tb = 23;
    /* Timebases 11..23 use the progressive divide-by-2 extrapolation in
     * get_tb_chan_bytes(); higher tb = longer blocks (up to tens of seconds
     * at tb=20+). The poll loop in fast_block_capture() is sized to wait
     * long enough for any of these. */

    bool both = dev->ch[0].enabled && dev->ch[1].enabled;
    int block_size = both ? MAX_SAMPLES_DUAL : MAX_SAMPLES_SINGLE;

    /* Expected capture time per block. In fast streaming the hardware
     * always samples at ~1280 ns regardless of the timebase byte, so the
     * block duration is only a function of block_size. ps2204a_get_streaming_dt_ns
     * returns this value once measured; during the very first block the field
     * is still 0, so we use the canonical 1280 ns up-front. */
    int dt_ns_est = ps2204a_get_streaming_dt_ns(dev);
    if (dt_ns_est <= 0) dt_ns_est = 1280;
    int expected_us = (int)(((long long)block_size * dt_ns_est) / 1000);

    /* Pre-build commands */
    uint8_t cmd1[CMD_SIZE], cmd2[CMD_SIZE], trigger[CMD_SIZE];
    build_capture_cmd1(dev, cmd1, block_size, tb);
    build_capture_cmd2(dev, cmd2);
    build_block_trigger(trigger);

    uint8_t raw[DATA_BUF_SIZE];
    float   *tmp_a = (float *)malloc(block_size * sizeof(float));
    float   *tmp_b = both ? (float *)malloc(block_size * sizeof(float)) : NULL;

    if (!tmp_a || (both && !tmp_b)) {
        free(tmp_a); free(tmp_b);
        /* Signal thread death to Go side / callers via streaming flag. */
        dev->streaming = false;
        return NULL;
    }

    float range_a_mv = (float)get_range_mv(dev->ch[0].range);
    float range_b_mv = (float)get_range_mv(dev->ch[1].range);

    /* Single initial flush */
    flush_buffers(dev);

    clock_gettime(CLOCK_MONOTONIC, &dev->stream_start);
    dev->stream_blocks = 0;
    dev->stream_samples_total = 0;
    int consecutive_fails = 0;

    while (dev->streaming) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        int n = fast_block_capture(dev, cmd1, cmd2, trigger,
                                   raw, DATA_BUF_SIZE, expected_us);
        if (n <= 2) {
            consecutive_fails++;
            /* Recovery: clear halts on all endpoints + flush + retry.
             * libusb_clear_halt is a no-op if the endpoint isn't stalled. */
            libusb_clear_halt(dev->handle, EP_CMD_OUT);
            libusb_clear_halt(dev->handle, EP_RESP_IN);
            libusb_clear_halt(dev->handle, EP_DATA_IN);
            flush_buffers(dev);
            usleep(50000);
            if (consecutive_fails > 20) {
                /* Give up: leave loop. streaming flag stays in user intent —
                 * we signal thread death by setting it false just below. */
                break;
            }
            continue;
        }
        consecutive_fails = 0;

        clock_gettime(CLOCK_MONOTONIC, &t1);

        /* Parse waveform(s) */
        int got = both
            ? parse_waveform_dual(raw, n, block_size,
                                  range_a_mv, range_b_mv, tmp_a, tmp_b)
            : parse_waveform(raw, n, block_size, range_a_mv, tmp_a);
        if (got <= 0) continue;

        /* Apply per-range calibration BEFORE storing in the ring so the
         * streaming display and measurements use the same corrected values
         * as single-shot capture_block does. Clamp to ±range_mv after
         * calibration so a saturated ADC byte doesn't extrapolate past the
         * screen rails when gain > 1. */
        {
            int ida = (int)dev->ch[0].range - 2;
            int idb = (int)dev->ch[1].range - 2;
            if (ida >= 0 && ida < 9) {
                float off = dev->cal_offset_mv[ida], g = dev->cal_gain[ida];
                if (off != 0.0f || g != 1.0f) {
                    for (int k = 0; k < got; k++) {
                        float v = (tmp_a[k] - off) * g;
                        if (v >  range_a_mv) v =  range_a_mv;
                        if (v < -range_a_mv) v = -range_a_mv;
                        tmp_a[k] = v;
                    }
                }
            }
            if (both && idb >= 0 && idb < 9) {
                float off = dev->cal_offset_mv[idb], g = dev->cal_gain[idb];
                if (off != 0.0f || g != 1.0f) {
                    for (int k = 0; k < got; k++) {
                        float v = (tmp_b[k] - off) * g;
                        if (v >  range_b_mv) v =  range_b_mv;
                        if (v < -range_b_mv) v = -range_b_mv;
                        tmp_b[k] = v;
                    }
                }
            }
        }

        /* Write to ring buffer(s) + update stats under mutex. */
        pthread_mutex_lock(&dev->stream_mutex);
        size_t cap = dev->ring_capacity;
        size_t wp  = dev->ring_write_pos;
        ring_write(dev->ring_a, cap, wp, tmp_a, got);
        if (both) ring_write(dev->ring_b, cap, wp, tmp_b, got);

        dev->ring_write_pos = wp + got;
        dev->stream_blocks++;
        dev->stream_samples_total += got;
        dev->stream_last_block_ms = timespec_ms(&t0, &t1);
        pthread_mutex_unlock(&dev->stream_mutex);

        /* Callback */
        if (dev->stream_cb) {
            dev->stream_cb(tmp_a, tmp_b, got, dev->stream_user);
        }
    }

    free(tmp_a);
    free(tmp_b);
    /* Signal completion (clean stop or fatal error) to any external observer
     * polling ps2204a_is_streaming(). stop_streaming still joins via
     * thread_started so this write never races with a live reader. */
    dev->streaming = false;
    return NULL;
}

/* ========================================================================
 * Native Streaming Thread (FPGA continuous mode, ~100 S/s hardware-limited)
 * ========================================================================
 *
 * PS2204A native streaming is capped at ~100 S/s by the hardware (verified
 * against the official SDK — run_streaming_ns at 10 us produced 0 samples,
 * legacy run_streaming at 10 ms produced ~80 B/s sustained).
 *
 * This mode is useful for truly gap-free slow monitoring (DC, low-freq).
 * For higher throughput, use PS_STREAM_FAST (~330+ kS/s via rapid block).
 *
 * Protocol (reverse-engineered from SDK USB trace):
 *   - Send streaming cmd1 + cmd2 + streaming trigger
 *   - Data arrives continuously on EP 0x82 in small packets (uint8 samples)
 *   - Send stop command (opcode 0x0a then 0x99) to terminate
 */
static void *native_streaming_thread(void *arg)
{
    ps2204a_device_t *dev = (ps2204a_device_t *)arg;

    /* Native streaming is single-channel on PS2204A (channel A) —
     * the FPGA pushes a single interleaved stream. */
    float range_a_mv = (float)get_range_mv(dev->ch[0].range);
    float scale_a = range_a_mv / ADC_HALF_RANGE;

    /* Build and send setup: cmd1, cmd2, trigger */
    uint8_t cmd1[CMD_SIZE], cmd2[CMD_SIZE], trigger[CMD_SIZE];
    build_streaming_cmd1(dev, cmd1, (int)dev->ring_capacity);
    build_streaming_cmd2(cmd2);
    build_stream_trigger(trigger);

    flush_buffers(dev);

    int transferred;
    if (libusb_bulk_transfer(dev->handle, EP_CMD_OUT, cmd1, CMD_SIZE,
                             &transferred, 1000) < 0) goto cleanup;
    if (libusb_bulk_transfer(dev->handle, EP_CMD_OUT, cmd2, CMD_SIZE,
                             &transferred, 1000) < 0) goto cleanup;

    /* Drain ACKs (best effort) */
    uint8_t ack[CMD_SIZE];
    for (int i = 0; i < 3; i++) {
        if (read_resp(dev, ack, CMD_SIZE, 30) <= 0) break;
    }

    if (libusb_bulk_transfer(dev->handle, EP_CMD_OUT, trigger, CMD_SIZE,
                             &transferred, 1000) < 0) goto cleanup;

    clock_gettime(CLOCK_MONOTONIC, &dev->stream_start);
    dev->stream_blocks = 0;
    dev->stream_samples_total = 0;

    /* Streaming read loop. Read small chunks with short timeout so we can
     * respond promptly to ps2204a_stop_streaming(). */
    uint8_t raw[512];
    float  *tmp_a = (float *)malloc(512 * sizeof(float));
    if (!tmp_a) goto cleanup;

    int consecutive_timeouts = 0;
    const int MAX_TIMEOUTS = 200;  /* ~10 s at 50ms timeout */

    /* The FPGA emits a short framing header at the start of the native
     * stream: in practice a 0x00 at byte 0, and a 0x00 0x01 pair ~14 bytes
     * in. Both get scaled to ~−range_mv (near-saturated negative) and
     * stick in the ring as persistent spikes — the "vertical lines at
     * regular intervals" observed in the GUI when switching from fast to
     * native. Drop the first NATIVE_SKIP_BYTES of the stream so only real
     * sample bytes reach the ring. */
    const int NATIVE_SKIP_BYTES = 32;
    int bytes_skipped = 0;

    while (dev->streaming) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        int n = 0;
        int r = libusb_bulk_transfer(dev->handle, EP_DATA_IN, raw,
                                     sizeof(raw), &n, 50);
        if (r == LIBUSB_ERROR_TIMEOUT || (r == 0 && n == 0)) {
            if (++consecutive_timeouts > MAX_TIMEOUTS) break;
            continue;
        }
        if (r < 0) {
            /* Try to recover a stalled endpoint once; if it persists,
             * bail out and let stop_streaming/close clean up. */
            libusb_clear_halt(dev->handle, EP_DATA_IN);
            break;
        }
        consecutive_timeouts = 0;

        clock_gettime(CLOCK_MONOTONIC, &t1);

        const uint8_t *src = raw;
        int src_n = n;
        if (bytes_skipped < NATIVE_SKIP_BYTES) {
            int skip = NATIVE_SKIP_BYTES - bytes_skipped;
            if (skip > src_n) skip = src_n;
            src += skip;
            src_n -= skip;
            bytes_skipped += skip;
            if (src_n == 0) continue;
        }

        /* Each uint8 byte is one sample centered at 128. */
        for (int i = 0; i < src_n; i++) {
            tmp_a[i] = ((float)src[i] - ADC_CENTER) * scale_a;
        }

        pthread_mutex_lock(&dev->stream_mutex);
        ring_write(dev->ring_a, dev->ring_capacity, dev->ring_write_pos,
                   tmp_a, src_n);
        dev->ring_write_pos += src_n;
        dev->stream_blocks++;
        dev->stream_samples_total += src_n;
        dev->stream_last_block_ms = timespec_ms(&t0, &t1);
        pthread_mutex_unlock(&dev->stream_mutex);

        if (dev->stream_cb) {
            dev->stream_cb(tmp_a, NULL, src_n, dev->stream_user);
        }
    }

    free(tmp_a);

cleanup:
    /* Send stop command to cleanly terminate streaming */
    send_stream_stop(dev);
    usleep(100000);
    flush_buffers(dev);
    /* Signal completion / thread death to observers. */
    dev->streaming = false;
    return NULL;
}

/* ========================================================================
 * SDK-style continuous streaming (PS_STREAM_SDK)
 *
 * Replays the exact USB dialogue captured from ps2000_run_streaming_ns on a
 * PS2204A. Produces gap-free 2-byte-interleaved (B,A) samples at ~1 MS/s
 * via continuous 32 KB reads on EP 0x82. Unlike PS_STREAM_NATIVE (capped at
 * ~100 S/s) this is the actual fast-streaming mode PicoScope 7 uses.
 *
 * Startup sequence (exact trace bytes):
 *   [32] 02 85 04 80 00 00 00 81 03 b2 00 08 00 00 ...
 *   [33] 02 85 04 80 00 00 00 05 04 8f 00 10 00 00 ...   → ACK drained
 *   [35] 02 85 04 9b ... 85 21 8c <zero params> ...
 *   [36] 8192-byte LUT on EP 0x06
 *   [37] 02 85 05 82 00 08 00 01          (buf_type=1)
 *   [38] 02 85 04 9b ... 85 21 8c <real params> ...
 *   [39] 8192-byte LUT on EP 0x06 (identical)
 *   [40] 02 85 05 82 00 08 00 01          (buf_type=1)
 *   [41] 02 85 04 9a 00 00 00 85 07 97 ...  (timebase + gain)
 *   [47] cmd1 (sample count 10000, buf 100, buf_type=0x41)
 *   [48] cmd2 (siggen sub-command + 85 08 8a / 85 04 81)
 *   [49] 02 07 06 00 00 00 00 01 00 00    (trigger)
 * ======================================================================== */

/* Exact 64-byte SDK setup packets, captured 2026-04-18. */
static const uint8_t SDK_SETUP_PRE1[CMD_SIZE] = {
    0x02, 0x85, 0x04, 0x80, 0x00, 0x00, 0x00, 0x81, 0x03, 0xb2, 0x00, 0x08, 0x00, 0x00
};
static const uint8_t SDK_SETUP_PRE2[CMD_SIZE] = {
    0x02, 0x85, 0x04, 0x80, 0x00, 0x00, 0x00, 0x05, 0x04, 0x8f, 0x00, 0x10, 0x00, 0x00
};
static const uint8_t SDK_SETUP_9B_A[CMD_SIZE] = {
    0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00, 0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x85, 0x05, 0x82, 0x00, 0x08, 0x00,
    0x00, 0x85, 0x04, 0x8b, 0x00, 0x00, 0x00, 0x87, 0x06, 0x00, 0x20, 0x00, 0x00, 0x03, 0x01, 0x00
};
static const uint8_t SDK_SETUP_9B_B[CMD_SIZE] = {
    0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00, 0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80,
    0x00, 0x00, 0x01, 0x5d, 0x86, 0x00, 0x01, 0x5d, 0x86, 0x00, 0x00, 0x00, 0x59, 0x02, 0xdc, 0x6c,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x85, 0x05, 0x82, 0x00, 0x08, 0x00,
    0x00, 0x85, 0x04, 0x8b, 0x00, 0x00, 0x00, 0x87, 0x06, 0x00, 0x20, 0x00, 0x00, 0x03, 0x01, 0x00
};
static const uint8_t SDK_SETUP_BUFTYPE1[CMD_SIZE] = {
    0x02, 0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01
};
static const uint8_t SDK_SETUP_TB_GAIN[CMD_SIZE] = {
    0x02, 0x85, 0x04, 0x9a, 0x00, 0x00, 0x00, 0x85, 0x07, 0x97, 0x00, 0x14, 0x00, 0x3f, 0x04, 0x40
};

/* cmd2 captured from SDK trace 2026-04-19. Bytes 7..9 were host-side
 * opaque counter values in the original SDK trace; hardware-validated
 * as safe to zero (EXP4 2026-04-19 → byte-identical stream: 229 360
 * samples, ~983 kS/s across 5×5 trials). Zeroed here for determinism. */
static const uint8_t SDK_CMD2[CMD_SIZE] = {
    0x02, 0x85, 0x0c, 0x86, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x85,
    0x05, 0x87, 0x00, 0x08, 0x00, 0x00, 0x85, 0x0b, 0x90, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x85, 0x08, 0x8a, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x03, 0x00,
    0x02, 0x02, 0x0c, 0x03, 0x0a, 0x00, 0x00, 0x85, 0x04, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t SDK_TRIGGER[CMD_SIZE] = {
    0x02, 0x07, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00
};

/* Forward decls — the real siggen LUT/cmd builders live with the siggen
 * public API below. sdk_streaming_thread injects the user's siggen LUT as
 * a 3rd 9b/LUT/buftype1 phase so the DAC keeps running during the stream.
 * Verified against SDK trace 2026-04-19 (packets [0035]/[0036]). */
static void build_awg_lut(uint8_t *lut, ps_wave_t type, uint32_t pkpk_uv,
                          int32_t offset_uv, uint8_t duty_pct);
static void build_siggen_cmd(uint8_t *cmd, uint32_t freq_param,
                             uint32_t stop_param, uint32_t inc_param,
                             uint16_t dwell_samples);

/* Build cmd1 for SDK streaming.
 *
 * Byte-level layout verified 2026-04-19 from a parametric SDK cold-plug
 * capture (docs/sdk-streaming-protocol.md):
 *
 *   - `85 08 85` sub-command: sample_count is 5-byte BE at cmd[6..10].
 *     (This supersedes the block-mode "2-byte BE at bytes 9..10" claim
 *     — in SDK mode max_samples may exceed 65 535, e.g. 1 000 000 =
 *     `00 00 0f 42 40`.)
 *   - `85 08 93` sub-command: channel-mode is hard-coded to `00 06`
 *     in SDK/native mode (the block-mode timebase lookup does not
 *     apply here).
 *   - `85 08 89` sub-command: sample_interval as 3-byte BE count of
 *     10 ns FPGA ticks, at cmd[28..30]. ticks = interval_ns / 10.
 *     (Not `2^timebase`; that encoding is block-mode only.)
 *
 * Only the gain bytes (50..52) also vary with the channel/range
 * configuration. All other bytes are fixed across every SDK streaming
 * start observed on the wire.
 *
 * @param sample_count   value to emit into the 85 08 85 sub-command
 *                       (clamped to 40-bit range on the 5-byte BE field)
 * @param interval_ticks value to emit into the 85 08 89 sub-command
 *                       (= sample_interval_ns / 10, clamped to 24 bits)
 */
static void build_sdk_stream_cmd1(ps2204a_device_t *dev, uint8_t *cmd,
                                  uint64_t sample_count,
                                  uint32_t interval_ticks)
{
    uint8_t b50, b51, b52;
    encode_gain_bytes(dev, &b50, &b51, &b52);

    memset(cmd, 0, CMD_SIZE);
    const uint8_t tpl[] = {
        0x02,
        0x85, 0x08, 0x85, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x08, 0x93, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x06,
        0x85, 0x08, 0x89, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x41,
        0x85, 0x04, 0x9a, 0x00, 0x00, 0x00,
        0x85, 0x07, 0x97, 0x00, 0x14, 0x00, b50, b51, b52,
        0x85, 0x05, 0x95, 0x00, 0x08, 0x00, 0xff
    };
    memcpy(cmd, tpl, sizeof(tpl));

    /* sample_count — 5-byte BE at cmd[6..10] (inside the 85 08 85 body). */
    if (sample_count > 0xFFFFFFFFFFULL) sample_count = 0xFFFFFFFFFFULL;
    cmd[6]  = (uint8_t)((sample_count >> 32) & 0xFF);
    cmd[7]  = (uint8_t)((sample_count >> 24) & 0xFF);
    cmd[8]  = (uint8_t)((sample_count >> 16) & 0xFF);
    cmd[9]  = (uint8_t)((sample_count >>  8) & 0xFF);
    cmd[10] = (uint8_t)( sample_count        & 0xFF);

    /* interval_ticks — 3-byte BE at cmd[28..30] (inside 85 08 89 body). */
    if (interval_ticks > 0xFFFFFF) interval_ticks = 0xFFFFFF;
    cmd[28] = (uint8_t)((interval_ticks >> 16) & 0xFF);
    cmd[29] = (uint8_t)((interval_ticks >>  8) & 0xFF);
    cmd[30] = (uint8_t)( interval_ticks        & 0xFF);
}

/* Upload the 8192-byte streaming LUT on EP 0x06 synchronously. */
static int upload_stream_lut(ps2204a_device_t *dev)
{
    int transferred = 0;
    return libusb_bulk_transfer(dev->handle, EP_FW_OUT,
                                dev->fw.stream_lut,
                                (int)dev->fw.stream_lut_len, &transferred,
                                TIMEOUT_FW);
}

#define SDK_STREAM_POOL_SIZE    4
#define SDK_STREAM_SKIP_BYTES   32   /* First-read framing header */

/* FPGA commits EP 0x82 data in 16 384-byte blocks. Requesting transfers
 * that aren't aligned to that boundary stalls the pipe at intermediate
 * rates (observed: xfer=4k/10k/20k returned 0 samples for 50 µs/20 µs/10 µs
 * respectively). We therefore fix the chunk size at 16 384 — half the
 * old 32 KB — and only scale the libusb timeout. Empirically this
 * unlocks clean capture down to 1 ms/sample. */
#define SDK_STREAM_XFER_FIXED   16384

static unsigned sdk_stream_timeout_for_ticks(uint32_t ticks)
{
    if (ticks == 0) ticks = 100;
    /* Samples per chunk = SDK_STREAM_XFER_FIXED / 2 = 8192.
     * Fill time (ms) = 8192 × ticks × 10 ns / 1e6 = ticks × 0.08192.
     * Give ourselves 4× margin; clamp to [500 ms, 30 s]. */
    unsigned long fill_ms = (unsigned long)ticks * 82UL / 1000UL;
    unsigned long timeout = fill_ms * 4;
    if (timeout < 500)   timeout = 500;
    if (timeout > 30000) timeout = 30000;
    return (unsigned)timeout;
}

typedef struct {
    ps2204a_device_t *dev;
    uint8_t          *buf;
    struct libusb_transfer *xfer;
    volatile bool     in_flight;
    volatile bool     fatal_error;
} sdk_xfer_ctx_t;

/* Shared state between the streaming thread and the async callbacks. */
typedef struct {
    ps2204a_device_t *dev;
    float             scale_a;
    float             scale_b;
    float             offset_a_mv;
    float             offset_b_mv;
    float             gain_a;
    float             gain_b;
    bool              ch_a_enabled;
    bool              ch_b_enabled;
    int               bytes_skipped;
    volatile int      fatal;
    /* Reusable scratch so callback doesn't malloc per-transfer. */
    float            *tmp_a;
    float            *tmp_b;
} sdk_stream_state_t;

/* Completion callback — runs on the streaming thread via
 * libusb_handle_events_timeout. Parses B,A interleaved uint8 samples,
 * writes floats to the ring buffer, then resubmits the transfer. */
static void LIBUSB_CALL sdk_stream_cb(struct libusb_transfer *xfer)
{
    sdk_xfer_ctx_t     *xctx = (sdk_xfer_ctx_t *)xfer->user_data;
    ps2204a_device_t   *dev  = xctx->dev;
    sdk_stream_state_t *st   = (sdk_stream_state_t *)dev->stream_internal;

    xctx->in_flight = false;

    if (xfer->status != LIBUSB_TRANSFER_COMPLETED) {
        /* TIMEOUT from a short-returning device is expected on the final
         * transfer after the stop command; only treat real USB errors as
         * fatal. */
        if (xfer->status != LIBUSB_TRANSFER_TIMED_OUT &&
            xfer->status != LIBUSB_TRANSFER_CANCELLED) {
            xctx->fatal_error = true;
            st->fatal = 1;
        }
        return;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int n = xfer->actual_length;
    uint8_t *src = xfer->buffer;

    /* Skip the first-packet framing header exactly once. */
    if (st->bytes_skipped < SDK_STREAM_SKIP_BYTES) {
        int skip = SDK_STREAM_SKIP_BYTES - st->bytes_skipped;
        if (skip > n) skip = n;
        src += skip;
        n -= skip;
        st->bytes_skipped += skip;
    }

    /* Drop a trailing odd byte (only happens on truncated transfers). */
    if (n & 1) n--;
    if (n <= 0) goto resubmit;

    int pairs = n / 2;
    /* Layout: byte[2k]=B sample, byte[2k+1]=A sample (as observed in trace). */
    for (int i = 0; i < pairs; i++) {
        if (st->ch_b_enabled || true) {
            float raw_b = ((float)src[2*i    ] - ADC_CENTER) * st->scale_b;
            st->tmp_b[i] = (raw_b - st->offset_b_mv) * st->gain_b;
        }
        float raw_a = ((float)src[2*i + 1] - ADC_CENTER) * st->scale_a;
        st->tmp_a[i] = (raw_a - st->offset_a_mv) * st->gain_a;
    }

    pthread_mutex_lock(&dev->stream_mutex);
    if (dev->ring_a) {
        ring_write(dev->ring_a, dev->ring_capacity, dev->ring_write_pos,
                   st->tmp_a, pairs);
    }
    if (dev->ring_b) {
        ring_write(dev->ring_b, dev->ring_capacity, dev->ring_write_pos,
                   st->tmp_b, pairs);
    }
    dev->ring_write_pos += pairs;
    dev->stream_blocks++;
    dev->stream_samples_total += pairs;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    dev->stream_last_block_ms = timespec_ms(&t0, &t1);
    pthread_mutex_unlock(&dev->stream_mutex);

    if (dev->stream_cb) {
        dev->stream_cb(st->tmp_a, st->ch_b_enabled ? st->tmp_b : NULL,
                       pairs, dev->stream_user);
    }

    /* Client-side auto_stop: the SDK doesn't encode this in the device
     * commands — it just stops polling once max_samples has been
     * delivered. Mirror that here by clearing dev->streaming, which the
     * outer event loop polls on every iteration. */
    if (dev->sdk_auto_stop && dev->sdk_max_samples &&
        dev->stream_samples_total >= dev->sdk_max_samples) {
        dev->streaming = false;
    }

resubmit:
    if (dev->streaming && !st->fatal) {
        int r = libusb_submit_transfer(xfer);
        if (r < 0) {
            xctx->fatal_error = true;
            st->fatal = 1;
        } else {
            xctx->in_flight = true;
        }
    }
}

static void *sdk_streaming_thread(void *arg)
{
    ps2204a_device_t *dev = (ps2204a_device_t *)arg;

    /* Build per-channel scaling up front. Ranges are frozen for the lifetime
     * of this stream; callers must stop/restart to change ranges (same
     * limitation as PS_STREAM_FAST). */
    sdk_stream_state_t state = {0};
    state.dev = dev;
    state.ch_a_enabled = dev->ch[0].enabled;
    state.ch_b_enabled = dev->ch[1].enabled;
    state.scale_a = (float)get_range_mv(dev->ch[0].range) / ADC_HALF_RANGE;
    state.scale_b = (float)get_range_mv(dev->ch[1].range) / ADC_HALF_RANGE;
    int ia = (int)dev->ch[0].range - 2;
    int ib = (int)dev->ch[1].range - 2;
    state.offset_a_mv = (ia >= 0 && ia < 9) ? dev->cal_offset_mv[ia] : 0.0f;
    state.offset_b_mv = (ib >= 0 && ib < 9) ? dev->cal_offset_mv[ib] : 0.0f;
    state.gain_a      = (ia >= 0 && ia < 9) ? dev->cal_gain[ia]      : 1.0f;
    state.gain_b      = (ib >= 0 && ib < 9) ? dev->cal_gain[ib]      : 1.0f;

    state.tmp_a = (float *)malloc((SDK_STREAM_XFER_FIXED / 2) * sizeof(float));
    state.tmp_b = (float *)malloc((SDK_STREAM_XFER_FIXED / 2) * sizeof(float));
    if (!state.tmp_a || !state.tmp_b) goto cleanup_mem;

    dev->stream_internal = &state;

    flush_buffers(dev);

    /* === Setup phase === */
    int transferred;
    if (libusb_bulk_transfer(dev->handle, EP_CMD_OUT,
                             (uint8_t *)SDK_SETUP_PRE1, CMD_SIZE,
                             &transferred, 1000) < 0) goto cleanup;
    if (libusb_bulk_transfer(dev->handle, EP_CMD_OUT,
                             (uint8_t *)SDK_SETUP_PRE2, CMD_SIZE,
                             &transferred, 1000) < 0) goto cleanup;

    /* Drain ACK from pre2 (observed: 2 bytes "ca ac"). */
    uint8_t ack[CMD_SIZE];
    for (int i = 0; i < 3; i++) {
        if (read_resp(dev, ack, CMD_SIZE, 100) <= 0) break;
    }

    /* First 9b setup + first LUT + buf_type=1. */
    if (libusb_bulk_transfer(dev->handle, EP_CMD_OUT,
                             (uint8_t *)SDK_SETUP_9B_A, CMD_SIZE,
                             &transferred, 1000) < 0) goto cleanup;
    if (upload_stream_lut(dev) < 0) goto cleanup;
    if (libusb_bulk_transfer(dev->handle, EP_CMD_OUT,
                             (uint8_t *)SDK_SETUP_BUFTYPE1, CMD_SIZE,
                             &transferred, 1000) < 0) goto cleanup;

    /* Second 9b setup + second LUT + buf_type=1. */
    if (libusb_bulk_transfer(dev->handle, EP_CMD_OUT,
                             (uint8_t *)SDK_SETUP_9B_B, CMD_SIZE,
                             &transferred, 1000) < 0) goto cleanup;
    if (upload_stream_lut(dev) < 0) goto cleanup;
    if (libusb_bulk_transfer(dev->handle, EP_CMD_OUT,
                             (uint8_t *)SDK_SETUP_BUFTYPE1, CMD_SIZE,
                             &transferred, 1000) < 0) goto cleanup;

    /* Timebase+gain packet. */
    if (libusb_bulk_transfer(dev->handle, EP_CMD_OUT,
                             (uint8_t *)SDK_SETUP_TB_GAIN, CMD_SIZE,
                             &transferred, 1000) < 0) goto cleanup;

    /* === Siggen injection phase (the SDK-trace fix) ===
     *
     * If the user has an active siggen config, the official SDK inserts a
     * 3rd `85 04 9b + 85 21 8c` packet followed by the actual waveform LUT
     * (and a BUFTYPE1 commit) right here — between TB_GAIN and cmd1/cmd2/
     * trigger. Without this phase the FPGA loads stream_lut.bin (DC) into
     * the DAC and siggen stays silent for the whole stream.
     *
     * Verified by diffing our replay against a trace captured with
     * ps2000_set_sig_gen_built_in + ps2000_run_streaming_ns active on the
     * real SDK (usb_trace.log packets [0035]/[0036]/[0037], 2026-04-19).
     */
    if (dev->siggen_configured) {
        uint8_t siggen_cmd[CMD_SIZE];
        build_siggen_cmd(siggen_cmd,
                         dev->siggen_freq_param,
                         dev->siggen_freq_stop_param > 0
                             ? dev->siggen_freq_stop_param
                             : dev->siggen_freq_param,
                         dev->siggen_inc_param,
                         dev->siggen_dwell_samples);
        if (libusb_bulk_transfer(dev->handle, EP_CMD_OUT,
                                 siggen_cmd, CMD_SIZE,
                                 &transferred, 1000) < 0) goto cleanup;

        uint8_t siggen_lut[8192];
        if (dev->siggen_use_arb) {
            for (int i = 0; i < 4096; i++) {
                siggen_lut[2*i]   = (uint8_t)( dev->siggen_arb_lut[i]       & 0xFF);
                siggen_lut[2*i+1] = (uint8_t)((dev->siggen_arb_lut[i] >> 8) & 0xFF);
            }
        } else {
            build_awg_lut(siggen_lut,
                          (dev->siggen_freq_param == 0) ? PS_WAVE_DC
                                                        : (ps_wave_t)dev->siggen_wave,
                          dev->siggen_pkpk_uv ? dev->siggen_pkpk_uv : 1000000,
                          dev->siggen_offset_uv,
                          dev->siggen_duty_pct ? dev->siggen_duty_pct : 50);
        }
        int lut_transferred = 0;
        if (libusb_bulk_transfer(dev->handle, EP_FW_OUT,
                                 siggen_lut, sizeof(siggen_lut),
                                 &lut_transferred, TIMEOUT_FW) < 0) goto cleanup;
        if (libusb_bulk_transfer(dev->handle, EP_CMD_OUT,
                                 (uint8_t *)SDK_SETUP_BUFTYPE1, CMD_SIZE,
                                 &transferred, 1000) < 0) goto cleanup;
    }

    /* Give the FPGA a moment to stabilise after LUT uploads. */
    usleep(50000);

    /* === Capture start: cmd1 + cmd2 + trigger ===
     *
     * sample_count/interval defaults mirror the 2026-04-19 trace
     * baseline (10 000 samples, 100 ticks = 1 µs = 1 MS/s). Callers can
     * override via ps2204a_set_sdk_stream_interval_ns() and
     * ps2204a_set_sdk_stream_auto_stop(); zero-valued fields fall back
     * to the default. auto_stop is handled client-side in sdk_stream_cb
     * (the SDK does exactly this: it counts delivered samples and stops
     * polling — the device sees identical commands in either case). */
    uint32_t ticks   = dev->sdk_interval_ticks ? dev->sdk_interval_ticks : 100;
    uint64_t samples = dev->sdk_max_samples    ? dev->sdk_max_samples    : 10000;

    uint8_t cmd1[CMD_SIZE];
    build_sdk_stream_cmd1(dev, cmd1, samples, ticks);
    if (libusb_bulk_transfer(dev->handle, EP_CMD_OUT,
                             cmd1, CMD_SIZE, &transferred, 1000) < 0) goto cleanup;

    if (libusb_bulk_transfer(dev->handle, EP_CMD_OUT,
                             (uint8_t *)SDK_CMD2, CMD_SIZE,
                             &transferred, 1000) < 0) goto cleanup;
    if (libusb_bulk_transfer(dev->handle, EP_CMD_OUT,
                             (uint8_t *)SDK_TRIGGER, CMD_SIZE,
                             &transferred, 1000) < 0) goto cleanup;

    clock_gettime(CLOCK_MONOTONIC, &dev->stream_start);
    dev->stream_blocks = 0;
    dev->stream_samples_total = 0;

    /* === Async transfer pool on EP 0x82 === */
    unsigned timeout_ms = sdk_stream_timeout_for_ticks(ticks);

    sdk_xfer_ctx_t pool[SDK_STREAM_POOL_SIZE] = {0};
    bool pool_ok = true;
    for (int i = 0; i < SDK_STREAM_POOL_SIZE; i++) {
        pool[i].dev = dev;
        pool[i].buf = (uint8_t *)malloc(SDK_STREAM_XFER_FIXED);
        pool[i].xfer = libusb_alloc_transfer(0);
        if (!pool[i].buf || !pool[i].xfer) { pool_ok = false; break; }

        libusb_fill_bulk_transfer(pool[i].xfer, dev->handle, EP_DATA_IN,
                                  pool[i].buf, SDK_STREAM_XFER_FIXED,
                                  sdk_stream_cb, &pool[i], timeout_ms);
        int r = libusb_submit_transfer(pool[i].xfer);
        if (r < 0) { pool_ok = false; break; }
        pool[i].in_flight = true;
    }

    /* Event loop. Callbacks do all the real work; we just pump events
     * until the caller asks us to stop or something fatal happens. */
    while (dev->streaming && !state.fatal && pool_ok) {
        struct timeval tv = {0, 100000};  /* 100 ms */
        libusb_handle_events_timeout(dev->ctx, &tv);
    }

    /* === Shutdown: cancel in-flight transfers, drain completions === */
    for (int i = 0; i < SDK_STREAM_POOL_SIZE; i++) {
        if (pool[i].xfer && pool[i].in_flight) {
            libusb_cancel_transfer(pool[i].xfer);
        }
    }
    /* Let cancellations complete (short loop is enough — 1s max). */
    for (int waited = 0; waited < 20; waited++) {
        bool any = false;
        for (int i = 0; i < SDK_STREAM_POOL_SIZE; i++) {
            if (pool[i].in_flight) { any = true; break; }
        }
        if (!any) break;
        struct timeval tv = {0, 50000};
        libusb_handle_events_timeout(dev->ctx, &tv);
    }

    for (int i = 0; i < SDK_STREAM_POOL_SIZE; i++) {
        if (pool[i].xfer) libusb_free_transfer(pool[i].xfer);
        free(pool[i].buf);
    }

    dev->stream_internal = NULL;

cleanup:
    send_stream_stop(dev);
    usleep(100000);
    flush_buffers(dev);

cleanup_mem:
    free(state.tmp_a);
    free(state.tmp_b);
    dev->streaming = false;
    return NULL;
}

/* ========================================================================
 * Public API Implementation
 * ======================================================================== */

/* Everything do_open() does after the FX2 upload + re-enumerate dance.
 * Factored out so the Android two-phase open path can invoke it once the
 * app has handed back a fresh USB file descriptor for the post-renum
 * device. */
static ps_status_t do_open_post_reenum(ps2204a_device_t *dev)
{
    ps_status_t st;

    /* ADC init */
    printf("\n[2] ADC initialization...\n");
    st = init_adc(dev);
    if (st != PS_OK) return st;

    /* Pre-FPGA config */
    printf("\n[3] Pre-FPGA configuration...\n");
    st = pre_fpga_config(dev);
    if (st != PS_OK) return st;

    /* FPGA upload */
    printf("\n[4] FPGA firmware upload...\n");
    st = upload_fpga(dev);
    if (st != PS_OK) return st;

    /* Post-FPGA config */
    printf("\n[5] Post-FPGA configuration...\n");
    st = post_fpga_config(dev);
    if (st != PS_OK) return st;

    /* Channel setup */
    printf("\n[6] Channel setup...\n");
    st = setup_channels(dev);
    if (st != PS_OK) return st;

    /* Device info */
    printf("\n[7] Reading device info...\n");
    read_device_info(dev);

    /* Default config */
    dev->ch[0].enabled = true;
    dev->ch[0].coupling = PS_DC;
    dev->ch[0].range = PS_5V;
    dev->ch[1].enabled = false;
    dev->ch[1].coupling = PS_DC;
    dev->ch[1].range = PS_5V;
    dev->timebase = 5;
    dev->n_samples = 1000;

    /* Factory-calibration defaults.
     *
     * These offsets were derived by comparing the official PicoSDK's
     * `ps2000_get_values` output to our raw ADC readings on the same
     * device (PS2204A reference unit, cal-date Feb-22) with a floating BNC.
     * For each range we computed:
     *
     *     offset_mv = our_raw_mean - sdk_reported_mean
     *
     * and built the table below. Applying it at capture time makes our
     * readings match what the SDK reports to its users — which is what
     * most downstream analysis code expects.
     *
     * Notes:
     *   - 50 mV shares the same PGA as 5 V plus a digital ÷70 scaling,
     *     so its offset sign is inverted compared to the other ranges.
     *   - Callers can override any entry via ps2204a_set_range_calibration()
     *     or auto-fit a fresh set via ps2204a_calibrate_dc_offset(). The
     *     latter is recommended whenever you have a known 0 V input on
     *     the channel — it produces a more accurate per-unit fit than
     *     this table (which is based on one reference device). */
    /* Factory DC-offset table, re-derived 2026-04-17 after fixing the
     * PGA_TABLE off-by-one bug. The values are (our_raw_mean − sdk_mean)
     * measured on a PS2204A reference unit with CH A shorted to GND:
     * they cancel the analog-chain DC offset so our driver reports ~0 V
     * when the input is at 0 V, matching the SDK's behaviour to within
     * ~10 mV on the worst range. Individual units will vary a little;
     * ps2204a_calibrate_dc_offset() will override these on demand. */
    static const float FACTORY_OFFSET_MV[9] = {
        /* PS_50MV  */    -1.16f,
        /* PS_100MV */    -2.23f,
        /* PS_200MV */    -5.89f,
        /* PS_500MV */   -13.97f,
        /* PS_1V    */   -29.91f,
        /* PS_2V    */   -45.61f,
        /* PS_5V    */  -148.15f,
        /* PS_10V   */  -270.87f,
        /* PS_20V   */  -598.69f,
    };
    /* Factory gain table, measured 2026-04-17 with a benchtop lab supply
     * and a DMM reference across mid-range DC inputs (~50–70 % of each
     * range). These correct the analog-chain gain error per PGA config. */
    static const float FACTORY_GAIN[9] = {
        /* PS_50MV  */ 1.472213f,   /* reference: 28 mV */
        /* PS_100MV */ 1.159257f,   /* reference: 80 mV */
        /* PS_200MV */ 1.160788f,   /* reference: 119 mV */
        /* PS_500MV */ 1.144097f,   /* reference: 250 mV */
        /* PS_1V    */ 1.145206f,   /* reference: 603 mV */
        /* PS_2V    */ 1.140339f,   /* reference: 995 mV */
        /* PS_5V    */ 1.111200f,   /* reference: 3.005 V */
        /* PS_10V   */ 1.119147f,   /* reference: 7.005 V */
        /* PS_20V   */ 1.187060f,   /* reference: 14.000 V */
    };
    for (int r = 0; r < 9; r++) {
        dev->cal_offset_mv[r] = FACTORY_OFFSET_MV[r];
        dev->cal_gain[r] = FACTORY_GAIN[r];
    }

    /* Default trigger (auto/free-run) */
    build_block_trigger(dev->trigger_cmd);

    printf("\n================================================\n");
    printf("Device ready!\n");
    printf("================================================\n");

    return PS_OK;
}

/* Full open path: used by desktop callers and by the single-phase
 * ps2204a_open_with_fd helper. Assumes the caller has already wrapped a
 * USB handle into dev->handle and claimed interface 0. */
static ps_status_t do_open(ps2204a_device_t *dev)
{
    ps_status_t st;

    printf("================================================\n");
    printf("PicoScope 2204A — libusb C Driver\n");
    printf("================================================\n");

    printf("\n[1] FX2 firmware upload...\n");
    st = upload_fx2(dev);
    if (st != PS_OK) return st;

    st = fx2_reenumerate(dev);
    if (st != PS_OK) return st;

    return do_open_post_reenum(dev);
}

ps_status_t ps2204a_open(ps2204a_device_t **out)
{
    if (!out) return PS_ERROR_PARAM;

    ps2204a_device_t *dev = (ps2204a_device_t *)calloc(1, sizeof(*dev));
    if (!dev) return PS_ERROR_ALLOC;

    pthread_mutex_init(&dev->stream_mutex, NULL);

    ps_status_t fw_st = load_firmware(dev);
    if (fw_st != PS_OK) {
        pthread_mutex_destroy(&dev->stream_mutex);
        free(dev);
        return fw_st;
    }

    int r = libusb_init(&dev->ctx);
    if (r < 0) {
        printf("libusb_init failed: %s\n", libusb_error_name(r));
        free_firmware(dev);
        pthread_mutex_destroy(&dev->stream_mutex);
        free(dev);
        return PS_ERROR_USB;
    }
    dev->owns_ctx = true;

    /* Find PicoScope */
    dev->handle = libusb_open_device_with_vid_pid(dev->ctx, PICO_VID, PICO_PID);
    if (!dev->handle) {
        printf("PicoScope 2204A not found (VID=%04x PID=%04x)\n",
               PICO_VID, PICO_PID);
        libusb_exit(dev->ctx);
        free_firmware(dev);
        free(dev);
        return PS_ERROR_USB;
    }

    /* Detach kernel driver */
    if (libusb_kernel_driver_active(dev->handle, 0) == 1) {
        libusb_detach_kernel_driver(dev->handle, 0);
    }

    r = libusb_claim_interface(dev->handle, 0);
    if (r < 0) {
        printf("Claim interface failed: %s\n", libusb_error_name(r));
        libusb_close(dev->handle);
        libusb_exit(dev->ctx);
        free_firmware(dev);
        free(dev);
        return PS_ERROR_USB;
    }

    ps_status_t st = do_open(dev);
    if (st != PS_OK) {
        ps2204a_close(dev);
        return st;
    }

    *out = dev;
    return PS_OK;
}

ps_status_t ps2204a_open_with_fd(ps2204a_device_t **out, int usb_fd)
{
    if (!out) return PS_ERROR_PARAM;

    ps2204a_device_t *dev = (ps2204a_device_t *)calloc(1, sizeof(*dev));
    if (!dev) return PS_ERROR_ALLOC;

    pthread_mutex_init(&dev->stream_mutex, NULL);

    ps_status_t fw_st = load_firmware(dev);
    if (fw_st != PS_OK) {
        pthread_mutex_destroy(&dev->stream_mutex);
        free(dev);
        return fw_st;
    }

#ifdef __ANDROID__
    /* Android apps can't enumerate /dev/bus/usb; we only ever wrap an FD
     * handed in by UsbDeviceConnection, so disable the startup scan. */
    libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
#endif

    int r = libusb_init(&dev->ctx);
    if (r < 0) {
        free_firmware(dev);
        free(dev);
        return PS_ERROR_USB;
    }
    dev->owns_ctx = true;

    /* Wrap Android file descriptor */
    r = libusb_wrap_sys_device(dev->ctx, (intptr_t)usb_fd, &dev->handle);
    if (r < 0 || !dev->handle) {
        printf("libusb_wrap_sys_device failed: %s\n", libusb_error_name(r));
        libusb_exit(dev->ctx);
        free_firmware(dev);
        free(dev);
        return PS_ERROR_USB;
    }

    r = libusb_claim_interface(dev->handle, 0);
    if (r < 0) {
        libusb_close(dev->handle);
        libusb_exit(dev->ctx);
        free_firmware(dev);
        free(dev);
        return PS_ERROR_USB;
    }

    ps_status_t st = do_open(dev);
    if (st != PS_OK) {
        ps2204a_close(dev);
        return st;
    }

    *out = dev;
    return PS_OK;
}

/* ------------------------------------------------------------------
 * Android two-phase open
 * ------------------------------------------------------------------
 *
 * On Android we cannot rescan /dev/bus/usb after the FX2 boots its new
 * firmware (LIBUSB_OPTION_NO_DEVICE_DISCOVERY is set, and even without
 * it the app sandbox forbids the scan). So the app has to perform the
 * Android-side re-enumeration dance itself: hand us a pre-renum fd,
 * wait for the re-attach intent, then hand us the post-renum fd.
 *
 * stage1 loads firmware, uploads FX2, then drops the USB handle so
 * Android's UsbDeviceConnection can be cleanly closed on the Java side.
 * stage2 wraps the new fd and runs the remaining ADC/FPGA init.
 *
 * The libusb context survives between stages; only the per-device
 * handle is recycled. */
ps_status_t ps2204a_open_fd_stage1(ps2204a_device_t **out, int usb_fd)
{
    if (!out) return PS_ERROR_PARAM;

    ps2204a_device_t *dev = (ps2204a_device_t *)calloc(1, sizeof(*dev));
    if (!dev) return PS_ERROR_ALLOC;

    pthread_mutex_init(&dev->stream_mutex, NULL);

    ps_status_t fw_st = load_firmware(dev);
    if (fw_st != PS_OK) {
        pthread_mutex_destroy(&dev->stream_mutex);
        free(dev);
        return fw_st;
    }

#ifdef __ANDROID__
    libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
#endif

    int r = libusb_init(&dev->ctx);
    if (r < 0) {
        free_firmware(dev);
        pthread_mutex_destroy(&dev->stream_mutex);
        free(dev);
        return PS_ERROR_USB;
    }
    dev->owns_ctx = true;

    r = libusb_wrap_sys_device(dev->ctx, (intptr_t)usb_fd, &dev->handle);
    if (r < 0 || !dev->handle) {
        libusb_exit(dev->ctx);
        free_firmware(dev);
        pthread_mutex_destroy(&dev->stream_mutex);
        free(dev);
        return PS_ERROR_USB;
    }

    r = libusb_claim_interface(dev->handle, 0);
    if (r < 0) {
        libusb_close(dev->handle);
        libusb_exit(dev->ctx);
        free_firmware(dev);
        pthread_mutex_destroy(&dev->stream_mutex);
        free(dev);
        return PS_ERROR_USB;
    }

    /* Upload FX2; after this the device will disappear from the bus and
     * come back with a new USB address. */
    ps_status_t st = upload_fx2(dev);
    if (st != PS_OK) {
        libusb_release_interface(dev->handle, 0);
        libusb_close(dev->handle);
        libusb_exit(dev->ctx);
        free_firmware(dev);
        pthread_mutex_destroy(&dev->stream_mutex);
        free(dev);
        return st;
    }

    /* Drop the old handle so the Java side can close its
     * UsbDeviceConnection without fighting us for the interface. */
    libusb_release_interface(dev->handle, 0);
    libusb_close(dev->handle);
    dev->handle = NULL;

    *out = dev;
    return PS_OK;
}

ps_status_t ps2204a_open_fd_stage2(ps2204a_device_t *dev, int new_usb_fd)
{
    if (!dev || !dev->ctx) return PS_ERROR_PARAM;
    if (dev->handle) return PS_ERROR_STATE;

    int r = libusb_wrap_sys_device(dev->ctx, (intptr_t)new_usb_fd,
                                   &dev->handle);
    if (r < 0 || !dev->handle) return PS_ERROR_USB;

    r = libusb_claim_interface(dev->handle, 0);
    if (r < 0) {
        libusb_close(dev->handle);
        dev->handle = NULL;
        return PS_ERROR_USB;
    }

    return do_open_post_reenum(dev);
}

void ps2204a_close(ps2204a_device_t *dev)
{
    if (!dev) return;

    /* Always join the streaming thread if it was created, even if it has
     * already exited on its own (we still need to reclaim the pthread_t). */
    if (dev->thread_started) {
        ps2204a_stop_streaming(dev);
    }

    free(dev->ring_a);
    free(dev->ring_b);
    pthread_mutex_destroy(&dev->stream_mutex);

    if (dev->handle) {
        libusb_release_interface(dev->handle, 0);
        libusb_close(dev->handle);
    }
    if (dev->owns_ctx && dev->ctx) {
        libusb_exit(dev->ctx);
    }
    free_firmware(dev);
    free(dev);
}

/* ========================================================================
 * Configuration
 * ======================================================================== */

ps_status_t ps2204a_set_channel(ps2204a_device_t *dev, ps_channel_t ch,
                                bool enabled, ps_coupling_t coupling,
                                ps_range_t range)
{
    if (!dev || ch > PS_CHANNEL_B) return PS_ERROR_PARAM;
    if (range < PS_50MV || range > PS_20V) return PS_ERROR_PARAM;

    dev->ch[ch].enabled = enabled;
    dev->ch[ch].coupling = coupling;
    dev->ch[ch].range = range;
    return PS_OK;
}

ps_status_t ps2204a_set_timebase(ps2204a_device_t *dev, int timebase,
                                 int samples)
{
    if (!dev) return PS_ERROR_PARAM;
    if (timebase < 0 || timebase > 23) return PS_ERROR_PARAM;

    dev->timebase = timebase;
    dev->n_samples = samples > 8192 ? 8192 : samples;
    return PS_OK;
}

ps_status_t ps2204a_set_trigger(ps2204a_device_t *dev, ps_channel_t source,
                                float threshold_mv, ps_trigger_dir_t dir,
                                float delay_pct, int auto_trigger_ms)
{
    if (!dev) return PS_ERROR_PARAM;
    (void)auto_trigger_ms;  /* emulated host-side in capture_block */

    int range_mv = get_range_mv(source == PS_CHANNEL_A
                                ? dev->ch[0].range : dev->ch[1].range);
    int thr_sdk = (int)(threshold_mv / (float)range_mv * 32767.0f);
    if (thr_sdk >  32767) thr_sdk =  32767;
    if (thr_sdk < -32768) thr_sdk = -32768;

    int dp = (int)delay_pct;
    if (dp < -100) dp = -100;
    if (dp >  100) dp =  100;

    dev->trigger_armed     = true;
    dev->trigger_source    = source;
    dev->trigger_thr_sdk   = (int16_t)thr_sdk;
    dev->trigger_dir       = dir;
    dev->trigger_delay_pct = (int16_t)dp;
    dev->trigger_mode      = PS_TRIGGER_LEVEL;
    if (dev->trigger_hyst == 0) dev->trigger_hyst = 10;  /* default */

    build_block_trigger(dev->trigger_cmd);
    return PS_OK;
}

/* Extended variant with tunable hysteresis. */
ps_status_t ps2204a_set_trigger_ex(ps2204a_device_t *dev, ps_channel_t source,
                                   float threshold_mv, ps_trigger_dir_t dir,
                                   float delay_pct, int auto_trigger_ms,
                                   int hysteresis_counts)
{
    ps_status_t st = ps2204a_set_trigger(dev, source, threshold_mv, dir,
                                         delay_pct, auto_trigger_ms);
    if (st != PS_OK) return st;
    if (hysteresis_counts < 1) hysteresis_counts = 1;
    if (hysteresis_counts > 127) hysteresis_counts = 127;
    dev->trigger_hyst = (uint8_t)hysteresis_counts;
    return PS_OK;
}

ps_status_t ps2204a_disable_trigger(ps2204a_device_t *dev)
{
    if (!dev) return PS_ERROR_PARAM;
    dev->trigger_armed = false;
    dev->trigger_mode  = PS_TRIGGER_LEVEL;
    build_block_trigger(dev->trigger_cmd);
    return PS_OK;
}

/* WINDOW trigger: fire when the signal enters [lower_mv, upper_mv]. Only
 * the ENTER direction has been traced against the SDK so far — PS_FALLING
 * (exit-window) currently maps to the same encoding; treat as experimental. */
ps_status_t ps2204a_set_trigger_window(ps2204a_device_t *dev,
                                       ps_channel_t source,
                                       float lower_mv, float upper_mv,
                                       ps_trigger_dir_t dir,
                                       float delay_pct, int auto_trigger_ms)
{
    if (!dev) return PS_ERROR_PARAM;
    (void)auto_trigger_ms;
    if (lower_mv > upper_mv) {
        float t = lower_mv; lower_mv = upper_mv; upper_mv = t;
    }

    int range_mv = get_range_mv(source == PS_CHANNEL_A
                                ? dev->ch[0].range : dev->ch[1].range);
    int lo_sdk = (int)(lower_mv / (float)range_mv * 32767.0f);
    int hi_sdk = (int)(upper_mv / (float)range_mv * 32767.0f);
    if (lo_sdk >  32767) lo_sdk =  32767;
    if (lo_sdk < -32768) lo_sdk = -32768;
    if (hi_sdk >  32767) hi_sdk =  32767;
    if (hi_sdk < -32768) hi_sdk = -32768;

    int dp = (int)delay_pct;
    if (dp < -100) dp = -100;
    if (dp >  100) dp =  100;

    dev->trigger_armed     = true;
    dev->trigger_source    = source;
    dev->trigger_thr_sdk   = (int16_t)lo_sdk;
    dev->trigger_thr2_sdk  = (int16_t)hi_sdk;
    dev->trigger_dir       = dir;
    dev->trigger_delay_pct = (int16_t)dp;
    dev->trigger_mode      = PS_TRIGGER_WINDOW;
    if (dev->trigger_hyst == 0) dev->trigger_hyst = 10;

    build_block_trigger(dev->trigger_cmd);
    return PS_OK;
}

/* Pulse-Width Qualifier: arms a LEVEL edge trigger, then sets the cmd1
 * status byte to 0x05 so the device requires the preceding pulse to match
 * the requested width. The pulse-width bounds (lower_ns / upper_ns) are
 * carried in a packet not yet captured in the SDK traces, so they are
 * stored but not sent — the current implementation therefore qualifies
 * the edge with the device's built-in PWQ filter at whatever bounds the
 * firmware last cached. Flagged experimental until the bounds packet is
 * reverse-engineered. */
ps_status_t ps2204a_set_trigger_pwq(ps2204a_device_t *dev,
                                    ps_channel_t source,
                                    float threshold_mv, ps_trigger_dir_t dir,
                                    int lower_ns, int upper_ns,
                                    float delay_pct, int auto_trigger_ms)
{
    if (!dev) return PS_ERROR_PARAM;
    (void)lower_ns; (void)upper_ns;
    ps_status_t st = ps2204a_set_trigger(dev, source, threshold_mv, dir,
                                         delay_pct, auto_trigger_ms);
    if (st != PS_OK) return st;
    dev->trigger_mode = PS_TRIGGER_PWQ;
    build_block_trigger(dev->trigger_cmd);
    return PS_OK;
}

ps_status_t ps2204a_set_resolution_enhancement(ps2204a_device_t *dev,
                                               int extra_bits)
{
    if (!dev) return PS_ERROR_PARAM;
    if (extra_bits < 0 || extra_bits > 4) return PS_ERROR_PARAM;
    dev->res_extra_bits = extra_bits;
    return PS_OK;
}

/* ========================================================================
 * Block Capture
 * ======================================================================== */

ps_status_t ps2204a_capture_block(ps2204a_device_t *dev, int samples,
                                  float *buf_a, float *buf_b,
                                  int *actual_samples)
{
    if (!dev || !dev->handle) return PS_ERROR_STATE;
    if (samples <= 0 || samples > 8192) return PS_ERROR_PARAM;

    bool both = dev->ch[0].enabled && dev->ch[1].enabled;
    int n = samples;

    flush_buffers(dev);

    /* Build and send config commands */
    uint8_t cmd1[CMD_SIZE], cmd2[CMD_SIZE];
    build_capture_cmd1(dev, cmd1, n, dev->timebase);
    build_capture_cmd2(dev, cmd2);

    int transferred;
    libusb_bulk_transfer(dev->handle, EP_CMD_OUT, cmd1, CMD_SIZE,
                         &transferred, 1000);
    usleep(10000);
    libusb_bulk_transfer(dev->handle, EP_CMD_OUT, cmd2, CMD_SIZE,
                         &transferred, 1000);
    usleep(20000);

    /* Flush config responses */
    uint8_t resp[CMD_SIZE];
    for (int i = 0; i < 3; i++) {
        if (read_resp(dev, resp, CMD_SIZE, 100) <= 0) break;
    }

    /* Poll status. Timeout scales with timebase — at tb=20+ a single block
     * takes tens of seconds, so 5 s isn't enough. Formula: block_time_ns =
     * (10 * 2^tb) * samples. Add 2 s of slack. */
    uint64_t block_ms = ((uint64_t)10 << dev->timebase) * (uint64_t)samples / 1000000ULL;
    int poll_timeout = (int)(block_ms + 2000);
    if (poll_timeout < 5000) poll_timeout = 5000;
    int status = poll_status(dev, poll_timeout);

    /* Host-side auto-trigger: if an armed trigger didn't fire within the
     * timeout, fall back to a free-run capture so the caller still gets a
     * block instead of an error. Matches the SDK's `auto_trigger_ms`
     * behaviour (which the device itself doesn't implement). */
    if (status != 0x3b && dev->trigger_armed) {
        flush_buffers(dev);
        bool saved = dev->trigger_armed;
        dev->trigger_armed = false;
        build_capture_cmd1(dev, cmd1, n, dev->timebase);
        build_capture_cmd2(dev, cmd2);
        libusb_bulk_transfer(dev->handle, EP_CMD_OUT, cmd1, CMD_SIZE, &transferred, 1000);
        usleep(10000);
        libusb_bulk_transfer(dev->handle, EP_CMD_OUT, cmd2, CMD_SIZE, &transferred, 1000);
        usleep(20000);
        for (int i = 0; i < 3; i++) read_resp(dev, resp, CMD_SIZE, 100);
        status = poll_status(dev, 2000);
        dev->trigger_armed = saved;
    }
    if (status == 0x7b) {
        /* Error recovery: flush + retry */
        flush_buffers(dev);
        usleep(100000);
        libusb_bulk_transfer(dev->handle, EP_CMD_OUT, cmd1, CMD_SIZE,
                             &transferred, 1000);
        usleep(10000);
        libusb_bulk_transfer(dev->handle, EP_CMD_OUT, cmd2, CMD_SIZE,
                             &transferred, 1000);
        usleep(20000);
        flush_buffers(dev);
        status = poll_status(dev, 2000);
        if (status != 0x3b) return PS_ERROR_STATE;
    }
    if (status != 0x3b) return PS_ERROR_TIMEOUT;

    /* The `02 07 06 00 40 00 00 02 01 00` packet is the "commit data
     * transfer" signal, NOT a trigger — the SDK sends it identically for
     * both free-run and armed-trigger captures, after the status poll
     * returns 0x3b. Without it, EP 0x82 returns 0 bytes even when the
     * capture is complete. */
    libusb_bulk_transfer(dev->handle, EP_CMD_OUT, dev->trigger_cmd,
                         CMD_SIZE, &transferred, 1000);
    usleep(20000);

    /* Read waveform data. Both single and dual captures use the same 16 KB
     * buffer; in dual mode the two channels are packed into that buffer
     * (2 × n_samples bytes of useful data). */
    uint8_t *raw = (uint8_t *)malloc(DATA_BUF_SIZE);
    if (!raw) return PS_ERROR_ALLOC;

    int raw_n = read_data(dev, raw, DATA_BUF_SIZE, TIMEOUT_DATA);
    if (raw_n < 4) {
        usleep(200000);
        raw_n = read_data(dev, raw, DATA_BUF_SIZE, TIMEOUT_DATA);
    }
    if (raw_n < 4) {
        free(raw);
        return PS_ERROR_TIMEOUT;
    }

    /* Parse */
    float range_a_mv = (float)get_range_mv(dev->ch[0].range);
    float range_b_mv = (float)get_range_mv(dev->ch[1].range);
    int got = 0;

    if (both) {
        got = parse_waveform_dual(raw, raw_n, n,
                                  range_a_mv, range_b_mv, buf_a, buf_b);
    } else if (dev->ch[0].enabled && buf_a) {
        int post = dev->trigger_armed
                   ? (n * (100 + dev->trigger_delay_pct)) / 100
                   : n;
        int dp = dev->trigger_armed ? dev->trigger_delay_pct : -100;
        got = parse_waveform_ex(raw, raw_n, n, post, dp, range_a_mv, buf_a);
    } else if (dev->ch[1].enabled && buf_b) {
        int post = dev->trigger_armed
                   ? (n * (100 + dev->trigger_delay_pct)) / 100
                   : n;
        int dp = dev->trigger_armed ? dev->trigger_delay_pct : -100;
        got = parse_waveform_ex(raw, raw_n, n, post, dp, range_b_mv, buf_b);
    }

    /* Apply per-range calibration (DC offset subtracted, gain applied).
     * After calibration, clamp to ±range_mv — with gain > 1 the inverse
     * formula can extrapolate values above the nominal range (e.g. a
     * saturated ADC byte at 255 on 20V range turns into ~24 V after our
     * 1.187 gain), which would show spikes outside the screen rails and
     * trigger false-clip indicators in the GUI. Clamping keeps the
     * display honest and ensures the clip-check logic works as intended. */
    int idx_a = (int)dev->ch[0].range - 2;
    int idx_b = (int)dev->ch[1].range - 2;
    if (buf_a && dev->ch[0].enabled && idx_a >= 0 && idx_a < 9) {
        float off = dev->cal_offset_mv[idx_a];
        float g   = dev->cal_gain[idx_a];
        float r   = (float)range_a_mv;
        if (off != 0.0f || g != 1.0f) {
            for (int i = 0; i < got; i++) {
                float v = (buf_a[i] - off) * g;
                if (v >  r) v =  r;
                if (v < -r) v = -r;
                buf_a[i] = v;
            }
        }
    }
    if (buf_b && dev->ch[1].enabled && idx_b >= 0 && idx_b < 9) {
        float off = dev->cal_offset_mv[idx_b];
        float g   = dev->cal_gain[idx_b];
        float r   = (float)range_b_mv;
        if (off != 0.0f || g != 1.0f) {
            for (int i = 0; i < got; i++) {
                float v = (buf_b[i] - off) * g;
                if (v >  r) v =  r;
                if (v < -r) v = -r;
                buf_b[i] = v;
            }
        }
    }

    if (buf_a && dev->ch[0].enabled) apply_res_enhancement(dev, buf_a, got);
    if (buf_b && dev->ch[1].enabled) apply_res_enhancement(dev, buf_b, got);

    if (actual_samples) *actual_samples = got;

    free(raw);
    return PS_OK;
}

/* ========================================================================
 * Equivalent-Time Sampling (software reconstruction)
 * ========================================================================
 * The SDK's ETS path captures at 100 MS/s and leans on the FPGA to vary
 * the trigger-to-sample phase across cycles. The interleave-advance
 * packet (`02 01 01 80 00 ...`) is emitted identically for every step
 * in the captured SDK trace (trace_ets.log), which rules out a host-
 * controlled phase register and means the phase variation comes from
 * natural jitter between the asynchronous input signal and the sample
 * clock. So we reproduce ETS the same way, purely in software: capture
 * many triggered blocks at tb=0 and bin each by its measured trigger
 * phase. No new USB protocol required.
 * ======================================================================== */

/* Estimate the sub-sample trigger phase of a captured block. Returns a
 * fraction in [0, 1): the position of the trigger crossing between two
 * adjacent samples, where 0 means the crossing is AT sample trig_idx
 * and values close to 1 mean it is almost at trig_idx+1. Returns -1.0f
 * if no valid crossing is found in the search window. */
static float estimate_trigger_phase(const float *samples, int n,
                                    int trig_idx, float thr_mv,
                                    ps_trigger_dir_t dir)
{
    if (!samples || n < 2) return -1.0f;

    const int WIN = 32;
    int lo = trig_idx - WIN;
    int hi = trig_idx + WIN;
    if (lo < 0) lo = 0;
    if (hi > n - 2) hi = n - 2;

    int best = -1;
    float best_dist = 1e30f;
    for (int j = lo; j <= hi; j++) {
        float s0 = samples[j];
        float s1 = samples[j + 1];
        bool cross = (dir == PS_RISING)
                     ? (s0 <  thr_mv && s1 >= thr_mv)
                     : (s0 >= thr_mv && s1 <  thr_mv);
        if (!cross) continue;
        float d = (float)(j - trig_idx);
        if (d < 0) d = -d;
        if (d < best_dist) {
            best_dist = d;
            best = j;
        }
    }
    if (best < 0) return -1.0f;

    float s0 = samples[best];
    float s1 = samples[best + 1];
    float denom = s1 - s0;
    if (denom == 0.0f) return 0.0f;
    float frac = (thr_mv - s0) / denom;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 0.9999f) frac = 0.9999f;
    return frac;
}

ps_status_t ps2204a_set_ets(ps2204a_device_t *dev, ps_ets_mode_t mode,
                            int interleaves, int cycles,
                            int *out_interval_ps)
{
    if (!dev) return PS_ERROR_PARAM;

    int base_ns = ps2204a_timebase_to_ns(dev->timebase);
    if (base_ns < 10) base_ns = 10;

    if (mode == PS_ETS_OFF) {
        dev->ets_interleaves = 0;
        dev->ets_cycles      = 0;
        if (out_interval_ps) *out_interval_ps = base_ns * 1000;
        return PS_OK;
    }

    if (interleaves == 0) interleaves = (mode == PS_ETS_SLOW) ? 20 : 10;
    if (cycles      == 0) cycles      = (mode == PS_ETS_SLOW) ? 4  : 2;

    if (interleaves < 2 || interleaves > 20) return PS_ERROR_PARAM;
    if (cycles < 1 || cycles > 32)           return PS_ERROR_PARAM;

    dev->ets_interleaves = interleaves;
    dev->ets_cycles      = cycles;
    if (out_interval_ps) *out_interval_ps = (base_ns * 1000) / interleaves;
    return PS_OK;
}

ps_status_t ps2204a_disable_ets(ps2204a_device_t *dev)
{
    if (!dev) return PS_ERROR_PARAM;
    dev->ets_interleaves = 0;
    dev->ets_cycles      = 0;
    return PS_OK;
}

ps_status_t ps2204a_capture_ets(ps2204a_device_t *dev, int n_samples,
                                float *out_a, float *out_b,
                                int out_cap, int *actual_samples,
                                int *actual_interval_ps)
{
    if (!dev || !dev->handle)                 return PS_ERROR_STATE;
    if (n_samples <= 0 || n_samples > 8192)   return PS_ERROR_PARAM;
    if (dev->ets_interleaves < 2)             return PS_ERROR_STATE;
    if (!dev->trigger_armed)                  return PS_ERROR_STATE;
    if (dev->trigger_mode != PS_TRIGGER_LEVEL) return PS_ERROR_PARAM;

    int inter   = dev->ets_interleaves;
    int cycles  = dev->ets_cycles;
    int want_ch = dev->trigger_source;
    int total   = n_samples * inter;
    if (out_cap < total) return PS_ERROR_PARAM;

    /* ETS honours the caller's current timebase as the base rate.
     * Effective per-sample interval is (base_ns × 1000) / interleaves in ps.
     * A lower timebase produces a finer ETS grid, but tb=0..2 are known to
     * return stale buffer data on some PS2204A units — callers should use
     * tb=3 (80 ns) or lower if verified working on their unit. */
    int base_ns = ps2204a_timebase_to_ns(dev->timebase);
    if (base_ns < 10) base_ns = 10;

    /* Disable the res-enhancement box filter while we capture — binning
     * smoothed samples blurs the phase. */
    int saved_res_bits = dev->res_extra_bits;
    dev->res_extra_bits = 0;

    /* Force trigger_delay_pct = +100 (trigger at start of block). On this
     * hardware, armed-trigger captures return stale buffer data at any
     * other delay (verified via bench_trigger_delay: only dp=+100 shows a
     * real signal). For ETS we look at the first edge inside the block
     * anyway, so trigger-at-start is the natural choice. */
    int saved_dp           = dev->trigger_delay_pct;
    dev->trigger_delay_pct = 100;

    /* Convert trigger threshold from SDK 16-bit signed counts to mV on the
     * source channel's range. */
    float range_mv = (float)get_range_mv(dev->ch[want_ch].range);
    float thr_mv   = (float)dev->trigger_thr_sdk * range_mv / 32768.0f;

    /* Trigger sits near start of block (dp=+100 → trig_idx=0). Look for
     * the first edge in the first few samples. */
    int trig_idx = 2;

    float   *buf_a = NULL, *buf_b = NULL;
    float   *acc_a = NULL, *acc_b = NULL;
    uint32_t *cnt_a = NULL, *cnt_b = NULL;

    if (out_a && dev->ch[0].enabled) {
        buf_a = (float *)calloc(n_samples, sizeof(float));
        acc_a = (float *)calloc(total, sizeof(float));
        cnt_a = (uint32_t *)calloc(total, sizeof(uint32_t));
        if (!buf_a || !acc_a || !cnt_a) goto out_alloc_err;
    }
    if (out_b && dev->ch[1].enabled) {
        buf_b = (float *)calloc(n_samples, sizeof(float));
        acc_b = (float *)calloc(total, sizeof(float));
        cnt_b = (uint32_t *)calloc(total, sizeof(uint32_t));
        if (!buf_b || !acc_b || !cnt_b) goto out_alloc_err;
    }
    if (!acc_a && !acc_b) goto out_alloc_err;

    int rounds = inter * cycles;
    int good_rounds = 0;
    for (int r = 0; r < rounds; r++) {
        int got = 0;
        ps_status_t st = ps2204a_capture_block(dev, n_samples,
                                               buf_a, buf_b, &got);
        if (st != PS_OK || got < n_samples) continue;

        const float *phase_src = (want_ch == PS_CHANNEL_A) ? buf_a : buf_b;
        if (!phase_src) continue;
        float frac = estimate_trigger_phase(phase_src, got, trig_idx,
                                            thr_mv, dev->trigger_dir);
        if (frac < 0.0f) continue;

        /* Binning: higher frac = trigger closer to next sample, meaning
         * the sample clock ran *later* than the trigger by (1 - frac).
         * Map the block into a single phase slot within each 10 ns cell. */
        int bin = (int)(frac * (float)inter);
        if (bin < 0)          bin = 0;
        if (bin >= inter)     bin = inter - 1;

        for (int i = 0; i < n_samples; i++) {
            int k = i * inter + bin;
            if (acc_a) { acc_a[k] += buf_a[i]; cnt_a[k]++; }
            if (acc_b) { acc_b[k] += buf_b[i]; cnt_b[k]++; }
        }
        good_rounds++;
    }

    if (good_rounds < inter) {
        free(buf_a); free(buf_b);
        free(acc_a); free(acc_b);
        free(cnt_a); free(cnt_b);
        dev->res_extra_bits    = saved_res_bits;
        dev->trigger_delay_pct = saved_dp;
        return PS_ERROR_TIMEOUT;
    }

    /* Finalise: normalise, fill empty bins by nearest-neighbour copy. */
    if (acc_a && out_a) {
        for (int k = 0; k < total; k++) {
            if (cnt_a[k] > 0) out_a[k] = acc_a[k] / (float)cnt_a[k];
            else              out_a[k] = 0.0f;
        }
        /* Single pass left-to-right, then right-to-left, copying last
         * known value into zero-count slots. */
        float last = 0.0f; bool have = false;
        for (int k = 0; k < total; k++) {
            if (cnt_a[k] > 0) { last = out_a[k]; have = true; }
            else if (have)    out_a[k] = last;
        }
        have = false;
        for (int k = total - 1; k >= 0; k--) {
            if (cnt_a[k] > 0) { last = out_a[k]; have = true; }
            else if (have)    out_a[k] = last;
        }
        apply_res_enhancement(dev, out_a, total);
    }
    if (acc_b && out_b) {
        for (int k = 0; k < total; k++) {
            if (cnt_b[k] > 0) out_b[k] = acc_b[k] / (float)cnt_b[k];
            else              out_b[k] = 0.0f;
        }
        float last = 0.0f; bool have = false;
        for (int k = 0; k < total; k++) {
            if (cnt_b[k] > 0) { last = out_b[k]; have = true; }
            else if (have)    out_b[k] = last;
        }
        have = false;
        for (int k = total - 1; k >= 0; k--) {
            if (cnt_b[k] > 0) { last = out_b[k]; have = true; }
            else if (have)    out_b[k] = last;
        }
        apply_res_enhancement(dev, out_b, total);
    }

    free(buf_a); free(buf_b);
    free(acc_a); free(acc_b);
    free(cnt_a); free(cnt_b);

    dev->res_extra_bits    = saved_res_bits;
    dev->trigger_delay_pct = saved_dp;

    if (actual_samples)     *actual_samples     = total;
    if (actual_interval_ps) *actual_interval_ps = (base_ns * 1000) / inter;
    return PS_OK;

out_alloc_err:
    free(buf_a); free(buf_b);
    free(acc_a); free(acc_b);
    free(cnt_a); free(cnt_b);
    dev->res_extra_bits    = saved_res_bits;
    dev->trigger_delay_pct = saved_dp;
    return PS_ERROR_ALLOC;
}

/* ========================================================================
 * Raw capture (diagnostic)
 * ======================================================================== */

ps_status_t ps2204a_debug_capture_cmds(ps2204a_device_t *dev, int samples,
                                       uint8_t cmd1_out[64],
                                       uint8_t cmd2_out[64])
{
    if (!dev || !cmd1_out || !cmd2_out) return PS_ERROR_PARAM;
    if (samples <= 0 || samples > 8192) return PS_ERROR_PARAM;
    build_capture_cmd1(dev, cmd1_out, samples, dev->timebase);
    build_capture_cmd2(dev, cmd2_out);
    return PS_OK;
}

ps_status_t ps2204a_capture_raw(ps2204a_device_t *dev, int samples,
                                uint8_t *raw_out, int raw_cap,
                                int *actual_bytes)
{
    if (!dev || !dev->handle || !raw_out) return PS_ERROR_PARAM;
    if (samples <= 0 || samples > 8192) return PS_ERROR_PARAM;

    int n = samples;

    flush_buffers(dev);

    uint8_t cmd1[CMD_SIZE], cmd2[CMD_SIZE];
    build_capture_cmd1(dev, cmd1, n, dev->timebase);
    build_capture_cmd2(dev, cmd2);

    int transferred;
    libusb_bulk_transfer(dev->handle, EP_CMD_OUT, cmd1, CMD_SIZE,
                         &transferred, 1000);
    usleep(10000);
    libusb_bulk_transfer(dev->handle, EP_CMD_OUT, cmd2, CMD_SIZE,
                         &transferred, 1000);
    usleep(20000);

    uint8_t resp[CMD_SIZE];
    for (int i = 0; i < 3; i++) {
        if (read_resp(dev, resp, CMD_SIZE, 100) <= 0) break;
    }

    int status = poll_status(dev, 5000);
    if (status == 0x7b) {
        flush_buffers(dev);
        usleep(100000);
        libusb_bulk_transfer(dev->handle, EP_CMD_OUT, cmd1, CMD_SIZE,
                             &transferred, 1000);
        usleep(10000);
        libusb_bulk_transfer(dev->handle, EP_CMD_OUT, cmd2, CMD_SIZE,
                             &transferred, 1000);
        usleep(20000);
        flush_buffers(dev);
        status = poll_status(dev, 2000);
        if (status != 0x3b) return PS_ERROR_STATE;
    }
    if (status != 0x3b) return PS_ERROR_TIMEOUT;

    libusb_bulk_transfer(dev->handle, EP_CMD_OUT, dev->trigger_cmd,
                         CMD_SIZE, &transferred, 1000);
    usleep(20000);

    uint8_t *raw = (uint8_t *)malloc(DATA_BUF_SIZE);
    if (!raw) return PS_ERROR_ALLOC;

    int raw_n = read_data(dev, raw, DATA_BUF_SIZE, TIMEOUT_DATA);
    if (raw_n < 4) {
        usleep(200000);
        raw_n = read_data(dev, raw, DATA_BUF_SIZE, TIMEOUT_DATA);
    }
    if (raw_n < 4) {
        free(raw);
        return PS_ERROR_TIMEOUT;
    }

    const uint8_t *valid;
    int valid_len = find_valid_segment(raw, raw_n, &valid);

    int copy_len = valid_len < raw_cap ? valid_len : raw_cap;
    if (copy_len > 0) memcpy(raw_out, valid, copy_len);

    if (actual_bytes) *actual_bytes = valid_len;

    free(raw);
    return PS_OK;
}

/* ========================================================================
 * Streaming
 * ======================================================================== */

ps_status_t ps2204a_start_streaming_mode(ps2204a_device_t *dev,
                                         ps_stream_mode_t mode,
                                         int sample_interval_us,
                                         ps_stream_cb_t callback,
                                         void *user_data,
                                         int ring_size)
{
    if (!dev || !dev->handle) return PS_ERROR_STATE;
    if (dev->streaming) return PS_ERROR_STATE;

    /* Compute timebase from interval (used by fast mode only) */
    if (sample_interval_us > 0) {
        int target_ns = sample_interval_us * 1000;
        int tb = 0;
        if (target_ns > 10) {
            tb = (int)round(log2(target_ns / 10.0));
        }
        if (tb < 0) tb = 0;
        if (tb > 23) tb = 23;
        dev->timebase = tb;
    }

    /* Allocate ring buffer */
    size_t cap = ring_size > 0 ? (size_t)ring_size : DEFAULT_RING_SIZE;
    free(dev->ring_a);
    free(dev->ring_b);
    dev->ring_a = (float *)calloc(cap, sizeof(float));
    /* Native mode is single-channel; fast and SDK modes support dual */
    dev->ring_b = ((mode == PS_STREAM_FAST || mode == PS_STREAM_SDK)
                   && dev->ch[1].enabled)
                  ? (float *)calloc(cap, sizeof(float)) : NULL;
    if (!dev->ring_a) return PS_ERROR_ALLOC;
    dev->ring_capacity = cap;
    dev->ring_write_pos = 0;

    dev->stream_cb = callback;
    dev->stream_user = user_data;
    dev->stream_mode = mode;
    dev->streaming = true;

    void *(*thread_fn)(void *);
    switch (mode) {
        case PS_STREAM_NATIVE: thread_fn = native_streaming_thread; break;
        case PS_STREAM_SDK:    thread_fn = sdk_streaming_thread;    break;
        default:               thread_fn = fast_streaming_thread;   break;
    }

    int r = pthread_create(&dev->stream_thread, NULL, thread_fn, dev);
    if (r != 0) {
        dev->streaming = false;
        return PS_ERROR_USB;
    }
    dev->thread_started = true;

    return PS_OK;
}

ps_status_t ps2204a_start_streaming(ps2204a_device_t *dev,
                                    int sample_interval_us,
                                    ps_stream_cb_t callback,
                                    void *user_data,
                                    int ring_size)
{
    /* Default to fast mode for backward compatibility */
    return ps2204a_start_streaming_mode(dev, PS_STREAM_FAST,
                                        sample_interval_us, callback,
                                        user_data, ring_size);
}

/* Public: set SDK-stream per-sample interval. 0 resets to default (1 µs).
 * Accepts 500 ns → 1 ms in steps of 10 ns (the FPGA tick quantum). */
ps_status_t ps2204a_set_sdk_stream_interval_ns(ps2204a_device_t *dev,
                                               uint32_t interval_ns)
{
    if (!dev) return PS_ERROR_PARAM;
    if (interval_ns == 0) {
        dev->sdk_interval_ticks = 0;
        return PS_OK;
    }
    if (interval_ns < 500 || interval_ns > 1000000) return PS_ERROR_PARAM;
    if (interval_ns % 10 != 0)                      return PS_ERROR_PARAM;
    dev->sdk_interval_ticks = interval_ns / 10;
    return PS_OK;
}

/* Public: client-side auto_stop. 0 = free-running (default). */
ps_status_t ps2204a_set_sdk_stream_auto_stop(ps2204a_device_t *dev,
                                             uint64_t max_samples)
{
    if (!dev) return PS_ERROR_PARAM;
    dev->sdk_max_samples = max_samples;
    dev->sdk_auto_stop   = max_samples > 0 ? 1 : 0;
    return PS_OK;
}

ps_status_t ps2204a_stop_streaming(ps2204a_device_t *dev)
{
    if (!dev) return PS_ERROR_PARAM;
    /* Use thread_started (not streaming) so that a thread which died due to
     * repeated USB errors is still properly joined, and the pthread_t is
     * reclaimed instead of leaking. Safe to call multiple times. */
    if (!dev->thread_started) return PS_OK;

    dev->streaming = false;
    pthread_join(dev->stream_thread, NULL);
    dev->thread_started = false;
    return PS_OK;
}

ps_status_t ps2204a_get_streaming_latest(ps2204a_device_t *dev,
                                         float *buf_a, float *buf_b,
                                         int n, int *actual)
{
    if (!dev || n <= 0) return PS_ERROR_PARAM;

    pthread_mutex_lock(&dev->stream_mutex);

    size_t wp = dev->ring_write_pos;
    size_t cap = dev->ring_capacity;

    if (wp == 0 || cap == 0) {
        pthread_mutex_unlock(&dev->stream_mutex);
        if (actual) *actual = 0;
        return PS_OK;
    }

    int avail = (int)(wp < (size_t)n ? wp : (size_t)n);
    if ((size_t)avail > cap) avail = (int)cap;

    size_t start = (wp - avail) % cap;

    if (buf_a && dev->ring_a) {
        if (start + (size_t)avail <= cap) {
            memcpy(buf_a, &dev->ring_a[start], avail * sizeof(float));
        } else {
            size_t first = cap - start;
            memcpy(buf_a, &dev->ring_a[start], first * sizeof(float));
            memcpy(&buf_a[first], &dev->ring_a[0],
                   (avail - first) * sizeof(float));
        }
    }

    if (buf_b && dev->ring_b) {
        if (start + (size_t)avail <= cap) {
            memcpy(buf_b, &dev->ring_b[start], avail * sizeof(float));
        } else {
            size_t first = cap - start;
            memcpy(buf_b, &dev->ring_b[start], first * sizeof(float));
            memcpy(&buf_b[first], &dev->ring_b[0],
                   (avail - first) * sizeof(float));
        }
    }

    pthread_mutex_unlock(&dev->stream_mutex);

    if (buf_a) apply_res_enhancement(dev, buf_a, avail);
    if (buf_b) apply_res_enhancement(dev, buf_b, avail);

    if (actual) *actual = avail;
    return PS_OK;
}

ps_status_t ps2204a_get_streaming_stats(ps2204a_device_t *dev,
                                        ps_stream_stats_t *stats)
{
    if (!dev || !stats) return PS_ERROR_PARAM;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    pthread_mutex_lock(&dev->stream_mutex);
    stats->blocks = dev->stream_blocks;
    stats->total_samples = dev->stream_samples_total;
    stats->elapsed_s = timespec_ms(&dev->stream_start, &now) / 1000.0;
    stats->samples_per_sec = stats->elapsed_s > 0
        ? stats->total_samples / stats->elapsed_s : 0;
    stats->blocks_per_sec = stats->elapsed_s > 0
        ? stats->blocks / stats->elapsed_s : 0;
    stats->last_block_ms = dev->stream_last_block_ms;
    pthread_mutex_unlock(&dev->stream_mutex);

    return PS_OK;
}

bool ps2204a_is_streaming(ps2204a_device_t *dev)
{
    return dev ? dev->streaming : false;
}

/* ========================================================================
 * Signal Generator
 * ======================================================================== */

/* Fill an 8192-byte (4096 int16 LE) waveform table for the AWG.
 *
 * Hardware convention, reverse-engineered from SDK USB captures:
 *   - center (DC level) = 2030
 *   - half_amp counts   = round(pkpk_uv * 475 / 1_000_000)
 *     (500 mVpp→237, 1 Vpp→475, 2 Vpp→950 — linear in pkpk)
 *   - 4096 samples per cycle
 *   - For DC (wave=5) or freq=0 we fill a flat table (all 2030) which
 *     silences the siggen output. */
static void build_awg_lut(uint8_t *lut, ps_wave_t type, uint32_t pkpk_uv,
                          int32_t offset_uv, uint8_t duty_pct)
{
    const int N = 4096;
    const int CENTER_DEFAULT = 2030;
    /* Amplitude: 297 counts per 1 Vpp (0.625 calibration factor applied).
     * Same scale applies to DC offset (both go through the same DAC). */
    int half_amp = (int)((pkpk_uv * 297ULL + 500000ULL) / 1000000ULL);
    int offset_counts = (int)((int64_t)offset_uv * 297 / 500000);
    /* The DAC inverts LUT values relative to output: higher LUT value ⇒
     * lower output voltage. So to produce a POSITIVE voltage offset we
     * must SHIFT THE CENTER DOWN in LUT space. Similarly for asymmetric
     * waveforms (duty cycle, ramps) we flip the LUT polarity so the
     * output shape matches user intent. */
    int center = CENTER_DEFAULT - offset_counts;
    if (duty_pct == 0) duty_pct = 50;
    if (duty_pct > 100) duty_pct = 100;
    int duty_i = (N * duty_pct) / 100;

    for (int i = 0; i < N; i++) {
        int16_t s;
        if (type == PS_WAVE_DC) {
            s = center;
        } else if (type == PS_WAVE_SQUARE) {
            /* User wants HIGH for duty_pct % of the period → LUT LOW for
             * that span (DAC inverts). */
            s = (i < duty_i) ? (center - half_amp) : (center + half_amp);
        } else if (type == PS_WAVE_TRIANGLE) {
            /* / \ : rises 0..N/2 then falls N/2..N */
            int ramp;
            if (i < N / 2) {
                ramp = -half_amp + (2 * half_amp * i) / (N / 2);
            } else {
                ramp = half_amp - (2 * half_amp * (i - N / 2)) / (N / 2);
            }
            s = (int16_t)(center + ramp);
        } else if (type == PS_WAVE_RAMPUP) {
            /* DAC inverts: output "up-ramp" needs LUT "down-ramp". */
            int ramp = half_amp - (2 * half_amp * i) / N;
            s = (int16_t)(center + ramp);
        } else if (type == PS_WAVE_RAMPDOWN) {
            int ramp = -half_amp + (2 * half_amp * i) / N;
            s = (int16_t)(center + ramp);
        } else {
            /* SINE — matches SDK convention: starts at center going negative
             * (equivalent to sin(-phase)). The scope doesn't care about the
             * absolute phase reference. */
            double phase = -2.0 * M_PI * (double)i / (double)N;
            s = (int16_t)(center + (int)(half_amp * sin(phase) + 0.5));
        }
        lut[2 * i]     = (uint8_t)(s & 0xFF);         /* little-endian int16 */
        lut[2 * i + 1] = (uint8_t)((s >> 8) & 0xFF);
    }
}

/* Build the 64-byte compound control packet for set_sig_gen.
 * Frequency is big-endian at [18..21] (start) and [22..25] (stop).
 * Every other byte is fixed — verified byte-for-byte against the SDK trace. */
/* Compound siggen control packet.
 *   freq_param      : the user-facing output frequency (or sweep START, the
 *                     lower of the two when sweeping — BE uint32).
 *   stop_param      : the sweep STOP (higher bound) — same as freq_param
 *                     for fixed-freq operation.
 *   inc_param       : sweep increment as a 32-bit scaled value
 *                     (inc_hz × 22906.368, low byte masked). 0 = no sweep.
 *   dwell_samples   : dwell time × 187500 (the DDS sample clock). 0 = no
 *                     sweep.
 *
 * Layout verified from SDK USB traces.
 */
static void build_siggen_cmd(uint8_t *cmd, uint32_t freq_param,
                             uint32_t stop_param, uint32_t inc_param,
                             uint16_t dwell_samples)
{
    memset(cmd, 0, CMD_SIZE);
    static const uint8_t tpl[64] = {
        0x02, 0x85, 0x04, 0x9b, 0x00, 0x00, 0x00,
        0x85, 0x21, 0x8c, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
        /* [18..25] freq_stop + freq_start BE (patched)*/  0,0,0,0, 0,0,0,0,
        /* [26..29] increment BE (patched for sweep)  */   0, 0, 0, 0,
        /* [30..31] dwell_samples BE (patched)        */   0, 0,
        /* [32..41] padding — 10 bytes                */   0,0,0,0,0, 0,0,0,0,0,
        0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x00,
        0x85, 0x04, 0x8b, 0x00, 0x00, 0x00,
        0x87, 0x06, 0x00, 0x20, 0x00, 0x00, 0x03, 0x01, 0x00
    };
    memcpy(cmd, tpl, sizeof(tpl));

    /* For sweeps [18..21] = STOP (higher), [22..25] = START (lower).
     * For fixed-freq both slots equal freq_param. */
    uint32_t hi = stop_param > freq_param ? stop_param : freq_param;
    uint32_t lo = freq_param;
    cmd[18] = (hi >> 24) & 0xFF; cmd[19] = (hi >> 16) & 0xFF;
    cmd[20] = (hi >>  8) & 0xFF; cmd[21] = (hi >>  0) & 0xFF;
    cmd[22] = (lo >> 24) & 0xFF; cmd[23] = (lo >> 16) & 0xFF;
    cmd[24] = (lo >>  8) & 0xFF; cmd[25] = (lo >>  0) & 0xFF;
    cmd[26] = (inc_param >> 24) & 0xFF; cmd[27] = (inc_param >> 16) & 0xFF;
    cmd[28] = (inc_param >>  8) & 0xFF; cmd[29] = (inc_param >>  0) & 0xFF;
    cmd[30] = (dwell_samples >> 8) & 0xFF;
    cmd[31] = (dwell_samples >> 0) & 0xFF;
}

/* Encode a frequency in the SDK's native 32-bit freq_param scale and mask
 * the low byte (see CLAUDE.md note in set_siggen for the hardware quirk). */
static uint32_t encode_freq_param(float hz)
{
    if (hz <= 0) return 0;
    uint32_t fp = (uint32_t)(hz * 22906.368f + 0.5f);
    return fp & 0xFFFFFF00u;
}

/* Full-featured siggen. Supports fixed freq, sweep, DC offset, duty cycle
 * for squares. Set start==stop (and inc=0, dwell=0) for fixed freq. */
ps_status_t ps2204a_set_siggen_ex(ps2204a_device_t *dev, ps_wave_t type,
                                  float start_hz, float stop_hz,
                                  float increment_hz, float dwell_s,
                                  uint32_t pkpk_uv, int32_t offset_uv,
                                  uint8_t duty_pct)
{
    if (!dev || !dev->handle) return PS_ERROR_STATE;
    if (pkpk_uv == 0) pkpk_uv = 1000000;

    dev->siggen_wave           = (uint8_t)(type & 0xFF);
    dev->siggen_offset_uv      = offset_uv;
    dev->siggen_duty_pct       = duty_pct ? duty_pct : 50;
    dev->siggen_use_arb        = false;
    /* Start + stop frequencies. For fixed-freq output just pass start==stop. */
    dev->siggen_freq_param     = encode_freq_param(start_hz);
    dev->siggen_freq_stop_param= encode_freq_param(stop_hz > start_hz ? stop_hz : start_hz);
    dev->siggen_inc_param      = encode_freq_param(increment_hz);
    /* Dwell in DDS-clock samples: 187 500 Hz clock verified. */
    uint32_t dwell = (uint32_t)(dwell_s * 187500.0f + 0.5f);
    if (dwell > 0xFFFF) dwell = 0xFFFF;
    dev->siggen_dwell_samples  = (uint16_t)dwell;

    return ps2204a_set_siggen_raw(dev, type, dev->siggen_freq_param, pkpk_uv);
}

/* Arbitrary waveform: caller provides up to 4096 int16 samples. Shorter
 * arrays are linearly resampled; longer ones are averaged down. */
ps_status_t ps2204a_set_siggen_arbitrary(ps2204a_device_t *dev,
                                         const int16_t *lut, int lut_n,
                                         float frequency_hz, uint32_t pkpk_uv)
{
    if (!dev || !dev->handle) return PS_ERROR_STATE;
    if (!lut || lut_n < 2) return PS_ERROR_PARAM;
    if (pkpk_uv == 0) pkpk_uv = 1000000;

    const int N = 4096;
    if (lut_n == N) {
        memcpy(dev->siggen_arb_lut, lut, N * sizeof(int16_t));
    } else {
        /* Linear resample to N samples. */
        for (int i = 0; i < N; i++) {
            float frac = (float)i * (lut_n - 1) / (N - 1);
            int idx = (int)frac;
            float t = frac - idx;
            int v0 = lut[idx];
            int v1 = lut[idx + 1 < lut_n ? idx + 1 : idx];
            dev->siggen_arb_lut[i] = (int16_t)(v0 + (v1 - v0) * t);
        }
    }
    dev->siggen_use_arb = true;
    dev->siggen_freq_param = encode_freq_param(frequency_hz);
    dev->siggen_freq_stop_param = dev->siggen_freq_param;
    dev->siggen_inc_param = 0;
    dev->siggen_dwell_samples = 0;
    /* Call ex path which knows about siggen_use_arb. */
    return ps2204a_set_siggen_raw(dev, PS_WAVE_SINE, dev->siggen_freq_param, pkpk_uv);
}

ps_status_t ps2204a_set_siggen(ps2204a_device_t *dev, ps_wave_t type,
                               float frequency_hz, uint32_t pkpk_uv)
{
    if (!dev || !dev->handle) return PS_ERROR_STATE;
    if (pkpk_uv == 0) pkpk_uv = 1000000;   /* default 1 Vpp */

    /* Reset any sweep / offset / duty state — callers using this simple
     * entry point expect a plain fixed-freq output. */
    dev->siggen_offset_uv = 0;
    dev->siggen_duty_pct = 50;
    dev->siggen_use_arb = false;
    dev->siggen_freq_stop_param = 0;
    dev->siggen_inc_param = 0;
    dev->siggen_dwell_samples = 0;

    /* SDK-verified DDS scaling: freq_param = round(freq_hz * 22906.368).
     *
     * Critical quirk: the low byte of freq_param MUST be 0x00 on this
     * hardware. Non-zero low bytes produce a severely distorted / aliased
     * output fundamental (measured: fp=0x015D8620 → 298 Hz instead of the
     * 1 kHz we'd expect). fp=0x015D8600 / 0x015D8700 / 0x015D8800 all
     * produce a clean 1 kHz, so we mask the low byte after scaling.
     * Resolution cost: 256 / 22906 ≈ 0.011 Hz — imperceptible. */
    uint32_t freq_param = (frequency_hz > 0.0f)
        ? ((uint32_t)(frequency_hz * 22906.368f + 0.5f) & 0xFFFFFF00u)
        : 0u;

    /* Track user intent so callers / GUI can read back the current state.
     * The frequency is stored in the SDK's native big-endian param space —
     * the field name is kept for source compatibility with earlier versions. */
    dev->siggen_freq_param = freq_param;
    dev->siggen_wave = (uint8_t)(type & 0xFF);
    dev->siggen_pkpk_uv = pkpk_uv;
    /* Arm the SDK-streaming 3rd-phase siggen injection. Cleared only when
     * a disable call is made (set_siggen with DC + freq=0). */
    dev->siggen_configured = !(type == PS_WAVE_DC && freq_param == 0);

    /* 1. Control packet on EP 0x01 (compound: 85 04 9b + 85 21 8c + trailers). */
    /* The waveform upload on EP 0x06 MUST be submitted asynchronously
     * BEFORE the control packet is sent. This matches the pattern used by
     * setup_channels() (and the real SDK): the `85 04 9b / 85 21 8c`
     * compound command triggers the FPGA to pull from EP 0x06 — a plain
     * synchronous bulk_transfer after the command times out because the
     * device is no longer in a state where it accepts waveform data. */

    libusb_clear_halt(dev->handle, EP_FW_OUT);

    uint8_t drain[CMD_SIZE];
    for (int i = 0; i < 5; i++) {
        if (read_resp(dev, drain, CMD_SIZE, 30) <= 0) break;
    }

    /* Build LUT. Supports offset, duty cycle, and arbitrary overrides. */
    uint8_t lut[8192];
    if (dev->siggen_use_arb) {
        for (int i = 0; i < 4096; i++) {
            lut[2*i]   = (uint8_t)( dev->siggen_arb_lut[i]       & 0xFF);
            lut[2*i+1] = (uint8_t)((dev->siggen_arb_lut[i] >> 8) & 0xFF);
        }
    } else {
        build_awg_lut(lut, (freq_param == 0) ? PS_WAVE_DC : type, pkpk_uv,
                      dev->siggen_offset_uv, dev->siggen_duty_pct);
    }

    uint8_t cmd_buf[CMD_SIZE];
    build_siggen_cmd(cmd_buf, freq_param, dev->siggen_freq_stop_param,
                     dev->siggen_inc_param, dev->siggen_dwell_samples);

    static const uint8_t get_data[CMD_SIZE] = {
        0x02, 0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01
    };

    /* Submit all three transfers async in SDK order:
     *   CMD → LUT → GET_DATA
     * The FPGA appears to batch these: it processes CMD, then pulls from
     * EP 0x06, then responds to GET_DATA. Submitting them all at once
     * (before any blocks on events) lets the device see them in the right
     * order without us blocking in between. */

    struct wf_ctx cmd_ctx = {false, false};
    struct wf_ctx lut_ctx = {false, false};
    struct wf_ctx get_ctx = {false, false};

    struct libusb_transfer *xfer_cmd = libusb_alloc_transfer(0);
    struct libusb_transfer *xfer_lut = libusb_alloc_transfer(0);
    struct libusb_transfer *xfer_get = libusb_alloc_transfer(0);
    if (!xfer_cmd || !xfer_lut || !xfer_get) {
        if (xfer_cmd) libusb_free_transfer(xfer_cmd);
        if (xfer_lut) libusb_free_transfer(xfer_lut);
        if (xfer_get) libusb_free_transfer(xfer_get);
        return PS_ERROR_ALLOC;
    }

    libusb_fill_bulk_transfer(xfer_cmd, dev->handle, EP_CMD_OUT,
                              cmd_buf, CMD_SIZE, wf_callback, &cmd_ctx, 5000);
    libusb_fill_bulk_transfer(xfer_lut, dev->handle, EP_FW_OUT,
                              lut, sizeof(lut), wf_callback, &lut_ctx, 5000);
    libusb_fill_bulk_transfer(xfer_get, dev->handle, EP_CMD_OUT,
                              (uint8_t *)get_data, CMD_SIZE,
                              wf_callback, &get_ctx, 5000);

    int sc = libusb_submit_transfer(xfer_cmd);
    int sl = libusb_submit_transfer(xfer_lut);
    int sg = libusb_submit_transfer(xfer_get);
    if (sc || sl || sg) {
        PS_LOG("set_siggen submit rc cmd=%d lut=%d get=%d (streaming=%d)",
               sc, sl, sg, (int)dev->streaming);
    }

    /* Wait for all three transfers to complete. */
    struct timeval tv = {3, 0};
    while (!cmd_ctx.done || !lut_ctx.done || !get_ctx.done) {
        libusb_handle_events_timeout(dev->ctx, &tv);
    }

    bool cmd_ok = cmd_ctx.success;
    bool upload_ok = lut_ctx.success;
    bool get_ok = get_ctx.success;

    libusb_free_transfer(xfer_cmd);
    libusb_free_transfer(xfer_lut);
    libusb_free_transfer(xfer_get);

    if (!cmd_ok || !upload_ok) {
        PS_LOG("set_siggen transfer status cmd_ok=%d upload_ok=%d get_ok=%d streaming=%d",
               (int)cmd_ok, (int)upload_ok, (int)get_ok, (int)dev->streaming);
        return PS_ERROR_USB;
    }

    usleep(50000);
    return PS_OK;
}

ps_status_t ps2204a_disable_siggen(ps2204a_device_t *dev)
{
    return ps2204a_set_siggen(dev, PS_WAVE_DC, 0.0f, 0);
}

/* Raw variant: skip freq_hz→freq_param conversion, send exactly what is asked.
 * Lets us bisect the true SDK mapping without fighting the scaling constant. */
ps_status_t ps2204a_set_siggen_raw(ps2204a_device_t *dev, ps_wave_t type,
                                   uint32_t freq_param, uint32_t pkpk_uv)
{
    if (!dev || !dev->handle) return PS_ERROR_STATE;
    if (pkpk_uv == 0) pkpk_uv = 1000000;

    dev->siggen_freq_param = freq_param;
    dev->siggen_wave = (uint8_t)(type & 0xFF);
    dev->siggen_pkpk_uv = pkpk_uv;
    /* A plain disable (set_siggen with DC+freq=0) clears the flag so the
     * SDK setup falls back to stream_lut.bin. Anything else means the user
     * wants output → let the SDK stream pick it up on next start. */
    dev->siggen_configured = !(type == PS_WAVE_DC && freq_param == 0);

    libusb_clear_halt(dev->handle, EP_FW_OUT);
    uint8_t drain[CMD_SIZE];
    for (int i = 0; i < 5; i++) {
        if (read_resp(dev, drain, CMD_SIZE, 30) <= 0) break;
    }

    uint8_t lut[8192];
    build_awg_lut(lut, (freq_param == 0) ? PS_WAVE_DC : type, pkpk_uv,
                  dev->siggen_offset_uv, dev->siggen_duty_pct);

    /* Use the stop/inc/dwell stashed on the device (set by set_siggen_ex);
     * set_siggen resets these to zero so fixed-freq output still works. */
    uint32_t stop = dev->siggen_freq_stop_param > 0
                    ? dev->siggen_freq_stop_param : freq_param;
    uint8_t cmd_buf[CMD_SIZE];
    build_siggen_cmd(cmd_buf, freq_param, stop,
                     dev->siggen_inc_param, dev->siggen_dwell_samples);

    static const uint8_t get_data[CMD_SIZE] = {
        0x02, 0x85, 0x05, 0x82, 0x00, 0x08, 0x00, 0x01
    };

    struct wf_ctx cmd_ctx = {false, false};
    struct wf_ctx lut_ctx = {false, false};
    struct wf_ctx get_ctx = {false, false};
    struct libusb_transfer *xc = libusb_alloc_transfer(0);
    struct libusb_transfer *xl = libusb_alloc_transfer(0);
    struct libusb_transfer *xg = libusb_alloc_transfer(0);
    if (!xc || !xl || !xg) {
        if (xc) libusb_free_transfer(xc);
        if (xl) libusb_free_transfer(xl);
        if (xg) libusb_free_transfer(xg);
        return PS_ERROR_ALLOC;
    }
    libusb_fill_bulk_transfer(xc, dev->handle, EP_CMD_OUT, cmd_buf, CMD_SIZE, wf_callback, &cmd_ctx, 5000);
    libusb_fill_bulk_transfer(xl, dev->handle, EP_FW_OUT,  lut,     sizeof(lut), wf_callback, &lut_ctx, 5000);
    libusb_fill_bulk_transfer(xg, dev->handle, EP_CMD_OUT, (uint8_t *)get_data, CMD_SIZE, wf_callback, &get_ctx, 5000);
    libusb_submit_transfer(xc);
    libusb_submit_transfer(xl);
    libusb_submit_transfer(xg);
    struct timeval tv = {3, 0};
    while (!cmd_ctx.done || !lut_ctx.done || !get_ctx.done) {
        libusb_handle_events_timeout(dev->ctx, &tv);
    }
    bool ok = cmd_ctx.success && lut_ctx.success && get_ctx.success;
    libusb_free_transfer(xc);
    libusb_free_transfer(xl);
    libusb_free_transfer(xg);
    usleep(50000);
    return ok ? PS_OK : PS_ERROR_USB;
}

/* ========================================================================
 * Info & Utilities
 * ======================================================================== */

/* ========================================================================
 * Calibration
 * ======================================================================== */

/* Set per-range calibration constants. `range` must be one of the ps_range_t
 * values (PS_50MV..PS_20V). Subsequent captures on that range have:
 *   out_mv = (raw_mv - offset_mv) * gain
 * Default offset=0, gain=1 (no correction). Pass gain=0 to reset a range. */
ps_status_t ps2204a_set_range_calibration(ps2204a_device_t *dev,
                                          ps_range_t range,
                                          float offset_mv, float gain)
{
    if (!dev) return PS_ERROR_PARAM;
    int idx = (int)range - 2;
    if (idx < 0 || idx >= 9) return PS_ERROR_PARAM;
    dev->cal_offset_mv[idx] = offset_mv;
    dev->cal_gain[idx] = (gain <= 0) ? 1.0f : gain;
    return PS_OK;
}

ps_status_t ps2204a_get_range_calibration(ps2204a_device_t *dev,
                                          ps_range_t range,
                                          float *offset_mv, float *gain)
{
    if (!dev) return PS_ERROR_PARAM;
    int idx = (int)range - 2;
    if (idx < 0 || idx >= 9) return PS_ERROR_PARAM;
    if (offset_mv) *offset_mv = dev->cal_offset_mv[idx];
    if (gain)      *gain      = dev->cal_gain[idx];
    return PS_OK;
}

/* Auto-calibrate DC offset for the currently-active ranges by capturing
 * a short block with the input assumed shorted (or at a known 0 V). The
 * measured mean becomes the new `cal_offset_mv` — subsequent captures
 * will read zero on both channels until user input is applied. */
ps_status_t ps2204a_calibrate_dc_offset(ps2204a_device_t *dev)
{
    if (!dev || !dev->handle) return PS_ERROR_STATE;

    /* Save/restore current trigger state; we must capture in free-run. */
    bool was_armed = dev->trigger_armed;
    dev->trigger_armed = false;

    const int N = 1000;
    float *buf_a = (float *)malloc(N * sizeof(float));
    float *buf_b = (float *)malloc(N * sizeof(float));
    if (!buf_a || !buf_b) { free(buf_a); free(buf_b); return PS_ERROR_ALLOC; }

    ps_status_t st = PS_OK;
    int got = 0;
    /* Two captures: let the first settle, use the second. */
    ps2204a_capture_block(dev, N, buf_a, buf_b, &got);
    st = ps2204a_capture_block(dev, N, buf_a, buf_b, &got);

    if (st == PS_OK && got > 0) {
        if (dev->ch[0].enabled) {
            double sum = 0;
            for (int i = 0; i < got; i++) sum += buf_a[i];
            int idx = (int)dev->ch[0].range - 2;
            if (idx >= 0 && idx < 9)
                dev->cal_offset_mv[idx] = (float)(sum / got);
        }
        if (dev->ch[1].enabled) {
            double sum = 0;
            for (int i = 0; i < got; i++) sum += buf_b[i];
            int idx = (int)dev->ch[1].range - 2;
            if (idx >= 0 && idx < 9)
                dev->cal_offset_mv[idx] = (float)(sum / got);
        }
    }

    dev->trigger_armed = was_armed;
    free(buf_a);
    free(buf_b);
    return st;
}

/* Read the raw EEPROM bytes that were fetched during open_unit (pages
 * 0x00, 0x40, 0x80, 0xC0 = 256 bytes total). Useful for anyone who wants
 * to reverse-engineer the PicoTech-specific calibration layout. */
ps_status_t ps2204a_get_eeprom_raw(ps2204a_device_t *dev,
                                   uint8_t *out, int out_len)
{
    if (!dev || !out) return PS_ERROR_PARAM;
    int n = out_len < 256 ? out_len : 256;
    memcpy(out, dev->eeprom_raw, n);
    return PS_OK;
}

ps_status_t ps2204a_get_info(ps2204a_device_t *dev, char *serial,
                             int serial_len, char *cal_date, int date_len)
{
    if (!dev) return PS_ERROR_PARAM;

    if (serial && serial_len > 0) {
        strncpy(serial, dev->serial, serial_len - 1);
        serial[serial_len - 1] = '\0';
    }
    if (cal_date && date_len > 0) {
        strncpy(cal_date, dev->cal_date, date_len - 1);
        cal_date[date_len - 1] = '\0';
    }
    return PS_OK;
}

int ps2204a_timebase_to_ns(int timebase)
{
    if (timebase < 0) return 10;
    return 10 * (1 << timebase);
}

/* Actual per-sample interval delivered by the hardware in the current mode.
 * Distinct from ps2204a_timebase_to_ns() because fast-streaming mode on the
 * PS2204A ignores the cmd1 timebase bytes and always samples at ~1280 ns per
 * sample (verified empirically across tb=0..10 with a 9600-baud UART source —
 * the bit period is constant at ~81 samples regardless of tb). Callers that
 * drive a time axis (scope trace, protocol decoder bit period) must use this
 * value; using timebase_to_ns in streaming produces the classic 4× frequency
 * scale error (a 1 kHz siggen output displayed as 4 kHz at tb=5).
 *
 * Native streaming is hardware-capped at ~100 S/s (~10 ms/sample). */
int ps2204a_get_streaming_dt_ns(const ps2204a_device_t *dev)
{
    if (!dev) return 0;
    if (!dev->streaming) return ps2204a_timebase_to_ns(dev->timebase);
    if (dev->stream_mode == PS_STREAM_NATIVE) return 10000000;
    if (dev->stream_mode == PS_STREAM_SDK) {
        uint32_t ticks = dev->sdk_interval_ticks ? dev->sdk_interval_ticks : 100;
        return (int)(ticks * 10); /* each tick = 10 ns FPGA cycle */
    }
    return 1280;
}

int ps2204a_max_samples(ps2204a_device_t *dev)
{
    if (!dev) return MAX_SAMPLES_SINGLE;
    if (dev->ch[0].enabled && dev->ch[1].enabled) return MAX_SAMPLES_DUAL;
    return MAX_SAMPLES_SINGLE;
}
