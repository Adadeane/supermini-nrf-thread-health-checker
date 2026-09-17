/*
 * SuperMini nRF52840 Thread Network Health Checker
 *
 * Copyright (c) 2026 Antigravity
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/net/openthread.h>
#include <stdio.h>

#include <openthread/instance.h>
#include <openthread/thread.h>
#include <openthread/cli.h>

#if defined(CONFIG_CHIP)
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
using namespace chip;
using namespace chip::DeviceLayer;
#endif

#include "thread_health.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* Standard Matter Test Commissioning Constants */
#define MATTER_TEST_VENDOR_ID     0xFFF1
#define MATTER_TEST_PRODUCT_ID    0x8000
#define MATTER_TEST_DISCRIMINATOR 3840
#define MATTER_TEST_PASSCODE      20202021
#define MATTER_MANUAL_PAIRING_CODE "34970112332"
#define MATTER_QR_CODE_PAYLOAD     "MT:Y.K9042C00KA0648G00"

static void init_usb_cdc_acm(void)
{
#if defined(CONFIG_USB_DEVICE_STACK)
    const struct device *const dev = DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);
    if (!device_is_ready(dev)) {
        LOG_WRN("CDC ACM device not ready");
        return;
    }

    int ret = usb_enable(NULL);
    if (ret != 0) {
        LOG_ERR("Failed to enable USB (%d)", ret);
        return;
    }

    LOG_INF("USB CDC ACM serial initialized successfully.");
#endif
}

static void print_banner(void)
{
    printf("\r\n\r\n");
    printf("######################################################################\r\n");
    printf("#     SuperMini nRF52840 Thread Health Checker & Matter Node        #\r\n");
    printf("######################################################################\r\n");
    printf("Hardware: Nordic nRF52840 SuperMini / nice!nano footprint\r\n");
    printf("Bootloader: Adafruit UF2 (Offset 0x26000 safe)\r\n");
    printf("Clock: Internal LFRC 32.768 kHz (XTAL-free safe fallback)\r\n");
    printf("----------------------------------------------------------------------\r\n");
    printf("MATTER COMMISSIONING CREDENTIALS (for Google Home App):\r\n");
    printf("  Device Type:          On/Off Light (0x0100)\r\n");
    printf("  Vendor ID (VID):      0x%04X (Test VID)\r\n", MATTER_TEST_VENDOR_ID);
    printf("  Product ID (PID):     0x%04X (Test PID)\r\n", MATTER_TEST_PRODUCT_ID);
    printf("  Discriminator:        %u\r\n", MATTER_TEST_DISCRIMINATOR);
    printf("  Setup Passcode:       %u\r\n", MATTER_TEST_PASSCODE);
    printf("  Manual Pairing Code:  %s\r\n", MATTER_MANUAL_PAIRING_CODE);
    printf("  QR Code Payload:      %s\r\n", MATTER_QR_CODE_PAYLOAD);
    printf("----------------------------------------------------------------------\r\n");
    printf("HOW TO PAIR WITH GOOGLE HOME:\r\n");
    printf("  1. Open Google Home app on your phone.\r\n");
    printf("  2. Tap '+' -> 'Set up a device' -> 'Matter-enabled device'.\r\n");
    printf("  3. Tap 'Set up without barcode' and enter: %s\r\n", MATTER_MANUAL_PAIRING_CODE);
    printf("  4. Google Home will commission the SuperMini over BLE and feed it\r\n");
    printf("     your Google Nest Thread network credentials.\r\n");
    printf("######################################################################\r\n\r\n");
}

int main(void)
{
    /* Initialize USB CDC ACM virtual serial port */
    init_usb_cdc_acm();

    /* Short delay for USB enumeration on host */
    k_sleep(K_MSEC(1500));

    /* Print startup banner and Matter pairing instructions */
    print_banner();

#if defined(CONFIG_CHIP)
    LOG_INF("Initializing Matter Server Stack...");
    CHIP_ERROR err = PlatformMgr().InitChipStack();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("PlatformMgr().InitChipStack() failed: %s", ErrorStr(err));
    } else {
        err = PlatformMgr().StartEventLoopTask();
        if (err != CHIP_NO_ERROR) {
            LOG_ERR("PlatformMgr().StartEventLoopTask() failed: %s", ErrorStr(err));
        }
    }
#endif

    /* Get OpenThread instance from Zephyr context */
    otInstance *ot_inst = openthread_get_default_instance();
    if (!ot_inst) {
        LOG_ERR("OpenThread instance not available!");
        return -1;
    }

    LOG_INF("OpenThread stack active. Initializing Health Diagnostic Engine...");
    thread_health_init(ot_inst);

    LOG_INF("System ready. OpenThread CLI is active on this serial port.");
    return 0;
}
