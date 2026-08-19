#include "adc_battery_monitor.h"
#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "i2c_device.h"
#include "ota.h"
#include "persona_select_ui.h"
#include "system_info.h"
#include "tca8418_keyboard.h"
#include "wifi_board.h"
#include "wifi_config_ui.h"

#include <driver/i2c_master.h>
#include <driver/i2s_common.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ssid_manager.h>
#include <wifi_manager.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <string>

#define TAG "CardputerAdv"

// Backlight uses percentage scale (0-100). Keep a minimum of 30% to avoid a too-dim screen.
#define MIN_BRIGHTNESS 30

// Deliberately require a modifier and a long hold so normal typing cannot enter AP mode.
static constexpr uint64_t WIFI_CONFIG_HOLD_TIME_US = 3 * 1000 * 1000ULL;
static constexpr uint64_t WIFI_CONFIG_CONFIRMATION_DELAY_US = 750 * 1000ULL;
static constexpr size_t MAX_PERSONAS = 10;

class M5StackCardputerAdvBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    LcdDisplay* display_;
    Button boot_button_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Tca8418Keyboard* keyboard_ = nullptr;
    AdcBatteryMonitor* battery_monitor_ = nullptr;
    std::unique_ptr<WifiConfigUI> wifi_config_ui_;
    std::unique_ptr<PersonaSelectUI> persona_select_ui_;
    bool wifi_config_mode_ = false;
    // The regular WifiStation owns the radio while connected.  On-device
    // reconfiguration temporarily starts the driver without that station so
    // its automatic saved-network reconnect cannot consume the UI scan.
    bool on_device_wifi_config_radio_active_ = false;
    esp_timer_handle_t wifi_config_hold_timer_ = nullptr;
    esp_timer_handle_t wifi_config_confirmation_timer_ = nullptr;
    std::atomic_bool wifi_config_key_pressed_{false};
    std::atomic_bool wifi_config_hold_triggered_{false};
    std::atomic_bool persona_selection_active_{false};
    std::atomic_bool persona_switch_in_progress_{false};
    std::string persona_api_url_;
    std::string persona_token_;

    struct PersonaSwitchRequest {
        M5StackCardputerAdvBoard* board;
        std::string agent_id;
        std::string name;
        std::string api_url;
        std::string token;
    };

    void StartPersonaSelection() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateIdle) {
            persona_selection_active_.store(false);
            return;
        }

        ESP_LOGI(TAG, "Opening device persona selector");
        persona_select_ui_ = std::make_unique<PersonaSelectUI>(display_);
        persona_select_ui_->ShowLoading();
        StartPersonaLoad();
    }

    void ExitPersonaSelection() {
        persona_select_ui_.reset();
        persona_api_url_.clear();
        persona_token_.clear();
        persona_selection_active_.store(false);
        persona_switch_in_progress_.store(false);
    }

    void StartPersonaLoad() {
        persona_api_url_.clear();
        persona_token_.clear();
        persona_switch_in_progress_.store(false);
        if (persona_select_ui_) {
            persona_select_ui_->ShowLoading();
        }

        BaseType_t created = xTaskCreate(
            [](void* arg) {
                static_cast<M5StackCardputerAdvBoard*>(arg)->LoadPersonasTask();
                vTaskDelete(nullptr);
            },
            "persona_list", 4096 * 2, this, 2, nullptr);
        if (created != pdPASS) {
            ShowPersonaLoadError("読み込みタスクを開始できません");
        }
    }

    void ShowPersonaLoadError(const std::string& message) {
        Application::GetInstance().Schedule([this, message]() {
            persona_switch_in_progress_.store(false);
            if (persona_selection_active_.load() && persona_select_ui_) {
                persona_select_ui_->ShowError(message);
            }
        });
    }

    bool SetupPersonaAccess(Ota& ota, std::string& api_url, std::string& token,
                            std::string& error) {
        esp_err_t err = ota.CheckVersion();
        if (err != ESP_OK) {
            error = "OTA設定を取得できません";
            return false;
        }

        api_url = ota.GetDevicePersonaApiUrl();
        token = ota.GetDevicePersonaToken();
        if (api_url.empty() || token.empty()) {
            error = "ペルソナAPIの認証情報がありません";
            return false;
        }

        return true;
    }

    std::unique_ptr<Http> CreatePersonaHttp(const std::string& token) {
        auto http = GetNetwork()->CreateHttp(0);
        http->SetTimeout(15000);
        http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
        http->SetHeader("Client-Id", GetUuid());
        http->SetHeader("Authorization", "Bearer " + token);
        http->SetHeader("Accept-Language", Lang::CODE);
        http->SetHeader("Content-Type", "application/json");
        return http;
    }

    void LoadPersonasTask() {
        Ota ota;
        std::string error;
        std::string api_url;
        std::string token;
        if (!SetupPersonaAccess(ota, api_url, token, error)) {
            ShowPersonaLoadError(error);
            return;
        }

        auto http = CreatePersonaHttp(token);
        if (!http->Open("GET", api_url)) {
            ShowPersonaLoadError("ペルソナAPIに接続できません");
            return;
        }
        if (http->GetStatusCode() != 200) {
            http->Close();
            ShowPersonaLoadError("ペルソナ一覧の取得に失敗しました");
            return;
        }

        std::string body = http->ReadAll();
        http->Close();
        cJSON* root = cJSON_Parse(body.c_str());
        if (root == nullptr) {
            ShowPersonaLoadError("ペルソナ一覧の応答が不正です");
            return;
        }

        std::vector<PersonaOption> personas;
        cJSON* code = cJSON_GetObjectItem(root, "code");
        cJSON* data = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsNumber(code) && code->valueint == 0 && cJSON_IsArray(data)) {
            cJSON* item = nullptr;
            cJSON_ArrayForEach (item, data) {
                if (personas.size() >= MAX_PERSONAS) {
                    break;
                }
                cJSON* agent_id = cJSON_GetObjectItem(item, "agentId");
                cJSON* name = cJSON_GetObjectItem(item, "name");
                cJSON* active = cJSON_GetObjectItem(item, "active");
                if (!cJSON_IsString(agent_id) || !cJSON_IsString(name)) {
                    continue;
                }
                personas.push_back({.agent_id = agent_id->valuestring,
                                    .name = name->valuestring,
                                    .active = cJSON_IsTrue(active) ||
                                              (cJSON_IsNumber(active) && active->valueint != 0)});
            }
        }
        cJSON_Delete(root);

        if (personas.empty()) {
            ShowPersonaLoadError("利用可能なペルソナがありません");
            return;
        }

        Application::GetInstance().Schedule([this, personas = std::move(personas),
                                             api_url = std::move(api_url),
                                             token = std::move(token)]() mutable {
            if (persona_selection_active_.load() && persona_select_ui_) {
                persona_api_url_ = std::move(api_url);
                persona_token_ = std::move(token);
                persona_select_ui_->ShowPersonas(std::move(personas));
            }
        });
    }

    void StartPersonaSwitch(const PersonaOption& persona) {
        bool expected = false;
        if (!persona_switch_in_progress_.compare_exchange_strong(expected, true)) {
            return;
        }
        persona_select_ui_->ShowSwitching(persona.name);
        auto* request = new PersonaSwitchRequest{.board = this,
                                                 .agent_id = persona.agent_id,
                                                 .name = persona.name,
                                                 .api_url = persona_api_url_,
                                                 .token = persona_token_};
        BaseType_t created = xTaskCreate(
            [](void* arg) {
                std::unique_ptr<PersonaSwitchRequest> request(
                    static_cast<PersonaSwitchRequest*>(arg));
                request->board->SwitchPersonaTask(*request);
                vTaskDelete(nullptr);
            },
            "persona_switch", 4096 * 2, request, 2, nullptr);
        if (created != pdPASS) {
            delete request;
            persona_switch_in_progress_.store(false);
            ShowPersonaLoadError("切替タスクを開始できません");
        }
    }

    void SwitchPersonaTask(const PersonaSwitchRequest& request) {
        auto http = CreatePersonaHttp(request.token);
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "agentId", request.agent_id.c_str());
        char* json = cJSON_PrintUnformatted(root);
        std::string payload = json == nullptr ? "" : json;
        cJSON_free(json);
        cJSON_Delete(root);
        if (payload.empty()) {
            ShowPersonaLoadError("切替要求を作成できません");
            return;
        }

        http->SetContent(std::move(payload));
        if (!http->Open("POST", request.api_url + "/activate")) {
            ShowPersonaLoadError("ペルソナAPIに接続できません");
            return;
        }
        if (http->GetStatusCode() != 200) {
            http->Close();
            ShowPersonaLoadError("ペルソナの切替に失敗しました");
            return;
        }

        std::string body = http->ReadAll();
        http->Close();
        cJSON* response = cJSON_Parse(body.c_str());
        cJSON* code = response == nullptr ? nullptr : cJSON_GetObjectItem(response, "code");
        const bool success = cJSON_IsNumber(code) && code->valueint == 0;
        cJSON_Delete(response);
        if (!success) {
            ShowPersonaLoadError("ペルソナの切替に失敗しました");
            return;
        }

        Application::GetInstance().Schedule([this, name = request.name]() {
            if (!persona_selection_active_.load()) {
                return;
            }
            ExitPersonaSelection();
            display_->ShowNotification(std::string("「") + name + "」に切替中...", 5000);
            Application::GetInstance().ReloadProtocolConfiguration();
        });
    }

    void HandlePersonaShortcut(const KeyEvent& event) {
        if (!event.pressed || event.key_code != KC_P ||
            (keyboard_->GetModifierMask() & KEY_MOD_CTRL) == 0) {
            return;
        }
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            return;
        }

        bool expected = false;
        if (!persona_selection_active_.compare_exchange_strong(expected, true)) {
            return;
        }
        Application::GetInstance().Schedule([this]() { StartPersonaSelection(); });
    }

    void HandlePersonaUiKeyEvent(const KeyEvent& event) {
        Application::GetInstance().Schedule([this, event]() {
            if (!persona_select_ui_) {
                return;
            }
            switch (persona_select_ui_->HandleKeyEvent(event)) {
                case PersonaSelectResult::Cancelled:
                    ExitPersonaSelection();
                    break;
                case PersonaSelectResult::Retry:
                    StartPersonaLoad();
                    break;
                case PersonaSelectResult::Selected:
                    StartPersonaSwitch(persona_select_ui_->selected_persona());
                    break;
                case PersonaSelectResult::None:
                    break;
            }
        });
    }

    void InitializeWifiConfigTimers() {
        const esp_timer_create_args_t hold_timer_args = {
            .callback =
                [](void* arg) {
                    static_cast<M5StackCardputerAdvBoard*>(arg)->OnWifiConfigKeyHeld();
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wifi_cfg_key_hold",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&hold_timer_args, &wifi_config_hold_timer_));

        const esp_timer_create_args_t confirmation_timer_args = {
            .callback =
                [](void* arg) {
                    static_cast<M5StackCardputerAdvBoard*>(arg)->OnWifiConfigConfirmationElapsed();
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wifi_cfg_confirm",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(
            esp_timer_create(&confirmation_timer_args, &wifi_config_confirmation_timer_));
    }

    void CancelWifiConfigKeyHold() {
        wifi_config_key_pressed_.store(false);
        wifi_config_hold_triggered_.store(false);
        if (wifi_config_hold_timer_ != nullptr) {
            esp_timer_stop(wifi_config_hold_timer_);
        }
        if (wifi_config_confirmation_timer_ != nullptr) {
            esp_timer_stop(wifi_config_confirmation_timer_);
        }
    }

    void OnWifiConfigKeyHeld() {
        if (!wifi_config_key_pressed_.load()) {
            return;
        }

        wifi_config_hold_triggered_.store(true);
        Application::GetInstance().Schedule([this]() {
            auto& app = Application::GetInstance();
            if (!wifi_config_key_pressed_.load() || app.GetDeviceState() != kDeviceStateIdle) {
                wifi_config_hold_triggered_.store(false);
                return;
            }

            // Show a visible confirmation before interrupting the network session.
            display_->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE, 1000);
            ESP_LOGI(TAG, "Ctrl+W held for three seconds; WiFi config starts after confirmation");
            ESP_ERROR_CHECK(esp_timer_start_once(wifi_config_confirmation_timer_,
                                                 WIFI_CONFIG_CONFIRMATION_DELAY_US));
        });
    }

    void OnWifiConfigConfirmationElapsed() {
        Application::GetInstance().Schedule([this]() {
            auto& app = Application::GetInstance();
            if (!wifi_config_hold_triggered_.exchange(false) ||
                app.GetDeviceState() != kDeviceStateIdle) {
                return;
            }

            // Close the current protocol before disconnecting Wi-Fi.  Queue the
            // UI start behind ResetProtocol() so it runs after that cleanup in
            // the main task.
            app.ResetProtocol();
            app.Schedule([this]() { StartOnDeviceWifiConfig(); });
        });
    }

    bool StartOnDeviceWifiConfigRadio() {
        auto& wifi_manager = WifiManager::GetInstance();

        // Stop WifiStation first.  Its scan-done handler automatically
        // reconnects saved SSIDs, which would otherwise race the SSID list
        // displayed by WifiConfigUI.
        wifi_manager.StopStation();

        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set on-device WiFi scan mode: %s", esp_err_to_name(err));
            return false;
        }

        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start on-device WiFi scanner: %s", esp_err_to_name(err));
            return false;
        }

        on_device_wifi_config_radio_active_ = true;
        return true;
    }

    void StopOnDeviceWifiConfigRadio() {
        if (!on_device_wifi_config_radio_active_) {
            return;
        }

        esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "Failed to stop on-device WiFi scanner: %s", esp_err_to_name(err));
        }
        on_device_wifi_config_radio_active_ = false;
    }

    void StartOnDeviceWifiConfig() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateIdle) {
            return;
        }

        // This uses only Cardputer's display and keyboard.  Do not call
        // WifiBoard::EnterWifiConfigMode(), which starts the captive portal.
        app.SetDeviceState(kDeviceStateWifiConfiguring);
        if (!StartOnDeviceWifiConfigRadio()) {
            display_->ShowNotification(Lang::Strings::WIFI_NOT_FOUND, 3000);
            TryWifiConnect();
            return;
        }

        ESP_LOGI(TAG, "Opening on-device WiFi configuration UI");
        StartKeyboardWifiConfig();
    }

    void HandleWifiConfigShortcut(const KeyEvent& event) {
        if (event.key_code == KC_LCTRL && !event.pressed) {
            CancelWifiConfigKeyHold();
            return;
        }

        if (event.key_code != KC_W) {
            return;
        }

        if (!event.pressed) {
            CancelWifiConfigKeyHold();
            return;
        }

        if ((keyboard_->GetModifierMask() & KEY_MOD_CTRL) == 0) {
            return;
        }

        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            ESP_LOGD(TAG, "Ignoring Ctrl+W WiFi shortcut outside idle state");
            return;
        }

        wifi_config_key_pressed_.store(true);
        wifi_config_hold_triggered_.store(false);
        ESP_ERROR_CHECK(esp_timer_start_once(wifi_config_hold_timer_, WIFI_CONFIG_HOLD_TIME_US));
    }

    void InitializeI2c() {
        ESP_LOGI(TAG, "Initialize I2C bus");
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void I2cDetect() {
        uint8_t address;
        ESP_LOGI(TAG, "I2C device scan:");
        printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
        for (int i = 0; i < 128; i += 16) {
            printf("%02x: ", i);
            for (int j = 0; j < 16; j++) {
                fflush(stdout);
                address = i + j;
                esp_err_t ret = i2c_master_probe(i2c_bus_, address, pdMS_TO_TICKS(200));
                if (ret == ESP_OK) {
                    printf("%02x ", address);
                } else if (ret == ESP_ERR_TIMEOUT) {
                    printf("UU ");
                } else {
                    printf("-- ");
                }
            }
            printf("\r\n");
        }
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SPI bus");
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_SCLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeSt7789Display() {
        ESP_LOGI(TAG, "Initialize ST7789V2 display");

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        io_config.flags.sio_mode = 1;  // 3-wire SPI mode (M5GFX uses spi_3wire = true)
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install ST7789 panel driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));

        display_ = new SpiLcdDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            if (persona_selection_active_.load()) {
                return;
            }
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeBatteryMonitor() {
        ESP_LOGI(TAG, "Initialize battery monitor (GPIO%d, %dK/%dK divider)", BATTERY_ADC_CHANNEL,
                 (int)(BATTERY_UPPER_RESISTOR / 1000), (int)(BATTERY_LOWER_RESISTOR / 1000));
        battery_monitor_ =
            new AdcBatteryMonitor(BATTERY_ADC_UNIT, BATTERY_ADC_CHANNEL, BATTERY_UPPER_RESISTOR,
                                  BATTERY_LOWER_RESISTOR, BATTERY_CHARGING_PIN);
    }

    void InitializeKeyboard() {
        ESP_LOGI(TAG, "Initialize TCA8418 keyboard");
        keyboard_ = new Tca8418Keyboard(i2c_bus_, KEYBOARD_TCA8418_ADDR, KEYBOARD_INT_PIN);
        keyboard_->Initialize();
        InitializeWifiConfigTimers();

        // Set legacy callback for volume/brightness control
        keyboard_->SetKeyCallback([this](LegacyKeyCode key) { HandleLegacyKeyPress(key); });

        // Set full key event callback for WiFi config and text input
        keyboard_->SetKeyEventCallback([this](const KeyEvent& event) { HandleKeyEvent(event); });
    }

    void HandleKeyEvent(const KeyEvent& event) {
        // Handle WiFi config mode
        if (wifi_config_mode_ && wifi_config_ui_) {
            auto result = wifi_config_ui_->HandleKeyEvent(event);
            if (result == WifiConfigResult::Connected) {
                ESP_LOGI(TAG, "WiFi connected via keyboard config");
                ExitWifiConfigMode();
            } else if (result == WifiConfigResult::Cancelled) {
                ESP_LOGI(TAG, "WiFi config cancelled");
                ExitWifiConfigMode();
            }
            return;
        }

        // Persona UI is driven in the main task so keyboard callbacks never
        // manipulate LVGL or start network I/O directly.
        if (persona_selection_active_.load()) {
            HandlePersonaUiKeyEvent(event);
            return;
        }

        HandleWifiConfigShortcut(event);
        HandlePersonaShortcut(event);

        // Handle W and S keys during WiFi configuring state (scanning screen)
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateWifiConfiguring && event.pressed) {
            if (event.key_code == KC_W) {
                ESP_LOGI(TAG, "W key pressed - entering keyboard WiFi config");
                app.Schedule([this]() { StartKeyboardWifiConfig(); });
            } else if (event.key_code == KC_S) {
                ESP_LOGI(TAG, "S key pressed - showing saved WiFi list");
                app.Schedule([this]() { StartKeyboardWifiConfigSaved(); });
            }
        }
    }

    void HandleLegacyKeyPress(LegacyKeyCode key) {
        // Skip while either board-owned configuration UI has focus.
        if (wifi_config_mode_ || persona_selection_active_.load()) {
            return;
        }

        auto& app = Application::GetInstance();
        auto* codec = GetAudioCodec();
        auto* backlight = GetBacklight();

        switch (key) {
            case KEY_UP: {
                // Volume up
                int current_vol = codec->output_volume();
                int step = (current_vol <= 20 || current_vol >= 80) ? 1 : 10;
                int new_vol = std::min(100, current_vol + step);
                codec->SetOutputVolume(new_vol);
                display_->ShowNotification(
                    std::string(Lang::Strings::VOLUME) + std::to_string(new_vol) + "%", 1500);
                ESP_LOGI(TAG, "Volume up: %d%%", new_vol);
                break;
            }
            case KEY_DOWN: {
                // Volume down
                int current_vol = codec->output_volume();
                int step = (current_vol <= 20 || current_vol >= 80) ? 1 : 10;
                int new_vol = std::max(0, current_vol - step);
                codec->SetOutputVolume(new_vol);
                display_->ShowNotification(
                    std::string(Lang::Strings::VOLUME) + std::to_string(new_vol) + "%", 1500);
                ESP_LOGI(TAG, "Volume down: %d%%", new_vol);
                break;
            }
            case KEY_RIGHT: {
                // Brightness up
                uint8_t current_br = backlight->brightness();
                int step = (current_br <= (MIN_BRIGHTNESS + 20) || current_br >= 80) ? 1 : 10;
                int new_br = std::min(100, (int)current_br + step);
                backlight->SetBrightness(new_br, true);
                display_->ShowNotification(
                    std::string(Lang::Strings::BRIGHTNESS) + std::to_string(new_br) + "%", 1500);
                ESP_LOGI(TAG, "Brightness up: %d%%", new_br);
                break;
            }
            case KEY_LEFT: {
                // Brightness down (minimum 30%)
                uint8_t current_br = backlight->brightness();
                int step = (current_br <= (MIN_BRIGHTNESS + 20) || current_br >= 80) ? 1 : 10;
                int new_br = std::max((int)MIN_BRIGHTNESS, (int)current_br - step);
                backlight->SetBrightness(new_br, true);
                display_->ShowNotification(
                    std::string(Lang::Strings::BRIGHTNESS) + std::to_string(new_br) + "%", 1500);
                ESP_LOGI(TAG, "Brightness down: %d%%", new_br);
                break;
            }
            case KEY_ENTER: {
                // Match boot button behavior (start/stop chat depending on current state).
                if (app.GetDeviceState() != kDeviceStateStarting) {
                    app.ToggleChatState();
                    ESP_LOGI(TAG, "Enter key: Toggle chat state");
                }
                break;
            }
            default:
                break;
        }
    }

    void StartKeyboardWifiConfig() {
        ESP_LOGI(TAG, "Starting keyboard WiFi config UI");
        wifi_config_mode_ = true;
        wifi_config_ui_ = std::make_unique<WifiConfigUI>(display_);
        wifi_config_ui_->SetConnectCallback(
            [this](const std::string& ssid, const std::string& password) {
                AttemptWifiConnection(ssid, password);
            });
        wifi_config_ui_->Start();
    }

    void StartKeyboardWifiConfigSaved() {
        ESP_LOGI(TAG, "Starting keyboard WiFi config UI (saved list)");
        wifi_config_mode_ = true;
        wifi_config_ui_ = std::make_unique<WifiConfigUI>(display_);
        wifi_config_ui_->SetConnectCallback(
            [this](const std::string& ssid, const std::string& password) {
                AttemptWifiConnection(ssid, password);
            });
        wifi_config_ui_->StartWithSavedList();
    }

    void AttemptWifiConnection(const std::string& ssid, const std::string& password) {
        ESP_LOGI(TAG, "Attempting WiFi connection to: %s", ssid.c_str());

        // The direct scanner deliberately runs outside WifiManager.  Return
        // control of the radio before asking WifiManager to own the station
        // connection again.
        StopOnDeviceWifiConfigRadio();

        // Add to SSID manager (will be saved and used for connection)
        auto& ssid_manager = SsidManager::GetInstance();
        ssid_manager.AddSsid(ssid, password);

        // Stop config AP mode and trigger reconnection with new credentials
        auto& wifi_manager = WifiManager::GetInstance();
        if (wifi_manager.IsConfigMode()) {
            wifi_manager.StopConfigAp();
        }

        // Start station mode to connect
        wifi_manager.StartStation();

        // Wait for connection result (with timeout)
        bool connected = false;
        for (int i = 0; i < 100; i++) {  // 10 second timeout
            vTaskDelay(pdMS_TO_TICKS(100));
            if (wifi_manager.IsConnected()) {
                connected = true;
                break;
            }
        }

        if (wifi_config_ui_) {
            wifi_config_ui_->OnConnectResult(connected);
        }
    }

    void ExitWifiConfigMode() {
        ESP_LOGI(TAG, "Exiting keyboard WiFi config mode");
        StopOnDeviceWifiConfigRadio();
        wifi_config_mode_ = false;
        wifi_config_ui_.reset();

        // Restart normal WiFi connection flow
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateWifiConfiguring) {
            // Try to connect with saved credentials
            TryWifiConnect();
        }
    }

public:
    M5StackCardputerAdvBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        I2cDetect();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeButtons();
        InitializeKeyboard();
        InitializeBatteryMonitor();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        // Cardputer Adv (no MCLK, internal clocking) needs I2S channels
        // disabled after construction so esp_codec_dev_open can configure
        // the ES8311 codec before channels start running.
        static struct CardputerAdvEs8311 : public Es8311AudioCodec {
            CardputerAdvEs8311(void* i2c, i2c_port_t port, int in_rate, int out_rate,
                               gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout,
                               gpio_num_t din, gpio_num_t pa, uint8_t addr, bool use_mclk)
                : Es8311AudioCodec(i2c, port, in_rate, out_rate, mclk, bclk, ws, dout, din, pa,
                                   addr, use_mclk) {
                i2s_channel_disable(tx_handle_);
                i2s_channel_disable(rx_handle_);
            }
        } audio_codec(i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                      AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                      AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
                      AUDIO_CODEC_ES8311_ADDR,
                      false);  // use_mclk = false
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        // M5GFX uses 256Hz PWM frequency for Cardputer backlight
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 256);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (battery_monitor_ == nullptr) {
            return false;
        }
        charging = battery_monitor_->IsCharging();
        discharging = battery_monitor_->IsDischarging();
        level = battery_monitor_->GetBatteryLevel();
        return true;
    }
};

DECLARE_BOARD(M5StackCardputerAdvBoard);
