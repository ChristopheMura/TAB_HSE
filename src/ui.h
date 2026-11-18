#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { UI_UNKNOWN = 0, UI_OK, UI_BAD, UI_WARN } ui_state_t;

typedef enum {
    UI_VERDICT_NO_PERSON = 0,
    UI_VERDICT_MULTI_PERSON,
    UI_VERDICT_CHECKING,
    UI_VERDICT_DENIED,
    UI_VERDICT_GRANTED,
} ui_verdict_t;

#define UI_ITEM_COUNT 5   // casque, gants, gilet, chaussures, lunettes

typedef struct {
    ui_state_t   person;                   // UI_OK : une personne, UI_WARN : plusieurs, UI_UNKNOWN : aucune
    ui_state_t   items[UI_ITEM_COUNT];     // etat lisse de chaque EPI
    bool         required[UI_ITEM_COUNT];  // EPI obligatoire ou non
    ui_verdict_t verdict;
    int          infer_ms;                 // temps de la derniere inference
} ui_status_t;

// Construit l'ecran. A appeler une fois, apres bsp_display_start().
void ui_init(void);

// Met a jour les indicateurs. Peut etre appelee depuis n'importe quelle tache.
void ui_update(const ui_status_t *s);

// Bouton rouge "Demande d'ouverture critique"
typedef int (*ui_critical_cb_t)(void);
void ui_set_critical_cb(ui_critical_cb_t cb);

// Etat du reseau
void ui_set_net_status(bool wifi_ok, bool mqtt_ok);

// Reponse a la demande critique : prend le bandeau pendant UI_AUTOR_DISPLAY_S secondes.
#define UI_AUTOR_DISPLAY_S 10
void ui_show_autorisation(bool autorise);

#ifdef __cplusplus
}
#endif