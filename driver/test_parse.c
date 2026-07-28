/* Unit tests for the pure buffer-parsing and DSP helpers.
 *
 * These live behind `static` in picoscope2204a.c, so we include the
 * translation unit directly rather than widening the public surface just to
 * make it testable. No device is touched — every case is a synthetic buffer.
 *
 * Build:  make test_parse    Run:  ./test_parse
 */
#include "picoscope2204a.c"

#include <stdio.h>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("  FAIL %s:%d: ", __func__, __LINE__);                  \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

/* The device zero-fills the head of the transfer and writes the payload flush
 * against the end. Build one that way: 2-byte sync header, `pad` zero bytes,
 * then `payload_len` bytes taken from `payload`. */
static int make_buf(uint8_t *raw, int raw_len, int pad,
                    const uint8_t *payload, int payload_len)
{
    memset(raw, 0, (size_t)raw_len);
    raw[0] = 0x57; raw[1] = 0xa7;
    memcpy(raw + 2 + pad, payload, (size_t)payload_len);
    return 2 + pad + payload_len;
}

/* ---------------------------------------------------------------- */

static void test_segment_trims_only_leading_padding(void)
{
    uint8_t raw[64];
    const uint8_t payload[] = { 0x80, 0x7f, 0x81, 0x7e };
    int raw_len = make_buf(raw, (int)sizeof(payload) + 12, 8,
                           payload, (int)sizeof(payload));

    const uint8_t *seg = NULL;
    int len = find_valid_segment(raw, raw_len, &seg);

    CHECK(len == (int)sizeof(payload), "expected len %zu, got %d",
          sizeof(payload), len);
    CHECK(seg == raw + 10, "segment should start after the padding");
    CHECK(seg + len == raw + raw_len, "segment must run flush to the end");
}

/* 0x00 is negative full scale, not padding. A payload that ends on the rail
 * must keep those samples — trimming them is what used to shift the trigger
 * position and flip the dual-channel assignment. */
static void test_segment_keeps_trailing_zero_samples(void)
{
    uint8_t raw[64];
    const uint8_t payload[] = { 0x80, 0x7f, 0x00, 0x00 };
    int raw_len = make_buf(raw, 20, 8, payload, (int)sizeof(payload));

    const uint8_t *seg = NULL;
    int len = find_valid_segment(raw, raw_len, &seg);

    CHECK(len == 4, "trailing rail samples must survive, got len %d", len);
    CHECK(seg[2] == 0x00 && seg[3] == 0x00, "trailing zeros must be preserved");
}

/* After a channel-count change the device does not clear its buffer: a single
 * channel transfer arrives as [stale dual capture][padding][fresh data], with a
 * NON-zero byte 0. Anchoring on the first non-zero byte splices the stale block
 * and the whole padding run into the trace. */
static void test_segment_skips_stale_capture_before_padding(void)
{
    enum { RAW = 2 + 400 + PAD_RUN_MIN + 100 };
    static uint8_t raw[RAW];
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x57; raw[1] = 0xa7;

    /* Stale dual-interleaved leftovers at the head. */
    for (int i = 0; i < 400; i++) raw[2 + i] = (i & 1) ? 0x7c : 0x7f;
    /* PAD_RUN_MIN zeros stay in the middle, then fresh data flush to the end. */
    for (int i = 0; i < 100; i++) raw[2 + 400 + PAD_RUN_MIN + i] = (uint8_t)(0x90 + (i & 7));

    const uint8_t *seg = NULL;
    int len = find_valid_segment(raw, RAW, &seg);

    CHECK(len == 100, "should return only the fresh block, got %d", len);
    CHECK(seg == raw + 2 + 400 + PAD_RUN_MIN, "segment must start after the padding run");
    CHECK(seg[0] == 0x90, "first fresh byte, got 0x%02x", seg[0]);
    CHECK(seg + len == raw + RAW, "segment must run flush to the end");
}

/* A short run of railed samples inside the payload is signal, not padding. */
static void test_segment_keeps_short_rail_run(void)
{
    enum { RAW = 2 + 64 + 40 };
    static uint8_t raw[RAW];
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x57; raw[1] = 0xa7;
    for (int i = 0; i < 64; i++) raw[2 + i] = 0x00;          /* short rail run */
    for (int i = 0; i < 40; i++) raw[2 + 64 + i] = (uint8_t)(0x70 + i);

    const uint8_t *seg = NULL;
    int len = find_valid_segment(raw, RAW, &seg);
    /* 64 < PAD_RUN_MIN, so the fallback trims it as a leading zero run — but it
     * must never be mistaken for a padding *boundary* that discards data past
     * it. What matters is that the fresh block survives intact. */
    CHECK(len >= 40, "the 40 real samples must survive, got %d", len);
    CHECK(seg + len == raw + RAW, "segment must run flush to the end");
}

static void test_segment_all_padding(void)
{
    uint8_t raw[32];
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x57; raw[1] = 0xa7;

    const uint8_t *seg = NULL;
    int len = find_valid_segment(raw, (int)sizeof(raw), &seg);
    CHECK(seg != NULL, "must not return NULL for an all-zero payload");
    CHECK(len == (int)sizeof(raw) - 2, "all-zero payload should yield full length");
}

static void test_segment_runt_buffer(void)
{
    uint8_t raw[2] = { 0x57, 0xa7 };
    const uint8_t *seg = (const uint8_t *)1;
    int len = find_valid_segment(raw, 2, &seg);
    CHECK(len == 0 && seg == NULL, "a header-only transfer yields nothing");
}

/* The regression this whole exercise is about: the pair grid is anchored on
 * the END of the transfer, so B/A assignment must not depend on how many
 * trailing samples happen to sit at 0x00. */
static void test_dual_channel_assignment_is_stable(void)
{
    enum { PAIRS = 6, RAW = 64 };

    for (int rail_samples = 0; rail_samples <= 3; rail_samples++) {
        uint8_t payload[2 * PAIRS];
        /* B samples at even offsets, A at odd. Distinct, recognisable values. */
        for (int i = 0; i < PAIRS; i++) {
            payload[2 * i]     = (uint8_t)(0x20 + i);   /* B */
            payload[2 * i + 1] = (uint8_t)(0xB0 + i);   /* A */
        }
        /* Drive the final `rail_samples` bytes onto the negative rail. */
        for (int i = 0; i < rail_samples; i++) {
            payload[2 * PAIRS - 1 - i] = 0x00;
        }

        uint8_t raw[RAW];
        int raw_len = make_buf(raw, RAW, RAW - 2 - 2 * PAIRS,
                               payload, 2 * PAIRS);

        float a[PAIRS], b[PAIRS];
        uint32_t clip_a = 0, clip_b = 0;
        int got = parse_waveform_dual(raw, raw_len, PAIRS, 5000.0f, 5000.0f,
                                      a, b, &clip_a, &clip_b);
        /* Byte 0x00 is negative full scale, so the railed samples we planted
         * must be reported as clipped rather than silently absorbed. */
        CHECK((int)(clip_a + clip_b) == rail_samples,
              "rail=%d: expected %d clipped samples, got %u",
              rail_samples, rail_samples, clip_a + clip_b);
        CHECK(got == PAIRS, "rail=%d: expected %d pairs, got %d",
              rail_samples, PAIRS, got);

        /* Sample 0 is never on the rail, so it pins the channel mapping. */
        float want_b = ((float)0x20 - ADC_CENTER) * (5000.0f / ADC_HALF_RANGE);
        float want_a = ((float)0xB0 - ADC_CENTER) * (5000.0f / ADC_HALF_RANGE);
        CHECK(b[0] == want_b, "rail=%d: channel B misassigned (%.1f vs %.1f)",
              rail_samples, b[0], want_b);
        CHECK(a[0] == want_a, "rail=%d: channel A misassigned (%.1f vs %.1f)",
              rail_samples, a[0], want_a);
    }
}

/* ---------------------------------------------------------------- */

static void naive_moving_average(const float *in, int n, int N, float *out)
{
    int half = N / 2;
    for (int i = 0; i < n; i++) {
        double sum = 0;
        for (int k = 0; k < N; k++) {
            int idx = i - half + k;
            if (idx < 0) idx = 0;
            if (idx >= n) idx = n - 1;
            sum += in[idx];
        }
        out[i] = (float)(sum / N);
    }
}

static void test_moving_average_matches_naive(void)
{
    enum { N = 200 };
    float in[N], got[N], want[N];
    for (int i = 0; i < N; i++) {
        in[i] = (float)((i * 37) % 101) - 50.0f;
    }

    const int taps[] = { 4, 16, 64, 256 };
    for (unsigned t = 0; t < sizeof(taps) / sizeof(taps[0]); t++) {
        int k = taps[t];
        memcpy(got, in, sizeof(in));
        apply_moving_average(got, N, k);
        naive_moving_average(in, N, k > N ? N : k, want);

        double worst = 0;
        for (int i = 0; i < N; i++) {
            double d = fabs((double)got[i] - (double)want[i]);
            if (d > worst) worst = d;
        }
        CHECK(worst < 1e-3, "taps=%d: running-sum drifts from naive by %.6f",
              k, worst);
    }
}

static void test_moving_average_noop_cases(void)
{
    float buf[4] = { 1, 2, 3, 4 };
    apply_moving_average(buf, 4, 1);
    CHECK(buf[0] == 1 && buf[3] == 4, "N=1 must be a no-op");
    apply_moving_average(NULL, 4, 8);   /* must not crash */
    apply_moving_average(buf, 0, 8);
}

/* ---------------------------------------------------------------- */

static void test_ring_read_unwraps(void)
{
    enum { CAP = 8 };
    float ring[CAP];
    for (int i = 0; i < CAP; i++) ring[i] = (float)i;

    /* Write position 11 => the logical stream is ...8,9,10 held at slots 0,1,2.
     * Reading the last 5 must span the wrap: slots 6,7,0,1,2. */
    float out[5];
    ring_read(ring, CAP, 11, 5, out);
    const float want[5] = { 6, 7, 0, 1, 2 };
    for (int i = 0; i < 5; i++) {
        CHECK(out[i] == want[i], "wrapped read [%d]: got %.0f want %.0f",
              i, out[i], want[i]);
    }

    /* Non-wrapping read. */
    float out2[3];
    ring_read(ring, CAP, 6, 3, out2);
    CHECK(out2[0] == 3 && out2[1] == 4 && out2[2] == 5, "contiguous read");

    ring_read(NULL, CAP, 6, 3, out2);   /* must not crash */
    ring_read(ring, CAP, 6, 0, out2);
}

/* ---------------------------------------------------------------- */

static void test_range_index_bounds(void)
{
    CHECK(range_index(PS_50MV) == 0, "PS_50MV must map to 0");
    CHECK(range_index(PS_20V) == PS_RANGE_COUNT - 1, "PS_20V must map to last");
    CHECK(range_index((ps_range_t)1) < 0, "below-range must be rejected");
    CHECK(range_index((ps_range_t)11) < 0, "above-range must be rejected");
    CHECK(get_range_mv(PS_50MV) == 50 && get_range_mv(PS_20V) == 20000,
          "range table lookup");
    CHECK(get_range_mv((ps_range_t)99) == 5000, "out-of-range falls back");
}

/* ---------------------------------------------------------------- */

int main(void)
{
    printf("picoscope2204a — parser / DSP unit tests\n");
    printf("========================================\n");

    struct { const char *name; void (*fn)(void); } tests[] = {
        { "segment: trims only leading padding",  test_segment_trims_only_leading_padding },
        { "segment: keeps trailing rail samples", test_segment_keeps_trailing_zero_samples },
        { "segment: skips stale capture + padding", test_segment_skips_stale_capture_before_padding },
        { "segment: keeps short rail run",         test_segment_keeps_short_rail_run },
        { "segment: all-padding buffer",          test_segment_all_padding },
        { "segment: runt buffer",                 test_segment_runt_buffer },
        { "dual: A/B stable across rail samples", test_dual_channel_assignment_is_stable },
        { "movavg: matches naive reference",      test_moving_average_matches_naive },
        { "movavg: no-op and guard cases",        test_moving_average_noop_cases },
        { "ring_read: unwraps correctly",         test_ring_read_unwraps },
        { "range_index: bounds",                  test_range_index_bounds },
    };

    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = failures;
        tests[i].fn();
        printf("  %-42s %s\n", tests[i].name,
               failures == before ? "ok" : "FAILED");
    }

    printf("========================================\n");
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
