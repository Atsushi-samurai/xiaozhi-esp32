#include "persona_select_ui.h"

#include <algorithm>

PersonaSelectUI::PersonaSelectUI(LcdDisplay* display) : display_(display) {}

PersonaSelectUI::~PersonaSelectUI() {
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
}

void PersonaSelectUI::CreateRoot() {
    if (root_ != nullptr) {
        lv_obj_delete(root_);
    }

    root_ = lv_obj_create(lv_scr_act());
    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
}

void PersonaSelectUI::DrawHeader(const char* title) {
    lv_obj_t* header = lv_label_create(root_);
    lv_label_set_text(header, title);
    lv_obj_set_style_text_color(header, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 5, 2);
}

void PersonaSelectUI::DrawFooter(const char* hint) {
    lv_obj_t* footer = lv_label_create(root_);
    lv_label_set_text(footer, hint);
    lv_obj_set_style_text_color(footer, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_14, 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 5, -2);
}

void PersonaSelectUI::ShowLoading() {
    state_ = State::Loading;
    CreateRoot();
    DrawHeader("ペルソナを読み込み中...");

    lv_obj_t* message = lv_label_create(root_);
    lv_label_set_text(message, "しばらくお待ちください");
    lv_obj_set_style_text_color(message, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, 0);
    DrawFooter("Esc: キャンセル");
}

void PersonaSelectUI::ShowPersonas(std::vector<PersonaOption> personas) {
    personas_ = std::move(personas);
    selected_index_ = 0;
    scroll_offset_ = 0;
    state_ = State::Selecting;
    DrawPersonas();
}

void PersonaSelectUI::ShowSwitching(const std::string& name) {
    state_ = State::Switching;
    CreateRoot();
    DrawHeader("ペルソナを切替中...");

    lv_obj_t* message = lv_label_create(root_);
    lv_label_set_text(message, name.c_str());
    lv_obj_set_style_text_color(message, lv_color_hex(0xFFFF00), 0);
    lv_obj_set_width(message, LV_PCT(95));
    lv_label_set_long_mode(message, LV_LABEL_LONG_DOT);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, 0);
    DrawFooter("しばらくお待ちください");
}

void PersonaSelectUI::ShowError(const std::string& message) {
    state_ = State::Error;
    CreateRoot();
    DrawHeader("ペルソナを取得できません");

    lv_obj_t* detail = lv_label_create(root_);
    lv_label_set_text(detail, message.c_str());
    lv_obj_set_style_text_color(detail, lv_color_hex(0xFF6666), 0);
    lv_obj_set_width(detail, LV_PCT(95));
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_align(detail, LV_ALIGN_CENTER, 0, 0);
    DrawFooter("Enter: 再試行  Esc: 戻る");
}

void PersonaSelectUI::DrawPersonas() {
    CreateRoot();
    DrawHeader("ペルソナを選択");

    if (personas_.empty()) {
        ShowError("利用可能なペルソナがありません");
        return;
    }

    int y_offset = 25;
    const int visible_count =
        std::min(static_cast<int>(personas_.size()) - scroll_offset_, MAX_VISIBLE_ITEMS);
    for (int i = 0; i < visible_count; ++i) {
        const int index = scroll_offset_ + i;
        const auto& persona = personas_[index];
        const bool selected = index == selected_index_;

        lv_obj_t* item = lv_label_create(root_);
        std::string text = selected ? "> " : "  ";
        text += std::to_string(index + 1);
        text += ". ";
        text += persona.name;
        if (persona.active) {
            text += " *";
        }
        lv_label_set_text(item, text.c_str());
        lv_obj_set_width(item, LV_PCT(96));
        lv_label_set_long_mode(item, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(item,
                                    selected ? lv_color_hex(0x00FF00) : lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(item, LV_ALIGN_TOP_LEFT, 5, y_offset);
        y_offset += 20;
    }

    DrawFooter(";/. : 選択  Enter: 決定  Esc: 戻る");
}

PersonaSelectResult PersonaSelectUI::HandleKeyEvent(const KeyEvent& event) {
    if (!event.pressed || event.is_modifier) {
        return PersonaSelectResult::None;
    }

    if (state_ == State::Loading || state_ == State::Switching) {
        return PersonaSelectResult::None;
    }
    if (event.key_code == KC_ESC) {
        return PersonaSelectResult::Cancelled;
    }
    if (state_ == State::Error) {
        return event.key_code == KC_ENTER ? PersonaSelectResult::Retry : PersonaSelectResult::None;
    }
    if (personas_.empty()) {
        return PersonaSelectResult::None;
    }

    switch (event.key_code) {
        case KC_UP:
        case KC_SEMICOLON:
            if (selected_index_ > 0) {
                --selected_index_;
                if (selected_index_ < scroll_offset_) {
                    scroll_offset_ = selected_index_;
                }
                DrawPersonas();
            }
            break;
        case KC_DOWN:
        case KC_DOT:
            if (selected_index_ < static_cast<int>(personas_.size()) - 1) {
                ++selected_index_;
                if (selected_index_ >= scroll_offset_ + MAX_VISIBLE_ITEMS) {
                    scroll_offset_ = selected_index_ - MAX_VISIBLE_ITEMS + 1;
                }
                DrawPersonas();
            }
            break;
        case KC_ENTER:
            return PersonaSelectResult::Selected;
        default:
            break;
    }
    return PersonaSelectResult::None;
}

const PersonaOption& PersonaSelectUI::selected_persona() const {
    return personas_[selected_index_];
}
