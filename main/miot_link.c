#include "miot_link.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "miot_crypto.h"
#include "miot_spec.h"

#if __has_include("gatt_ltmk_private.h")
#include "gatt_ltmk_private.h"
#define HAVE_LTMK 1
#else
#define HAVE_LTMK 0
#endif

static const char *TAG = "miot_link";
#define MIOT_CHAR_COUNT 5
#define MIOT_NOTIFY_QUEUE_LEN 12
#define MIOT_MAX_NOTIFY 256
#define AUTH_DMTU_MAX 242
#define MIOT_MAX_CHARACTERISTICS 24

enum { CHAR_CTRL, CHAR_AUTH, CHAR_CMD, CHAR_DAT, CHAR_MCU };
static const uint16_t char_uuids[MIOT_CHAR_COUNT] = {0x0010, 0x0016, 0x001a, 0x001b, 0x001c};

struct char_info {
    uint16_t def_handle;
    uint16_t val_handle;
    uint16_t end_handle;
    uint16_t cccd_handle;
    uint8_t properties;
};

struct notification {
    uint16_t handle;
    uint16_t len;
    uint8_t data[MIOT_MAX_NOTIFY];
};

struct char_definition {
    uint16_t def_handle;
    uint16_t val_handle;
};

static struct char_info chars[MIOT_CHAR_COUNT];
static struct char_definition all_char_defs[MIOT_MAX_CHARACTERISTICS];
static size_t all_char_count;
static struct notification notify_buf;
static QueueHandle_t notify_queue;
static SemaphoreHandle_t ready_sem;
static SemaphoreHandle_t command_mutex;
static uint16_t active_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t service_end_handle;
static int dsc_index;
static int subscribe_index;
static uint8_t session_key[64];
static uint16_t tx_counter;
static uint16_t tid_counter = 0x7200;
static uint8_t auth_dmtu = 18;
static volatile bool authenticated;

static int write_encrypted_packet(const uint8_t *plain, size_t plain_len);
static int read_data_response(uint16_t expected_tid, uint8_t *plain,
                              size_t capacity, size_t *plain_len);

static void discover_next_descriptors(void);
static void subscribe_next(void);

static int char_slot(const ble_uuid_t *uuid)
{
    if (uuid == NULL || uuid->type != BLE_UUID_TYPE_16) return -1;
    uint16_t short_uuid = ble_uuid_u16(uuid);
    for (int i = 0; i < MIOT_CHAR_COUNT; ++i) {
        if (char_uuids[i] == short_uuid) return i;
    }
    return -1;
}

static int char_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error->status == 0 && chr != NULL) {
        if (all_char_count < MIOT_MAX_CHARACTERISTICS) {
            all_char_defs[all_char_count++] = (struct char_definition){
                .def_handle = chr->def_handle,
                .val_handle = chr->val_handle,
            };
        }
        int slot = char_slot(&chr->uuid.u);
        if (slot >= 0) {
            chars[slot].def_handle = chr->def_handle;
            chars[slot].val_handle = chr->val_handle;
            chars[slot].properties = chr->properties;
            ESP_LOGI(TAG, "MIoT characteristic 0x%04x discovered", char_uuids[slot]);
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        /* Characteristics arrive in handle order. Bound each descriptor search
         * by the next characteristic declaration, or the FE95 service end. */
        for (int i = 0; i < MIOT_CHAR_COUNT; ++i) {
            for (size_t j = 0; j < all_char_count; ++j) {
                if (all_char_defs[j].val_handle == chars[i].val_handle) {
                    uint16_t next_def = j + 1 < all_char_count ?
                                        all_char_defs[j + 1].def_handle : service_end_handle + 1;
                    chars[i].end_handle = next_def > chars[i].val_handle ?
                                          next_def - 1 : chars[i].val_handle;
                    break;
                }
            }
        }
        for (int i = 0; i < MIOT_CHAR_COUNT; ++i) {
            if (chars[i].val_handle == 0 || (chars[i].properties & BLE_GATT_CHR_F_NOTIFY) == 0) {
                ESP_LOGE(TAG, "required MIoT notify characteristic 0x%04x unavailable", char_uuids[i]);
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                return 0;
            }
        }
        dsc_index = 0;
        discover_next_descriptors();
        return 0;
    }
    ESP_LOGE(TAG, "MIoT characteristic discovery failed: %u", error->status);
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static int descriptor_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                                 uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                                 void *arg)
{
    (void)arg;
    if (error->status == 0 && dsc != NULL) {
        if (dsc->uuid.u.type == BLE_UUID_TYPE_16 &&
            ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16 &&
            dsc_index >= 0 && dsc_index < MIOT_CHAR_COUNT &&
            chars[dsc_index].val_handle == chr_val_handle) {
            chars[dsc_index].cccd_handle = dsc->handle;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (dsc_index < MIOT_CHAR_COUNT) ++dsc_index;
        discover_next_descriptors();
        return 0;
    }
    ESP_LOGE(TAG, "MIoT descriptor discovery failed: %u", error->status);
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static void discover_next_descriptors(void)
{
    while (dsc_index < MIOT_CHAR_COUNT && chars[dsc_index].val_handle == 0) ++dsc_index;
    if (dsc_index >= MIOT_CHAR_COUNT) {
        for (int i = 0; i < MIOT_CHAR_COUNT; ++i) {
            if (chars[i].cccd_handle == 0) {
                ESP_LOGE(TAG, "missing CCCD for MIoT characteristic 0x%04x", char_uuids[i]);
                ble_gap_terminate(active_conn, BLE_ERR_REM_USER_CONN_TERM);
                return;
            }
        }
        subscribe_index = 0;
        subscribe_next();
        return;
    }
    uint16_t start = chars[dsc_index].val_handle;
    uint16_t end = chars[dsc_index].end_handle;
    if (start > end) {
        ++dsc_index;
        discover_next_descriptors();
        return;
    }
    int rc = ble_gattc_disc_all_dscs(active_conn, start, end, descriptor_discovered, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "cannot discover MIoT descriptors: rc=%d", rc);
        ble_gap_terminate(active_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static int subscribe_written(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)arg;
    if (error->status != 0) {
        ESP_LOGE(TAG, "MIoT notify subscription failed: status=%u", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    ++subscribe_index;
    subscribe_next();
    return 0;
}

static void subscribe_next(void)
{
    while (subscribe_index < MIOT_CHAR_COUNT && chars[subscribe_index].cccd_handle == 0) ++subscribe_index;
    if (subscribe_index >= MIOT_CHAR_COUNT) {
        ESP_LOGI(TAG, "MIoT notification subscriptions active");
        xSemaphoreGive(ready_sem);
        return;
    }
    const uint8_t cccd_notify[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(active_conn, chars[subscribe_index].cccd_handle,
                                  cccd_notify, sizeof(cccd_notify), subscribe_written, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "cannot subscribe to MIoT notify characteristic: rc=%d", rc);
        ble_gap_terminate(active_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void miot_link_init(void)
{
    notify_queue = xQueueCreate(MIOT_NOTIFY_QUEUE_LEN, sizeof(struct notification));
    ready_sem = xSemaphoreCreateBinary();
    if (notify_queue == NULL || ready_sem == NULL) {
        ESP_LOGE(TAG, "cannot allocate MIoT protocol queues");
    }
}

void miot_link_begin(uint16_t conn_handle, uint16_t service_start, uint16_t service_end)
{
    (void)service_start;
    memset(chars, 0, sizeof(chars));
    memset(all_char_defs, 0, sizeof(all_char_defs));
    all_char_count = 0;
    active_conn = conn_handle;
    service_end_handle = service_end;
    while (xSemaphoreTake(ready_sem, 0) == pdTRUE) {}
    int rc = ble_gattc_disc_all_chrs(conn_handle, service_start, service_end,
                                     char_discovered, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "cannot start MIoT characteristic discovery: rc=%d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void miot_link_on_gap_event(struct ble_gap_event *event)
{
    if (event->type != BLE_GAP_EVENT_NOTIFY_RX || notify_queue == NULL) return;
    size_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
    if (len > MIOT_MAX_NOTIFY) return;
    notify_buf.handle = event->notify_rx.attr_handle;
    notify_buf.len = (uint16_t)len;
    if (os_mbuf_copydata(event->notify_rx.om, 0, len, notify_buf.data) != 0) return;
    /* Log only the auth frame class and length. Never print ephemeral keys or
     * encrypted payloads while diagnosing the handshake. */
    if (notify_buf.handle == chars[CHAR_AUTH].val_handle && len >= 4) {
        ESP_LOGI(TAG, "auth notification class=%02x%02x kind=%u id=%u len=%u",
                 notify_buf.data[0], notify_buf.data[1], notify_buf.data[2],
                 notify_buf.data[3], (unsigned)len);
    }
    if (xQueueSend(notify_queue, &notify_buf, 0) != pdTRUE) {
        ESP_LOGW(TAG, "MIoT notification queue full; dropping frame");
    }
}

void miot_link_disconnected(void)
{
    active_conn = BLE_HS_CONN_HANDLE_NONE;
    authenticated = false;
    memset(session_key, 0, sizeof(session_key));
    memset(chars, 0, sizeof(chars));
    memset(all_char_defs, 0, sizeof(all_char_defs));
    all_char_count = 0;
    if (notify_queue != NULL) xQueueReset(notify_queue);
    if (ready_sem != NULL) xSemaphoreTake(ready_sem, 0);
}

static int write_no_rsp(uint16_t handle, const uint8_t *data, size_t len)
{
    if (active_conn == BLE_HS_CONN_HANDLE_NONE || handle == 0 || len > UINT16_MAX) return BLE_HS_ENOTCONN;
    return ble_gattc_write_no_rsp_flat(active_conn, handle, data, (uint16_t)len);
}

static int pump_one(uint32_t timeout_ms, struct notification *out)
{
    return xQueueReceive(notify_queue, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? 0 : -1;
}

static void handle_auth_control(const struct notification *n)
{
    if (n->len < 4 || n->data[0] != 0 || n->data[1] != 0) return;
    const uint8_t kind = n->data[2], ident = n->data[3];
    if (kind == 4 && ident == 0 && n->len >= 6) {
        auth_dmtu = n->data[5] < AUTH_DMTU_MAX ? n->data[5] : AUTH_DMTU_MAX;
        const uint8_t response[] = {0, 0, 5, 0, 6, auth_dmtu};
        write_no_rsp(chars[CHAR_AUTH].val_handle, response, sizeof(response));
    } else if (kind == 4 && ident == 1) {
        uint8_t size = n->len >= 2 && n->len - 2 == auth_dmtu ? (uint8_t)(n->len - 2) : 18;
        if (size < 3 || size > AUTH_DMTU_MAX) size = 18;
        auth_dmtu = size;
        uint8_t response[MIOT_MAX_NOTIFY];
        response[0] = 0; response[1] = 0; response[2] = 5; response[3] = 1;
        memset(response + 4, size, size - 2);
        write_no_rsp(chars[CHAR_AUTH].val_handle, response, size + 2);
    } else if (kind == 0) {
        const uint8_t response[] = {0, 0, 1, ident, 1, 0};
        write_no_rsp(chars[CHAR_AUTH].val_handle, response, sizeof(response));
    }
}

static void pump_background(const struct notification *n)
{
    if (n->handle == chars[CHAR_AUTH].val_handle) handle_auth_control(n);
    else if (n->handle == chars[CHAR_DAT].val_handle && n->len >= 4 &&
             memcmp(n->data, "\0\0\x02\0", 4) == 0) {
        const uint8_t ack[] = {0, 0, 3, 0};
        write_no_rsp(chars[CHAR_DAT].val_handle, ack, sizeof(ack));
    }
}

static void pump_for(uint32_t duration_ms)
{
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
    while ((int32_t)(until - xTaskGetTickCount()) > 0) {
        struct notification n;
        uint32_t remaining = (uint32_t)((until - xTaskGetTickCount()) * portTICK_PERIOD_MS);
        if (pump_one(remaining > 50 ? 50 : remaining, &n) == 0) pump_background(&n);
    }
}

static int wait_matching(uint16_t handle, const uint8_t *prefix, size_t prefix_len,
                         uint32_t timeout_ms, struct notification *matched)
{
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while ((int32_t)(until - xTaskGetTickCount()) > 0) {
        struct notification n;
        uint32_t remaining = (uint32_t)((until - xTaskGetTickCount()) * portTICK_PERIOD_MS);
        if (pump_one(remaining > 100 ? 100 : remaining, &n) != 0) continue;
        if (n.handle == handle && n.len >= prefix_len && memcmp(n.data, prefix, prefix_len) == 0) {
            if (matched != NULL) *matched = n;
            return 0;
        }
        pump_background(&n);
    }
    return -1;
}

static int send_auth_package(uint8_t package_id, const uint8_t *payload, size_t len)
{
    uint16_t mtu = auth_dmtu < 3 ? 18 : auth_dmtu;
    uint16_t count = (uint16_t)((len + mtu - 1) / mtu);
    if (count == 0) count = 1;
    uint8_t header[] = {0, 0, 0, package_id, (uint8_t)count, (uint8_t)(count >> 8)};
    if (write_no_rsp(chars[CHAR_AUTH].val_handle, header, sizeof(header)) != 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(350));
    for (uint16_t seq = 0; seq < count; ++seq) {
        size_t offset = (size_t)seq * mtu;
        size_t chunk_len = len > offset ? len - offset : 0;
        if (chunk_len > mtu) chunk_len = mtu;
        uint8_t frame[MIOT_MAX_NOTIFY];
        frame[0] = (uint8_t)(seq + 1);
        frame[1] = (uint8_t)((seq + 1) >> 8);
        if (chunk_len) memcpy(frame + 2, payload + offset, chunk_len);
        if (write_no_rsp(chars[CHAR_AUTH].val_handle, frame, chunk_len + 2) != 0) return -1;
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return 0;
}

static int wait_device_public(uint8_t public_key[64])
{
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(6000);
    while ((int32_t)(until - xTaskGetTickCount()) > 0) {
        struct notification n;
        uint32_t remaining = (uint32_t)((until - xTaskGetTickCount()) * portTICK_PERIOD_MS);
        if (pump_one(remaining > 100 ? 100 : remaining, &n) != 0) continue;
        if (n.handle == chars[CHAR_AUTH].val_handle && n.len >= 68 &&
            memcmp(n.data, "\0\0\x02\x03", 4) == 0) {
            memcpy(public_key, n.data + 4, 64);
            return 0;
        }
        pump_background(&n);
    }
    return -1;
}

static int wait_login_success(void)
{
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(6000);
    while ((int32_t)(until - xTaskGetTickCount()) > 0) {
        struct notification n;
        uint32_t remaining = (uint32_t)((until - xTaskGetTickCount()) * portTICK_PERIOD_MS);
        if (pump_one(remaining > 100 ? 100 : remaining, &n) != 0) continue;
        if (n.handle == chars[CHAR_CTRL].val_handle && n.len >= 1) {
            if (n.data[0] == 0x51) return 0;
            if (n.data[0] == 0x52) return -2;
        }
        pump_background(&n);
    }
    return -1;
}

static int miot_login(void)
{
#if !HAVE_LTMK
    ESP_LOGE(TAG, "private LTMK header is missing; run tools/provision_firmware_key.py before building");
    return -1;
#else
    if (miot_crypto_init() != 0) return -1;
    const uint8_t mcu_packets[][3] = {{0x00, 0, 0}, {0x08, 0x01, 0}, {0x03, 0, 0}};
    const uint8_t mcu_lengths[] = {1, 3, 1};
    for (int i = 0; i < 3; ++i) {
        if (write_no_rsp(chars[CHAR_MCU].val_handle, mcu_packets[i], mcu_lengths[i]) != 0) return -1;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    const uint8_t start_mng = 0xa4;
    if (write_no_rsp(chars[CHAR_CTRL].val_handle, &start_mng, 1) != 0) return -1;
    pump_for(2200);

    const uint8_t request_login[] = {0x50, 0, 0, 0};
    if (write_no_rsp(chars[CHAR_CTRL].val_handle, request_login, sizeof(request_login)) != 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(200));

    uint8_t host_public[64], device_public[64];
    if (miot_crypto_make_public_key(host_public) != 0) {
        ESP_LOGE(TAG, "cannot create ephemeral ECDH key");
        return -1;
    }
    if (send_auth_package(3, host_public, sizeof(host_public)) != 0) {
        ESP_LOGE(TAG, "cannot send host ECDH key package");
        return -1;
    }
    if (wait_device_public(device_public) != 0) {
        ESP_LOGE(TAG, "timed out waiting for device ECDH key package");
        return -1;
    }
    const uint8_t auth_ack[] = {0, 0, 3, 0};
    write_no_rsp(chars[CHAR_AUTH].val_handle, auth_ack, sizeof(auth_ack));
    if (miot_crypto_derive_session(device_public, SMARTLED_GATT_LTMK, session_key) != 0) return -1;
    uint8_t proof[8];
    if (miot_crypto_make_login_proof(session_key, device_public, proof) != 0 ||
        send_auth_package(5, proof, sizeof(proof)) != 0) return -1;
    int rc = wait_login_success();
    memset(host_public, 0, sizeof(host_public));
    memset(device_public, 0, sizeof(device_public));
    memset(proof, 0, sizeof(proof));
    if (rc != 0) return rc;
    /* MIoT BLE v2 requires F0 immediately after login. Use counter 0; later
     * property requests continue from counter 1 with no nonce reuse. */
    tx_counter = 0;
    const uint8_t version_exchange[] = {0x05, 0x20, 0x01, 0x00, 0xf0};
    uint8_t version_reply[32]; size_t version_reply_len = 0;
    if (write_encrypted_packet(version_exchange, sizeof(version_exchange)) != 0 ||
        read_data_response(1, version_reply, sizeof(version_reply), &version_reply_len) != 0 ||
        version_reply_len != 5 || version_reply[2] != 1 || version_reply[3] != 0 ||
        version_reply[4] != 0xf0) {
        ESP_LOGE(TAG, "MIoT protocol version exchange failed");
        return -1;
    }
    ESP_LOGI(TAG, "MIoT BLE login succeeded");
    return 0;
#endif
}

static int write_encrypted_packet(const uint8_t *plain, size_t plain_len)
{
    const uint8_t ready[] = {0, 0, 1, 1};
    const uint8_t tx_header[] = {0, 0, 0, 0, 1, 0};
    if (write_no_rsp(chars[CHAR_CMD].val_handle, tx_header, sizeof(tx_header)) != 0 ||
        wait_matching(chars[CHAR_CMD].val_handle, ready, sizeof(ready), 1800, NULL) != 0) return -1;
    uint8_t frame[MIOT_MAX_NOTIFY];
    if (plain_len + 10 > sizeof(frame)) return -1;
    frame[0] = 1; frame[1] = 0;
    frame[2] = (uint8_t)tx_counter;
    frame[3] = (uint8_t)(tx_counter >> 8);
    size_t encrypted_len = 0;
    int rc = miot_crypto_encrypt(session_key, tx_counter, 0, plain, plain_len,
                                 frame + 4, sizeof(frame) - 4, &encrypted_len);
    if (rc != 0) return rc;
    ++tx_counter;
    return write_no_rsp(chars[CHAR_CMD].val_handle, frame, encrypted_len + 4);
}

static int read_data_response(uint16_t expected_tid, uint8_t *plain,
                              size_t capacity, size_t *plain_len)
{
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    while ((int32_t)(until - xTaskGetTickCount()) > 0) {
        struct notification n;
        uint32_t remaining = (uint32_t)((until - xTaskGetTickCount()) * portTICK_PERIOD_MS);
        if (pump_one(remaining > 100 ? 100 : remaining, &n) != 0) continue;
        if (n.handle == chars[CHAR_DAT].val_handle && n.len >= 10 &&
            memcmp(n.data, "\0\0\x02\0", 4) == 0) {
            const uint8_t ack[] = {0, 0, 3, 0};
            write_no_rsp(chars[CHAR_DAT].val_handle, ack, sizeof(ack));
            uint16_t counter = (uint16_t)n.data[4] | ((uint16_t)n.data[5] << 8);
            size_t decoded_len = 0;
            int rc = miot_crypto_decrypt(session_key, counter, n.data + 6, n.len - 6,
                                         plain, capacity, &decoded_len);
            if (rc != 0 || decoded_len < 5) continue;
            uint16_t response_tid = (uint16_t)plain[2] | ((uint16_t)plain[3] << 8);
            if (response_tid != expected_tid) continue;
            *plain_len = decoded_len;
            return 0;
        }
        pump_background(&n);
    }
    return -1;
}

static bool get_property_value(const uint8_t *packet, size_t len, uint8_t siid,
                               uint16_t piid, uint16_t *status_out,
                               uint16_t *type_out, uint32_t *value_out)
{
    if (len < 6 || packet[4] != 3 ||
        (((uint16_t)packet[0] | ((uint16_t)packet[1] << 8)) >> 12) != 2 ||
        (((uint16_t)packet[0] | ((uint16_t)packet[1] << 8)) & 0x0fff) != len) return false;
    size_t offset = 6;
    uint8_t count = packet[5];
    for (uint8_t i = 0; i < count; ++i) {
        if (offset + 5 > len) return false;
        uint8_t item_siid = packet[offset++];
        uint16_t item_piid = (uint16_t)packet[offset] | ((uint16_t)packet[offset + 1] << 8);
        offset += 2;
        uint16_t status = (uint16_t)packet[offset] | ((uint16_t)packet[offset + 1] << 8);
        offset += 2;
        uint16_t type_len = 0;
        uint32_t value = 0;
        if (status == 0) {
            if (offset + 2 > len) return false;
            type_len = (uint16_t)packet[offset] | ((uint16_t)packet[offset + 1] << 8);
            offset += 2;
            uint16_t width = type_len & 0x0fff;
            if (width > 4 || offset + width > len) return false;
            for (uint16_t b = 0; b < width; ++b) value |= (uint32_t)packet[offset + b] << (8 * b);
            offset += width;
        }
        if (item_siid == siid && item_piid == piid) {
            if (status_out) *status_out = status;
            if (type_out) *type_out = type_len >> 12;
            if (value_out) *value_out = value;
            return true;
        }
    }
    return false;
}

static int get_uint(uint16_t piid, uint32_t *value)
{
    uint8_t request[16];
    uint16_t tid = ++tid_counter;
    size_t request_len = miot_build_get_property(request, sizeof(request), tid, 2, piid);
    if (request_len == 0 || write_encrypted_packet(request, request_len) != 0) return -1;
    uint8_t response[MIOT_MAX_NOTIFY]; size_t response_len = 0;
    if (read_data_response(tid, response, sizeof(response), &response_len) != 0) return -1;
    uint16_t status = 0xffff, type = 0;
    if (response_len < 6 || response[4] != 3 ||
        ((uint16_t)response[2] | ((uint16_t)response[3] << 8)) != tid ||
        !get_property_value(response, response_len, 2, piid, &status, &type, value) || status != 0) return -1;
    ESP_LOGI(TAG, "read service 2 property %u: value=%" PRIu32 " type=%u", piid, *value, type);
    return 0;
}

static int set_uint(uint16_t piid, uint32_t value, uint8_t width)
{
    uint32_t current = 0;
    if (get_uint(piid, &current) != 0) {
        ESP_LOGE(TAG, "cannot read property before write; refusing");
        return -1;
    }
    if (current == value) {
        ESP_LOGI(TAG, "property already has requested value");
        return 0;
    }
    uint8_t request[20];
    uint16_t tid = ++tid_counter;
    size_t request_len = miot_build_set_uint(request, sizeof(request), tid, 2, piid, value, width);
    if (request_len == 0 || write_encrypted_packet(request, request_len) != 0) return -1;
    uint8_t response[MIOT_MAX_NOTIFY]; size_t response_len = 0;
    if (read_data_response(tid, response, sizeof(response), &response_len) != 0 ||
        response_len != 11 || response[4] != 1 || response[5] != 1 ||
        (((uint16_t)response[0] | ((uint16_t)response[1] << 8)) >> 12) != 2 ||
        (((uint16_t)response[0] | ((uint16_t)response[1] << 8)) & 0x0fff) != response_len ||
        ((uint16_t)response[2] | ((uint16_t)response[3] << 8)) != tid ||
        response[6] != 2 || response[7] != (uint8_t)piid || response[8] != (uint8_t)(piid >> 8) ||
        response[9] != 0 || response[10] != 0) {
        ESP_LOGE(TAG, "SetProperty was not acknowledged successfully");
        return -1;
    }
    uint32_t verified = 0;
    if (get_uint(piid, &verified) != 0 || verified != value) {
        ESP_LOGE(TAG, "property read-back did not match requested value");
        return -1;
    }
    ESP_LOGI(TAG, "property %u set and verified", piid);
    return 0;
}

static int set_power(bool on)
{
    uint32_t current;
    if (get_uint(1, &current) != 0 || current > 1) return -1;
    if (current == (uint32_t)on) return 0;
    uint8_t request[20];
    uint16_t tid = ++tid_counter;
    size_t len = miot_build_set_bool(request, sizeof(request), tid, 2, 1, on);
    if (len == 0 || write_encrypted_packet(request, len) != 0) return -1;
    uint8_t response[MIOT_MAX_NOTIFY]; size_t response_len = 0;
    if (read_data_response(tid, response, sizeof(response), &response_len) != 0 ||
        response_len != 11 || response[4] != 1 || response[5] != 1 ||
        response[6] != 2 || response[7] != 1 || response[8] != 0 ||
        response[9] != 0 || response[10] != 0) return -1;
    uint32_t verified;
    return get_uint(1, &verified) == 0 && verified == (uint32_t)on ? 0 : -1;
}

static void control_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(ready_sem, portMAX_DELAY) != pdTRUE) continue;
        ESP_LOGI(TAG, "FE95 services ready; starting MIoT login");
        if (miot_login() != 0) {
            ESP_LOGE(TAG, "MIoT login failed; closing connection without retrying writes");
            ble_gap_terminate(active_conn, BLE_ERR_REM_USER_CONN_TERM);
            continue;
        }
        authenticated = true;
        uint32_t value;
        xSemaphoreTake(command_mutex, portMAX_DELAY);
        get_uint(1, &value);
        get_uint(2, &value);
        get_uint(3, &value);
        xSemaphoreGive(command_mutex);
    }
}

int miot_link_command_state(void)
{
    miot_light_state_t state;
    return miot_link_read_state(&state);
}

int miot_link_read_state(miot_light_state_t *state)
{
    if (!state || !command_mutex || xSemaphoreTake(command_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) return -1;
    uint32_t power, brightness, temperature;
    int rc = -1;
    if (authenticated && active_conn != BLE_HS_CONN_HANDLE_NONE &&
        get_uint(1, &power) == 0 && get_uint(2, &brightness) == 0 &&
        get_uint(3, &temperature) == 0 && power <= 1 &&
        brightness >= 1 && brightness <= 100 &&
        temperature >= 2700 && temperature <= 6500) {
        *state = (miot_light_state_t){power, brightness, temperature};
        rc = 0;
    }
    xSemaphoreGive(command_mutex);
    return rc;
}

int miot_link_command_brightness(uint32_t value)
{
    if (value < 1 || value > 100 || !command_mutex || xSemaphoreTake(command_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) return -1;
    int rc = authenticated ? set_uint(2, value, 1) : -1;
    xSemaphoreGive(command_mutex);
    return rc;
}

int miot_link_command_temperature(uint32_t value)
{
    if (value < 2700 || value > 6500 || !command_mutex || xSemaphoreTake(command_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) return -1;
    int rc = authenticated ? set_uint(3, value, 2) : -1;
    xSemaphoreGive(command_mutex);
    return rc;
}

int miot_link_command_power(bool on)
{
    if (!command_mutex || xSemaphoreTake(command_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) return -1;
    int rc = authenticated ? set_power(on) : -1;
    xSemaphoreGive(command_mutex);
    return rc;
}

void miot_link_start_task(void)
{
    command_mutex = xSemaphoreCreateMutex();
    xTaskCreate(control_task, "miot_control", 8192, NULL, 5, NULL);
}
