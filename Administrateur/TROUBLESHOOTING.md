# Troubleshooting — Tablette Administrateur Tab5

## 1. Programmation / Arduino IDE

| Symptôme | Cause probable | Solution |
|---|---|---|
| PlatformIO bloqué sur « Loading tasks » | L'ESP32-P4 est mal supporté par la plateforme PlatformIO officielle | Utiliser **Arduino IDE 2.x** avec le paquet de cartes M5Stack |
| Carte **M5Tab5** absente de la liste | Paquet de cartes M5Stack trop ancien | Gestionnaire de cartes : mettre **M5Stack** à jour (3.2.2 ou plus), puis redémarrer l'IDE. La liste défile : utiliser la flèche ▲ |
| Erreur `Missing FQBN` | Aucune carte n'est sélectionnée | **Outils → Carte → M5Stack → M5Tab5** |
| Aucun port COM | Câble de charge seule, ou tablette pas en mode téléchargement | Utiliser un câble **data**. Appui long sur reset jusqu'au clignotement rapide de la LED verte |
| Échec du téléversement | Port occupé par le moniteur série ou un autre logiciel | Fermer le moniteur série et les autres terminaux, puis réessayer |
| Écran noir après le téléversement | Bibliothèques M5Unified ou M5GFX trop anciennes (plusieurs versions de dalle existent) | Mettre à jour **M5Unified** et **M5GFX** vers la dernière version. Appuyer sur reset |
| Erreur de compilation ArduinoJson (`JsonDocument`) | ArduinoJson v6 installé | Installer **ArduinoJson v7** |

## 2. Wi-Fi

| Symptôme | Cause probable | Solution |
|---|---|---|
| Bandeau « WiFi -- » | SSID ou mot de passe incorrect | Vérifier `WIFI_SSID` et `WIFI_PASS`, en respectant les majuscules et les espaces |
| « WiFi -- » avec un partage de connexion iPhone | Le partage émet en 5 GHz | Activer **Réglages → Partage de connexion → Optimiser la compatibilité**. La Tab5 ne gère que le **2,4 GHz** |
| « WiFi -- » sur le Wi-Fi de l'école | Réseau d'entreprise (eduroam, identifiant personnel) non supporté | Utiliser un partage de connexion ou un routeur dédié |
| Le Wi-Fi ne démarre jamais | `WiFi.setPins(...)` absent ou appelé après `WiFi.begin()` | La Tab5 passe par l'ESP32-C6 en SDIO : `setPins` doit être appelé **avant** `WiFi.begin` |

## 3. MQTT

Le bandeau affiche `MQTT erreur rc=X`. Signification des codes :

| rc | Signification | Que faire |
|---|---|---|
| **-4** | Pas de réponse dans le délai | Le réseau est lent, ou une mauvaise IP pointe vers un autre appareil |
| **-3** | Connexion perdue | Le broker a redémarré, ou le Wi-Fi est instable |
| **-2** | **Broker injoignable** | Voir la liste juste en dessous |
| 2 | Identifiant client refusé | Un autre client utilise le même ID |
| 4 / 5 | Mauvais identifiant ou mot de passe, connexion non autorisée | Vérifier `MQTT_USER` / `MQTT_PASSW`, ou `allow_anonymous true` |

**Que vérifier pour `rc=-2` :**
1. **L'IP du broker** : sur le PC, taper `ipconfig` et lire l'**adresse IPv4 de la carte Wi-Fi**. Elle doit être identique à `MQTT_HOST`. Un partage iPhone peut changer cette IP d'une connexion à l'autre.
2. **Le même réseau** : le PC, la Tab5 admin et les postes doivent être connectés au même Wi-Fi.
3. **Mosquitto lancé et ouvert** : taper `netstat -an | findstr 1883` sur le PC.
   - Aucune ligne : Mosquitto n'est pas lancé.
   - `127.0.0.1:1883` : ajouter `listener 1883 0.0.0.0` et `allow_anonymous true` dans `mosquitto.conf`, puis redémarrer le service.
   - `0.0.0.0:1883 LISTENING` : la configuration est correcte.
4. **Le pare-feu Windows** : ajouter la règle (invite de commande en administrateur) :
   `netsh advfirewall firewall add rule name="Mosquitto" dir=in action=allow protocol=TCP localport=1883`
5. **Un test d'isolement** : mettre `MQTT_HOST = "broker.hivemq.com"`.
   - Si on obtient « MQTT OK », le problème vient du PC ou du pare-feu.
   - Si on a toujours rc=-2, le réseau bloque le port 1883 ou isole les appareils entre eux.

## 4. Messages non affichés

| Symptôme | Cause probable | Solution |
|---|---|---|
| « MQTT OK » mais les widgets restent en attente | Personne ne publie, ou les noms de topics ne correspondent pas | Lancer `mosquitto_sub -h <IP> -t "#" -v` pour voir **tout** le trafic, puis comparer avec `Detection` / `Demande_critique` (majuscules, `_`, accents) |
| Bandeau « JSON invalide sur Detection » | Le message n'est pas du JSON valide | Vérifier les guillemets, les virgules (`"id": 1,`) et les valeurs `true`/`false` sans guillemets |
| Bandeau « Pas d'id sur ... » | Le champ `id` est absent du message | Ajouter `"id": 1` dans le message publié par le poste |
| Pastilles EPI toujours rouges | Noms de clés différents | Utiliser `Casque`, `Gilet`, `Chaussures`, dans l'objet `"Detection"` |
| Plus de 6 postes ignorés | `MAX_TABS = 6` | Augmenter `MAX_TABS` et adapter la grille (`COLS` / `ROWS`) |
| Le poste ne réagit pas à AUTORISER | Le poste n'écoute pas le bon topic, ou ne filtre pas son `id` | Le poste doit s'abonner à `Autorisation_critique` et comparer `id` avec son propre numéro |

## 5. Commandes de diagnostic utiles (invite de commande Windows)

Ouvrir l'invite : touche **Windows**, taper `cmd`, puis **Invite de commandes**. Pour la version administrateur : clic droit, puis **Exécuter en tant qu'administrateur**.

```
ipconfig                                   :: IP du PC
netstat -an | findstr 1883                 :: Mosquitto écoute-t-il ?
mosquitto_sub -h 172.20.10.2 -t "#" -v     :: voir tous les messages
```

Le **moniteur série** d'Arduino IDE (115200 bauds) affiche l'IP de la Tab5, les messages reçus et les commandes envoyées.
