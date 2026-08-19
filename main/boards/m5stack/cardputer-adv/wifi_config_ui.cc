#include "wifi_config_ui.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <inttypes.h>
#include <ssid_manager.h>
#include <wifi_manager.h>
#include <cstring>
#include "assets/lang_config.h"

#define TAG "WifiConfigUI"

namespace {

lv_obj_t* CreateLabel(lv_obj_t* parent) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    return label;
}

}  // namespace

WifiConfigUI::WifiConfigUI(LcdDisplay* display)
    : display_(display),
      state_(WifiConfigState::Scanning),
      is_active_(false),
      selected_index_(0),
      scroll_offset_(0),
      saved_selected_index_(0),
      saved_scroll_offset_(0),
      input_focus_on_password_(false),
      cursor_visible_(true),
      last_cursor_toggle_(0) {}

WifiConfigUI::~WifiConfigUI() {
    if (container_ != nullptr) {
        lv_obj_del(container_);
        container_ = nullptr;
    }
}

lv_obj_t* WifiConfigUI::GetContainer() {
    if (container_ == nullptr) {
        lv_obj_t* screen = lv_scr_act();
        uint32_t child_count_before = lv_obj_get_child_cnt(screen);

        // Follow LcdDisplay::SetupUI's known-good full-screen container pattern.
        container_ = lv_obj_create(screen);
        lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_radius(container_, 0, 0);
        lv_obj_set_style_pad_all(container_, 0, 0);
        lv_obj_set_style_border_width(container_, 0, 0);
        lv_obj_set_style_bg_color(container_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_color(container_, lv_color_hex(0x000000), 0);
        lv_obj_set_scrollbar_mode(container_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_move_foreground(container_);

        // Temporary diagnostics for the next on-device WiFi UI invocation.
        // Remove or simplify after the background rendering issue is confirmed resolved.
        lv_color_t container_bg_color = lv_obj_get_style_bg_color(container_, LV_PART_MAIN);
        lv_opa_t container_bg_opa = lv_obj_get_style_bg_opa(container_, LV_PART_MAIN);
        lv_color_t screen_bg_color = lv_obj_get_style_bg_color(screen, LV_PART_MAIN);
        lv_opa_t screen_bg_opa = lv_obj_get_style_bg_opa(screen, LV_PART_MAIN);
        uint32_t child_count_after = lv_obj_get_child_cnt(screen);
        int32_t container_index = lv_obj_get_index(container_);
        ESP_LOGI(TAG,
                 "Temporary UI diagnostics: screen children=%" PRIu32 " -> %" PRIu32
                 ", container index=%" PRId32 ", container bg=0x%08" PRIX32 ", opa=%u",
                 child_count_before, child_count_after, container_index,
                 lv_color_to_u32(container_bg_color), static_cast<unsigned>(container_bg_opa));
        ESP_LOGI(TAG, "Temporary UI diagnostics: screen bg=0x%08" PRIX32 ", opa=%u",
                 lv_color_to_u32(screen_bg_color), static_cast<unsigned>(screen_bg_opa));
    }

    lv_obj_clean(container_);
    return container_;
}

void WifiConfigUI::Start() {
    ESP_LOGI(TAG, "Starting WiFi config UI");
    is_active_ = true;
    state_ = WifiConfigState::Scanning;
    selected_index_ = 0;
    scroll_offset_ = 0;
    input_ssid_.clear();
    input_password_.clear();
    selected_ssid_.clear();

    // Load saved WiFi list
    LoadSavedWifiList();

    // Start scanning
    StartScanning();
}

void WifiConfigUI::StartWithSavedList() {
    ESP_LOGI(TAG, "Starting WiFi config UI with saved list");
    is_active_ = true;
    selected_index_ = 0;
    scroll_offset_ = 0;
    input_ssid_.clear();
    input_password_.clear();
    selected_ssid_.clear();

    // Show saved list directly (ShowSavedList will load the list)
    ShowSavedList();
}

void WifiConfigUI::StartScanning() {
    state_ = WifiConfigState::Scanning;

    lv_obj_t* canvas = GetContainer();
    DrawHeader(Lang::Strings::SCANNING_WIFI);
    DrawFooter(Lang::Strings::PLEASE_WAIT);

    // Perform WiFi scan
    DoWifiScan();

    // Show results
    if (scan_results_.empty()) {
        lv_obj_clean(canvas);
        DrawHeader(Lang::Strings::WIFI_NOT_FOUND);
        DrawFooter(Lang::Strings::WIFI_MANUAL_EXIT_HINT);
    } else {
        state_ = WifiConfigState::SelectWifi;
        ShowScanResults();
    }
}

void WifiConfigUI::DoWifiScan() {
    scan_results_.clear();

    // Use WifiManager's scan capability if available, otherwise do direct scan
    // Note: We need to be careful not to disrupt existing WiFi state

    // Configure scan
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 100;
    scan_config.scan_time.active.max = 300;

    // Start scan (blocking) - WiFi should already be initialized by WifiManager
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return;
    }

    // Get scan results
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count > 0) {
        wifi_ap_record_t* ap_records = new wifi_ap_record_t[ap_count];
        esp_wifi_scan_get_ap_records(&ap_count, ap_records);

        for (int i = 0; i < ap_count && i < 20; i++) {
            WifiScanResult result;
            result.ssid = std::string(reinterpret_cast<char*>(ap_records[i].ssid));
            result.rssi = ap_records[i].rssi;
            result.is_encrypted = (ap_records[i].authmode != WIFI_AUTH_OPEN);

            // Skip empty SSIDs
            if (!result.ssid.empty()) {
                scan_results_.push_back(result);
            }
        }

        delete[] ap_records;
    }

    ESP_LOGI(TAG, "Found %d WiFi networks", (int)scan_results_.size());
}

void WifiConfigUI::ShowScanResults() {
    DrawWifiList(scan_results_, selected_index_, scroll_offset_);
}

void WifiConfigUI::ShowPasswordInput() {
    // Only clear password and set state on first entry (not on redraw)
    if (state_ != WifiConfigState::InputPassword) {
        state_ = WifiConfigState::InputPassword;
        input_password_.clear();
    }

    RedrawPasswordInput();
}

void WifiConfigUI::RedrawPasswordInput() {
    lv_obj_t* canvas = GetContainer();

    DrawHeader(Lang::Strings::WIFI_ENTER_PASSWORD);

    // Show selected SSID
    lv_obj_t* label = CreateLabel(canvas);
    lv_label_set_text_fmt(label, "%s%s", Lang::Strings::CONNECT_TO, selected_ssid_.c_str());
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 5, 5);

    lv_obj_t* pwd_label = CreateLabel(canvas);
    lv_label_set_text(pwd_label, Lang::Strings::WIFI_PASSWORD);
    lv_obj_align(pwd_label, LV_ALIGN_TOP_LEFT, 5, 30);

    lv_obj_t* input_label = CreateLabel(canvas);
    std::string display_pwd(input_password_.length(), '*');
    display_pwd += cursor_visible_ ? "_" : " ";
    lv_label_set_text_fmt(input_label, ">>> %s", display_pwd.c_str());
    lv_obj_align(input_label, LV_ALIGN_TOP_LEFT, 5, 55);

    DrawFooter(Lang::Strings::WIFI_CONFIRM_BACK_HINT);
}

void WifiConfigUI::ShowManualInput() {
    // Only clear inputs and set state on first entry (not on redraw)
    if (state_ != WifiConfigState::InputSsid && state_ != WifiConfigState::InputManualPwd) {
        state_ = WifiConfigState::InputSsid;
        input_ssid_.clear();
        input_password_.clear();
        input_focus_on_password_ = false;
    }

    RedrawManualInput();
}

void WifiConfigUI::RedrawManualInput() {
    lv_obj_t* canvas = GetContainer();

    DrawHeader(Lang::Strings::WIFI_MANUAL_SETUP);

    lv_obj_t* ssid_label = CreateLabel(canvas);
    lv_label_set_text(ssid_label, "SSID:");
    lv_obj_align(ssid_label, LV_ALIGN_TOP_LEFT, 5, 25);

    lv_obj_t* ssid_input = CreateLabel(canvas);
    std::string ssid_display = ">>> " + input_ssid_;
    if (!input_focus_on_password_) {
        ssid_display += cursor_visible_ ? "_" : " ";
    }
    lv_label_set_text(ssid_input, ssid_display.c_str());
    lv_obj_align(ssid_input, LV_ALIGN_TOP_LEFT, 5, 45);

    lv_obj_t* pwd_label = CreateLabel(canvas);
    lv_label_set_text(pwd_label, Lang::Strings::WIFI_PASSWORD);
    lv_obj_align(pwd_label, LV_ALIGN_TOP_LEFT, 5, 70);

    lv_obj_t* pwd_input = CreateLabel(canvas);
    std::string pwd_display = ">>> " + std::string(input_password_.length(), '*');
    if (input_focus_on_password_) {
        pwd_display += cursor_visible_ ? "_" : " ";
    }
    lv_label_set_text(pwd_input, pwd_display.c_str());
    lv_obj_align(pwd_input, LV_ALIGN_TOP_LEFT, 5, 90);

    DrawFooter(Lang::Strings::WIFI_MANUAL_CONFIRM_BACK_HINT);
}

void WifiConfigUI::ShowSavedList() {
    state_ = WifiConfigState::SavedList;
    saved_selected_index_ = 0;
    saved_scroll_offset_ = 0;

    LoadSavedWifiList();
    DrawSavedWifiList();
}

void WifiConfigUI::DrawSavedWifiList() {
    lv_obj_t* canvas = GetContainer();

    char title[48];
    snprintf(title, sizeof(title), Lang::Strings::WIFI_SAVED_NETWORKS,
             (int)saved_wifi_list_.size());
    DrawHeader(title);

    if (saved_wifi_list_.empty()) {
        lv_obj_t* empty_label = CreateLabel(canvas);
        lv_label_set_text(empty_label, Lang::Strings::WIFI_NO_SAVED_NETWORKS);
        lv_obj_align(empty_label, LV_ALIGN_CENTER, 0, 0);
        DrawFooter(Lang::Strings::WIFI_BACK_HINT);
        return;
    }

    int y_offset = 25;
    int visible_count =
        std::min((int)saved_wifi_list_.size() - saved_scroll_offset_, MAX_VISIBLE_ITEMS);

    for (int i = 0; i < visible_count; i++) {
        int idx = saved_scroll_offset_ + i;
        bool is_selected = (idx == saved_selected_index_);

        lv_obj_t* item_label = CreateLabel(canvas);
        char item_text[48];
        snprintf(item_text, sizeof(item_text), "%s %d. %s", is_selected ? ">" : " ", idx + 1,
                 saved_wifi_list_[idx].first.c_str());
        lv_label_set_text(item_label, item_text);
        lv_obj_align(item_label, LV_ALIGN_TOP_LEFT, 5, y_offset);
        y_offset += 20;
    }

    DrawFooter(Lang::Strings::WIFI_SAVED_LIST_HINT);
}

void WifiConfigUI::ShowConnecting() {
    state_ = WifiConfigState::Connecting;

    lv_obj_t* canvas = GetContainer();

    DrawHeader(Lang::Strings::CONNECTING);

    lv_obj_t* ssid_label = CreateLabel(canvas);
    lv_label_set_text_fmt(ssid_label, Lang::Strings::WIFI_CONNECTING_TO, selected_ssid_.c_str());
    lv_obj_align(ssid_label, LV_ALIGN_CENTER, 0, 0);

    DrawFooter(Lang::Strings::PLEASE_WAIT);
}

void WifiConfigUI::ShowSuccess() {
    state_ = WifiConfigState::Success;

    lv_obj_t* canvas = GetContainer();

    DrawHeader(Lang::Strings::CONNECTION_SUCCESSFUL);

    lv_obj_t* ssid_label = CreateLabel(canvas);
    lv_label_set_text_fmt(ssid_label, "%s%s", Lang::Strings::CONNECTED_TO, selected_ssid_.c_str());
    lv_obj_align(ssid_label, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t* saved_label = CreateLabel(canvas);
    lv_label_set_text(saved_label, Lang::Strings::WIFI_SETTINGS_SAVED);
    lv_obj_align(saved_label, LV_ALIGN_CENTER, 0, 15);

    DrawFooter(Lang::Strings::WIFI_CONTINUE_HINT);
}

void WifiConfigUI::ShowFailed() {
    state_ = WifiConfigState::Failed;

    lv_obj_t* canvas = GetContainer();

    DrawHeader(Lang::Strings::WIFI_CONNECTION_FAILED);

    lv_obj_t* ssid_label = CreateLabel(canvas);
    lv_label_set_text_fmt(ssid_label, Lang::Strings::WIFI_CANNOT_CONNECT_TO,
                          selected_ssid_.c_str());
    lv_obj_align(ssid_label, LV_ALIGN_CENTER, 0, 0);

    DrawFooter(Lang::Strings::WIFI_RETRY_BACK_HINT);
}

void WifiConfigUI::DrawHeader(const char* title) {
    if (container_ == nullptr) {
        return;
    }
    lv_obj_t* canvas = container_;

    lv_obj_t* header = CreateLabel(canvas);
    lv_label_set_text(header, title);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 5, 2);
}

void WifiConfigUI::DrawFooter(const char* hint) {
    if (container_ == nullptr) {
        return;
    }
    lv_obj_t* canvas = container_;

    lv_obj_t* footer = CreateLabel(canvas);
    lv_label_set_text(footer, hint);
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_14, 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 5, -2);
}

void WifiConfigUI::DrawWifiList(const std::vector<WifiScanResult>& list, int selected, int scroll) {
    lv_obj_t* canvas = GetContainer();

    DrawHeader(Lang::Strings::WIFI_SELECT_NETWORK);

    int y_offset = 25;
    int visible_count = std::min((int)list.size() - scroll, MAX_VISIBLE_ITEMS);

    for (int i = 0; i < visible_count; i++) {
        int idx = scroll + i;
        bool is_selected = (idx == selected);
        const WifiScanResult& wifi = list[idx];

        lv_obj_t* item_label = CreateLabel(canvas);
        std::string signal = GetSignalBars(wifi.rssi);
        char item_text[64];
        snprintf(item_text, sizeof(item_text), "%s%d.%-12s %4ddBm %s", is_selected ? ">" : " ",
                 idx + 1, wifi.ssid.substr(0, 12).c_str(), wifi.rssi, signal.c_str());
        lv_label_set_text(item_label, item_text);
        lv_obj_align(item_label, LV_ALIGN_TOP_LEFT, 2, y_offset);
        y_offset += 20;
    }

    DrawFooter(Lang::Strings::WIFI_SELECT_NETWORK_HINT);
}

std::string WifiConfigUI::GetSignalBars(int8_t rssi) {
    if (rssi >= -50)
        return "████";
    if (rssi >= -60)
        return "███░";
    if (rssi >= -70)
        return "██░░";
    if (rssi >= -80)
        return "█░░░";
    return "░░░░";
}

void WifiConfigUI::LoadSavedWifiList() {
    saved_wifi_list_.clear();
    auto& ssid_manager = SsidManager::GetInstance();
    const auto& ssid_list = ssid_manager.GetSsidList();

    for (const auto& item : ssid_list) {
        saved_wifi_list_.push_back({item.ssid, item.password});
    }
}

void WifiConfigUI::SaveWifiCredentials(const std::string& ssid, const std::string& password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
    ESP_LOGI(TAG, "Saved WiFi credentials for: %s", ssid.c_str());
}

void WifiConfigUI::DeleteSavedWifi(int index) {
    if (index >= 0 && index < (int)saved_wifi_list_.size()) {
        auto& ssid_manager = SsidManager::GetInstance();
        ssid_manager.RemoveSsid(index);
        ESP_LOGI(TAG, "Deleted saved WiFi at index: %d", index);
        LoadSavedWifiList();
    }
}

void WifiConfigUI::AttemptConnection() {
    ShowConnecting();

    if (connect_callback_) {
        connect_callback_(selected_ssid_, input_password_);
    }
}

void WifiConfigUI::OnConnectResult(bool success) {
    if (success) {
        SaveWifiCredentials(selected_ssid_, input_password_);
        ShowSuccess();
    } else {
        ShowFailed();
    }
}

WifiConfigResult WifiConfigUI::HandleKeyEvent(const KeyEvent& event) {
    // Only handle key press events, skip modifiers
    if (!event.pressed || event.is_modifier) {
        return WifiConfigResult::None;
    }

    // Check for ESC to cancel from Scanning or SelectWifi states
    // (other states handle ESC in their own handlers to navigate back)
    if (event.key_code == KC_ESC) {
        if (state_ == WifiConfigState::Scanning || state_ == WifiConfigState::SelectWifi) {
            is_active_ = false;
            return WifiConfigResult::Cancelled;
        }
    }

    // Check if not active (was cancelled in a handler)
    if (!is_active_) {
        return WifiConfigResult::Cancelled;
    }

    switch (state_) {
        case WifiConfigState::Scanning:
            HandleScanningKey(event);
            break;
        case WifiConfigState::SelectWifi:
            HandleSelectWifiKey(event);
            break;
        case WifiConfigState::InputPassword:
            HandlePasswordInputKey(event);
            break;
        case WifiConfigState::InputSsid:
        case WifiConfigState::InputManualPwd:
            HandleManualInputKey(event);
            break;
        case WifiConfigState::SavedList:
            HandleSavedListKey(event);
            break;
        case WifiConfigState::Connecting:
            HandleConnectingKey(event);
            break;
        case WifiConfigState::Success:
            HandleResultKey(event);
            if (event.key_code == KC_ENTER) {
                is_active_ = false;
                return WifiConfigResult::Connected;
            }
            break;
        case WifiConfigState::Failed:
            HandleResultKey(event);
            break;
    }

    // Check if cancelled by a handler
    if (!is_active_) {
        return WifiConfigResult::Cancelled;
    }

    return WifiConfigResult::None;
}

void WifiConfigUI::HandleScanningKey(const KeyEvent& event) {
    if (event.key_code == KC_W) {
        ShowManualInput();
    } else if (event.key_code == KC_S) {
        ShowSavedList();
    }
    // ESC is handled in HandleKeyEvent
}

void WifiConfigUI::HandleSelectWifiKey(const KeyEvent& event) {
    switch (event.key_code) {
        case KC_UP:
        case KC_SEMICOLON:  // ; key as UP
            if (selected_index_ > 0) {
                selected_index_--;
                if (selected_index_ < scroll_offset_) {
                    scroll_offset_ = selected_index_;
                }
                ShowScanResults();
            }
            break;

        case KC_DOWN:
        case KC_DOT:  // . key as DOWN
            if (selected_index_ < (int)scan_results_.size() - 1) {
                selected_index_++;
                if (selected_index_ >= scroll_offset_ + MAX_VISIBLE_ITEMS) {
                    scroll_offset_ = selected_index_ - MAX_VISIBLE_ITEMS + 1;
                }
                ShowScanResults();
            }
            break;

        case KC_ENTER:
            if (!scan_results_.empty()) {
                selected_ssid_ = scan_results_[selected_index_].ssid;
                ShowPasswordInput();
            }
            break;

        case KC_W:
            ShowManualInput();
            break;

        case KC_S:
            ShowSavedList();
            break;

        default:
            break;
    }
    // ESC is handled in HandleKeyEvent
}

void WifiConfigUI::HandlePasswordInputKey(const KeyEvent& event) {
    switch (event.key_code) {
        case KC_ENTER:
            if (!input_password_.empty()) {
                AttemptConnection();
            }
            break;

        case KC_ESC:
            state_ = WifiConfigState::SelectWifi;
            ShowScanResults();
            break;

        case KC_BACKSPACE:
            if (!input_password_.empty()) {
                input_password_.pop_back();
                RedrawPasswordInput();
            }
            break;

        case KC_SPACE:
            if (input_password_.length() < MAX_INPUT_LENGTH) {
                input_password_ += ' ';
                RedrawPasswordInput();
            }
            break;

        default:
            // Add character if it's a printable key
            if (event.key_char && strlen(event.key_char) > 0 &&
                input_password_.length() < MAX_INPUT_LENGTH) {
                input_password_ += event.key_char;
                RedrawPasswordInput();
            }
            break;
    }
}

void WifiConfigUI::HandleManualInputKey(const KeyEvent& event) {
    std::string* current_input = input_focus_on_password_ ? &input_password_ : &input_ssid_;

    switch (event.key_code) {
        case KC_TAB:
            input_focus_on_password_ = !input_focus_on_password_;
            if (input_focus_on_password_) {
                state_ = WifiConfigState::InputManualPwd;
            } else {
                state_ = WifiConfigState::InputSsid;
            }
            RedrawManualInput();
            break;

        case KC_ENTER:
            if (!input_ssid_.empty()) {
                selected_ssid_ = input_ssid_;
                AttemptConnection();
            }
            break;

        case KC_ESC:
            state_ = WifiConfigState::SelectWifi;
            ShowScanResults();
            break;

        case KC_BACKSPACE:
            if (!current_input->empty()) {
                current_input->pop_back();
                RedrawManualInput();
            }
            break;

        case KC_SPACE:
            if (current_input->length() < MAX_INPUT_LENGTH) {
                *current_input += ' ';
                RedrawManualInput();
            }
            break;

        default:
            // Add character if it's a printable key
            if (event.key_char && strlen(event.key_char) > 0 &&
                current_input->length() < MAX_INPUT_LENGTH) {
                *current_input += event.key_char;
                RedrawManualInput();
            }
            break;
    }
}

void WifiConfigUI::HandleSavedListKey(const KeyEvent& event) {
    switch (event.key_code) {
        case KC_UP:
        case KC_SEMICOLON:
            if (saved_selected_index_ > 0) {
                saved_selected_index_--;
                if (saved_selected_index_ < saved_scroll_offset_) {
                    saved_scroll_offset_ = saved_selected_index_;
                }
                DrawSavedWifiList();
            }
            break;

        case KC_DOWN:
        case KC_DOT:
            if (saved_selected_index_ < (int)saved_wifi_list_.size() - 1) {
                saved_selected_index_++;
                if (saved_selected_index_ >= saved_scroll_offset_ + MAX_VISIBLE_ITEMS) {
                    saved_scroll_offset_ = saved_selected_index_ - MAX_VISIBLE_ITEMS + 1;
                }
                DrawSavedWifiList();
            }
            break;

        case KC_ENTER:
            if (!saved_wifi_list_.empty()) {
                selected_ssid_ = saved_wifi_list_[saved_selected_index_].first;
                input_password_ = saved_wifi_list_[saved_selected_index_].second;
                AttemptConnection();
            }
            break;

        case KC_BACKSPACE:  // Del key for delete
            if (!saved_wifi_list_.empty()) {
                DeleteSavedWifi(saved_selected_index_);
                if (saved_selected_index_ >= (int)saved_wifi_list_.size() &&
                    saved_selected_index_ > 0) {
                    saved_selected_index_--;
                }
                DrawSavedWifiList();
            }
            break;

        case KC_ESC:
            state_ = WifiConfigState::SelectWifi;
            ShowScanResults();
            break;

        default:
            break;
    }
}

void WifiConfigUI::HandleConnectingKey(const KeyEvent& event) {
    // No key handling during connection
    (void)event;
}

void WifiConfigUI::HandleResultKey(const KeyEvent& event) {
    if (state_ == WifiConfigState::Success) {
        if (event.key_code == KC_ENTER) {
            // Will be handled in HandleKeyEvent to return Connected
        }
    } else if (state_ == WifiConfigState::Failed) {
        if (event.key_code == KC_ENTER) {
            // Retry - go back to password input (keep password for retry)
            state_ = WifiConfigState::InputPassword;
            RedrawPasswordInput();
        } else if (event.key_code == KC_ESC) {
            state_ = WifiConfigState::SelectWifi;
            ShowScanResults();
        }
    }
}

void WifiConfigUI::UpdateCursor() {
    if (container_ == nullptr) {
        return;
    }

    uint32_t now = esp_log_timestamp();
    if (now - last_cursor_toggle_ >= CURSOR_BLINK_MS) {
        cursor_visible_ = !cursor_visible_;
        last_cursor_toggle_ = now;

        // Refresh display for input states (use Redraw functions to avoid clearing input)
        if (state_ == WifiConfigState::InputPassword) {
            RedrawPasswordInput();
        } else if (state_ == WifiConfigState::InputSsid ||
                   state_ == WifiConfigState::InputManualPwd) {
            RedrawManualInput();
        }
    }
}
