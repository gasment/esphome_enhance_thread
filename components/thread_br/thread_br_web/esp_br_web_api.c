/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#include "cJSON.h"
#include "esp_br_web_api.h"
#include "esp_br_web_base.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_net_stack.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "malloc.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include <assert.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "openthread/border_agent.h"
#include "openthread/border_router.h"
#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"
#include "openthread/error.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/netdata.h"
#include "openthread/ping_sender.h"
#include "openthread/server.h"
#include "openthread/thread_ftd.h"

/* TODO(IDF-5.2-compat): Remove this fallback once IDF release/v5.2 is no longer maintained. */
#ifndef ESP_RETURN_VOID_ON_FALSE
#if defined(CONFIG_COMPILER_OPTIMIZATION_CHECKS_SILENT)
#define ESP_RETURN_VOID_ON_FALSE(a, log_tag, format, ...) \
    do {                                                  \
        (void)log_tag;                                    \
        if (unlikely(!(a))) {                             \
            return;                                       \
        }                                                 \
    } while (0)
#else
#define ESP_RETURN_VOID_ON_FALSE(a, log_tag, format, ...)                                \
    do {                                                                                 \
        if (unlikely(!(a))) {                                                            \
            ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            return;                                                                      \
        }                                                                                \
    } while (0)
#endif
#endif

#define API_TAG "web_api"

/* Forward declarations for semaphores initialized in esp_br_web_api_init() */
static SemaphoreHandle_t s_diagnostic_semaphore;
static SemaphoreHandle_t s_ping_done_semaphore;
static SemaphoreHandle_t s_ping_mutex;

void esp_br_web_api_init(void)
{
    s_diagnostic_semaphore = xSemaphoreCreateMutex();
    s_ping_done_semaphore = xSemaphoreCreateBinary();
    s_ping_mutex = xSemaphoreCreateMutex();
}

static const char s_ot_state[5][10] = {"disabled", "detached", "child", "router", "leader"};

/*----------------------------------------------------------------------
                            Resource REST API
----------------------------------------------------------------------*/
cJSON *handle_ot_resource_node_rloc_request()
{
    char rloc[OT_IP6_ADDRESS_STRING_SIZE];
    esp_openthread_lock_acquire(portMAX_DELAY);
    otIp6AddressToString((const otIp6Address *)otThreadGetRloc(esp_openthread_get_instance()), rloc,
                         OT_IP6_ADDRESS_STRING_SIZE);
    esp_openthread_lock_release();
    return cJSON_CreateString(rloc);
}

cJSON *handle_ot_resource_node_rloc16_request()
{
    uint16_t rloc16 = 0xffff;
    esp_openthread_lock_acquire(portMAX_DELAY);
    rloc16 = otThreadGetRloc16(esp_openthread_get_instance());
    esp_openthread_lock_release();
    return cJSON_CreateNumber(rloc16);
}

cJSON *handle_ot_resource_node_state_request()
{
    otDeviceRole state = 0;
    esp_openthread_lock_acquire(portMAX_DELAY);
    state = otThreadGetDeviceRole(esp_openthread_get_instance());
    char state_str[10];
    strcpy(state_str, s_ot_state[state]);
    esp_openthread_lock_release();
    return cJSON_CreateString(state_str);
}

cJSON *handle_ot_resource_node_extaddress_request()
{
    char format[OT_EXT_ADDRESS_SIZE * 2 + 1];
    esp_openthread_lock_acquire(portMAX_DELAY);
    const otExtAddress *address = otLinkGetExtendedAddress(esp_openthread_get_instance());
    esp_openthread_lock_release();
    ESP_RETURN_ON_FALSE(!hex_to_string(address->m8, format, OT_EXT_ADDRESS_SIZE), NULL, API_TAG,
                        "Failed to convert thread extended address");
    return cJSON_CreateString(format);
}

cJSON *handle_ot_resource_node_network_name_request()
{
    const char *ot_network_name;
    esp_openthread_lock_acquire(portMAX_DELAY);
    ot_network_name = otThreadGetNetworkName(esp_openthread_get_instance());
    esp_openthread_lock_release();
    return cJSON_CreateString(ot_network_name);
}

cJSON *handle_ot_resource_node_leader_data_request()
{
    cJSON *root = NULL;
    otLeaderData data;
    esp_openthread_lock_acquire(portMAX_DELAY);
    if (otThreadGetLeaderData(esp_openthread_get_instance(), &data) == OT_ERROR_NONE) {
        root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "PartitionId", cJSON_CreateNumber(data.mPartitionId));
        cJSON_AddItemToObject(root, "Weighting", cJSON_CreateNumber(data.mWeighting));
        cJSON_AddItemToObject(root, "DataVersion", cJSON_CreateNumber(data.mDataVersion));
        cJSON_AddItemToObject(root, "StableDataVersion", cJSON_CreateNumber(data.mStableDataVersion));
        cJSON_AddItemToObject(root, "LeaderRouterId", cJSON_CreateNumber(data.mLeaderRouterId));
    } else {
        ESP_LOGE(API_TAG, "Failed to get thread leader data");
    }
    esp_openthread_lock_release();
    return root;
}

cJSON *handle_ot_resource_node_numofrouter_request()
{
    esp_openthread_lock_acquire(portMAX_DELAY);
    int8_t max_router_id = otThreadGetMaxRouterId(esp_openthread_get_instance());
    otRouterInfo router_info;
    uint8_t router_number = 0;
    for (uint8_t i = 0; i <= max_router_id; ++i) {
        if (otThreadGetRouterInfo(esp_openthread_get_instance(), i, &router_info) != OT_ERROR_NONE)
            continue;
        ++router_number;
    }
    esp_openthread_lock_release();
    return cJSON_CreateNumber(router_number);
}

cJSON *handle_ot_resource_node_extpanid_request()
{
    char format[OT_EXT_PAN_ID_SIZE * 2 + 1];
    esp_openthread_lock_acquire(portMAX_DELAY);
    const otExtendedPanId *extpanid = otThreadGetExtendedPanId(esp_openthread_get_instance());
    esp_openthread_lock_release();
    ESP_RETURN_ON_FALSE(!hex_to_string(extpanid->m8, format, OT_EXT_ADDRESS_SIZE), NULL, API_TAG,
                        "Failed to convert thread extended panid");
    return cJSON_CreateString(format);
}

cJSON *handle_ot_resource_node_baid_request()
{
    char format[OT_BORDER_AGENT_ID_LENGTH * 2 + 1];
    otBorderAgentId id;
    esp_openthread_lock_acquire(portMAX_DELAY);
    otError err = otBorderAgentGetId(esp_openthread_get_instance(), &id);
    esp_openthread_lock_release();
    ESP_RETURN_ON_FALSE(err == OT_ERROR_NONE, NULL, API_TAG, "Failed to get border agent id");
    ESP_RETURN_ON_FALSE(!hex_to_string(id.mId, format, OT_BORDER_AGENT_ID_LENGTH), NULL, API_TAG,
                        "Failed to convert border agent id");
    return cJSON_CreateString(format);
}

cJSON *handle_ot_resource_node_get_dataset_request(const cJSON *request, cJSON *log)
{
    uint16_t errcode = 200;
    cJSON *response = NULL;
    char format[509] = "";
    otOperationalDataset dataset;
    otOperationalDatasetTlvs datasetTlvs;
    otError ret = OT_ERROR_NONE;
    const char *accept_format =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, ESP_OT_REST_ACCEPT_HEADER));
    const char *dataset_type =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, ESP_OT_REST_DATASET_TYPE));

    if (!accept_format || !dataset_type) {
        cJSON_SetValuestring(log, "Error: Missing dataset type or accept format");
        return NULL;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ins = esp_openthread_get_instance();
    if (strcmp(accept_format, ESP_OT_REST_CONTENT_TYPE_PLAIN) == 0) {
        if (strcmp(dataset_type, ESP_OT_DATASET_TYPE_ACTIVE) == 0) {
            ERROR_EXIT(otDatasetGetActiveTlvs(ins, &datasetTlvs), exit, API_TAG,
                       "Failed to get Thread active dataset tlv");
        } else if (strcmp(dataset_type, ESP_OT_DATASET_TYPE_PENDING) == 0) {
            ERROR_EXIT(otDatasetGetPendingTlvs(ins, &datasetTlvs), exit, API_TAG,
                       "Failed to get Thread pending dataset tlv");
        } else {
            ESP_GOTO_ON_FALSE(false, OT_ERROR_FAILED, exit, API_TAG, "Invalid DatasetTlv Type");
        }
        ESP_GOTO_ON_FALSE(sizeof(format) > (datasetTlvs.mLength * 2), OT_ERROR_FAILED, exit, API_TAG, "Invalid Size");
        ESP_GOTO_ON_FALSE(hex_to_string(datasetTlvs.mTlvs, format, datasetTlvs.mLength) == ESP_OK, OT_ERROR_FAILED,
                          exit, API_TAG, "Failed to convert Thread dataset tlv");
        response = cJSON_CreateString(format);
    } else {
        if (strcmp(dataset_type, ESP_OT_DATASET_TYPE_ACTIVE) == 0) {
            ERROR_EXIT(otDatasetGetActive(ins, &dataset), exit, API_TAG, "Failed to get Thread active dataset");
            response = ActiveDataset2Json(dataset);
        } else if (strcmp(dataset_type, ESP_OT_DATASET_TYPE_PENDING) == 0) {
            ERROR_EXIT(otDatasetGetPending(ins, &dataset), exit, API_TAG, "Failed to get Thread pending dataset");
            response = PendingDataset2Json(dataset);
        } else {
            ESP_GOTO_ON_FALSE(false, OT_ERROR_FAILED, exit, API_TAG, "Invalid Dataset Type");
        }
    }
exit:
    esp_openthread_lock_release();
    if (ret != OT_ERROR_NONE) {
        errcode = 204;
    }
    cJSON_AddItemToObject(log, "ErrorCode", cJSON_CreateNumber(errcode));
    return response;
}

static esp_err_t get_openthread_ipv6_properties(otInstance *ins, thread_ipv6_status_t *ipv6)
{
    ESP_RETURN_ON_FALSE(ins && ipv6, ESP_FAIL, API_TAG, "Invalid instance or ipv6 information");
    memcpy(&ipv6->link_local_address, otThreadGetLinkLocalIp6Address(ins),
           sizeof(otIp6Address));                                                           /* 1. linkLocalAddress */
    memcpy(&ipv6->routing_local_address, otThreadGetRloc(ins), sizeof(otIp6Address));       /* 2. routingLocalAddress */
    memcpy(&ipv6->mesh_local_address, otThreadGetMeshLocalEid(ins), sizeof(otIp6Address));  /* 3. meshLocalAddress */
    memcpy(&ipv6->mesh_local_prefix, otThreadGetMeshLocalPrefix(ins), sizeof(otIp6Prefix)); /* 4. meshLocalPrefix */

    return ESP_OK;
}

static esp_err_t get_openthread_network_properties(otInstance *ins, thread_network_status_t *network)
{
    ESP_RETURN_ON_FALSE(ins && network, ESP_FAIL, API_TAG, "Invalid instance or openthread network");
    char output[64] = "";
    sprintf(output, "%s", otThreadGetNetworkName(ins));
    ESP_RETURN_ON_FALSE(strlen(output) < sizeof(otNetworkName), ESP_FAIL, API_TAG, "Network name is too long");

    memcpy(&network->name, output, strlen(output) + 1);                               /* 1. name */
    network->panid = otLinkGetPanId(ins);                                             /* 2. PANID */
    network->partition_id = otThreadGetPartitionId(ins);                              /* 3. paritionID */
    memcpy(&network->xpanid, otThreadGetExtendedPanId(ins), sizeof(otExtendedPanId)); /* 4. XPANID */
    otBorderAgentGetId(ins, &network->baid);                                          /* 5. border agent id*/
    return ESP_OK;
}

static esp_err_t get_openthread_information_properties(otInstance *ins, thread_information_status_t *ot_info)
{
    ESP_RETURN_ON_FALSE(ins && ot_info, ESP_FAIL, API_TAG, "Invalid instance or openthread information");
    ot_info->version = (char *)otGetVersionString(); /* 1. version */
    ot_info->version_api = OPENTHREAD_API_VERSION;   /* 2. version_api */
    ot_info->role = otThreadGetDeviceRole(ins);      /* 3. role */
    otThreadGetPskc(ins, &ot_info->PSKc);            /* 4. PSKc */
    return ESP_OK;
}

static esp_err_t get_openthread_rcp_properties(otInstance *ins, thread_rcp_status_t *rcp)
{
    otError error = OT_ERROR_NONE;
    ESP_RETURN_ON_FALSE(ins && rcp, ESP_FAIL, API_TAG, "Invalid instance or openthread rcp");
    rcp->channel = otLinkGetChannel(ins);                    /* 1. channel */
    error = otPlatRadioGetTransmitPower(ins, &rcp->txpower); /* 2. txPower */
    otLinkGetFactoryAssignedIeeeEui64(ins, &rcp->EUI64);     /* 3. eui */
    rcp->version = (char *)otPlatRadioGetVersionString(ins); /* 4. rcp Version */

    return !error ? ESP_OK : ESP_FAIL;
}

static esp_err_t get_openthread_wpan_properties(otInstance *ins, thread_wpan_status_t *wpan)
{
    ESP_RETURN_ON_FALSE(ins && wpan, ESP_FAIL, API_TAG, "Invalid instance or openthread wpan");
    const char *role = otThreadDeviceRoleToString(otThreadGetDeviceRole(ins));
    ESP_RETURN_ON_FALSE(ins && role, ESP_FAIL, API_TAG, "Failed to get thread device role");
    if (!strcmp(role, "disabled")) {
        memcpy(wpan->service, WPAN_STATUS_OFFLINE, sizeof(WPAN_STATUS_OFFLINE));
    } else if (!strcmp(role, "detached")) {
        memcpy(wpan->service, WPAN_STATUS_ASSOCIATING, sizeof(WPAN_STATUS_ASSOCIATING));
    } else {
        memcpy(wpan->service, WPAN_STATUS_ASSOCIATED, sizeof(WPAN_STATUS_ASSOCIATED));
    }

    return ESP_OK;
}

static esp_err_t get_openthread_properties(openthread_properties_t *properties)
{
    esp_err_t ret = ESP_OK;
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ins = esp_openthread_get_instance(); /* get the api of openthread */
    ESP_GOTO_ON_FALSE(ins, ESP_FAIL, exit, API_TAG, "Failed to get openthread instance");

    /* get openthread border router information */
    ESP_GOTO_ON_ERROR(get_openthread_ipv6_properties(ins, &properties->ipv6), exit, API_TAG,
                      "Failed to get status of ipv6"); /* ipv6 */
    ESP_GOTO_ON_ERROR(get_openthread_network_properties(ins, &properties->network), exit, API_TAG,
                      "Failed to get status of network"); /* network */
    ESP_GOTO_ON_ERROR(get_openthread_information_properties(ins, &properties->information), exit, API_TAG,
                      "Failed to get status of information"); /* ot information */
    ESP_GOTO_ON_ERROR(get_openthread_rcp_properties(ins, &properties->rcp), exit, API_TAG,
                      "Failed to get status of rcp"); /* rcp */
    ESP_GOTO_ON_ERROR(get_openthread_wpan_properties(ins, &properties->wpan), exit, API_TAG,
                      "Failed to get status of wpan"); /* wpan */

exit:
    esp_openthread_lock_release();
    return ret;
}

cJSON *handle_openthread_network_properties_request()
{
    otError ret = OT_ERROR_NONE;
    cJSON *root = NULL;
    openthread_properties_t properties;
    otbr_properties_reset(&properties);
    ESP_GOTO_ON_FALSE(!get_openthread_properties(&properties), OT_ERROR_FAILED, exit, API_TAG,
                      "Failed to get openthread status");
    root = otbr_properties_struct_convert2_json(&properties);
exit:
    return ret ? NULL : root;
}

/*----------------------------------------------------------------------
               scan thread available networks
----------------------------------------------------------------------*/
static thread_diagnosticTlv_set_t *s_diagnosticTlv_set = NULL;
/* s_diagnostic_semaphore is initialized in esp_br_web_api_init() */
/* Only request TLV types the topology page actually uses:
 *   0=ExtAddress, 1=Address16, 2=Mode, 5=Route, 6=LeaderData,
 *   8=IPv6AddressList, 16=ChildTable */
static const uint8_t kAllTlvTypes[] = {0, 1, 2, 5, 6, 8, 16};
static const char *kMulticastAddrAllRouters = "ff03::2";
static volatile bool s_diag_collecting = false;           /* true while actively collecting responses */
static volatile int s_diag_response_count = 0;            /* number of responses received this collection */
static volatile TickType_t s_diag_last_response_tick = 0; /* tick of most recent response */
#define DIAG_QUIET_PERIOD_MS 3000                         /* stop after no new response for 3s */
#define DIAG_MIN_WAIT_MS 2000                             /* always wait at least 2s */
#define DIAG_MAX_TIMEOUT_MS 30000                         /* hard cap to avoid blocking forever */
#define DIAG_POLL_INTERVAL_MS 500                         /* polling interval */

/**
 * @brief Update the diagnostic Tlv set with @param key and @param diag_list
 *
 */
static void update_diagnosticTlv(char *key, thread_diagnosticTlv_list_t *diag_list)
{
    thread_diagnosticTlv_list_t *head = diag_list->next; /* skip the first invalid node. */
    free(diag_list->diagTlv);                            /* avoid to lack memory, free the first invalid node. */
    free(diag_list);
    update_thread_diagnosticTlv_set(s_diagnosticTlv_set, key, head);
}

/**
 * @brief Get the result of Thread diagnostic for Thread's topology and update the diagnostic set.
 *
 * @param[in]  aError        The error when failed to get the response.
 * @param[in]  aMessage      A pointer to the message buffer containing the received Network Diagnostic
 *                           Get response payload. Available only when @param aError is `OT_ERROR_NONE`.
 * @param[in]  aMessageInfo  A pointer to the message info for @param aMessage. Available only when
 *                           @param aError is `OT_ERROR_NONE`.
 */
static void get_diagnosticTlv_information(otError aError, const otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    thread_diagnosticTlv_list_t *diag_list = (thread_diagnosticTlv_list_t *)malloc(sizeof(thread_diagnosticTlv_list_t));
    if (diag_list == NULL) {
        ESP_LOGW(API_TAG, "Diagnostic: out of memory, skipping node");
        return;
    }
    otNetworkDiagTlv diagTlv;
    otNetworkDiagIterator iterator = OT_NETWORK_DIAGNOSTIC_ITERATOR_INIT;
    otError error;
    char rloc[7];
    char keyRloc[7] = "0xffee";
    initialize_thread_diagnosticTlv_list(diag_list);
    while ((error = otThreadGetNextDiagnosticTlv(aMessage, &iterator, &diagTlv)) == OT_ERROR_NONE) {
        if (diagTlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_SHORT_ADDRESS) {
            sprintf(rloc, "0x%04x", diagTlv.mData.mAddr16);
            strcpy(keyRloc, rloc);
        }
        append_thread_diagnosticTlv_list(diag_list, diagTlv);
    }
    update_diagnosticTlv(keyRloc, diag_list);
}

/**
 * @brief A callback for `otThreadSendDiagnosticGet()` to form the diagnostic set.
 *
 * @param[in]  aError        The error when failed to get the response.
 * @param[in]  aMessage      A pointer to the message buffer containing the received Network Diagnostic
 *                           Get response payload. Available only when @p aError is `OT_ERROR_NONE`.
 * @param[in]  aMessageInfo  A pointer to the message info for @p aMessage. Available only when
 *                           @p aError is `OT_ERROR_NONE`.
 * @param[in]  aContext      A pointer to application-specific context.
 *
 */
#define DIAG_MIN_FREE_HEAP 20000 /* bytes – stop collecting before starving OpenThread */

static void diagnosticTlv_result_handler(otError aError, otMessage *aMessage, const otMessageInfo *aMessageInfo,
                                         void *aContext)
{
    if (aError == OT_ERROR_NONE) {
        /* Skip child nodes — the topology only needs router data.
           Children are already represented in their parent router's ChildTable TLV.
           Diagnostic responses are sent from RLOC addresses where the
           lower 10 bits encode the child ID (0 for routers).

           Note: the RLOC16 filtering and heap check below happen outside the
           s_diagnostic_semaphore, creating a small TOCTOU window with
           s_diag_collecting. This is acceptable on ESP32 where the OT task
           is pinned to a single core and callback ordering is deterministic. */
        const uint8_t *src = aMessageInfo->mPeerAddr.mFields.m8;
        uint16_t rloc16 = ((uint16_t)src[14] << 8) | src[15];
        if ((rloc16 & 0x03FF) != 0) {
            return;
        }

        if (esp_get_free_heap_size() < DIAG_MIN_FREE_HEAP) {
            ESP_LOGW(API_TAG, "Diagnostic callback: low heap (%lu), dropping result",
                     (unsigned long)esp_get_free_heap_size());
            return;
        }
        if (xSemaphoreTake(s_diagnostic_semaphore, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (s_diag_collecting) {
                get_diagnosticTlv_information(aError, aMessage, aMessageInfo);
                s_diag_response_count++;
                s_diag_last_response_tick = xTaskGetTickCount();
            }
            xSemaphoreGive(s_diagnostic_semaphore);
        } else {
            ESP_LOGW(API_TAG, "Diagnostic callback: failed to acquire semaphore, dropping result");
        }
    }
}

/**
 * @brief the function will send diagnostic to get network's topology message and update the set of diagnostic.
 *
 */
static esp_err_t build_thread_network_topology(void)
{
    esp_err_t ret = ESP_OK;
    otInstance *ins = esp_openthread_get_instance();
    esp_openthread_lock_acquire(portMAX_DELAY);
    otIp6Address rloc16address = *otThreadGetRloc(ins);
    otIp6Address multicastAddress;
    ESP_GOTO_ON_FALSE(otThreadSendDiagnosticGet(ins, &rloc16address, kAllTlvTypes, sizeof(kAllTlvTypes),
                                                &diagnosticTlv_result_handler, NULL) == OT_ERROR_NONE,
                      ESP_FAIL, exit, API_TAG, "Fail to send diagnostic rloc16address.");
    ESP_GOTO_ON_FALSE(otIp6AddressFromString(kMulticastAddrAllRouters, &multicastAddress) == OT_ERROR_NONE, ESP_FAIL,
                      exit, API_TAG, "Fail to convert ipv6 to string.");
    ESP_GOTO_ON_FALSE(otThreadSendDiagnosticGet(ins, &multicastAddress, kAllTlvTypes, sizeof(kAllTlvTypes),
                                                &diagnosticTlv_result_handler, NULL) == OT_ERROR_NONE,
                      ESP_FAIL, exit, API_TAG, "Fail to send diagnostic multicastAddress.");
exit:
    esp_openthread_lock_release();
    return ret;
}

static thread_node_information_t get_openthread_node_information(otInstance *ins)
{
    thread_node_information_t node;
    thread_node_information_reset(&node);
    if (otThreadGetLeaderData(ins, &node.leader_data))
        ESP_LOGW(API_TAG, "Fail to get node's leader data.");

    node.role = otThreadGetDeviceRole(ins);
    node.rloc16 = otThreadGetRloc16(ins);
    memcpy(&node.extended_address, otLinkGetExtendedAddress(ins), OT_EXT_ADDRESS_SIZE);
    memcpy(&node.network_name, otThreadGetNetworkName(ins), OT_NETWORK_NAME_MAX_SIZE);
    memcpy(&node.extended_panid, otThreadGetExtendedPanId(ins), OT_EXT_PAN_ID_SIZE);
    memcpy(node.rloc_address.mFields.m8, otThreadGetRloc(ins), OT_IP6_ADDRESS_SIZE);

    uint8_t maxRouterId = otThreadGetMaxRouterId(ins);
    otRouterInfo router_info;
    node.router_number = 0;
    for (uint8_t i = 0; i <= maxRouterId; ++i) {
        if (otThreadGetRouterInfo(ins, i, &router_info) != OT_ERROR_NONE)
            continue;
        ++node.router_number;
    }
    return node;
}

cJSON *handle_ot_resource_node_information_request()
{
    esp_openthread_lock_acquire(portMAX_DELAY);
    thread_node_information_t node = get_openthread_node_information(esp_openthread_get_instance());
    esp_openthread_lock_release();
    return thread_node_struct_convert2_json(&node);
}

cJSON *handle_ot_resource_network_diagnostics_request()
{
    /* Stop accepting any late callbacks from a previous collection */
    xSemaphoreTake(s_diagnostic_semaphore, portMAX_DELAY);
    s_diag_collecting = false;

    destroy_thread_diagnosticTlv_set(s_diagnosticTlv_set);
    s_diagnosticTlv_set = NULL;
    s_diagnosticTlv_set = (thread_diagnosticTlv_set_t *)malloc(sizeof(thread_diagnosticTlv_set_t));
    initialize_thread_diagnosticTlv_set(s_diagnosticTlv_set, OPENTHREAD_INVALID_RLOC16);

    s_diag_response_count = 0;
    s_diag_last_response_tick = xTaskGetTickCount();
    s_diag_collecting = true;
    xSemaphoreGive(s_diagnostic_semaphore);

    build_thread_network_topology();

    /* Get the expected router count as a minimum threshold */
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *ins = esp_openthread_get_instance();
    uint8_t maxRouterId = otThreadGetMaxRouterId(ins);
    otRouterInfo routerInfo;
    int expected_routers = 0;
    for (uint8_t i = 0; i <= maxRouterId; i++) {
        if (otThreadGetRouterInfo(ins, i, &routerInfo) == OT_ERROR_NONE)
            expected_routers++;
    }
    esp_openthread_lock_release();

    /* Wait for responses:
     * - At least expected_routers responses received
     * - AND no new response for DIAG_QUIET_PERIOD_MS (responses done)
     * - OR hard timeout reached
     *
     * Compare in ticks (not ms) to avoid overflow when multiplying
     * TickType_t by portTICK_PERIOD_MS on systems with large tick counts. */
    const TickType_t max_timeout_ticks = pdMS_TO_TICKS(DIAG_MAX_TIMEOUT_MS);
    const TickType_t min_wait_ticks = pdMS_TO_TICKS(DIAG_MIN_WAIT_MS);
    const TickType_t quiet_ticks = pdMS_TO_TICKS(DIAG_QUIET_PERIOD_MS);
    const TickType_t poll_ticks = pdMS_TO_TICKS(DIAG_POLL_INTERVAL_MS);
    TickType_t start = xTaskGetTickCount();
    while (1) {
        vTaskDelay(poll_ticks);
        TickType_t now = xTaskGetTickCount();
        TickType_t elapsed = now - start;
        TickType_t quiet = now - s_diag_last_response_tick;

        if (elapsed >= max_timeout_ticks) {
            ESP_LOGW(API_TAG, "Diagnostic collection: max timeout reached (%d responses)", s_diag_response_count);
            break;
        }
        if (elapsed < min_wait_ticks) {
            continue;
        }
        if (s_diag_response_count >= expected_routers && quiet >= quiet_ticks) {
            ESP_LOGI(API_TAG, "Diagnostic collection complete: %d responses from %d routers", s_diag_response_count,
                     expected_routers);
            break;
        }
    }

    /* Stop accepting new responses and serialize the result.
       Free the diagnostic set immediately after conversion so the
       JSON string serialization in the HTTP handler has more heap. */
    xSemaphoreTake(s_diagnostic_semaphore, portMAX_DELAY);
    s_diag_collecting = false;
    cJSON *result = diagnosticTlv_set_convert2_json(s_diagnosticTlv_set);
    destroy_thread_diagnosticTlv_set(s_diagnosticTlv_set);
    s_diagnosticTlv_set = NULL;
    xSemaphoreGive(s_diagnostic_semaphore);

    return result;
}

/*----------------------------------------------------------------------
+                       Set Thread dataset
+-----------------------------------------------------------------------*/
#define PING_MAX_COUNT 10
#define PING_DEFAULT_COUNT 4
#define PING_DEFAULT_SIZE 32
#define PING_TIMEOUT_MS 8000

typedef struct {
    char sender[OT_IP6_ADDRESS_STRING_SIZE];
    uint16_t rtt;
    uint16_t size;
    uint16_t seq;
    uint8_t hop_limit;
} ping_reply_entry_t;

static ping_reply_entry_t s_ping_replies[PING_MAX_COUNT];
static uint16_t s_ping_reply_count = 0;
static otPingSenderStatistics s_ping_statistics;
/* s_ping_done_semaphore is initialized in esp_br_web_api_init() */

static void ping_reply_callback(const otPingSenderReply *aReply, void *aContext)
{
    if (s_ping_reply_count < PING_MAX_COUNT) {
        ping_reply_entry_t *entry = &s_ping_replies[s_ping_reply_count];
        otIp6AddressToString(&aReply->mSenderAddress, entry->sender, sizeof(entry->sender));
        entry->rtt = aReply->mRoundTripTime;
        entry->size = aReply->mSize;
        entry->seq = aReply->mSequenceNumber;
        entry->hop_limit = aReply->mHopLimit;
        s_ping_reply_count++;
    }
}

static void ping_statistics_callback(const otPingSenderStatistics *aStatistics, void *aContext)
{
    memcpy(&s_ping_statistics, aStatistics, sizeof(otPingSenderStatistics));
    if (s_ping_done_semaphore) {
        xSemaphoreGive(s_ping_done_semaphore);
    }
}

cJSON *handle_openthread_ping_request(const cJSON *request)
{
    ESP_RETURN_ON_FALSE(request, NULL, API_TAG, "Invalid ping request");

    /* Only one ping at a time — return busy if another is in progress */
    if (xSemaphoreTake(s_ping_mutex, 0) != pdTRUE) {
        ESP_LOGW(API_TAG, "Ping already in progress");
        return NULL;
    }

    cJSON *root = NULL;
    cJSON *addr_item = cJSON_GetObjectItemCaseSensitive(request, "address");
    if (!cJSON_IsString(addr_item) || !addr_item->valuestring) {
        ESP_LOGE(API_TAG, "Missing or invalid 'address' field");
        goto ping_exit;
    }

    cJSON *count_item = cJSON_GetObjectItemCaseSensitive(request, "count");
    uint16_t count = (cJSON_IsNumber(count_item) && count_item->valuedouble > 0) ? (uint16_t)count_item->valuedouble
                                                                                 : PING_DEFAULT_COUNT;
    if (count > PING_MAX_COUNT) {
        count = PING_MAX_COUNT;
    }

    cJSON *size_item = cJSON_GetObjectItemCaseSensitive(request, "size");
    uint16_t size = (cJSON_IsNumber(size_item) && size_item->valuedouble > 0) ? (uint16_t)size_item->valuedouble
                                                                              : PING_DEFAULT_SIZE;

    /* Reset state */
    s_ping_reply_count = 0;
    memset(s_ping_replies, 0, sizeof(s_ping_replies));
    memset(&s_ping_statistics, 0, sizeof(s_ping_statistics));

    /* Configure and start ping */
    otPingSenderConfig config;
    memset(&config, 0, sizeof(config));

    otError err = otIp6AddressFromString(addr_item->valuestring, &config.mDestination);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(API_TAG, "Failed to parse IPv6 address: %s", addr_item->valuestring);
        goto ping_exit;
    }

    config.mReplyCallback = ping_reply_callback;
    config.mStatisticsCallback = ping_statistics_callback;
    config.mCallbackContext = NULL;
    config.mSize = size;
    config.mCount = count;
    config.mInterval = 1000; /* 1 second between pings */
    config.mTimeout = 3000;  /* 3 second timeout for final reply */
    config.mHopLimit = 0;    /* default */
    config.mAllowZeroHopLimit = false;
    config.mMulticastLoop = false;

    esp_openthread_lock_acquire(portMAX_DELAY);
    err = otPingSenderPing(esp_openthread_get_instance(), &config);
    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE) {
        ESP_LOGE(API_TAG, "Failed to start ping: %d", err);
        goto ping_exit;
    }

    /* Wait for ping to complete. Timeout: count * interval + extra timeout + margin */
    TickType_t wait_ticks = pdMS_TO_TICKS((count * config.mInterval) + config.mTimeout + 1000);
    if (xSemaphoreTake(s_ping_done_semaphore, wait_ticks) != pdTRUE) {
        ESP_LOGW(API_TAG, "Ping timed out waiting for statistics callback");
        /* Stop any in-progress ping */
        esp_openthread_lock_acquire(portMAX_DELAY);
        otPingSenderStop(esp_openthread_get_instance());
        esp_openthread_lock_release();
    }

    /* Build JSON response */
    root = cJSON_CreateObject();

    /* Replies array */
    cJSON *replies = cJSON_CreateArray();
    for (uint16_t i = 0; i < s_ping_reply_count; i++) {
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "from", s_ping_replies[i].sender);
        cJSON_AddNumberToObject(reply, "seq", s_ping_replies[i].seq);
        cJSON_AddNumberToObject(reply, "size", s_ping_replies[i].size);
        cJSON_AddNumberToObject(reply, "rtt", s_ping_replies[i].rtt);
        cJSON_AddNumberToObject(reply, "hlim", s_ping_replies[i].hop_limit);
        cJSON_AddItemToArray(replies, reply);
    }
    cJSON_AddItemToObject(root, "replies", replies);

    /* Statistics */
    cJSON *stats = cJSON_CreateObject();
    cJSON_AddNumberToObject(stats, "sent", s_ping_statistics.mSentCount);
    cJSON_AddNumberToObject(stats, "received", s_ping_statistics.mReceivedCount);
    uint16_t lost = s_ping_statistics.mSentCount > s_ping_statistics.mReceivedCount
        ? s_ping_statistics.mSentCount - s_ping_statistics.mReceivedCount
        : 0;
    double loss_pct = s_ping_statistics.mSentCount > 0 ? (double)lost / s_ping_statistics.mSentCount * 100.0 : 0.0;
    cJSON_AddNumberToObject(stats, "loss_percent", loss_pct);
    if (s_ping_statistics.mReceivedCount > 0) {
        uint32_t avg_rtt = s_ping_statistics.mTotalRoundTripTime / s_ping_statistics.mReceivedCount;
        cJSON_AddNumberToObject(stats, "rtt_min", s_ping_statistics.mMinRoundTripTime);
        cJSON_AddNumberToObject(stats, "rtt_avg", avg_rtt);
        cJSON_AddNumberToObject(stats, "rtt_max", s_ping_statistics.mMaxRoundTripTime);
    }
    cJSON_AddItemToObject(root, "statistics", stats);

ping_exit:
    xSemaphoreGive(s_ping_mutex);
    return root;
}

/*----------------------------------------------------------------------
                       IPv6 Address Management
-----------------------------------------------------------------------*/
cJSON *handle_openthread_ipaddr_list_request(void)
{
    cJSON *arr = cJSON_CreateArray();
    char addr_str[OT_IP6_ADDRESS_STRING_SIZE];

    esp_openthread_lock_acquire(portMAX_DELAY);
    const otNetifAddress *addr = otIp6GetUnicastAddresses(esp_openthread_get_instance());
    while (addr) {
        cJSON *entry = cJSON_CreateObject();
        otIp6AddressToString(&addr->mAddress, addr_str, sizeof(addr_str));
        cJSON_AddStringToObject(entry, "address", addr_str);
        cJSON_AddNumberToObject(entry, "prefixLength", addr->mPrefixLength);
        const char *origin;
        switch (addr->mAddressOrigin) {
        case OT_ADDRESS_ORIGIN_THREAD:
            origin = "thread";
            break;
        case OT_ADDRESS_ORIGIN_SLAAC:
            origin = "slaac";
            break;
        case OT_ADDRESS_ORIGIN_DHCPV6:
            origin = "dhcpv6";
            break;
        case OT_ADDRESS_ORIGIN_MANUAL:
            origin = "manual";
            break;
        default:
            origin = "unknown";
            break;
        }
        cJSON_AddStringToObject(entry, "origin", origin);
        cJSON_AddBoolToObject(entry, "preferred", addr->mPreferred);
        cJSON_AddBoolToObject(entry, "meshLocal", addr->mMeshLocal);
        cJSON_AddBoolToObject(entry, "rloc", addr->mRloc);
        cJSON_AddItemToArray(arr, entry);
        addr = addr->mNext;
    }
    esp_openthread_lock_release();
    return arr;
}

