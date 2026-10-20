/* SPDX-License-Identifier: Apache-2.0
 * ESPHome transport for the vendored Espressif diagnostic APIs and Web GUI.
 * No network mutation, commissioning, scanning or firmware update routes.
 */
#include <stdlib.h>
#include <string.h>
#include "thread_br_web.h"
#include "thread_br_srp.h"
#include "esp_br_web_api.h"
#include "esp_br_web_base.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "web_assets.h"

static httpd_handle_t server;
static char *auth_header;
static bool api_initialized;
static bool prefer_psram;
#ifdef CONFIG_THREAD_BR_GATEWAY_WEB
static thread_br_thread_gateway_web_t thread_gateway;
void thread_br_web_set_thread_gateway(const thread_br_thread_gateway_web_t *value)
{
    if (value) thread_gateway = *value;
    else memset(&thread_gateway, 0, sizeof(thread_gateway));
}
static void thread_gateway_status(httpd_req_t *req, int status)
{
    const char *text = "500 Internal Server Error";
    switch (status) {
      case 200: text = "200 OK"; break;
      case 400: text = "400 Bad Request"; break;
      case 404: text = "404 Not Found"; break;
      case 409: text = "409 Conflict"; break;
      case 503: text = "503 Service Unavailable"; break;
    }
    httpd_resp_set_status(req, text);
}

#endif

// cJSON hooks are global, so leave them untouched. Copy serialized responses
// into PSRAM using the preallocated API, with an internal-heap fallback.
static char *print_json(const cJSON *json)
{
    if (!prefer_psram) return cJSON_PrintUnformatted(json);
    for (size_t size = 512; size <= 65536; size *= 2) {
        char *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buffer) break;
        if (cJSON_PrintPreallocated((cJSON *)json, buffer, size, false)) return buffer;
        free(buffer);
    }
    return cJSON_PrintUnformatted(json);
}

static bool authorize(httpd_req_t *req)
{
    if (!auth_header || !auth_header[0]) return true;
    size_t size = httpd_req_get_hdr_value_len(req, "Authorization");
    char header[384];
    if (size > 0 && size < sizeof(header) &&
        httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK &&
        strcmp(header, auth_header) == 0) return true;
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Thread BR\"");
    httpd_resp_sendstr(req, "Authentication required");
    return false;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json, bool wrapped)
{
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Diagnostic failed");
    if (wrapped) {
        cJSON *root = cJSON_CreateObject();
        if (!root) { cJSON_Delete(json); return ESP_ERR_NO_MEM; }
        cJSON_AddNumberToObject(root, "error", 0);
        cJSON_AddItemToObject(root, "result", json);
        cJSON_AddStringToObject(root, "message", "OK");
        json = root;
    }
    // Stream one item at a time for topology results to limit peak RAM usage.
    cJSON *array = wrapped ? cJSON_GetObjectItemCaseSensitive(json, "result") : NULL;
    esp_err_t err = httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (err == ESP_OK && cJSON_IsArray(array)) {
        err = httpd_resp_sendstr_chunk(req, "{\"error\":0,\"result\":[");
        bool first = true;
        cJSON *item;
        cJSON_ArrayForEach(item, array) {
            if (err != ESP_OK) break;
            char *chunk = print_json(item);
            if (!chunk) { err = ESP_ERR_NO_MEM; break; }
            if (!first) err = httpd_resp_sendstr_chunk(req, ",");
            if (err == ESP_OK) err = httpd_resp_sendstr_chunk(req, chunk);
            cJSON_free(chunk);
            first = false;
        }
        if (err == ESP_OK) err = httpd_resp_sendstr_chunk(req, "],\"message\":\"OK\"}");
        if (err == ESP_OK) err = httpd_resp_sendstr_chunk(req, NULL);
    } else if (err == ESP_OK) {
        char *text = print_json(json);
        err = text ? httpd_resp_sendstr(req, text) : ESP_ERR_NO_MEM;
        cJSON_free(text);
    }
    cJSON_Delete(json);
    return err;
}

typedef cJSON *(*getter_t)(void);
typedef struct { const char *path; getter_t get; bool wrapped; } route_t;
static const route_t routes[] = {
    {"/get_properties", handle_openthread_network_properties_request, true},
    {"/topology", handle_ot_resource_network_diagnostics_request, true},
    {"/node_information", handle_ot_resource_node_information_request, true},
    {"/ipaddr", handle_openthread_ipaddr_list_request, true},
    {"/srp", thread_br_srp_snapshot, true},
    {"/node", handle_ot_resource_node_information_request, false},
    {"/node/state", handle_ot_resource_node_state_request, false},
    {"/node/rloc", handle_ot_resource_node_rloc_request, false},
    {"/node/rloc16", handle_ot_resource_node_rloc16_request, false},
    {"/node/ext-address", handle_ot_resource_node_extaddress_request, false},
    {"/node/network-name", handle_ot_resource_node_network_name_request, false},
    {"/node/leader-data", handle_ot_resource_node_leader_data_request, false},
    {"/node/num-of-router", handle_ot_resource_node_numofrouter_request, false},
    {"/node/ext-panid", handle_ot_resource_node_extpanid_request, false},
    {"/node/ba-id", handle_ot_resource_node_baid_request, false},
    {"/diagnostics", handle_ot_resource_network_diagnostics_request, false},
};

static esp_err_t get_handler(httpd_req_t *req)
{
    if (!authorize(req)) return ESP_OK;
    char path[96];
    size_t len = strcspn(req->uri, "?");
    if (len >= sizeof(path)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    memcpy(path, req->uri, len);
    path[len] = 0;
#ifdef CONFIG_THREAD_BR_GATEWAY_WEB
    if (!strncmp(path, "/thread_gateway/api/", sizeof("/thread_gateway/api/") - 1)) {
        if (!thread_gateway.get) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Thread gateway disabled");
        int status = 200;
        cJSON *result = thread_gateway.get(thread_gateway.context, path, &status);
        thread_gateway_status(req, status);
        return send_json(req, result, false);
    }
    if (!strcmp(path, "/thread_gateway") || !strncmp(path, "/thread_gateway/node/", sizeof("/thread_gateway/node/") - 1)) {
        if (!thread_gateway.get) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Thread gateway disabled");
        strcpy(path, "/thread_gateway.html");
    }
    if (!strcmp(path, "/thread_gateway.html") && !thread_gateway.get)
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Thread gateway disabled");
#endif
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i)
        if (strcmp(path, routes[i].path) == 0) return send_json(req, routes[i].get(), routes[i].wrapped);
    if (strcmp(path, "/node/dataset/active") == 0) {
        cJSON *request = cJSON_CreateObject(), *log = cJSON_CreateObject();
        if (!request || !log) { cJSON_Delete(request); cJSON_Delete(log); return ESP_ERR_NO_MEM; }
        cJSON_AddStringToObject(request, ESP_OT_REST_ACCEPT_HEADER, ESP_OT_REST_CONTENT_TYPE_JSON);
        cJSON_AddStringToObject(request, ESP_OT_REST_DATASET_TYPE, ESP_OT_DATASET_TYPE_ACTIVE);
        cJSON *dataset = handle_ot_resource_node_get_dataset_request(request, log);
        cJSON_Delete(request);
        cJSON_Delete(log);
        return send_json(req, dataset, false);
    }
    if (!strcmp(path, "/")) strcpy(path, "/index.html");
    for (size_t i = 0; i < sizeof(br_assets) / sizeof(br_assets[0]); ++i) {
        const br_asset_t *asset = &br_assets[i];
        if (strcmp(path, asset->path)) continue;
        httpd_resp_set_type(req, asset->type);
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        // OTA or rollback can change the page; never use stale API/UI pairs.
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, (const char *)asset->data, asset->size);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

static esp_err_t post_handler(httpd_req_t *req)
{
    if (!authorize(req)) return ESP_OK;
#ifdef CONFIG_THREAD_BR_GATEWAY_WEB
    if (!strncmp(req->uri, "/thread_gateway/api/", sizeof("/thread_gateway/api/") - 1)) {
        if (!thread_gateway.post) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Thread gateway disabled");
        if (!req->content_len || req->content_len > 8192)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request size");
        char *body = malloc(req->content_len + 1);
        if (!body) return ESP_ERR_NO_MEM;
        size_t length = 0;
        while (length < req->content_len) {
            int n = httpd_req_recv(req, body + length, req->content_len - length);
            if (n <= 0) { free(body); return ESP_FAIL; }
            length += n;
        }
        body[length] = 0;
        cJSON *request = cJSON_ParseWithLengthOpts(body, length + 1, NULL, true);
        free(body);
        if (!cJSON_IsObject(request)) {
            cJSON_Delete(request);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON object");
        }
        int status = 200;
        cJSON *result = thread_gateway.post(thread_gateway.context, req->uri, request, &status);
        cJSON_Delete(request);
        thread_gateway_status(req, status);
        return send_json(req, result, false);
    }
#endif
    if (strcmp(req->uri, "/ping")) {
        httpd_resp_set_status(req, "405 Method Not Allowed");
        return httpd_resp_sendstr(req, "Network parameters are defined by firmware");
    }
    if (req->content_len == 0 || req->content_len > 512)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request size");
    char body[513];
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return ESP_FAIL;
        received += n;
    }
    body[received] = 0;
    cJSON *request = cJSON_Parse(body);
    if (!cJSON_IsObject(request)) {
        cJSON_Delete(request);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON object");
    }
    cJSON *result = handle_openthread_ping_request(request);
    cJSON_Delete(request);
    return send_json(req, result, true);
}

esp_err_t thread_br_web_start(uint16_t port, const char *authorization, bool use_psram)
{
    if (server) return ESP_OK;
    prefer_psram = use_psram;
    auth_header = strdup(authorization);
    if (!auth_header) return ESP_ERR_NO_MEM;
    if (!api_initialized) {
        esp_br_web_api_init();
        api_initialized = true;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    // A second IDF HTTP server (e.g. ESPHome captive portal) has its own control socket.
    config.ctrl_port = 32769;
    config.stack_size = 12288;
    config.max_open_sockets = 5;
    config.max_uri_handlers = 2;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) { free(auth_header); auth_header = NULL; return err; }
    httpd_uri_t get = {.uri = "/*", .method = HTTP_GET, .handler = get_handler};
    httpd_uri_t post = {.uri = "/*", .method = HTTP_POST, .handler = post_handler};
    err = httpd_register_uri_handler(server, &get);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &post);
    if (err != ESP_OK) thread_br_web_stop();
    return err;
}

void thread_br_web_stop(void)
{
    if (server) { httpd_stop(server); server = NULL; }
    free(auth_header);
    auth_header = NULL;
}
