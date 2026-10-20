#include "thread_br_srp.h"
#include <stdbool.h>
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/ip6.h"
#include "openthread/srp_server.h"

cJSON *thread_br_srp_snapshot(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(100))) {
        cJSON_Delete(root);
        return NULL;
    }
    otInstance *instance = esp_openthread_get_instance();
    if (!instance) goto failed;
    otSrpServerState state = otSrpServerGetState(instance);
    const char *name = state == OT_SRP_SERVER_STATE_RUNNING ? "running" :
                       state == OT_SRP_SERVER_STATE_STOPPED ? "stopped" : "disabled";
    if (!cJSON_AddStringToObject(root, "state", name) ||
        !cJSON_AddStringToObject(root, "domain", otSrpServerGetDomain(instance)) ||
        !cJSON_AddNumberToObject(root, "port", otSrpServerGetPort(instance))) goto failed;
    cJSON *hosts = cJSON_AddArrayToObject(root, "hosts");
    if (!hosts) goto failed;
    const otSrpServerHost *host = NULL;
    unsigned host_count = 0;
    bool truncated = false;
    while ((host = otSrpServerGetNextHost(instance, host)) != NULL) {
        if (++host_count > 64) { truncated = true; break; }
        cJSON *entry = cJSON_CreateObject();
        if (!entry) goto failed;
        cJSON_AddItemToArray(hosts, entry);
        if (!cJSON_AddStringToObject(entry, "name", otSrpServerHostGetFullName(host)) ||
            !cJSON_AddBoolToObject(entry, "deleted", otSrpServerHostIsDeleted(host))) goto failed;
        cJSON *addresses = cJSON_AddArrayToObject(entry, "addresses");
        cJSON *services = cJSON_AddArrayToObject(entry, "services");
        if (!addresses || !services) goto failed;
        uint8_t address_count = 0;
        const otIp6Address *ips = otSrpServerHostGetAddresses(host, &address_count);
        for (unsigned i = 0; i < address_count && i < 16; ++i) {
            char address[OT_IP6_ADDRESS_STRING_SIZE];
            otIp6AddressToString(&ips[i], address, sizeof(address));
            cJSON *value = cJSON_CreateString(address);
            if (!value) goto failed;
            cJSON_AddItemToArray(addresses, value);
        }
        if (address_count > 16) truncated = true;
        const otSrpServerService *service = NULL;
        unsigned service_count = 0;
        while ((service = otSrpServerHostGetNextService(host, service)) != NULL) {
            if (++service_count > 32) { truncated = true; break; }
            cJSON *item = cJSON_CreateObject();
            if (!item) goto failed;
            cJSON_AddItemToArray(services, item);
            if (!cJSON_AddStringToObject(item, "instance", otSrpServerServiceGetInstanceName(service)) ||
                !cJSON_AddStringToObject(item, "type", otSrpServerServiceGetServiceName(service)) ||
                !cJSON_AddNumberToObject(item, "port", otSrpServerServiceGetPort(service)) ||
                !cJSON_AddBoolToObject(item, "deleted", otSrpServerServiceIsDeleted(service))) goto failed;
        }
    }
    if (!cJSON_AddBoolToObject(root, "truncated", truncated)) goto failed;
    esp_openthread_lock_release();
    return root;
failed:
    esp_openthread_lock_release();
    cJSON_Delete(root);
    return NULL;
}
