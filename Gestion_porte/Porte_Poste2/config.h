// =====================================================================
//  config.h  -  Parametres de la Tab5 porte du POSTE 2
//  -> C'est le SEUL fichier a modifier pour adapter le projet.
// =====================================================================
#pragma once

// ---------------- Reseau Wi-Fi ----------------
#define WIFI_SSID      "VOTRE_SSID"
#define WIFI_PASS      "VOTRE_MOT_DE_PASSE"

// ---------------- Broker MQTT (Node-RED / Aedes) ----------------
// ATTENTION : mettre l'adresse IP LAN du PC qui fait tourner Node-RED
// (ipconfig / ip a), PAS 127.0.0.1 (qui designerait la Tab5 elle-meme).
#define MQTT_HOST      "XXX.XXX.X.XX"
#define MQTT_PORT      1883
#define MQTT_CLIENT_ID "M5Tab5-Porte"   // + "-P2-xxxx" ajoute automatiquement (unique)
#define MQTT_USER      ""            // vide = pas d'authentification
#define MQTT_PASS      ""

// ---------------- Identifiant du poste ----------------
// Chaque Tab5 "porte" est rattachee a UN poste de detection.
// Elle n'obeit qu'aux messages portant ce meme "id" et ajoute cet "id"
// a tout ce qu'elle publie.  Poste 1 -> 1, poste 2 -> 2, etc.
#define POSTE_ID     2      // <<< POSTE 2 : ne pas changer dans ce dossier
// true : un message SANS champ "id" est ignore (recommande).
// false : un message sans "id" est accepte (compatibilite ancien format).
#define EXIGER_ID    true

// ---------------- Topics ----------------
#define TOPIC_DETECTION    "Detection"     // publie par la Tab5 camera (EPI)
#define TOPIC_DEMANDE      "demande"       // demande d'ouverture critique
#define TOPIC_AUTORISATION "autorisation"  // decision de l'administrateur
// Topics propres a la porte : l'id du poste est ajoute a la fin
// -> porte/etat/1, porte/connexion/1 (un message retenu par poste)
#define TOPIC_ETAT_PORTE   "porte/etat"
#define TOPIC_CONNEXION    "porte/connexion"

// ---------------- Publisher DoorState (lu par le dashboard admin) ----------------
#define TOPIC_DOOR_STATE   "DoorState"
// true  -> {"DoorState":{"id":1,"State":true}}   (meme convention que {"Detection":{...}})
// false -> {"id":1,"State":true}
#define DOORSTATE_ENVELOPPE  true
#define DOORSTATE_RETAIN     true
// Republication periodique (ms) : tous les postes publient sur le MEME topic
// DoorState, or le broker ne garde qu'UN message retenu par topic. La
// republication garantit que le dashboard recoit l'etat de chaque poste
// peu apres sa connexion. 0 = desactive.
#define DOORSTATE_PERIODE_MS 10000
// State = true des que la porte quitte la position fermee,
// State = false seulement quand le servo est revenu en butee "fermee".

// ---------------- Servomoteur ----------------
// Port A (Grove) de la Tab5 : fil JAUNE = G53, fil BLANC = G54.
#define SERVO_PIN            53
#define SERVO_ANGLE_FERME    0       // angle porte fermee (a ajuster sur la maquette)
#define SERVO_ANGLE_OUVERT   90      // angle porte ouverte
#define SERVO_US_MIN         500     // largeur d'impulsion a 0 deg  (SG90/MG90S : 500)
#define SERVO_US_MAX         2500    // largeur d'impulsion a 180 deg (SG90/MG90S : 2500)
#define SERVO_VITESSE_DEG_S  120     // vitesse de rotation (mouvement doux)

// ---------------- Temporisations ----------------
#define DUREE_OUVERTURE_MS        8000    // porte ouverte puis refermeture auto
#define DELAI_REPONSE_ADMIN_MS    60000   // au-dela : demande d'urgence expiree
#define DUREE_AFFICHAGE_REFUS_MS  5000    // duree d'affichage "acces refuse"
#define IGNORER_RETENUS_MS        1500    // ignore les messages "retained" recus juste apres l'abonnement

// ---------------- Regles de conformite ----------------
// Dans le flow Node-RED fourni, "Chaussures" est consideree comme OK
// quand elle vaut false (test d.Chaussures !== false). On reproduit la
// meme regle ici. Mettre true si le detecteur envoie Chaussures:true
// quand les chaussures sont portees.
#define CHAUSSURES_VALEUR_OK      false

// Si la trame contient un champ "Personne" indiquant "aucune", on l'ignore.
#define IGNORER_SI_PERSONNE_ABSENTE  true

// La decision de l'admin n'est prise en compte que si une demande
// d'ouverture critique est en cours (securite).
#define EXIGER_DEMANDE_AVANT_AUTORISATION  true
