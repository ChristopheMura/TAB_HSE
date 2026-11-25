# TAB_HSE — Tablette Administrateur (M5Stack Tab5)

Interface de supervision qui tourne sur une **M5Stack Tab5** (ESP32-P4). Elle affiche en temps réel l'état des tablettes « postes ». Ces postes détectent les personnes et leurs EPI (casque, gilet, chaussures). L'administrateur peut **autoriser ou refuser le passage** quand un poste envoie une **demande critique**.

---

## Architecture

```
  Tab5 Poste 1 ─┐                                     ┌─ Tab5 ADMIN
  Tab5 Poste 2 ─┼──► Broker MQTT (Mosquitto, PC) ◄────┤  (ce code)
  Tab5 Poste N ─┘      172.20.10.2:1883               └─
```

Tous les appareils doivent être sur **le même réseau Wi-Fi** que le broker.

## Topics et messages MQTT

| Topic | Admin | Exemple de message |
|---|---|---|
| `Detection` | **lit** (subscriber) | `{"id":1,"Detection":{"Casque":true,"Gilet":false,"Chaussures":true}}` |
| `Demande_critique` | **lit** (subscriber) | `{"id":1,"Bouton":true}` |
| `Autorisation_critique` | **publie** (publisher) | `{"id":1,"Autorisation":true}` |

- Le champ `id` identifie le poste, sous forme de nombre ou de texte.
- `Autorisation: true` laisse passer la personne, `false` refuse le passage.

## Fonctionnalités de l'interface

- **Un widget par poste**, jusqu'à 6 postes, affichés dès le démarrage :
  - trois pastilles EPI : vertes si l'équipement est porté, rouges s'il manque, grises « ? » tant qu'aucune donnée n'est arrivée ;
  - bordure **verte** si les EPI sont complets, **rouge** s'il en manque, **orange clignotante** en cas de demande critique, **grise** si le poste est hors ligne (aucun message depuis 15 s).
- **Demande critique** : la Tab5 bipe et affiche les boutons tactiles **AUTORISER** et **REFUSER**. L'appui publie la décision sur `Autorisation_critique`.
- **Bandeau** en haut de l'écran : postes en ligne, alertes en cours, état du Wi-Fi et du MQTT, dernier événement.
- **Mode démo** (`DEMO_MODE = true`) : génère de fausses données pour présenter l'interface sans broker ni autres tablettes.

## Contenu du dossier

| Fichier | Rôle |
|---|---|
| `Tab5_Admin_EPI/Tab5_Admin_EPI.ino` | Programme Arduino de la tablette administrateur |
| `simulateur_postes.py` | Script Python qui simule des postes (pour tester sans les autres Tab5) |
| `TROUBLESHOOTING.md` | Problèmes fréquents et solutions |

## Installation (Arduino IDE 2.x)

1. **Fichier → Préférences → URL de gestionnaire de cartes supplémentaires** :
   `https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json`
2. **Outils → Carte → Gestionnaire de cartes** : installer **M5Stack**, version 3.2.2 ou plus récente.
3. **Gérer les bibliothèques** : installer **M5Unified**, **M5GFX**, **PubSubClient** (Nick O'Leary) et **ArduinoJson** (v7).
4. Ouvrir `Tab5_Admin_EPI/Tab5_Admin_EPI.ino`.
5. **Outils → Carte → M5Stack → M5Tab5**, puis choisir le port COM.
6. Passer en mode téléchargement : appui long (environ 2 s) sur **reset**, jusqu'à ce que la LED verte clignote vite.
7. Cliquer sur **Téléverser**, puis appuyer une fois sur reset.

## Configuration

En haut du fichier `.ino` :

```cpp
const char* WIFI_SSID = "NOM_DU_WIFI";        // réseau 2,4 GHz uniquement
const char* WIFI_PASS = "MOT_DE_PASSE";
const char* MQTT_HOST = "172.20.10.2";        // IP du PC qui fait tourner Mosquitto
const char* TOPIC_DETECTION    = "Detection";
const char* TOPIC_DEMANDE      = "Demande_critique";
const char* TOPIC_AUTORISATION = "Autorisation_critique";
const char* POSTES_PREVUS[] = { "1", "2", "3", "4", "5", "6" };
const bool  DEMO_MODE = false;
```

> ⚠️ Ne jamais pousser un vrai mot de passe Wi-Fi sur GitHub.

## Broker Mosquitto (sur le PC)

1. Installer Mosquitto depuis https://mosquitto.org/download
2. Ajouter dans `mosquitto.conf` :
   ```
   listener 1883 0.0.0.0
   allow_anonymous true
   ```
3. Redémarrer le service, dans une invite de commande en administrateur :
   `net stop mosquitto` puis `net start mosquitto`
4. Ouvrir le port dans le pare-feu, toujours en administrateur :
   ```
   netsh advfirewall firewall add rule name="Mosquitto" dir=in action=allow protocol=TCP localport=1883
   ```

## Tester

**Voir les décisions de l'admin :**
```
mosquitto_sub -h 172.20.10.2 -t Autorisation_critique -v
```

**Simuler un poste :**
```
mosquitto_pub -h 172.20.10.2 -t Detection -m "{\"id\":1,\"Detection\":{\"Casque\":true,\"Gilet\":false,\"Chaussures\":true}}"
mosquitto_pub -h 172.20.10.2 -t Demande_critique -m "{\"id\":1,\"Bouton\":true}"
```

**Simuler plusieurs postes avec le script Python :**
```
pip install paho-mqtt
python simulateur_postes.py --broker 172.20.10.2
```
