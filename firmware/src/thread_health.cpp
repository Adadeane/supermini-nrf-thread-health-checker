/*
 * SuperMini nRF52840 Thread Network Health Checker
 *
 * Copyright (c) 2026 Antigravity
 * SPDX-License-Identifier: Apache-2.0
 */

#include "thread_health.h"
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(thread_health, LOG_LEVEL_INF);

static otInstance *s_ot_instance = NULL;
static uint32_t s_last_ping_rtt = 0;
static bool s_ping_in_progress = false;
static bool s_ping_success = false;

/* Stack and thread definition for health monitor */
#define HEALTH_STACK_SIZE 2048
#define HEALTH_PRIORITY   7

K_THREAD_STACK_DEFINE(s_health_stack, HEALTH_STACK_SIZE);
static struct k_thread s_health_thread_data;

static const char *role_to_string(otDeviceRole role)
{
    switch (role) {
    case OT_DEVICE_ROLE_DISABLED: return "DISABLED";
    case OT_DEVICE_ROLE_DETACHED: return "DETACHED";
    case OT_DEVICE_ROLE_CHILD:    return "CHILD (FED/SED)";
    case OT_DEVICE_ROLE_ROUTER:   return "ROUTER";
    case OT_DEVICE_ROLE_LEADER:   return "LEADER";
    default:                      return "UNKNOWN";
    }
}

static void ping_reply_callback(const otPingSenderReply *aReply, void *aContext)
{
    ARG_UNUSED(aContext);
    s_last_ping_rtt = aReply->mRoundTripTime;
    s_ping_success = true;
    s_ping_in_progress = false;
}

static void ping_stats_callback(const otPingSenderStatistics *aStatistics, void *aContext)
{
    ARG_UNUSED(aContext);
    ARG_UNUSED(aStatistics);
    s_ping_in_progress = false;
}

void thread_health_init(otInstance *instance)
{
    s_ot_instance = instance;

    k_thread_create(&s_health_thread_data, s_health_stack,
                    K_THREAD_STACK_SIZEOF(s_health_stack),
                    thread_health_worker,
                    NULL, NULL, NULL,
                    HEALTH_PRIORITY, 0, K_MSEC(3000));
}

void thread_health_collect(thread_health_snapshot_t *snapshot)
{
    if (!s_ot_instance || !snapshot) {
        return;
    }

    memset(snapshot, 0, sizeof(thread_health_snapshot_t));

    openthread_api_mutex_lock(openthread_get_default_context());

    snapshot->role = otThreadGetDeviceRole(s_ot_instance);
    snapshot->rloc16 = otThreadGetRloc16(s_ot_instance);
    snapshot->partitionId = otThreadGetPartitionId(s_ot_instance);

    /* Operational Dataset */
    otOperationalDataset dataset;
    if (otDatasetGetActive(s_ot_instance, &dataset) == OT_ERROR_NONE) {
        if (dataset.mComponents.mIsChannelPresent) {
            snapshot->channel = dataset.mChannel;
        }
        if (dataset.mComponents.mIsPanIdPresent) {
            snapshot->panId = dataset.mPanId;
        }
        if (dataset.mComponents.mIsNetworkNamePresent) {
            strncpy(snapshot->networkName, dataset.mNetworkName.m8, OT_NETWORK_NAME_MAX_SIZE);
            snapshot->networkName[OT_NETWORK_NAME_MAX_SIZE] = '\0';
        }
        if (dataset.mComponents.mIsExtendedPanIdPresent) {
            memcpy(snapshot->extPanId, dataset.mExtendedPanId.m8, OT_EXT_PAN_ID_SIZE);
        }
    }

    /* Parent Link info if Child */
    if (snapshot->role == OT_DEVICE_ROLE_CHILD) {
        otRouterInfo parentInfo;
        if (otThreadGetParentInfo(s_ot_instance, &parentInfo) == OT_ERROR_NONE) {
            snapshot->hasParent = true;
            snapshot->parentRssi = parentInfo.mRssi;
            snapshot->parentLqi = parentInfo.mLinkQualityIn;
            snapshot->parentMargin = parentInfo.mLinkMargin;
        }
    }

    /* Neighbor table iteration */
    otNeighborInfoIterator iterator = OT_NEIGHBOR_INFO_ITERATOR_INIT;
    otNeighborInfo neighbor;
    uint8_t count = 0;

    while (count < MAX_TRACKED_NEIGHBORS &&
           otThreadGetNextNeighborInfo(s_ot_instance, &iterator, &neighbor) == OT_ERROR_NONE) {
        snapshot->neighbors[count].rloc16 = neighbor.mRloc16;
        memcpy(&snapshot->neighbors[count].extAddress, &neighbor.mExtAddress, sizeof(otExtAddress));
        snapshot->neighbors[count].avgRssi = neighbor.mAverageRssi;
        snapshot->neighbors[count].lastRssi = neighbor.mLastRssi;
        snapshot->neighbors[count].lqi = neighbor.mLinkQualityIn;
        snapshot->neighbors[count].margin = neighbor.mLinkMargin;
        snapshot->neighbors[count].frameErrorRate = (uint16_t)(((uint32_t)neighbor.mFrameErrorRate * 10000) / 0xFFFF);
        snapshot->neighbors[count].isChild = neighbor.mIsChild;
        count++;
    }
    snapshot->neighborCount = count;

    /* Search for Border Router Prefix (OMR or default route) */
    otNetworkDataIterator netDataIter = OT_NETWORK_DATA_ITERATOR_INIT;
    otBorderRouterConfig config;
    while (otNetDataGetNextOnMeshPrefix(s_ot_instance, &netDataIter, &config) == OT_ERROR_NONE) {
        if (config.mDefaultRoute || config.mOnMesh) {
            snapshot->borderRouterFound = true;
            /* Use prefix to construct border router target IP (::1) */
            memcpy(snapshot->borderRouterIp.mFields.m8, config.mPrefix.mPrefix.mFields.m8, 8);
            snapshot->borderRouterIp.mFields.m8[15] = 0x01;
            break;
        }
    }

    snapshot->lastPingRttMs = s_last_ping_rtt;
    snapshot->pingSuccess = s_ping_success;

    openthread_api_mutex_unlock(openthread_get_default_context());
}

void thread_health_print(const thread_health_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }

    printf("\r\n======================================================================\r\n");
    printf("[THREAD HEALTH CHECKER - SuperMini nRF52840]\r\n");
    printf("State: %-16s | Channel: %2d | PAN ID: 0x%04X\r\n",
           role_to_string(snapshot->role), snapshot->channel, snapshot->panId);
    printf("Network Name: %-16s | RLOC16: 0x%04X | Partition ID: 0x%08X\r\n",
           snapshot->networkName[0] ? snapshot->networkName : "Not Commissioned",
           snapshot->rloc16, snapshot->partitionId);
    printf("----------------------------------------------------------------------\r\n");

    /* Parent Link */
    if (snapshot->hasParent) {
        printf("PARENT LINK: Avg RSSI: %3d dBm | LQI: %u/3 | Link Margin: %2d dB\r\n",
               snapshot->parentRssi, snapshot->parentLqi, snapshot->parentMargin);
        printf("----------------------------------------------------------------------\r\n");
    }

    /* Neighbors Table */
    printf("NEIGHBOR NODES (%u discovered):\r\n", snapshot->neighborCount);
    if (snapshot->neighborCount == 0) {
        printf("  (No direct 802.15.4 neighbors heard yet)\r\n");
    } else {
        printf("  # | RLOC16 | Extended MAC Address   | Avg RSSI | Last RSSI | Margin | LQI | FER\r\n");
        for (uint8_t i = 0; i < snapshot->neighborCount; i++) {
            const otExtAddress *mac = &snapshot->neighbors[i].extAddress;
            printf("  %u | 0x%04X | %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X |  %4d dBm |   %4d dBm |  %2d dB |  %u  | %u.%02u%%\r\n",
                   i + 1,
                   snapshot->neighbors[i].rloc16,
                   mac->m8[0], mac->m8[1], mac->m8[2], mac->m8[3],
                   mac->m8[4], mac->m8[5], mac->m8[6], mac->m8[7],
                   snapshot->neighbors[i].avgRssi,
                   snapshot->neighbors[i].lastRssi,
                   snapshot->neighbors[i].margin,
                   snapshot->neighbors[i].lqi,
                   snapshot->neighbors[i].frameErrorRate / 100,
                   snapshot->neighbors[i].frameErrorRate % 100);
        }
    }
    printf("----------------------------------------------------------------------\r\n");

    /* Border Router Connectivity */
    if (snapshot->borderRouterFound) {
        printf("GOOGLE NEST BORDER ROUTER: %s | Ping Latency: %u ms\r\n",
               snapshot->pingSuccess ? "ONLINE" : "NO REPLY",
               snapshot->lastPingRttMs);
    } else {
        printf("GOOGLE NEST BORDER ROUTER: Searching for OMR Prefix...\r\n");
    }
    printf("======================================================================\r\n");
}

void thread_health_ping_border_router(void)
{
    if (!s_ot_instance || s_ping_in_progress) {
        return;
    }

    openthread_api_mutex_lock(openthread_get_default_context());

    otDeviceRole role = otThreadGetDeviceRole(s_ot_instance);
    if (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER) {
        otPingSenderConfig pingConfig;
        memset(&pingConfig, 0, sizeof(pingConfig));

        /* Ping leader or discovered border router */
        const otIp6Address *leaderAddr = otThreadGetLeaderData(s_ot_instance, NULL) != NULL ?
                                         otThreadGetRloc(s_ot_instance) : NULL;

        if (leaderAddr) {
            pingConfig.mDestination = *leaderAddr;
            pingConfig.mCount = 1;
            pingConfig.mInterval = 1000;
            pingConfig.mTimeout = 1000;
            pingConfig.mReplyCallback = ping_reply_callback;
            pingConfig.mStatisticsCallback = ping_stats_callback;

            s_ping_in_progress = true;
            s_ping_success = false;
            otPingSenderPing(s_ot_instance, &pingConfig);
        }
    }

    openthread_api_mutex_unlock(openthread_get_default_context());
}

void thread_health_worker(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    thread_health_snapshot_t snapshot;

    while (1) {
        k_sleep(K_SECONDS(5));

        if (s_ot_instance) {
            thread_health_ping_border_router();
            k_sleep(K_MSEC(500));
            thread_health_collect(&snapshot);
            thread_health_print(&snapshot);
        }
    }
}
