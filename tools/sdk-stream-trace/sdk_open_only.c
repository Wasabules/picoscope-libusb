/*
 * ps2000_open_unit + ps2000_close_unit with NO streaming, NO set_channel.
 * Run under the interceptor to see which bytes the SDK emits at pure open
 * time vs. what it reserves for the streaming start phase.
 */
#include <stdio.h>
#include <unistd.h>
#include "ps2000.h"

int main(void)
{
    printf("[sdk-open] opening...\n");
    int16_t h = ps2000_open_unit();
    if (h <= 0) { fprintf(stderr, "open failed: %d\n", h); return 1; }
    printf("[sdk-open] handle=%d — closing immediately\n", h);
    ps2000_close_unit(h);
    printf("[sdk-open] done.\n");
    return 0;
}
