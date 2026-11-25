#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Rappels appeles depuis la tache MQTT (ne pas y bloquer)
typedef void (*net_status_cb_t)(bool wifi_ok, bool mqtt_ok);
typedef void (*net_autorisation_cb_t)(bool autorise);

void net_set_status_cb(net_status_cb_t cb);
void net_set_autorisation_cb(net_autorisation_cb_t cb);

// Allume le C6, connecte le Wi-Fi puis le client MQTT (non bloquant). 0 si le demarrage est lance.
int net_start(void);

bool net_mqtt_connected(void);

// Publications non bloquantes (mise en file). Renvoient 0 si le message est accepte.
int net_publish_demande_critique(void);
int net_publish_detection(bool casque, bool gilet, bool chaussures);

#ifdef __cplusplus
}
#endif