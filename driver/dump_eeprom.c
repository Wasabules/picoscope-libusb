/*
 * Dump all 4 EEPROM pages from PicoScope 2204A.
 * Pages 0x40-0xC0 likely contain per-range calibration data.
 */
#include "picoscope2204a.h"
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EP_CMD_OUT  0x01
#define EP_RESP_IN  0x81
#define CMD_SIZE    64

/* We access the internal device handle via a small trick:
 * ps2204a_open gives us the device, then we read info using the
 * same EEPROM commands the driver uses internally. */

/* Since we can't access internals directly, we'll open the device
 * normally and then issue EEPROM read commands ourselves.
 * But we need the libusb handle... Let's use a workaround. */

/* Actually, let's just open with the driver (which does full init)
 * and then issue the EEPROM read commands on top. The driver leaves
 * the USB connection open. We'll access it through libusb directly. */

static libusb_device_handle *find_device(void)
{
    libusb_context *ctx = NULL;
    libusb_init(&ctx);
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, 0x0CE9, 0x1007);
    if (!h) {
        libusb_exit(ctx);
        return NULL;
    }
    libusb_detach_kernel_driver(h, 0);
    libusb_claim_interface(h, 0);
    return h;
}

static void hex_dump(const uint8_t *data, int len, int base_addr)
{
    for (int i = 0; i < len; i += 16) {
        printf("  %04x: ", base_addr + i);
        for (int j = 0; j < 16 && i + j < len; j++) {
            printf("%02x ", data[i + j]);
        }
        /* Pad if short line */
        for (int j = len - i; j < 16; j++) printf("   ");
        printf(" |");
        for (int j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = data[i + j];
            printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        printf("|\n");
    }
}

int main(void)
{
    ps2204a_device_t *dev = NULL;

    printf("=== PicoScope 2204A - EEPROM Dump ===\n\n");

    /* Open device (full init) */
    ps_status_t st = ps2204a_open(&dev);
    if (st != PS_OK) {
        printf("Failed to open device (status=%d)\n", st);
        return 1;
    }

    /* Now open a second libusb handle to the same device for raw EEPROM access.
     * Actually, we can't have two handles. Let's use the internal handle.
     * Since the struct is opaque, we'll reopen after closing... no that won't work.
     *
     * Better: just read the EEPROM pages using the SAME protocol the driver uses,
     * by calling capture_block which goes through the same USB pipe. We can send
     * raw commands if we had the handle.
     *
     * The simplest approach: close the device and reopen with raw libusb,
     * replaying just enough init to read EEPROM.
     */

    /* Let's close the driver and use raw libusb */
    ps2204a_close(dev);
    dev = NULL;

    printf("\nRe-opening with raw libusb for EEPROM dump...\n\n");

    libusb_device_handle *h = find_device();
    if (!h) {
        printf("Could not re-open device\n");
        return 1;
    }

    /* The device has already been initialized by ps2204a_open (FX2 firmware persists).
     * We just need to issue the EEPROM read commands. */

    uint8_t cmd[CMD_SIZE];
    uint8_t resp[CMD_SIZE];
    int transferred;

    static const uint8_t addrs[] = {0x00, 0x40, 0x80, 0xC0};
    uint8_t eeprom[4][64];  /* 4 pages of up to 64 bytes each */
    int page_len[4] = {0};

    for (int a = 0; a < 4; a++) {
        printf("=== EEPROM Page 0x%02X ===\n", addrs[a]);

        /* Request: read from address */
        memset(cmd, 0, CMD_SIZE);
        uint8_t req[] = {0x02, 0x83, 0x02, 0x50, addrs[a]};
        memcpy(cmd, req, sizeof(req));
        libusb_bulk_transfer(h, EP_CMD_OUT, cmd, CMD_SIZE, &transferred, 5000);
        usleep(20000);

        /* Read ACK */
        libusb_bulk_transfer(h, EP_RESP_IN, resp, CMD_SIZE, &transferred, 500);

        /* Get data */
        memset(cmd, 0, CMD_SIZE);
        uint8_t get[] = {0x02, 0x03, 0x02, 0x50, 0x40};
        memcpy(cmd, get, sizeof(get));
        libusb_bulk_transfer(h, EP_CMD_OUT, cmd, CMD_SIZE, &transferred, 5000);
        usleep(30000);

        int n = 0;
        libusb_bulk_transfer(h, EP_RESP_IN, resp, CMD_SIZE, &n, 500);

        if (n > 0) {
            memcpy(eeprom[a], resp, n);
            page_len[a] = n;
            hex_dump(resp, n, addrs[a]);
        } else {
            printf("  (no data)\n");
        }
        printf("\n");
    }

    /* Analysis: look for calibration patterns */
    printf("\n=== Analysis ===\n\n");

    /* Page 0: Serial + cal date */
    if (page_len[0] > 0) {
        printf("Page 0x00: Device info\n");
        /* Look for serial */
        for (int i = 0; i < page_len[0] - 1; i++) {
            if (eeprom[0][i] == 'J' && eeprom[0][i+1] == 'O') {
                printf("  Serial at offset %d: ", i);
                for (int j = i; j < page_len[0] && eeprom[0][j] >= 32; j++)
                    printf("%c", eeprom[0][j]);
                printf("\n");
                break;
            }
        }
    }

    /* Pages 1-3: Look for int16 pairs that could be gain/offset calibration */
    for (int a = 1; a < 4; a++) {
        if (page_len[a] < 8) continue;
        printf("\nPage 0x%02X: Potential calibration data\n", addrs[a]);

        /* Interpret as int16 LE pairs (gain, offset) */
        printf("  As int16 LE pairs:\n");
        for (int i = 0; i < page_len[a] - 1; i += 2) {
            int16_t val = (int16_t)(eeprom[a][i] | (eeprom[a][i+1] << 8));
            printf("    [%02d] %6d (0x%04x)", i/2, val,
                   (unsigned)(eeprom[a][i] | (eeprom[a][i+1] << 8)));
            if (i % 8 == 6) printf("\n");
        }
        printf("\n");

        /* Interpret as uint16 LE */
        printf("  As uint16 LE:\n");
        for (int i = 0; i < page_len[a] - 1; i += 2) {
            uint16_t val = (uint16_t)(eeprom[a][i] | (eeprom[a][i+1] << 8));
            printf("    [%02d] %6u", i/2, val);
            if (i % 8 == 6) printf("\n");
        }
        printf("\n");
    }

    /* Cleanup */
    libusb_release_interface(h, 0);
    libusb_close(h);

    return 0;
}
