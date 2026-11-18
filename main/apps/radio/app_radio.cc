#include "apps/radio/app_radio.h"
#include "apps/radio/radio_ui.h"
#include "apps/radio/radio_player.h"
#include "apps/radio/radio_catalog.h"
#include "apps/home/home_profile.h"
#include "voice_intent.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include "ssid_manager.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <new>

static const char *TAG = "app_radio";

namespace {

std::atomic<bool> s_radio_active{false};
std::atomic<bool> s_wifi_connected{false};
std::atomic<bool> s_ui_alive{false};
std::atomic<bool> s_player_ready{false};
// Reserve before task creation so concurrent Wi-Fi/start paths own one worker.
std::atomic<bool> s_catalog_running{false};
std::atomic<bool> s_catalog_cancel{false};
// One successful city search per Radio session. Outdoor Wi-Fi drops and
// returns often; re-running the search on every reconnect stopped playback
// for several seconds each time.
std::atomic<bool> s_catalog_installed{false};
SemaphoreHandle_t s_catalog_commit = nullptr;
// Xiaozhi voice request for this session (heap; the catalog worker takes it).
std::atomic<voice_intent::Request *> s_voice{nullptr};
// Voice "stop after N minutes": pauses (never powers off, unlike the settings
// sleep timer). The task waits for app stop after firing, so its handle stays
// valid for the notification that ends it.
TaskHandle_t s_stop_task = nullptr;
std::atomic<bool> s_stop_running{false};

void stop_worker(void *arg) {
    const uint32_t minutes = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(minutes * 60000u)) && s_radio_active.load()) {
        ESP_LOGI(TAG, "Voice stop timer: pausing after %u min", (unsigned)minutes);
        radio_player_set_playing(false);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    s_stop_running.store(false);
    vTaskDelete(nullptr);
}

void end_stop_timer() {
    if (!s_stop_running.load()) return;
    xTaskNotifyGive(s_stop_task);
    for (int waited = 0; s_stop_running.load() && waited < 500; waited += 10) vTaskDelay(pdMS_TO_TICKS(10));
}

bool catalog_cancelled() { return s_catalog_cancel.load() || !s_radio_active.load(); }

// First station whose name contains the spoken keywords, else the top one.
std::size_t voice_station(const RadioStationList &stations, const char *keywords) {
    if (keywords && keywords[0])
        for (std::size_t i = 0; i < stations.size(); ++i) if (std::strstr(stations.at(i).name, keywords)) return i;
    return 0;
}

void catalog_worker(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Starting shared city station catalog search...");
    // Release the live decoder/TLS working set before directory HTTP/cJSON.
    radio_player_set_playing(false);
    if (!radio_player_wait_idle(2000) || catalog_cancelled()) {
        // A timed-out decoder/TLS worker still owns its memory. Do not overlap
        // it with another HTTP connection, and leave normal playback retryable.
        xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
        if (!catalog_cancelled()) radio_player_set_playing(true);
        s_catalog_running.store(false);
        xSemaphoreGive(s_catalog_commit);
        ESP_LOGW(TAG, "Catalog deferred: player has not released stream resources");
        vTaskDelete(nullptr);
        return;
    }
    voice_intent::Request *voice = s_voice.exchange(nullptr);
    RadioLocation preferred = {}, loc = {};
    if (voice) {
        // A spoken city replaces the profile city for this session only.
        // The tool limits the name to this field; preferred is zero-filled.
        std::memcpy(preferred.city, voice->name,
                    std::min(std::strlen(voice->name), sizeof(preferred.city) - 1));
        std::snprintf(preferred.country_code, sizeof(preferred.country_code), "MY");
    } else {
        home_profile_t profile;
        home_profile_get(&profile);
        std::snprintf(preferred.city, sizeof(preferred.city), "%s", profile.city);
        std::snprintf(preferred.region, sizeof(preferred.region), "%s", profile.region);
        std::snprintf(preferred.search_city, sizeof(preferred.search_city), "%s", profile.radio_city);
        std::snprintf(preferred.country_code, sizeof(preferred.country_code), "%s", profile.country_code);
    }
    RadioStationList stations;
    const bool complete = radio_catalog_discover_all(preferred, stations, &loc, catalog_cancelled,
                                                  voice ? voice->detail : nullptr);
    // Serialize installation/start with stop; no late playback in the next app.
    xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    const bool alive = !catalog_cancelled() && s_ui_alive.load();
    if (alive && stations.size()) {
        ESP_LOGI(TAG, "Location detected: %s (%s), found %u stations", loc.city, loc.region, (unsigned)stations.size());
        radio_ui_set_location(loc.city);
        const auto selected = voice ? voice_station(stations, voice->detail) : 0;
        if (radio_player_replace_stations_resuming(stations)) {
            if (voice) radio_player_set_station(selected);
            s_catalog_installed.store(complete);
        }
    }
    delete voice;
    stations.clear(); // FreeRTOS task deletion does not unwind the stack.
    if (alive) {
        // Start the MP3 decoder only after directory HTTP/cJSON is released.
        radio_player_set_playing(true);
    }
    xSemaphoreGive(s_catalog_commit);
    s_catalog_running.store(false);
    vTaskDelete(nullptr);
}

void start_catalog() {
#ifdef PASSPORT_RADIO_HLS_TEST
    // Keep the known test matrix visible; city discovery would replace it.
    radio_ui_set_location("Global radio test");
    radio_player_set_playing(true);
    return;
#endif
    if (!s_catalog_commit) return;
    xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    bool idle = false;
    // Reserve before task creation; a fast worker cannot leave a stale handle.
    if (!s_radio_active.load() || !s_player_ready.load() || s_catalog_installed.load() ||
        !s_catalog_running.compare_exchange_strong(idle, true)) {
        xSemaphoreGive(s_catalog_commit);
        return;
    }
    s_catalog_cancel.store(false);
    if (xTaskCreate(catalog_worker, "radio_cat", 6144, nullptr, 5, nullptr) != pdPASS) {
        s_catalog_running.store(false);
        ESP_LOGE(TAG, "Failed to create catalog task");
        radio_player_set_playing(true);
    }
    xSemaphoreGive(s_catalog_commit);
}

void on_wifi_event(WifiEvent event, const std::string &data) {
    if (!s_radio_active.load()) return;

    if (event == WifiEvent::Connected) {
        ESP_LOGI(TAG, "Wi-Fi connected: %s", data.c_str());
        s_wifi_connected.store(true);
        radio_ui_set_network(true, ("Connected: " + data).c_str());
        if (!s_player_ready.load()) return;
        radio_player_set_network(true);

        start_catalog();
    } else if (event == WifiEvent::Disconnected) {
        ESP_LOGI(TAG, "Wi-Fi disconnected: %s", data.c_str());
        s_wifi_connected.store(false);
        radio_ui_set_network(false, "Wi-Fi disconnected");
        if (s_player_ready.load()) radio_player_set_network(false);
    }
}

} // namespace

void radio_app_init(void) {
    ESP_LOGI(TAG, "Initializing Radio App subsystems...");
    radio_ui_init();
    static StaticSemaphore_t catalog_commit_storage;
    s_catalog_commit = xSemaphoreCreateMutexStatic(&catalog_commit_storage);
}

void radio_app_start(void) {
    ESP_LOGI(TAG, "Starting Radio App...");
    if (s_radio_active.load()) {
        ESP_LOGW(TAG, "Radio App is already active");
        return;
    }
    const voice_intent::Request request = voice_intent::take(APP_ID_RADIO);
    if (request.source == voice_intent::Source::Radio) {
        delete s_voice.exchange(new (std::nothrow) voice_intent::Request(request));
        if (request.stop_minutes) {
            end_stop_timer();
            s_stop_running.store(true);
            if (xTaskCreate(stop_worker, "radio_stop", 2048,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(request.stop_minutes)), 4,
                            &s_stop_task) != pdPASS) s_stop_running.store(false);
        }
    }
    // 1. Show the Radio UI
    radio_ui_show_main();
    radio_ui_set_volume(radio_player_volume());
    if (!s_catalog_commit || s_catalog_running.load()) {
        s_ui_alive.store(true);
        radio_ui_set_playback(RadioPlaybackState::Error, "Directory busy. Retry soon");
        return;
    }

    // A previous bounded stop may have retained a slow stream worker to avoid
    // freeing decoder/I2S resources underneath it.  Finish that cleanup before
    // creating the next session; never allow two player owners to coexist.
    if (s_player_ready.load()) {
        if (!radio_player_deinit(2000)) {
            s_radio_active.store(false);
            s_ui_alive.store(true);
            radio_ui_set_playback(RadioPlaybackState::Error,
                                  "Audio busy. Retry soon");
            ESP_LOGE(TAG, "Previous player worker is still alive; refusing a second owner");
            return;
        }
        s_player_ready.store(false);
    }

    if (!radio_player_init()) {
        s_player_ready.store(false);
        s_radio_active.store(false);
        s_ui_alive.store(true);
        radio_ui_set_playback(RadioPlaybackState::Error, "Low memory. Audio failed");
        return;
    }
    s_player_ready.store(true);
    s_catalog_installed.store(false); // stations were reset to presets above
    s_radio_active.store(true);
    s_ui_alive.store(true);
    radio_player_set_playing(false);

    // 2. Hook the global Wi-Fi event callback
    WifiManager::GetInstance().SetEventCallback(on_wifi_event);
    if (!WifiManager::GetInstance().IsConnected()) {
        ESP_LOGI(TAG, "Wi-Fi not connected yet, checking station...");
        if (!SsidManager::GetInstance().GetSsidList().empty()) {
            radio_ui_set_network(false, "Connecting to network...");
            WifiManager::GetInstance().StartStation();
        } else {
            radio_ui_set_playback(RadioPlaybackState::Error, "Set up Wi-Fi on phone");
        }
    } else {
        radio_ui_set_network(true, "Wi-Fi connected");
        radio_player_set_network(true);
        start_catalog();
    }

    // 3. Start playback only after the catalog query has freed its HTTP/cJSON memory.
}

bool radio_app_prepare_stop(void) {
    // Match catalog installation's lock: it must not resume playback after
    // this cancellation has stopped the player.
    if (s_catalog_commit) xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    s_catalog_cancel.store(true);
    if (s_radio_active.exchange(false) && s_player_ready.load())
        radio_player_set_playing(false);
    if (s_catalog_commit) xSemaphoreGive(s_catalog_commit);
    if (s_catalog_running.load()) return false;
    if (s_player_ready.load()) {
        if (!radio_player_deinit(0)) return false;
        s_player_ready.store(false);
    }
    return true;
}

void radio_app_stop(void) {
    ESP_LOGI(TAG, "Stopping Radio App (keeping global Wi-Fi active)...");
    if (s_catalog_commit) xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    s_catalog_cancel.store(true);
    s_radio_active.store(false);
    if (s_catalog_commit) xSemaphoreGive(s_catalog_commit);

    // 1. Stop playback and wait for the stream thread to exit
    if (s_player_ready.load()) {
        radio_player_set_playing(false);
        radio_player_wait_idle(3500);
    }

    // 2. 2026-09-13 fix (C2): the catalog task **must be joined**. It used to just set a flag and return,
    //    while the task still held a 6144B stack + 24KB response buffer (up to 5s per HTTP request), so entering
    //    Xiaozhi/voice right after easily failed to allocate. Wait up to 5s here; on timeout still do not vTaskDelete
    //    (it may hold the heap lock); just log and let it exit on its own.
    s_ui_alive.store(false);
    end_stop_timer();
    int waited_ms = 0;
    while (s_catalog_running.load() && waited_ms < 5000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited_ms += 50;
    }
    if (s_catalog_running.load()) {
        ESP_LOGW(TAG, "catalog task still running after 5s; left to exit by itself");
    }

    // An unused voice request (no catalog ran) must not apply to a later visit.
    if (!s_catalog_running.load()) delete s_voice.exchange(nullptr);

    // 3. Remove the event callback, keep the global Wi-Fi connection
    WifiManager::GetInstance().SetEventCallback(nullptr);

    // 4. Radio is a session, not a boot-lifetime service. Release the 7KB
    // stream task, decoder registry, codec objects and I2S DMA before the next
    // app (especially Xiaozhi) tries to allocate a large contiguous block.
    if (s_player_ready.load()) {
        if (radio_player_deinit(5000)) s_player_ready.store(false);
    }

    // 5. Hide the Radio UI and pause the high-rate timer
    radio_ui_hide();

    ESP_LOGI(TAG, "Radio App stopped. free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void radio_app_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    (void)mv;
    if (!s_player_ready.load()) return;

    if (event == BTN_EVT_CLICK) {
        if (btn == BSP_BTN_UP) {
            // "+" short press: next station
            radio_player_select_relative(1);
        } else if (btn == BSP_BTN_DOWN) {
            // "-" short press: previous station
            radio_player_select_relative(-1);
        } else if (btn == BSP_BTN_OK) {
            // "OK" short press: toggle play/pause
            radio_player_toggle();
        }
    } else if (event == BTN_EVT_LONG) {
        if (btn == BSP_BTN_UP) {
            // "+" long press: volume up
            uint8_t cur = radio_player_volume();
            uint8_t next = cur >= 90 ? 100 : cur + 10;
            radio_player_set_volume(next);
            ESP_LOGI(TAG, "Radio volume up: %u -> %u", cur, next);
        } else if (btn == BSP_BTN_DOWN) {
            // "-" long press: volume down
            uint8_t cur = radio_player_volume();
            uint8_t next = cur <= 10 ? 0 : cur - 10;
            radio_player_set_volume(next);
            ESP_LOGI(TAG, "Radio volume down: %u -> %u", cur, next);
        }
    }
}

const passport_app_t g_radio_app = {
    .id = APP_ID_RADIO,
    .name = "Radio",
    .en_name = "RADIO",
    .desc = "Listen to Malaysian radio\n↑↓ Station · OK Play",
    .tag = "Direct",
    .theme_color = 0xFFB74D,
    .init = radio_app_init,
    .start = radio_app_start,
    .stop = radio_app_stop,
    .on_key = radio_app_on_key,
    .prepare_stop = radio_app_prepare_stop,
};
