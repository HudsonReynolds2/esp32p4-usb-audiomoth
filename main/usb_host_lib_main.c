// uac_probe.c
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"

#include "usb/usb_host.h"
#include "usb/usb_types_stack.h"
#include "usb/usb_helpers.h"
#include "usb/usb_types_ch9.h"

static const char *TAG = "UAC_PROBE";

static usb_host_client_handle_t g_client;
static usb_device_handle_t      g_dev;

static SemaphoreHandle_t ctrl_sem;

// ================== Daemon task (handles library events) ==================
static void daemon_task(void *arg)
{
    while (1) {
        uint32_t flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "usb_host_lib_handle_events: %s", esp_err_to_name(err));
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            // No clients left
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            // All devices freed, safe to uninstall if desired
        }
    }
}

// ================== Control xfer completion ==================
static void ctrl_cb(usb_transfer_t *xfer)
{
    xSemaphoreGiveFromISR(ctrl_sem, NULL);
}

// SET_INTERFACE via control transfer
static esp_err_t ctrl_set_interface(uint8_t intf, uint8_t alt)
{
    usb_transfer_t *xfer;
    ESP_RETURN_ON_ERROR(usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &xfer),
                        TAG, "alloc ctrl");

    usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;
    USB_SETUP_PACKET_INIT_SET_INTERFACE(setup, intf, alt);

    xfer->device_handle     = g_dev;
    xfer->bEndpointAddress  = 0; // EP0
    xfer->num_bytes         = sizeof(*setup);
    xfer->callback          = ctrl_cb;
    xfer->context           = NULL;

    // Drain any leftover give
    while (xSemaphoreTake(ctrl_sem, 0) == pdTRUE) { }

    ESP_RETURN_ON_ERROR(usb_host_transfer_submit_control(g_client, xfer),
                        TAG, "submit ctrl");

    // Wait for completion, pumping events
    while (xSemaphoreTake(ctrl_sem, 10 / portTICK_PERIOD_MS) != pdTRUE) {
        usb_host_client_handle_events(g_client, 10 / portTICK_PERIOD_MS);
    }

    esp_err_t ret = ESP_OK;
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGE(TAG, "SET_INTERFACE failed, status=%d", xfer->status);
        ret = ESP_FAIL;
    }

    usb_host_transfer_free(xfer);
    return ret;
}

// ================== ISO IN callback ==================
static void isoc_in_cb(usb_transfer_t *t)
{
    const int mps = (int)(intptr_t)t->context;
    size_t off = 0;

    static uint64_t pkt_cnt = 0;
    static uint64_t byte_cnt = 0;
    static int64_t  last_log_us = 0;
    const int64_t now_us = esp_timer_get_time();
    int16_t first_sample_snapshot = 0;

    for (int i = 0; i < t->num_isoc_packets; i++) {
        const usb_isoc_packet_desc_t *d = &t->isoc_packet_desc[i];
        if (d->status == USB_TRANSFER_STATUS_COMPLETED && d->actual_num_bytes) {
            const int16_t *pcm = (const int16_t *)(t->data_buffer + off);
            first_sample_snapshot = pcm[0];
            pkt_cnt++;
            byte_cnt += d->actual_num_bytes;
        }
        off += mps;
    }

    // Log every 500 ms
    if (now_us - last_log_us > 500000) {
        float kbps = (byte_cnt * 8.0f) / ((now_us - last_log_us) / 1000.0f); // kb/s
        ESP_LOGI(TAG, "pkts=%llu bytes=%llu ~%.1f kbps first_sample=%d",
                 (unsigned long long)pkt_cnt,
                 (unsigned long long)byte_cnt,
                 kbps,
                 first_sample_snapshot);
        pkt_cnt = 0;
        byte_cnt = 0;
        last_log_us = now_us;
    }

    // Re-submit
    esp_err_t err = usb_host_transfer_submit(t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ISO resubmit failed: %s", esp_err_to_name(err));
        usb_host_transfer_free(t);
    }
}

static esp_err_t start_isoc_stream(uint8_t ep_addr, int mps)
{
    const int ISO_PKTS = 8; // 8 ms worth per URB
    size_t buf_size = mps * ISO_PKTS;

    usb_transfer_t *xfer;
    ESP_RETURN_ON_ERROR(usb_host_transfer_alloc(buf_size, ISO_PKTS, &xfer),
                        TAG, "alloc iso");

    xfer->device_handle    = g_dev;
    xfer->bEndpointAddress = ep_addr;
    xfer->callback         = isoc_in_cb;
    xfer->context          = (void*)(intptr_t)mps;
    xfer->num_bytes        = buf_size;     // <-- critical: must equal sum of per-packet sizes

    for (int i = 0; i < ISO_PKTS; i++) {
        xfer->isoc_packet_desc[i].num_bytes = mps;
    }

    return usb_host_transfer_submit(xfer);
}

// ================== Client event callback ==================
static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV: {
        ESP_LOGI(TAG, "NEW_DEV addr=%d", event_msg->new_dev.address);

        ESP_ERROR_CHECK(usb_host_device_open(g_client, event_msg->new_dev.address, &g_dev));

        // From your dump: IF=1 ALT=1, EP=0x82 (ISO IN), MPS=96
        ESP_ERROR_CHECK(usb_host_interface_claim(g_client, g_dev, 1, 1));
        ESP_ERROR_CHECK(ctrl_set_interface(1, 1));

        esp_err_t err = start_isoc_stream(0x82, 96);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "start_isoc_stream failed: %s", esp_err_to_name(err));
        }
        break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGW(TAG, "DEV_GONE");
        // TODO: stop ISO, release IF, close device
        break;
    default:
        break;
    }
}

// ================== Client task ==================
static void client_task(void *arg)
{
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 16,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cfg, &g_client));

    while (1) {
        usb_host_client_handle_events(g_client, portMAX_DELAY);
    }
}

void app_main(void)
{
    ctrl_sem = xSemaphoreCreateBinary();

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = 0,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));

    // Optional: reduce verbosity further at runtime
    // esp_log_level_set(TAG, ESP_LOG_WARN);

    xTaskCreatePinnedToCore(daemon_task, "usb_daemon", 4096, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(client_task, "usb_client", 8192, NULL, 4, NULL, 1);
}
