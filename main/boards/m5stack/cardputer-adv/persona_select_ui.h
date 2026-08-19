#ifndef PERSONA_SELECT_UI_H
#define PERSONA_SELECT_UI_H

#include "display/lcd_display.h"
#include "tca8418_keyboard.h"

#include <string>
#include <vector>

struct PersonaOption {
    std::string agent_id;
    std::string name;
    bool active = false;
};

enum class PersonaSelectResult {
    None,
    Cancelled,
    Selected,
    Retry,
};

class PersonaSelectUI {
public:
    explicit PersonaSelectUI(LcdDisplay* display);
    ~PersonaSelectUI();

    void ShowLoading();
    void ShowPersonas(std::vector<PersonaOption> personas);
    void ShowSwitching(const std::string& name);
    void ShowError(const std::string& message);
    PersonaSelectResult HandleKeyEvent(const KeyEvent& event);

    const PersonaOption& selected_persona() const;

private:
    enum class State {
        Loading,
        Selecting,
        Switching,
        Error,
    };

    static constexpr int MAX_VISIBLE_ITEMS = 4;

    LcdDisplay* display_;
    lv_obj_t* root_ = nullptr;
    State state_ = State::Loading;
    std::vector<PersonaOption> personas_;
    int selected_index_ = 0;
    int scroll_offset_ = 0;

    void CreateRoot();
    void DrawHeader(const char* title);
    void DrawFooter(const char* hint);
    void DrawPersonas();
};

#endif  // PERSONA_SELECT_UI_H
