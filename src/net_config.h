#pragma once
// Parametres reseau du projet TAB_HSE : a adapter

#define NET_WIFI_SSID      "iPhone de Christophe"
#define NET_WIFI_PASSWORD  "Aw2dytop"

// Broker MQTT (Node-RED avec un noeud broker type aedes, ou Mosquitto) sur le PC 192.168.1.10
#define NET_MQTT_URI       "mqtt://192.168.1.10:1883"
#define NET_MQTT_CLIENT_ID "tab5-hse"

#define NET_TOPIC_DEMANDE       "demande"       // publie : appui sur le bouton critique
#define NET_TOPIC_AUTORISATION  "autorisation"  // souscrit : reponse a la demande critique
#define NET_TOPIC_DETECTION     "detection"     // publie : resultat du controle des EPI

// 1 = publier aussi quand l'acces est refuse (valeurs reelles, utile pour la tracabilite HSE)
// 0 = publier uniquement quand l'acces est valide (comportement demande)
#define NET_PUBLISH_ON_DENIED 0