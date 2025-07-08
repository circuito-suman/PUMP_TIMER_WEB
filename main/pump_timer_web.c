#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_spiffs.h"


#define RELAY_START_GPIO  2
#define RELAY_STOP_GPIO   3
#define WIFI_SSID         "ESP32C3_Pump"
#define WIFI_PASS         "password123"
#define WEB_SERVER_PORT   80

typedef enum { MANUAL = 0, AUTO = 1 } Mode;

static Mode currentMode = MANUAL;
static bool pumpRunning = false;
static uint64_t pumpStartMillis = 0;
static uint64_t pumpRunTime = 0; // ms

static uint64_t autoDuration = 0; // ms
static uint64_t autoTimeLeft = 0; // ms
static bool autoActive = false;

static uint32_t pumpStartCount = 0;
static uint32_t pumpStopCount = 0;
static uint32_t irrigationCycleCount = 0;
static bool cycleInProgress = false;

static uint64_t manualRunTime = 0;
static uint64_t totalManualRunTime = 0;

static esp_timer_handle_t relay_timer = NULL;
static int relay_pulse_pin = -1;

static nvs_handle_t my_nvs_handle;
static const char *TAG = "PUMP";

static bool cycleEnabled = false;
static uint64_t cycleInterval = 0; // ms
static uint64_t nextCycleMillis = 0;
static uint64_t cycleTimeLeft = 0;

static bool emergencyLock = false;

void init_spiffs() {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
}

void load_nvs() {
    ESP_ERROR_CHECK(nvs_open("pumpctrl", NVS_READWRITE, &my_nvs_handle));
    nvs_get_u32(my_nvs_handle, "startCount", &pumpStartCount);
    nvs_get_u32(my_nvs_handle, "stopCount", &pumpStopCount);
    nvs_get_u32(my_nvs_handle, "cycleCount", &irrigationCycleCount);
    nvs_get_u64(my_nvs_handle, "totalManTime", &totalManualRunTime);
    nvs_get_u64(my_nvs_handle, "autoDuration", &autoDuration);
    nvs_get_u32(my_nvs_handle, "lastMode", (uint32_t*)&currentMode);
    nvs_get_u8(my_nvs_handle, "cycleEnabled", (uint8_t*)&cycleEnabled);
    nvs_get_u64(my_nvs_handle, "cycleInterval", &cycleInterval);
}
void save_nvs() {
    nvs_set_u32(my_nvs_handle, "startCount", pumpStartCount);
    nvs_set_u32(my_nvs_handle, "stopCount", pumpStopCount);
    nvs_set_u32(my_nvs_handle, "cycleCount", irrigationCycleCount);
    nvs_set_u64(my_nvs_handle, "totalManTime", totalManualRunTime);
    nvs_set_u64(my_nvs_handle, "autoDuration", autoDuration);
    nvs_set_u32(my_nvs_handle, "lastMode", currentMode);
    nvs_set_u8(my_nvs_handle, "cycleEnabled", cycleEnabled);
    nvs_set_u64(my_nvs_handle, "cycleInterval", cycleInterval);
    nvs_commit(my_nvs_handle);
}

// --- RELAY PULSE ---
void relay_pulse_callback(void* arg) {
    gpio_set_level(relay_pulse_pin, 0);
    relay_pulse_pin = -1;
}

void pulse_relay(int pin) {
    gpio_set_level(pin, 1);
    relay_pulse_pin = pin;
    if (relay_timer) esp_timer_stop(relay_timer);
    static esp_timer_create_args_t timer_args;
    timer_args.callback = relay_pulse_callback;
    timer_args.arg = NULL;
    timer_args.name = "relay_pulse";
    if (!relay_timer) esp_timer_create(&timer_args, &relay_timer);
    esp_timer_start_once(relay_timer, 2000000); // 2 seconds
}

// --- PUMP LOGIC ---
void start_pump() {
    if (!pumpRunning) {
        ESP_LOGI(TAG, "Pump ON (start_pump called)");
        pulse_relay(RELAY_START_GPIO);
        pumpRunning = true;
        pumpStartMillis = esp_timer_get_time() / 1000; // ms
        pumpStartCount++;
        save_nvs();
        if (!cycleInProgress) {
            cycleInProgress = true;
            ESP_LOGI(TAG, "Cycle started (cycleInProgress set)");
        }
    }
}
void stop_pump() {
    if (pumpRunning) {
        ESP_LOGI(TAG, "Pump OFF (stop_pump called)");
        if (currentMode == MANUAL) {
            manualRunTime = (esp_timer_get_time() / 1000) - pumpStartMillis;
            totalManualRunTime += manualRunTime;
            ESP_LOGI(TAG, "Manual run time this cycle: %llu ms, total: %llu ms", manualRunTime, totalManualRunTime);
            save_nvs();
            manualRunTime = 0;
        }
        pulse_relay(RELAY_STOP_GPIO);
        pumpRunning = false;
        pumpRunTime += (esp_timer_get_time() / 1000) - pumpStartMillis;
        pumpStopCount++;
        save_nvs();
        if (cycleInProgress) {
            irrigationCycleCount++;
            ESP_LOGI(TAG, "Irrigation cycle count incremented: %" PRIu32, irrigationCycleCount);
            save_nvs();
            cycleInProgress = false;
        }
    }
    autoActive = false;
    autoTimeLeft = 0;
}

// --- HTTP HANDLERS ---

esp_err_t status_get_handler(httpd_req_t *req) {
    char resp[512];
    uint64_t now = esp_timer_get_time() / 1000;
    uint64_t runTime = pumpRunTime;
    uint64_t autoLeft = autoTimeLeft;
    if (pumpRunning) {
        if (currentMode == MANUAL) runTime += now - pumpStartMillis;
        if (currentMode == AUTO && autoActive) autoLeft = (autoTimeLeft > now - pumpStartMillis) ? autoTimeLeft - (now - pumpStartMillis) : 0;
    }
    uint64_t manualTimeToSend = manualRunTime;
    if (pumpRunning && currentMode == MANUAL) manualTimeToSend = now - pumpStartMillis;
    snprintf(resp, sizeof(resp),
        "{"
        "\"mode\":%d,"
        "\"autoDuration\":%llu,"
        "\"autoTimeLeft\":%llu,"
        "\"pumpRunning\":%d,"
        "\"pumpRunTime\":%llu,"
        "\"pumpStartCount\":%" PRIu32 ","
        "\"pumpStopCount\":%" PRIu32 ","
        "\"irrigationCycleCount\":%" PRIu32 ","
        "\"manualRunTime\":%llu,"
        "\"totalManualRunTime\":%llu,"
        "\"cycleEnabled\":%d,"
        "\"cycleInterval\":%llu,"
        "\"cycleTimeLeft\":%llu,"
        "\"emergencyLock\":%d"
        "}",
        currentMode, autoDuration, autoLeft, pumpRunning ? 1 : 0, runTime,
        pumpStartCount, pumpStopCount, irrigationCycleCount,
        manualTimeToSend, totalManualRunTime,
        cycleEnabled ? 1 : 0, cycleInterval, cycleTimeLeft,
        emergencyLock ? 1 : 0
    );
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

esp_err_t setmode_post_handler(httpd_req_t *req) {
    char buf[32];
    int ret = httpd_req_get_url_query_str(req, buf, sizeof(buf));
    if (ret == ESP_OK) {
        char mode_str[8];
        if (httpd_query_key_value(buf, "mode", mode_str, sizeof(mode_str)) == ESP_OK) {
            Mode new_mode = (Mode)atoi(mode_str);
            if (new_mode != currentMode) {
                stop_pump();
                pumpRunTime = 0;
                autoActive = false;
                autoTimeLeft = 0;
                manualRunTime = 0;
                currentMode = new_mode;
                save_nvs();
                emergencyLock = false; // Allow operation after mode change
            }
        }
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

esp_err_t manual_post_handler(httpd_req_t *req) {
    char buf[32];
    int ret = httpd_req_get_url_query_str(req, buf, sizeof(buf));
    if (ret == ESP_OK) {
        char cmd[8];
        if (httpd_query_key_value(buf, "cmd", cmd, sizeof(cmd)) == ESP_OK) {
            if (currentMode != MANUAL) {
                httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Wrong mode");
                return ESP_OK;
            }
            if (strcmp(cmd, "start") == 0) start_pump();
            else if (strcmp(cmd, "stop") == 0) stop_pump();
        }
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

esp_err_t setauto_post_handler(httpd_req_t *req) {
    char buf[64];
    int ret = httpd_req_get_url_query_str(req, buf, sizeof(buf));
    if (ret == ESP_OK) {
        char dur_str[16];
        if (httpd_query_key_value(buf, "duration", dur_str, sizeof(dur_str)) == ESP_OK) {
            if (currentMode != AUTO) {
                httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Wrong mode");
                return ESP_OK;
            }
            autoDuration = strtoull(dur_str, NULL, 10);
            autoTimeLeft = autoDuration;
            autoActive = false;
            save_nvs();
        }
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

esp_err_t autostart_post_handler(httpd_req_t *req) {
    if (currentMode != AUTO || autoDuration == 0) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Wrong mode or duration");
        return ESP_OK;
    }
    start_pump();
    pumpStartMillis = esp_timer_get_time() / 1000;
    autoActive = true;
    autoTimeLeft = autoDuration;
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

esp_err_t setcycle_post_handler(httpd_req_t *req) {
    char buf[64];
    int ret = httpd_req_get_url_query_str(req, buf, sizeof(buf));
    uint64_t now = esp_timer_get_time() / 1000;
    if (ret == ESP_OK) {
        char enable_str[8], interval_str[16];
        bool enable_changed = false, interval_changed = false;
        if (httpd_query_key_value(buf, "enable", enable_str, sizeof(enable_str)) == ESP_OK) {
            cycleEnabled = atoi(enable_str) ? true : false;
            enable_changed = true;
        }
        if (httpd_query_key_value(buf, "interval", interval_str, sizeof(interval_str)) == ESP_OK) {
            cycleInterval = strtoull(interval_str, NULL, 10);
            interval_changed = true;
        }
        // If enabling or changing interval, and not running, set next cycle
        if (cycleEnabled && !pumpRunning && cycleInterval > 0 && (enable_changed || interval_changed)) {
            nextCycleMillis = now + cycleInterval;
        }
        save_nvs();
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

esp_err_t emergency_stop_post_handler(httpd_req_t *req) {
    if (!emergencyLock) {
        ESP_LOGW(TAG, "EMERGENCY STOP triggered!");
        stop_pump();
        autoActive = false;
        autoTimeLeft = 0;
        cycleEnabled = false;
        cycleTimeLeft = 0;
        nextCycleMillis = 0;
        emergencyLock = true;
        save_nvs();
        httpd_resp_sendstr(req, "EMERGENCY_STOPPED");
    } else {
        ESP_LOGW(TAG, "EMERGENCY STOP released!");
        emergencyLock = false;
        save_nvs();
        httpd_resp_sendstr(req, "EMERGENCY_RELEASED");
    }
    return ESP_OK;
}

// esp_err_t reset_post_handler(httpd_req_t *req) {
//     ESP_LOGI(TAG, "Resetting all timers and counts");
//     stop_pump();
//     pumpStartCount = 0;
//     pumpStopCount = 0;
//     irrigationCycleCount = 0;
//     manualRunTime = 0;
//     totalManualRunTime = 0;
//     pumpRunTime = 0;
//     autoDuration = 0;
//     autoTimeLeft = 0;
//     autoActive = false;
//     cycleEnabled = false;
//     cycleInterval = 0;
//     nextCycleMillis = 0;
//     cycleTimeLeft = 0;
//     emergencyLock = false; // Allow operation after reset
//     save_nvs();
//     httpd_resp_send(req, "OK", 2);
//     return ESP_OK;
// }

// --- Serve static files from SPIFFS ---
esp_err_t static_file_get_handler(httpd_req_t *req) {
    char filepath[64];
    const char *uri = req->uri;
    if (strcmp(uri, "/") == 0) uri = "/index.html";
    strncpy(filepath, "/spiffs", sizeof(filepath) - 1);
    filepath[sizeof(filepath) - 1] = '\0';
    strncat(filepath, uri, sizeof(filepath) - strlen(filepath) - 1);
    FILE *f = fopen(filepath, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    size_t filesize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *filebuf = malloc(filesize);
    fread(filebuf, 1, filesize, f);
    fclose(f);
    if (strstr(uri, ".css")) httpd_resp_set_type(req, "text/css");
    else if (strstr(uri, ".js")) httpd_resp_set_type(req, "application/javascript");
    else httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, filebuf, filesize);
    free(filebuf);
    return ESP_OK;
}


esp_err_t index_get_handler(httpd_req_t *req) {
    FILE *f = fopen("/spiffs/index.html", "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    size_t filesize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *filebuf = malloc(filesize);
    fread(filebuf, 1, filesize, f);
    fclose(f);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, filebuf, filesize);
    free(filebuf);
    return ESP_OK;
}


// --- HTTP SERVER INIT ---
void start_webserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t status_uri = { .uri = "/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &status_uri);

    httpd_uri_t setmode_uri = { .uri = "/setmode", .method = HTTP_POST, .handler = setmode_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &setmode_uri);

    httpd_uri_t manual_uri = { .uri = "/manual", .method = HTTP_POST, .handler = manual_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &manual_uri);

    httpd_uri_t setauto_uri = { .uri = "/setauto", .method = HTTP_POST, .handler = setauto_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &setauto_uri);

    httpd_uri_t autostart_uri = { .uri = "/autostart", .method = HTTP_POST, .handler = autostart_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &autostart_uri);

    httpd_uri_t setcycle_uri = { .uri = "/setcycle", .method = HTTP_POST, .handler = setcycle_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &setcycle_uri);

httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_get_handler,
    .user_ctx = NULL
};
httpd_register_uri_handler(server, &index_uri);

httpd_uri_t emergency_stop_uri = { .uri = "/emergencystop", .method = HTTP_POST, .handler = emergency_stop_post_handler, .user_ctx = NULL };
httpd_register_uri_handler(server, &emergency_stop_uri);





    // Static files
    httpd_uri_t static_uri = { .uri = "/*", .method = HTTP_GET, .handler = static_file_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &static_uri);
}

// --- WIFI INIT ---
void wifi_init_softap() {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .password = WIFI_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        }
    };
    if (strlen(WIFI_PASS) == 0) wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// --- GPIO INIT ---
void gpio_init() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RELAY_START_GPIO) | (1ULL << RELAY_STOP_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(RELAY_START_GPIO, 0);
    gpio_set_level(RELAY_STOP_GPIO, 0);
}

// --- APP MAIN ---
void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    gpio_init();

    // Safety: Always turn pump off at boot
    ESP_LOGW(TAG, "Bootup: Forcing pump OFF for safety");
    pulse_relay(RELAY_STOP_GPIO);

    wifi_init_softap();
    init_spiffs();
    load_nvs();
    start_webserver();
    ESP_LOGI(TAG, "Pump controller started. Connect to SSID: %s", WIFI_SSID);

    // Main loop for auto mode timer
    while (1) {
        uint64_t now = esp_timer_get_time() / 1000;
        if (!emergencyLock) {
            // If pump is running in AUTO mode, check if time expired
            if (currentMode == AUTO && autoActive && pumpRunning) {
                ESP_LOGI(TAG, "AUTO mode: Pump running, time left: %llu ms", (now - pumpStartMillis < autoTimeLeft) ? autoTimeLeft - (now - pumpStartMillis) : 0);
                if (now - pumpStartMillis >= autoTimeLeft) {
                    ESP_LOGI(TAG, "AUTO mode: Auto time expired, stopping pump");
                    stop_pump();
                    autoActive = false;
                    if (cycleEnabled && cycleInterval > 0) {
                        nextCycleMillis = now + cycleInterval;
                        ESP_LOGI(TAG, "Cycle enabled: Next cycle scheduled in %llu ms", cycleInterval);
                    }
                }
            }
            // If in AUTO mode, cycle enabled, pump is off, and interval elapsed, start next cycle
            if (currentMode == AUTO && cycleEnabled && !pumpRunning && cycleInterval > 0 && autoDuration > 0) {
                if (!autoActive && now >= nextCycleMillis) {
                    ESP_LOGI(TAG, "Starting next auto cycle");
                    autoActive = true;
                    start_pump();
                    pumpStartMillis = esp_timer_get_time() / 1000;
                    autoTimeLeft = autoDuration;
                    cycleTimeLeft = 0;
                } else if (!autoActive && nextCycleMillis > now) {
                    cycleTimeLeft = nextCycleMillis - now;
                    ESP_LOGI(TAG, "Waiting for next cycle: %llu ms left", cycleTimeLeft);
                }
            } else if (!pumpRunning) {
                cycleTimeLeft = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
