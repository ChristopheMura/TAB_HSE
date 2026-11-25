// Interface LVGL : bandeau de verdict + une ligne par indicateur (personne, EPI).
#include "ui.h"
#include <stdio.h>
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_timer.h"

static const char *ITEM_NAMES[UI_ITEM_COUNT] = {"Casque", "Gants", "Gilet", "Chaussures", "Lunettes"};

static lv_obj_t *s_banner = NULL;
static lv_obj_t *s_banner_label = NULL;
static lv_obj_t *s_rows[1 + UI_ITEM_COUNT];        // 0 = personne, puis EPI
static lv_obj_t *s_row_labels[1 + UI_ITEM_COUNT];
static lv_obj_t *s_row_states[1 + UI_ITEM_COUNT];
static lv_obj_t *s_footer = NULL;
static lv_obj_t *s_net_label = NULL;
static lv_obj_t *s_btn = NULL;
static lv_obj_t *s_btn_label = NULL;
static ui_critical_cb_t s_critical_cb = NULL;
static volatile int64_t s_override_until_us = 0;   // bandeau reserve a la reponse critique
static bool s_waiting_autor = false;

static const lv_font_t *font_big(void)
{
#if LV_FONT_MONTSERRAT_40
    return &lv_font_montserrat_40;
#elif LV_FONT_MONTSERRAT_28
    return &lv_font_montserrat_28;
#else
    return LV_FONT_DEFAULT;
#endif
}

static const lv_font_t *font_mid(void)
{
#if LV_FONT_MONTSERRAT_28
    return &lv_font_montserrat_28;
#else
    return LV_FONT_DEFAULT;
#endif
}

static lv_color_t state_color(ui_state_t s)
{
    switch (s) 
    {
        case UI_OK:   return lv_palette_main(LV_PALETTE_GREEN);
        case UI_BAD:  return lv_palette_main(LV_PALETTE_RED);
        case UI_WARN: return lv_palette_main(LV_PALETTE_ORANGE);
        default:      return lv_palette_darken(LV_PALETTE_GREY, 1);
    }
}

static const char *state_text(ui_state_t s, bool required)
{
    switch (s) 
    {
        case UI_OK:   return "OK";
        case UI_BAD:  return required ? "MANQUANT" : "absent";
        case UI_WARN: return "ATTENTION";
        default:      return "...";
    }
}

static lv_obj_t *make_row(lv_obj_t *parent, const char *name, lv_obj_t **out_label, lv_obj_t **out_state)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 90);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_color(row, state_color(UI_UNKNOWN), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, name);
    lv_obj_set_style_text_font(label, font_mid(), 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);

    lv_obj_t *state = lv_label_create(row);
    lv_label_set_text(state, "...");
    lv_obj_set_style_text_font(state, font_mid(), 0);
    lv_obj_set_style_text_color(state, lv_color_white(), 0);

    *out_label = label;
    *out_state = state;
    return row;
}

// Execute dans la tache LVGL (verrou deja pris) : ne pas appeler bsp_display_lock ici
static void critical_btn_event(lv_event_t *e)
{
    (void)e;
    if (s_critical_cb == NULL) return;
    if (s_critical_cb() == 0) 
    {
        s_waiting_autor = true;
        lv_label_set_text(s_btn_label, "Demande envoyee, en attente...");
    } 
    else 
    {
        lv_label_set_text(s_btn_label, "Echec : pas de connexion MQTT");
    }
}

void ui_init(void)
{
    bsp_display_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scr, 24, 0);
    lv_obj_set_style_pad_row(scr, 16, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, ("Controle des EPI"));
    lv_obj_set_style_text_font(title, font_mid(), 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    s_banner = lv_obj_create(scr);
    lv_obj_set_size(s_banner, lv_pct(100), 170);
    lv_obj_set_style_radius(s_banner, 16, 0);
    lv_obj_set_style_border_width(s_banner, 0, 0);
    lv_obj_set_style_bg_color(s_banner, state_color(UI_UNKNOWN), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    s_banner_label = lv_label_create(s_banner);
    lv_label_set_text(s_banner_label, "En attente");
    lv_obj_set_style_text_font(s_banner_label, font_big(), 0);
    lv_obj_set_style_text_color(s_banner_label, lv_color_white(), 0);
    lv_obj_center(s_banner_label);

    s_rows[0] = make_row(scr, "Personne", &s_row_labels[0], &s_row_states[0]);
    for (int i = 0; i < UI_ITEM_COUNT; i++) 
    {
        s_rows[1 + i] = make_row(scr, ITEM_NAMES[i], &s_row_labels[1 + i], &s_row_states[1 + i]);
    }

    // Bouton rouge de demande d'ouverture critique
    s_btn = lv_button_create(scr);
    lv_obj_set_size(s_btn, lv_pct(100), 120);
    lv_obj_set_style_radius(s_btn, 16, 0);
    lv_obj_set_style_bg_color(s_btn, lv_palette_darken(LV_PALETTE_RED, 2), 0);
    lv_obj_set_style_bg_color(s_btn, lv_palette_darken(LV_PALETTE_RED, 4), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_btn, critical_btn_event, LV_EVENT_CLICKED, NULL);
    s_btn_label = lv_label_create(s_btn);
    lv_label_set_text(s_btn_label, "Demande d'ouverture critique");
    lv_obj_set_style_text_font(s_btn_label, font_mid(), 0);
    lv_obj_set_style_text_color(s_btn_label, lv_color_white(), 0);
    lv_obj_center(s_btn_label);

    s_footer = lv_label_create(scr);
    lv_label_set_text(s_footer, "");
    lv_obj_set_style_text_color(s_footer, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);

    s_net_label = lv_label_create(scr);
    lv_label_set_text(s_net_label, "Wi-Fi : ...   MQTT : ...");
    lv_obj_set_style_text_color(s_net_label, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);

    bsp_display_unlock();
}

void ui_set_critical_cb(ui_critical_cb_t cb) 
{ 
    s_critical_cb = cb; 
}

void ui_set_net_status(bool wifi_ok, bool mqtt_ok)
{
    if (s_net_label == NULL) return;
    bsp_display_lock(0);
    lv_label_set_text_fmt(s_net_label, "Wi-Fi : %s   MQTT : %s",
                          wifi_ok ? "connecte" : "deconnecte", mqtt_ok ? "connecte" : "deconnecte");
    lv_obj_set_style_text_color(s_net_label, (wifi_ok && mqtt_ok) ? lv_palette_main(LV_PALETTE_GREEN)
                                                                   : lv_palette_main(LV_PALETTE_ORANGE), 0);
    bsp_display_unlock();
}

void ui_show_autorisation(bool autorise)
{
    if (s_banner == NULL) return;
    bsp_display_lock(0);
    s_waiting_autor = false;
    lv_label_set_text(s_btn_label, "Demande d'ouverture critique");
    lv_label_set_text(s_banner_label, autorise ? "OUVERTURE CRITIQUE AUTORISEE" : "OUVERTURE CRITIQUE REFUSEE");
    lv_obj_set_style_bg_color(s_banner, autorise ? lv_palette_main(LV_PALETTE_BLUE) : lv_palette_main(LV_PALETTE_RED), 0);
    s_override_until_us = esp_timer_get_time() + (int64_t)UI_AUTOR_DISPLAY_S * 1000000;
    bsp_display_unlock();
}

void ui_update(const ui_status_t *s)
{
    if (s_banner == NULL) return;

    bsp_display_lock(0);

    // Bandeau (laisse a la reponse critique tant qu'elle est affichee)
    const bool override = esp_timer_get_time() < s_override_until_us;
    const char *txt = "En attente";
    ui_state_t banner_state = UI_UNKNOWN;
    switch (s->verdict) 
    {
        case UI_VERDICT_NO_PERSON:    txt = "Placez-vous devant la camera"; banner_state = UI_UNKNOWN; break;
        case UI_VERDICT_MULTI_PERSON: txt = "Une personne a la fois";        banner_state = UI_WARN;    break;
        case UI_VERDICT_CHECKING:     txt = "Verification...";               banner_state = UI_UNKNOWN; break;
        case UI_VERDICT_DENIED:       txt = "ACCES REFUSE";                   banner_state = UI_BAD;     break;
        case UI_VERDICT_GRANTED:      txt = "ACCES AUTORISE";                 banner_state = UI_OK;      break;
    }
    if (!override) 
    {
        lv_label_set_text(s_banner_label, txt);
        lv_obj_set_style_bg_color(s_banner, state_color(banner_state), 0);
    }

    // Personne
    lv_obj_set_style_bg_color(s_rows[0], state_color(s->person), 0);
    lv_label_set_text(s_row_states[0], s->person == UI_OK ? "detectee" :
                                       s->person == UI_WARN ? "plusieurs" : "aucune");

    // EPI
    for (int i = 0; i < UI_ITEM_COUNT; i++) 
    {
        ui_state_t st = s->items[i];
        if (!s->required[i] && st == UI_BAD) st = UI_UNKNOWN;   // un EPI facultatif absent n'est pas une faute
        lv_obj_set_style_bg_color(s_rows[1 + i], state_color(st), 0);
        lv_label_set_text(s_row_states[1 + i], state_text(s->items[i], s->required[i]));
        lv_label_set_text_fmt(s_row_labels[1 + i], "%s%s", ITEM_NAMES[i], s->required[i] ? " *" : "");
    }

    lv_label_set_text_fmt(s_footer, "Inference : %d ms   (* = obligatoire)", s->infer_ms);

    bsp_display_unlock();
}