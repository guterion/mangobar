#ifndef MANGOBAR_STYLE_H
#define MANGOBAR_STYLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MANGOBAR_MAX_STYLE_RULES 128
#define MANGOBAR_MAX_SELECTORS 16
#define MANGOBAR_MAX_VARS 64

// Single style declaration block
typedef struct {
    uint32_t color; // ARGB foreground
    uint32_t background; // ARGB background
    uint32_t gradient_end; // ARGB end color for linear-gradient backgrounds
    int gradient_dir; // 0=to bottom (default) 1=to top 2=to left 3=to right
    bool bg_gradient; // background is a linear gradient (start = background)
    uint32_t border_color; // ARGB border (menu, bar and modules)
    int border_width; // border thickness in pixels; 0 = none
    int padding_left;
    int padding_right;
    int margin_top;
    int margin_left;
    int margin_right;
    int radius; // corner radius; -1 = pill, >=0 explicit (0 = square)
    int min_width; // minimum module width
    bool color_set;
    bool background_set;
    bool border_color_set;
    bool border_width_set;
    bool padding_left_set;
    bool padding_right_set;
    bool margin_top_set;
    bool margin_left_set;
    bool margin_right_set;
    bool radius_set;
    bool min_width_set;
} Style;

// One selector rule
typedef struct {
    char module[32]; // module name, "*" = all
    char state[32]; // state (active/occupied/urgent/...), empty = none
    Style style;
} StyleRule;

typedef struct {
    StyleRule rules[MANGOBAR_MAX_STYLE_RULES];
    int rule_count;
    // Global font settings (last font-* wins)
    char font_family[128];
    int font_size; // 0 = unset
    char font_weight[32]; // empty = unset
    bool font_set;
    // CSS variables (@define-color)
    char var_names[MANGOBAR_MAX_VARS][64];
    char var_values[MANGOBAR_MAX_VARS][64];
    int var_count;
    // Tray context menu styles
    Style menu;
    Style menuitem;
    Style menuitem_hover;
    int menu_font_size; // 0 = use default
    int menu_radius; // 0 = use default 10
    bool loaded;
} StyleSheet;

// Init style sheet (zeroed)
void style_sheet_init(StyleSheet *ss);

// Load CSS from file; 0 on success, -1 on failure
int style_sheet_load(StyleSheet *ss, const char *path);
// Parse CSS text into the style sheet; returns 0 on success
int style_sheet_parse(StyleSheet *ss, const char *buf);

// Resolve final style for module+state in cascade order:
// "*", "#module", "#module.state"; later rules override earlier ones
Style style_resolve(const StyleSheet *ss, const char *module, const char *state);

// Resolve only the #module / #module.state rules (skip "*"), so aliases like
// #workspaces can be overlaid without re-applying the global defaults.
Style style_resolve_module_only(const StyleSheet *ss, const char *module,
                                const char *state);

// Overlay `over` on top of `base`: fields set in `over` replace base values.
Style style_overlay(const Style *base, const Style *over);

// Build an fcft font string from CSS font settings.
// Returns fallback when no font is set. Static buffer.
const char *style_font_string(const StyleSheet *ss, const char *fallback);

// Find default style file path; 0 found (fills out), -1 not found
int style_find_default_path(char *out, size_t outsz);

#endif
