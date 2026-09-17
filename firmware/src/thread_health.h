/*
 * SuperMini nRF52840 Thread Network Health Checker
 *
 * Copyright (c) 2026 Antigravity
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef THREAD_HEALTH_H_
#define THREAD_HEALTH_H_

#include <zephyr/kernel.h>
#include <openthread/instance.h>
#include <openthread/thread.h>
#include <openthread/link.h>
#include <openthread/dataset.h>
#include <openthread/netdata.h>
#include <openthread/ip6.h>
#include <openthread/ping_sender.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum neighbors tracked in single snapshot */
#define MAX_TRACKED_NEIGHBORS 16

/* Diagnostic structure representing current link & mesh health */
typedef struct {
    otDeviceRole role;
    uint16_t rloc16;
    uint32_t partitionId;
    uint16_t channel;
    uint16_t panId;
    char networkName[OT_NETWORK_NAME_MAX_SIZE + 1];
    uint8_t extPanId[OT_EXT_PAN_ID_SIZE];

    /* Parent link if Child */
    bool hasParent;
    int8_t parentRssi;
    uint8_t parentLqi;
    uint8_t parentMargin;

    /* Neighbor statistics */
    uint8_t neighborCount;
    struct {
        uint16_t rloc16;
        otExtAddress extAddress;
        int8_t avgRssi;
        int8_t lastRssi;
        uint8_t lqi;
        uint8_t margin;
        uint16_t frameErrorRate; /* In percentage * 100 (e.g. 250 = 2.5%) */
        bool isChild;
    } neighbors[MAX_TRACKED_NEIGHBORS];

    /* Border Router ping latency */
    bool borderRouterFound;
    otIp6Address borderRouterIp;
    uint32_t lastPingRttMs;
    bool pingSuccess;
} thread_health_snapshot_t;

/**
 * @brief Initialize the Thread Health Checker engine.
 * @param instance OpenThread instance pointer.
 */
void thread_health_init(otInstance *instance);

/**
 * @brief Collect a health snapshot from OpenThread.
 * @param snapshot Output pointer to store current metrics.
 */
void thread_health_collect(thread_health_snapshot_t *snapshot);

/**
 * @brief Print formatted health dashboard over serial console.
 * @param snapshot Snapshot data to format and print.
 */
void thread_health_print(const thread_health_snapshot_t *snapshot);

/**
 * @brief Trigger an ICMPv6 ping to the Google Nest / Thread Border Router.
 */
void thread_health_ping_border_router(void);

/**
 * @brief Thread Health background worker thread entrypoint.
 */
void thread_health_worker(void *p1, void *p2, void *p3);

#ifdef __cplusplus
}
#endif

#endif /* THREAD_HEALTH_H_ */
