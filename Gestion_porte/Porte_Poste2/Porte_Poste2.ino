// =====================================================================
//  Porte_Poste2.ino  (code identique pour tous les postes, seul config.h change)
//  Subscriber MQTT "Porte" - M5Stack Tab5 (ESP32-P4 + ESP32-C6 Wi-Fi)
//
//  - S'abonne a : Detection (EPI), demande (ouverture critique),
//                 autorisation (decision de l'administrateur)
//  - Decide d'ouvrir / fermer la porte et pilote un servomoteur
//  - PUBLISHER : publie {"DoorState":{"id":N,"State":true|false}} sur DoorState
//    (dashboard admin) + etat detaille sur porte/etat/<id>
//  - POSTE_ID : la porte n obeit qu aux messages portant son "id"
//  - Affiche une IHM animee : porte rouge/verte + decor de Noel
//
//  Carte : "M5Tab5" (M5Stack board manager >= 3.2.2)
//  Bibliotheques : M5Unified, M5GFX, PubSubClient (Nick O'Leary),
//                  ArduinoJson (v7, Benoit Blanchon)
// =====================================================================
#include <M5Unified.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"

// ---------- Liaison SDIO vers le module Wi-Fi ESP32-C6 de la Tab5 ----------
#ifndef SDIO2_CLK
#define SDIO2_CLK GPIO_NUM_12
#define SDIO2_CMD GPIO_NUM_13
#define SDIO2_D0  GPIO_NUM_11
#define SDIO2_D1  GPIO_NUM_10
#define SDIO2_D2  GPIO_NUM_9
#define SDIO2_D3  GPIO_NUM_8
#define SDIO2_RST GPIO_NUM_15
#endif

#define SERVO_FREQ_HZ     50
#define SERVO_RESOLUTION  14   // bits LEDC

// =====================================================================
//  Objets globaux
// =====================================================================
WiFiClient   netClient;
PubSubClient mqtt(netClient);
M5Canvas     cv(&M5.Display);      // tampon d'ecran (PSRAM) -> pas de scintillement
int W = 720, H = 1280;

// ---------- Machine d'etats ----------
enum Mode : uint8_t { M_ATTENTE, M_OUVERT_EPI, M_ATTENTE_ADMIN, M_OUVERT_URGENCE, M_REFUSE };
static const char* NOM_MODE[] = { "ATTENTE", "OUVERT_EPI", "ATTENTE_ADMIN", "OUVERT_URGENCE", "REFUSE" };
Mode     mode = M_ATTENTE;
uint32_t tMode = 0;            // instant d'entree dans le mode courant
uint32_t tFinOuverture = 0;    // instant de refermeture automatique
String   motif = "Placez-vous devant la camera";

enum Urgence : uint8_t { U_AUCUNE, U_ATTENTE, U_ACCEPTEE, U_REFUSEE, U_EXPIREE };
static const char* NOM_URG[] = { "aucune", "en_attente", "acceptee", "refusee", "expiree" };
Urgence  urg = U_AUCUNE;
uint32_t tUrg = 0;

// ---------- EPI (etat : 1 = porte, 0 = manquant, -1 = inconnu) ----------
struct Epi { const char* nom; bool obligatoire; int8_t etat; };
Epi epis[] = {
  { "Casque",     true,  -1 },
  { "Gilet",      true,  -1 },
  { "Chaussures", true,  -1 },
  { "Gants",      false, -1 },
  { "Lunettes",   false, -1 },
};
const int NB_EPI = sizeof(epis) / sizeof(epis[0]);
bool     detectionRecue = false;
uint32_t tDetection = 0;

// ---------- Servomoteur ----------
float servoAngle = SERVO_ANGLE_FERME;
float servoCible = SERVO_ANGLE_FERME;
bool  servoOk = false;

// ---------- MQTT ----------
uint32_t tDerniereTentative = 0;
uint32_t tAbonnement = 0;
String   dernierEtatPublie;
int8_t   dernierDoorState = -1;    // -1 = jamais publie (force la 1re publication)

// ---------- IHM ----------
struct Rgb { uint8_t r, g, b; };
struct Flocon { float x, y, v, phase; uint8_t r; };
const int NB_FLOCONS = 70;
Flocon neige[NB_FLOCONS];

// ---------- Prototypes ----------
void changerMode(Mode m, const String& pourquoi);
void publierEtat(bool forcer);
void appliquerDecision(int8_t dec);

// =====================================================================
//  Outils
// =====================================================================
static bool porteCommandeeOuverte() { return mode == M_OUVERT_EPI || mode == M_OUVERT_URGENCE; }

static float fractionOuverture() {
  float f = (servoAngle - SERVO_ANGLE_FERME) / float(SERVO_ANGLE_OUVERT - SERVO_ANGLE_FERME);
  return constrain(f, 0.f, 1.f);
}

static String normaliser(const char* s) {
  String t(s ? s : "");
  t.trim();
  t.replace("\xC3\xA9", "e"); t.replace("\xC3\xA8", "e"); t.replace("\xC3\xAA", "e");   // é è ê
  t.replace("\xC3\x89", "e"); t.replace("\xC3\xA0", "a");                               // É à
  t.toLowerCase();
  return t;
}

// Interprete un texte libre : 1 = oui/autorise, 0 = non/refuse, -1 = inconnu
static int8_t decisionDepuisTexte(const String& brut) {
  String s = normaliser(brut.c_str());
  s.replace("\"", "");
  static const char* OUI[] = { "true", "1", "oui", "yes", "ok", "accepte", "acceptee", "autorise", "autorisee",
                               "valide", "validee", "ouvrir", "ouvert", "open", "granted", "allow" };
  static const char* NON[] = { "false", "0", "non", "no", "refuse", "refusee", "rejete", "rejetee", "fermer",
                               "ferme", "close", "denied", "deny", "annule", "annulee" };
  for (auto o : OUI) if (s == o) return 1;
  for (auto n : NON) if (s == n) return 0;
  return -1;
}

// Interprete une valeur JSON (bool, nombre, texte ou objet {"autorisation":..})
static int8_t lireDecision(JsonVariantConst v, int profondeur) {
  if (v.isNull() || profondeur > 3) return -1;
  if (v.is<bool>()) return v.as<bool>() ? 1 : 0;
  if (v.is<long>() || v.is<double>()) return v.as<double>() != 0 ? 1 : 0;
  if (v.is<const char*>()) return decisionDepuisTexte(String(v.as<const char*>()));
  if (v.is<JsonObjectConst>()) {
    static const char* CLES[] = { "autorisation", "decision", "reponse", "autorise", "accepte", "valide",
                                  "acces", "ouverture", "etat", "value", "valeur", "demande", "urgence" };
    JsonObjectConst o = v.as<JsonObjectConst>();
    for (auto c : CLES)
      for (JsonPairConst kv : o)
        if (normaliser(kv.key().c_str()) == c) return lireDecision(kv.value(), profondeur + 1);
  }
  return -1;
}

// Recherche d'une cle sans tenir compte de la casse
static JsonVariantConst chercher(JsonVariantConst o, const char* nom) {
  if (!o.is<JsonObjectConst>()) return JsonVariantConst();
  String cible = normaliser(nom);
  for (JsonPairConst kv : o.as<JsonObjectConst>())
    if (normaliser(kv.key().c_str()) == cible) return kv.value();
  return JsonVariantConst();
}

// L'objet contient-il une reponse de l'admin ?
static bool contientDecisionAdmin(JsonVariantConst o) {
  static const char* CLES[] = { "autorisation", "decision", "reponse", "autorise", "accepte", "valide" };
  for (auto c : CLES) if (!chercher(o, c).isNull()) return true;
  return false;
}

static bool personnePresente(JsonVariantConst p) {
  if (p.isNull()) return true;                       // champ absent : on suppose une personne
  if (p.is<bool>()) return p.as<bool>();
  if (p.is<long>() || p.is<double>()) return p.as<double>() > 0;
  if (p.is<const char*>()) {
    String s = normaliser(p.as<const char*>());
    return !(s == "" || s == "aucune" || s == "aucun" || s == "none" || s == "non" || s == "false" || s == "0");
  }
  return true;
}

// =====================================================================
//  Filtrage par identifiant de poste
// =====================================================================
// Cherche un champ id (id, ID, poste, id_poste, idPoste...) a la racine
// du message puis dans ses sous-objets ({"Detection":{"id":1,...}}).
static JsonVariantConst chercherId(JsonVariantConst o, int profondeur) {
  static const char* CLES[] = { "id", "poste", "id_poste", "idposte", "posteid", "poste_id" };
  for (auto c : CLES) {
    JsonVariantConst v = chercher(o, c);
    if (!v.isNull()) return v;
  }
  if (profondeur < 1 && o.is<JsonObjectConst>())
    for (JsonPairConst kv : o.as<JsonObjectConst>()) {
      JsonVariantConst v = chercherId(kv.value(), profondeur + 1);
      if (!v.isNull()) return v;
    }
  return JsonVariantConst();
}

// 1 = message pour ce poste, 0 = autre poste, -1 = pas d'id
static int8_t idCorrespond(JsonVariantConst v) {
  if (v.isNull()) return -1;
  if (v.is<long>() || v.is<double>()) return v.as<long>() == (long)POSTE_ID ? 1 : 0;
  if (v.is<const char*>()) {
    String s = normaliser(v.as<const char*>());
    s.replace("poste", ""); s.trim();             // accepte "1", "poste1", "Poste 1"
    if (s.isEmpty()) return -1;
    return s == String(POSTE_ID) ? 1 : 0;
  }
  return -1;
}

// Le message brut concerne-t-il ce poste ?
static bool messagePourCePoste(const String& brut, String& idLu) {
  JsonDocument doc;
  int8_t r = -1;
  if (!deserializeJson(doc, brut)) {
    JsonVariantConst v = chercherId(doc.as<JsonVariantConst>(), 0);
    r = idCorrespond(v);
    if (v.is<const char*>())               idLu = v.as<const char*>();
    else if (v.is<long>() || v.is<double>()) idLu = String(v.as<long>());
  }
  if (r == 1) return true;
  if (r == 0) return false;
  return !EXIGER_ID;                              // pas d'id : selon la config
}

// =====================================================================
//  Servomoteur (PWM LEDC 50 Hz, mouvement progressif non bloquant)
// =====================================================================
void servoEcrire(float angle) {
  if (!servoOk) return;
  float a = constrain(angle, 0.f, 180.f);
  uint32_t us   = SERVO_US_MIN + (uint32_t)((SERVO_US_MAX - SERVO_US_MIN) * a / 180.f);
  uint32_t duty = (uint32_t)((uint64_t)us * ((1u << SERVO_RESOLUTION) - 1) / (1000000u / SERVO_FREQ_HZ));
  ledcWrite(SERVO_PIN, duty);
}

void servoInit() {
  servoOk = ledcAttach(SERVO_PIN, SERVO_FREQ_HZ, SERVO_RESOLUTION);
  Serial.printf("[SERVO] GPIO%d %s\n", SERVO_PIN, servoOk ? "OK" : "ERREUR ledcAttach");
  servoEcrire(servoAngle);
}

void majServo(float dt) {
  if (servoAngle == servoCible) return;
  float pas = SERVO_VITESSE_DEG_S * dt;
  if (fabsf(servoCible - servoAngle) <= pas) servoAngle = servoCible;
  else servoAngle += (servoCible > servoAngle) ? pas : -pas;
  servoEcrire(servoAngle);
}

// =====================================================================
//  Logique d'acces
// =====================================================================
// Topics propres a ce poste : "porte/etat/1", "porte/connexion/1"
static String topicPoste(const char* base) { return String(base) + "/" + String(POSTE_ID); }

void publierEtat(bool forcer) {
  if (!mqtt.connected()) return;
  JsonDocument d;
  d["id"]      = POSTE_ID;
  d["porte"]   = porteCommandeeOuverte() ? "ouverte" : "fermee";
  d["mode"]    = NOM_MODE[mode];
  d["motif"]   = motif;
  d["urgence"] = NOM_URG[urg];
  String s;
  serializeJson(d, s);
  if (!forcer && s == dernierEtatPublie) return;
  if (mqtt.publish(topicPoste(TOPIC_ETAT_PORTE).c_str(), s.c_str(), true)) dernierEtatPublie = s;
}

// ---- Publisher DoorState : {"DoorState":{"id":1,"State":true|false}}
// La porte est consideree OUVERTE des que le servo n'est plus en butee
// "fermee" (y compris pendant l'ouverture et la fermeture) : securite.
static bool porteReellementOuverte() {
  return !(servoAngle == SERVO_ANGLE_FERME && servoCible == SERVO_ANGLE_FERME);
}

void publierDoorState(bool forcer) {
  int8_t etat = porteReellementOuverte() ? 1 : 0;
  static uint32_t tDernier = 0;
  bool periodique = DOORSTATE_PERIODE_MS > 0 && millis() - tDernier >= DOORSTATE_PERIODE_MS;
  if (!forcer && !periodique && etat == dernierDoorState) return;
  if (!mqtt.connected()) return;                // republie a la reconnexion
  JsonDocument d;
  JsonObject o = DOORSTATE_ENVELOPPE ? d["DoorState"].to<JsonObject>() : d.to<JsonObject>();
  o["id"]    = POSTE_ID;
  o["State"] = (etat == 1);
  String s;
  serializeJson(d, s);
  if (mqtt.publish(TOPIC_DOOR_STATE, s.c_str(), DOORSTATE_RETAIN)) {
    tDernier = millis();
    if (etat == dernierDoorState) return;       // simple republication : pas de log
    dernierDoorState = etat;
    Serial.printf("[MQTT] %s -> %s\n", TOPIC_DOOR_STATE, s.c_str());
  }
}

void changerMode(Mode m, const String& pourquoi) {
  bool etaitOuverte = porteCommandeeOuverte();
  mode  = m;
  tMode = millis();
  motif = pourquoi;
  if (porteCommandeeOuverte()) {
    servoCible    = SERVO_ANGLE_OUVERT;
    tFinOuverture = millis() + DUREE_OUVERTURE_MS;     // (re)arme la temporisation
  } else {
    servoCible = SERVO_ANGLE_FERME;
  }
  if (etaitOuverte != porteCommandeeOuverte())
    Serial.printf("[PORTE] %s  (%s)\n", porteCommandeeOuverte() ? "OUVERTURE" : "FERMETURE", pourquoi.c_str());
  publierEtat(false);
}

// ---- Trame "Detection" : {"Detection":{"Casque":true,"Gilet":true,"Chaussures":false,...}}
void traiterDetection(const String& brut) {
  JsonDocument doc;
  if (deserializeJson(doc, brut)) {
    Serial.println("[Detection] trame ignoree (vide ou non JSON)");
    return;
  }
  JsonVariantConst racine = doc.as<JsonVariantConst>();
  JsonVariantConst d = chercher(racine, "Detection");
  if (d.isNull()) d = racine;                         // accepte aussi {"Casque":true,...}
  if (!d.is<JsonObjectConst>()) return;

  if (IGNORER_SI_PERSONNE_ABSENTE && !personnePresente(chercher(d, "Personne"))) return;

  String manquants;
  bool auMoinsUnEpi = false;
  for (int i = 0; i < NB_EPI; i++) {
    JsonVariantConst v = chercher(d, epis[i].nom);
    int8_t r = v.isNull() ? -1 : lireDecision(v, 0);
    if (r < 0) {
      epis[i].etat = -1;
    } else {
      auMoinsUnEpi = true;
      bool val = (r == 1);
      if (strcmp(epis[i].nom, "Chaussures") == 0) epis[i].etat = (val == CHAUSSURES_VALEUR_OK) ? 1 : 0;
      else                                         epis[i].etat = val ? 1 : 0;
    }
    if (epis[i].obligatoire && epis[i].etat != 1) {
      if (manquants.length()) manquants += ", ";
      manquants += epis[i].nom;
    }
  }
  if (!auMoinsUnEpi) { Serial.println("[Detection] aucun EPI dans la trame"); return; }

  detectionRecue = true;
  tDetection = millis();

  // 1) EPI conformes -> ouverture
  if (manquants.isEmpty()) {
    if (urg == U_ATTENTE) { urg = U_AUCUNE; tUrg = millis(); }   // la demande n'est plus utile
    changerMode(M_OUVERT_EPI, "EPI conformes - acces autorise");
    return;
  }
  // Porte deja ouverte : on laisse la temporisation la refermer
  if (porteCommandeeOuverte()) return;

  // 2) Non conforme : une demande d'ouverture critique est-elle en cours ?
  if (urg == U_ATTENTE) {
    changerMode(M_ATTENTE_ADMIN, String("EPI manquants : ") + manquants);
    return;
  }
  // 3) Sinon : acces refuse
  changerMode(M_REFUSE, String("Acces refuse - manque : ") + manquants);
}

// ---- Decision de l'administrateur
void appliquerDecision(int8_t dec) {
  if (dec < 0) { Serial.println("[Admin] decision illisible, ignoree"); return; }
  if (EXIGER_DEMANDE_AVANT_AUTORISATION && urg != U_ATTENTE) {
    Serial.println("[Admin] decision ignoree : aucune demande critique en cours");
    return;
  }
  tUrg = millis();
  if (dec == 1) {
    urg = U_ACCEPTEE;
    changerMode(M_OUVERT_URGENCE, "Ouverture critique autorisee par l'admin");
  } else {
    urg = U_REFUSEE;
    if (!porteCommandeeOuverte()) changerMode(M_REFUSE, "Ouverture critique refusee par l'admin");
    else publierEtat(false);
  }
}

// ---- Trame "demande" (bouton "Demande d'ouverture critique" de la Tab5 camera)
void traiterDemande(const String& brut) {
  String b = brut; b.trim();
  if (b.isEmpty()) return;                       // ex. message "birth" vide de Node-RED

  JsonDocument doc;
  int8_t r;
  if (!deserializeJson(doc, b)) {
    JsonVariantConst v = doc.as<JsonVariantConst>();
    if (contientDecisionAdmin(v)) { appliquerDecision(lireDecision(v, 0)); return; }  // reponse admin sur ce topic
    r = lireDecision(v, 0);
  } else {
    r = decisionDepuisTexte(b);
  }

  if (r == 0) {                                  // demande annulee
    if (urg == U_ATTENTE) {
      urg = U_AUCUNE; tUrg = millis();
      if (mode == M_ATTENTE_ADMIN) changerMode(M_ATTENTE, "Demande critique annulee");
      else publierEtat(false);
    }
    return;
  }
  // Tout autre contenu non vide = nouvelle demande
  urg = U_ATTENTE; tUrg = millis();
  Serial.println("[Urgence] demande d'ouverture critique recue");
  if (!porteCommandeeOuverte()) changerMode(M_ATTENTE_ADMIN, "Demande critique transmise a l'admin");
  else publierEtat(false);
}

// ---- Trame "autorisation" : true/false, "accepte"/"refuse", {"autorisation":true}...
void traiterAutorisation(const String& brut) {
  String b = brut; b.trim();
  if (b.isEmpty()) return;
  JsonDocument doc;
  if (!deserializeJson(doc, b)) appliquerDecision(lireDecision(doc.as<JsonVariantConst>(), 0));
  else                          appliquerDecision(decisionDepuisTexte(b));
}

// ---- Temporisations
void majLogique(uint32_t now) {
  if (porteCommandeeOuverte() && (int32_t)(now - tFinOuverture) >= 0) {
    if (mode == M_OUVERT_URGENCE) { urg = U_AUCUNE; tUrg = now; }
    changerMode(M_ATTENTE, "Porte refermee automatiquement");
  }
  if (urg == U_ATTENTE && now - tUrg > DELAI_REPONSE_ADMIN_MS) {
    urg = U_EXPIREE; tUrg = now;
    if (mode == M_ATTENTE_ADMIN) changerMode(M_REFUSE, "Pas de reponse de l'admin");
    else publierEtat(false);
  }
  if (mode == M_REFUSE && now - tMode > DUREE_AFFICHAGE_REFUS_MS)
    changerMode(M_ATTENTE, "Placez-vous devant la camera");
  if ((urg == U_REFUSEE || urg == U_EXPIREE) && now - tUrg > 10000) {
    urg = U_AUCUNE;
    publierEtat(false);
  }
}

// =====================================================================
//  Wi-Fi / MQTT
// =====================================================================
void surMessage(char* topic, byte* payload, unsigned int len) {
  String t(topic);
  String brut; brut.reserve(len);
  for (unsigned int i = 0; i < len; i++) brut += (char)payload[i];
  Serial.printf("[MQTT] %s <- %s\n", t.c_str(), brut.c_str());

  // Les messages "retained" arrivent juste apres l'abonnement : on les ignore
  // pour ne pas ouvrir la porte sur une vieille trame au demarrage.
  if (millis() - tAbonnement < IGNORER_RETENUS_MS) { Serial.println("  (message retenu ignore)"); return; }

  // Seuls les messages portant l'id de CE poste sont traites
  String idLu;
  if (!messagePourCePoste(brut, idLu)) {
    if (idLu.length()) Serial.printf("  (ignore : poste %s, ce poste = %d)\n", idLu.c_str(), POSTE_ID);
    else               Serial.println("  (ignore : message sans id)");
    return;
  }

  if      (t == TOPIC_DETECTION)    traiterDetection(brut);
  else if (t == TOPIC_DEMANDE)      traiterDemande(brut);
  else if (t == TOPIC_AUTORISATION) traiterAutorisation(brut);
}

void wifiInit() {
  WiFi.setPins(SDIO2_CLK, SDIO2_CMD, SDIO2_D0, SDIO2_D1, SDIO2_D2, SDIO2_D3, SDIO2_RST);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WiFi] connexion a %s...\n", WIFI_SSID);
}

void gererWifi() {
  static uint32_t tTentative = millis();
  static bool etaitConnecte = false;
  bool c = (WiFi.status() == WL_CONNECTED);
  if (c && !etaitConnecte) Serial.printf("[WiFi] connecte, IP %s\n", WiFi.localIP().toString().c_str());
  etaitConnecte = c;
  if (!c && millis() - tTentative > 15000) {
    tTentative = millis();
    Serial.println("[WiFi] nouvelle tentative");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

void gererMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (tDerniereTentative != 0 && millis() - tDerniereTentative < 5000) return;
  tDerniereTentative = millis();

  String id = String(MQTT_CLIENT_ID) + "-P" + String(POSTE_ID) + "-" + String(esp_random() & 0xFFFF, HEX);
  static String topicCnx;                       // doit rester valide (last will)
  topicCnx = topicPoste(TOPIC_CONNEXION);
  Serial.printf("[MQTT] connexion a %s:%d (poste %d)...\n", MQTT_HOST, MQTT_PORT, POSTE_ID);
  bool ok = strlen(MQTT_USER)
          ? mqtt.connect(id.c_str(), MQTT_USER, MQTT_PASS, topicCnx.c_str(), 1, true, "hors_ligne")
          : mqtt.connect(id.c_str(), topicCnx.c_str(), 1, true, "hors_ligne");
  if (!ok) { Serial.printf("[MQTT] echec (rc=%d)\n", mqtt.state()); return; }

  mqtt.subscribe(TOPIC_DETECTION, 1);
  mqtt.subscribe(TOPIC_DEMANDE, 1);
  mqtt.subscribe(TOPIC_AUTORISATION, 1);
  tAbonnement = millis();
  mqtt.publish(topicCnx.c_str(), "en_ligne", true);
  publierEtat(true);
  publierDoorState(true);                      // etat courant pour le dashboard admin
  Serial.println("[MQTT] connecte et abonne");
}

// =====================================================================
//  IHM
// =====================================================================
static inline Rgb mix(Rgb a, Rgb b, float t) {
  t = constrain(t, 0.f, 1.f);
  return { (uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t), (uint8_t)(a.b + (b.b - a.b) * t) };
}
static inline uint16_t C(Rgb c) { return ((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3); }

const Rgb BLANC = {255, 255, 255}, NOIR = {0, 0, 0};
const Rgb ROUGE_HAUT = {160, 20, 32}, ROUGE_BAS = {60, 6, 14};
const Rgb VERT_HAUT  = {24, 158, 76},  VERT_BAS  = {4, 60, 30};
const Rgb OK_VERT = {46, 184, 92}, KO_ROUGE = {220, 48, 56}, AMBRE = {255, 176, 32}, GRIS = {120, 122, 135};

// Texte centre, reduit automatiquement s'il deborde, avec ombre portee
void texte(const String& s, int x, int y, int maxW, float tailleMax, Rgb coul, bool ombre) {
  cv.setTextSize(1.0f);
  int w = cv.textWidth(s);
  float sz = tailleMax;
  if (w > 0 && w * sz > maxW) sz = (float)maxW / w;
  cv.setTextSize(sz);
  if (ombre) { cv.setTextColor(C({20, 10, 10})); cv.drawString(s, x + 2, y + 3); }
  cv.setTextColor(C(coul));
  cv.drawString(s, x, y);
  cv.setTextSize(1.0f);
}

void initNeige() {
  for (auto& f : neige) {
    f.x = random(W); f.y = random(H);
    f.v = 25 + random(55); f.r = 1 + random(4);
    f.phase = random(628) / 100.f;
  }
}

void majNeige(float dt, float t) {
  for (auto& f : neige) {
    f.y += f.v * dt;
    f.x += sinf(t * 1.3f + f.phase) * 14.f * dt;
    if (f.y > H + 6) { f.y = -6; f.x = random(W); }
  }
}

void dessinerFond(float f) {
  Rgb haut = mix(ROUGE_HAUT, VERT_HAUT, f), bas = mix(ROUGE_BAS, VERT_BAS, f);
  for (int y = 0; y < H; y += 4) cv.fillRect(0, y, W, 4, C(mix(haut, bas, (float)y / H)));
}

void voyant(int x, int y, const char* label, bool ok) {
  cv.fillSmoothCircle(x, y, 11, C(BLANC));
  cv.fillSmoothCircle(x, y, 8, C(ok ? Rgb{40, 220, 90} : Rgb{235, 50, 50}));
  cv.setFont(&fonts::FreeSansBold9pt7b);
  cv.setTextDatum(middle_left);
  cv.setTextColor(C(BLANC));
  cv.drawString(label, x + 17, y);
}

void dessinerEntete() {
  cv.setFont(&fonts::FreeSansBold12pt7b);
  cv.setTextDatum(middle_left);
  cv.setTextColor(C(BLANC));
  cv.drawString("CONTROLE D'ACCES EPI", 24, 30);
  // badge du poste
  char poste[16]; snprintf(poste, sizeof(poste), "POSTE %d", POSTE_ID);
  int bw = cv.textWidth(poste) + 28, bx = 24 + cv.textWidth("CONTROLE D'ACCES EPI") + 14;
  cv.fillSmoothRoundRect(bx, 14, bw, 32, 16, C(BLANC));
  cv.setTextDatum(middle_center);
  cv.setTextColor(C({160, 20, 32}));
  cv.drawString(poste, bx + bw / 2, 30);
  voyant(W - 205, 30, "WiFi", WiFi.status() == WL_CONNECTED);
  voyant(W - 100, 30, "MQTT", mqtt.connected());
}

void noeud(int x, int y) {           // petit noeud rouge aux attaches de la guirlande
  uint16_t r = C({200, 20, 30});
  cv.fillTriangle(x, y, x - 16, y - 9, x - 16, y + 9, r);
  cv.fillTriangle(x, y, x + 16, y - 9, x + 16, y + 9, r);
  cv.fillSmoothCircle(x, y, 5, C({240, 60, 60}));
}

void dessinerGuirlande(float t, Rgb fond) {
  static const Rgb COUL[] = { {255, 60, 60}, {255, 214, 40}, {70, 200, 255}, {90, 240, 120}, {255, 120, 220} };
  const int nbSeg = 3, y0 = 62, creux = 42, nbAmp = 5;
  int idx = 0;
  for (int s = 0; s < nbSeg; s++) {
    float xa = W * s / (float)nbSeg, xb = W * (s + 1) / (float)nbSeg;
    auto pt = [&](float u, float& x, float& y) { x = xa + (xb - xa) * u; y = y0 + creux * (1 - (2 * u - 1) * (2 * u - 1)); };
    float px = xa, py = y0, x, y;
    for (int k = 1; k <= 24; k++) { pt(k / 24.f, x, y); cv.drawWideLine(px, py, x, y, 1.6f, C({18, 70, 32})); px = x; py = y; }
    for (int k = 1; k <= nbAmp; k++, idx++) {
      pt(k / (float)(nbAmp + 1), x, y);
      Rgb c = COUL[idx % 5];
      bool allumee = ((idx + (int)(t * 3.f)) % 3) != 0;
      int bx = (int)x, by = (int)y + 19;
      if (allumee) cv.fillSmoothCircle(bx, by, 22, C(mix(fond, c, 0.35f)));      // halo
      cv.fillRect(bx - 5, (int)y + 2, 10, 8, C({90, 95, 100}));                    // culot
      cv.fillEllipse(bx, by, 9, 13, C(allumee ? c : mix(c, NOIR, 0.6f)));
      if (allumee) cv.fillEllipse(bx - 3, by - 5, 2, 4, C(BLANC));
    }
  }
  for (int s = 0; s <= nbSeg; s++) noeud(W * s / nbSeg, y0);
}

void dessinerVoeux(float t) {
  cv.setFont(&fonts::Yellowtail_32);
  cv.setTextDatum(middle_center);
  float pulse = 0.5f + 0.5f * sinf(t * 2.f);
  texte("Joyeux Noel & Bonnes Fetes !", W / 2, 158, W - 80, 1.6f, mix({255, 205, 70}, {255, 240, 170}, pulse), true);
  // etincelles
  for (int i = 0; i < 4; i++) {
    float a = t * 2.f + i * 1.57f;
    int sx = (i < 2) ? 34 + i * 10 : W - 44 - (i - 2) * 10, sy = 150 + (int)(sinf(a) * 14);
    int r = 3 + (int)(2 * (0.5f + 0.5f * sinf(a * 1.7f)));
    cv.fillTriangle(sx, sy - r * 3, sx - r, sy, sx + r, sy, C({255, 240, 160}));
    cv.fillTriangle(sx, sy + r * 3, sx - r, sy, sx + r, sy, C({255, 240, 160}));
  }
}

// Porte en perspective : f = 0 fermee, 1 ouverte (suit l'angle reel du servo)
void dessinerPorte(float f, float t, Rgb fond) {
  const int fw = 320, fh = 420, fx = W / 2 - fw / 2, fy = 222;

  // mur + encadrement
  cv.fillSmoothRoundRect(fx - 36, fy - 36, fw + 72, fh + 52, 20, C(mix(fond, NOIR, 0.45f)));
  cv.fillSmoothRoundRect(fx - 24, fy - 24, fw + 48, fh + 30, 12, C({236, 226, 210}));
  // interieur : sombre quand ferme, lumiere chaude quand ouvert
  cv.fillRect(fx, fy, fw, fh, C(mix({22, 22, 32}, {255, 244, 200}, f)));
  if (f > 0.02f) {
    uint16_t lum = C(mix(fond, {255, 244, 200}, 0.35f * f));
    int y1 = fy + fh + 6, y2 = fy + fh + 52;
    cv.fillTriangle(fx, y1, fx + fw, y1, fx + fw + 70, y2, lum);
    cv.fillTriangle(fx, y1, fx + fw + 70, y2, fx - 70, y2, lum);
  }

  // battant (charniere a gauche, s'ouvre vers l'observateur)
  float th = f * 1.40f;                       // ~80 degres
  float wp = fw * cosf(th);
  float p  = fh * 0.07f * sinf(th);
  auto P = [&](float u, float v, int& x, int& y) {
    x = fx + (int)(u * wp);
    y = (int)((fy - p * u) + v * (fh + 2 * p * u));
  };
  auto quad = [&](float u0, float v0, float u1, float v1, uint16_t col) {
    int x0, y0, x1, y1, x2, y2, x3, y3;
    P(u0, v0, x0, y0); P(u1, v0, x1, y1); P(u1, v1, x2, y2); P(u0, v1, x3, y3);
    cv.fillTriangle(x0, y0, x1, y1, x2, y2, col);
    cv.fillTriangle(x0, y0, x2, y2, x3, y3, col);
  };
  if (wp > 3) {
    float sh = 1.0f - 0.45f * f;
    Rgb bois = { (uint8_t)(150 * sh), (uint8_t)(88 * sh), (uint8_t)(44 * sh) };
    Rgb fonce = mix(bois, NOIR, 0.28f), clair = mix(bois, {255, 220, 170}, 0.14f);
    quad(0, 0, 1, 1, C(bois));
    quad(0.12f, 0.05f, 0.88f, 0.44f, C(fonce)); quad(0.16f, 0.08f, 0.84f, 0.41f, C(clair));
    quad(0.12f, 0.53f, 0.88f, 0.95f, C(fonce)); quad(0.16f, 0.56f, 0.84f, 0.92f, C(clair));
    // poignee
    int hx, hy; P(0.90f, 0.49f, hx, hy);
    cv.fillSmoothCircle(hx, hy, 4 + (int)(8 * cosf(th)), C({240, 195, 60}));
    // couronne de Noel
    float sc = max(0.3f, cosf(th));
    const float cu = 0.5f, cvv = 0.24f, ru = 52.f / fw, rv = 52.f / fh;
    for (int k = 0; k < 18; k++) {
      float a = k * 6.2832f / 18;
      int x, y; P(cu + cosf(a) * ru, cvv + sinf(a) * rv, x, y);
      cv.fillSmoothCircle(x, y, (int)(15 * sc) + 1, C((k & 1) ? Rgb{20, 120, 50} : Rgb{30, 150, 64}));
    }
    for (int k = 0; k < 5; k++) {
      float a = 0.6f + k * 1.25f;
      int x, y; P(cu + cosf(a) * ru, cvv + sinf(a) * rv, x, y);
      cv.fillSmoothCircle(x, y, (int)(5 * sc) + 1, C({230, 30, 40}));
    }
    int bx, by; P(cu, cvv + rv, bx, by);
    int bw = (int)(18 * sc) + 2;
    cv.fillTriangle(bx, by, bx - bw, by - 10, bx - bw, by + 10, C({210, 20, 30}));
    cv.fillTriangle(bx, by, bx + bw, by - 10, bx + bw, by + 10, C({210, 20, 30}));
    cv.fillSmoothCircle(bx, by, 6, C({240, 60, 60}));
  }

  // pastille d'etat (cadenas / attente / coche)
  int bx = fx + fw + 4, by = fy + fh - 18, R = 46;
  cv.fillSmoothCircle(bx, by, R + 6, C(BLANC));
  if (f >= 0.5f) {
    cv.fillSmoothCircle(bx, by, R, C(OK_VERT));
    cv.drawWideLine(bx - 20, by + 2, bx - 6, by + 17, 5.f, C(BLANC));
    cv.drawWideLine(bx - 6, by + 17, bx + 22, by - 15, 5.f, C(BLANC));
  } else if (mode == M_ATTENTE_ADMIN) {
    float pulse = 0.5f + 0.5f * sinf(t * 5.f);
    cv.fillSmoothCircle(bx, by, R, C(mix(AMBRE, {255, 220, 120}, pulse)));
    cv.setFont(&fonts::FreeSansBold24pt7b);
    cv.setTextDatum(middle_center);
    cv.setTextColor(C(BLANC));
    cv.drawString("?", bx, by + 2);
  } else {
    cv.fillSmoothCircle(bx, by, R, C(KO_ROUGE));
    cv.fillArc(bx, by - 6, 16, 9, 180, 360, C(BLANC));           // anse
    cv.fillRect(bx - 16, by - 7, 7, 6, C(BLANC));
    cv.fillRect(bx + 9, by - 7, 7, 6, C(BLANC));
    cv.fillSmoothRoundRect(bx - 21, by - 4, 42, 30, 6, C(BLANC)); // corps
    cv.fillSmoothCircle(bx, by + 7, 5, C(KO_ROUGE));               // serrure
    cv.fillRect(bx - 2, by + 9, 4, 10, C(KO_ROUGE));
  }
}

void dessinerStatut(uint32_t now) {
  const char* titre;
  if (servoAngle != servoCible) titre = (servoCible == SERVO_ANGLE_OUVERT) ? "OUVERTURE..." : "FERMETURE...";
  else titre = porteCommandeeOuverte() ? "PORTE OUVERTE" : "PORTE FERMEE";

  cv.setTextDatum(middle_center);
  cv.setFont(&fonts::FreeSansBold24pt7b);
  texte(titre, W / 2, 722, W - 60, 1.5f, BLANC, true);
  cv.setFont(&fonts::FreeSansBold12pt7b);
  texte(motif, W / 2, 778, W - 60, 1.15f, {255, 245, 225}, true);

  // barre de progression
  const int bx = 60, by = 808, bw = W - 120, bh = 34;
  float frac = -1; Rgb coul = BLANC; char lib[64] = "";
  if (porteCommandeeOuverte()) {
    int32_t reste = (int32_t)(tFinOuverture - now); if (reste < 0) reste = 0;
    frac = (float)reste / DUREE_OUVERTURE_MS;
    coul = {255, 255, 255};
    snprintf(lib, sizeof(lib), "Fermeture automatique dans %d s", (int)((reste + 999) / 1000));
  } else if (urg == U_ATTENTE) {
    int32_t reste = (int32_t)DELAI_REPONSE_ADMIN_MS - (int32_t)(now - tUrg); if (reste < 0) reste = 0;
    frac = (float)reste / DELAI_REPONSE_ADMIN_MS;
    coul = AMBRE;
    snprintf(lib, sizeof(lib), "Reponse de l'admin attendue : %d s", (int)((reste + 999) / 1000));
  }
  if (frac >= 0) {
    cv.fillSmoothRoundRect(bx, by, bw, bh, bh / 2, C({30, 20, 20}));
    int wf = (int)(bw * frac);
    if (wf > bh) cv.fillSmoothRoundRect(bx, by, wf, bh, bh / 2, C(mix(coul, NOIR, 0.15f)));
    cv.setFont(&fonts::FreeSansBold9pt7b);
    texte(lib, W / 2, by + bh / 2, bw - 20, 1.0f, {255, 255, 255}, true);
  }
}

void iconeEtat(int x, int y, int8_t etat, Rgb fondChip) {
  cv.fillSmoothCircle(x, y, 15, C(BLANC));
  uint16_t c = C(fondChip);
  if (etat == 1) {
    cv.drawWideLine(x - 7, y, x - 2, y + 6, 2.5f, c);
    cv.drawWideLine(x - 2, y + 6, x + 8, y - 6, 2.5f, c);
  } else if (etat == 0) {
    cv.drawWideLine(x - 6, y - 6, x + 6, y + 6, 2.5f, c);
    cv.drawWideLine(x - 6, y + 6, x + 6, y - 6, 2.5f, c);
  } else {
    cv.setFont(&fonts::FreeSansBold12pt7b);
    cv.setTextDatum(middle_center);
    cv.setTextColor(c);
    cv.drawString("?", x, y + 1);
  }
}

void dessinerCarteEpi(Rgb fond, uint32_t now) {
  const int x = 30, y = 864, w = W - 60, h = 140;
  cv.fillSmoothRoundRect(x, y, w, h, 18, C(mix(fond, NOIR, 0.42f)));
  cv.setFont(&fonts::FreeSansBold12pt7b);
  cv.setTextDatum(middle_left);
  cv.setTextColor(C(BLANC));
  cv.drawString("EPI - derniere detection", x + 20, y + 24);
  cv.setFont(&fonts::FreeSans9pt7b);
  cv.setTextDatum(middle_right);
  cv.setTextColor(C({220, 220, 230}));
  char age[32];
  if (detectionRecue) snprintf(age, sizeof(age), "il y a %lu s", (unsigned long)((now - tDetection) / 1000));
  else                strcpy(age, "aucune");
  cv.drawString(age, x + w - 20, y + 24);

  const int gap = 10, cw = (w - 40 - gap * (NB_EPI - 1)) / NB_EPI, ch = 80, cy = y + 48;
  for (int i = 0; i < NB_EPI; i++) {
    int cx = x + 20 + i * (cw + gap);
    Rgb c = epis[i].etat == 1 ? OK_VERT : (epis[i].etat == 0 ? KO_ROUGE : GRIS);
    cv.fillSmoothRoundRect(cx, cy, cw, ch, 12, C(c));
    iconeEtat(cx + cw / 2, cy + 25, epis[i].etat, c);
    String nom = String(epis[i].nom) + (epis[i].obligatoire ? "*" : "");
    cv.setFont(&fonts::FreeSansBold9pt7b);
    cv.setTextDatum(middle_center);
    texte(nom, cx + cw / 2, cy + 61, cw - 8, 1.0f, BLANC, false);
  }
}

void dessinerCarteUrgence(Rgb fond, float t) {
  const int x = 30, y = 1016, w = W - 60, h = 62;
  cv.fillSmoothRoundRect(x, y, w, h, 18, C(mix(fond, NOIR, 0.42f)));
  Rgb c; const char* txt;
  switch (urg) {
    case U_ATTENTE:  c = AMBRE;    txt = "Ouverture critique : en attente de l'admin"; break;
    case U_ACCEPTEE: c = OK_VERT;  txt = "Ouverture critique : AUTORISEE";            break;
    case U_REFUSEE:  c = KO_ROUGE; txt = "Ouverture critique : REFUSEE";              break;
    case U_EXPIREE:  c = KO_ROUGE; txt = "Ouverture critique : sans reponse";         break;
    default:         c = GRIS;     txt = "Ouverture critique : aucune demande";       break;
  }
  bool visible = (urg != U_ATTENTE) || fmodf(t, 1.0f) < 0.6f;
  cv.fillSmoothCircle(x + 34, y + h / 2, 13, C(BLANC));
  if (visible) cv.fillSmoothCircle(x + 34, y + h / 2, 10, C(c));
  cv.setFont(&fonts::FreeSansBold12pt7b);
  cv.setTextDatum(middle_left);
  cv.setTextColor(C(BLANC));
  cv.drawString(txt, x + 60, y + h / 2);
}

void dessinerPereNoel(int sx, int sy, float t) {
  const Rgb rouge = {212, 30, 42}, peau = {255, 214, 180}, sombre = {40, 30, 30};
  cv.fillSmoothRoundRect(sx - 54, sy + 40, 108, 80, 32, C(rouge));         // corps
  cv.fillRect(sx - 54, sy + 76, 108, 12, C({30, 30, 30}));                  // ceinture
  cv.fillRect(sx - 10, sy + 74, 20, 16, C({240, 200, 60}));                 // boucle
  cv.fillSmoothCircle(sx, sy + 4, 32, C(peau));                             // visage
  cv.fillSmoothCircle(sx, sy + 38, 32, C(BLANC));                           // barbe
  cv.fillSmoothCircle(sx - 25, sy + 22, 18, C(BLANC));
  cv.fillSmoothCircle(sx + 25, sy + 22, 18, C(BLANC));
  cv.fillEllipse(sx - 12, sy + 17, 14, 7, C({235, 235, 235}));              // moustache
  cv.fillEllipse(sx + 12, sy + 17, 14, 7, C({235, 235, 235}));
  cv.fillSmoothCircle(sx, sy + 10, 7, C({240, 130, 120}));                  // nez
  cv.fillSmoothCircle(sx - 20, sy + 8, 6, C({252, 175, 165}));              // joues
  cv.fillSmoothCircle(sx + 20, sy + 8, 6, C({252, 175, 165}));
  if (fmodf(t, 4.0f) < 0.15f) {                                            // clin d'oeil
    cv.fillRect(sx - 16, sy - 3, 10, 3, C(sombre));
    cv.fillRect(sx + 6, sy - 3, 10, 3, C(sombre));
  } else {
    cv.fillSmoothCircle(sx - 11, sy - 2, 4, C(sombre));
    cv.fillSmoothCircle(sx + 11, sy - 2, 4, C(sombre));
  }
  float bal = sinf(t * 2.f) * 4.f;                                         // bonnet qui se balance
  cv.fillTriangle(sx - 36, sy - 20, sx + 36, sy - 20, sx + 44 + (int)bal, sy - 74, C(rouge));
  cv.fillSmoothCircle(sx + 46 + (int)bal, sy - 76, 10, C(BLANC));
  cv.fillSmoothRoundRect(sx - 42, sy - 31, 84, 18, 9, C(BLANC));
}

void etoile(int x, int y, int r, uint16_t c) {
  cv.fillTriangle(x, y - r, x - (int)(r * 0.87f), y + r / 2, x + (int)(r * 0.87f), y + r / 2, c);
  cv.fillTriangle(x, y + r, x - (int)(r * 0.87f), y - r / 2, x + (int)(r * 0.87f), y - r / 2, c);
}

void dessinerSapin(int x, int base, float t) {
  cv.fillRect(x - 10, base - 22, 20, 26, C({110, 70, 35}));
  cv.fillTriangle(x - 70, base - 20, x + 70, base - 20, x, base - 82, C({20, 110, 50}));
  cv.fillTriangle(x - 58, base - 60, x + 58, base - 60, x, base - 116, C({26, 128, 58}));
  cv.fillTriangle(x - 44, base - 96, x + 44, base - 96, x, base - 146, C({32, 145, 66}));
  static const int8_t BOULES[][2] = { {-40, -30}, {10, -40}, {42, -28}, {-22, -72}, {26, -76}, {-6, -104}, {14, -124} };
  static const Rgb COUL[] = { {230, 40, 50}, {255, 210, 40}, {70, 190, 255}, {255, 120, 220} };
  for (int i = 0; i < 7; i++) {
    bool on = ((i + (int)(t * 2.5f)) % 2) == 0;
    Rgb c = COUL[i % 4];
    cv.fillSmoothCircle(x + BOULES[i][0], base + BOULES[i][1], 6, C(on ? c : mix(c, NOIR, 0.45f)));
  }
  etoile(x, base - 150, 15, C({255, 215, 50}));
}

void dessinerPied(float t) {
  const Rgb neigeSol = {244, 248, 255}, neigeOmbre = {214, 226, 245};
  cv.fillEllipse(W * 3 / 4, H + 50, 340, 122, C(neigeOmbre));
  cv.fillEllipse(W / 4, H + 46, 280, 112, C(neigeSol));
  cv.fillRect(0, H - 34, W, 34, C(neigeSol));
  dessinerPereNoel(104, H - 92, t);
  dessinerSapin(W - 96, H - 26, t);
  cv.setFont(&fonts::FreeSans9pt7b);
  cv.setTextDatum(middle_center);
  cv.setTextColor(C({70, 80, 110}));
  String info = String("Broker ") + MQTT_HOST + ":" + String(MQTT_PORT);
  if (WiFi.status() == WL_CONNECTED) { info += "   IP "; info += WiFi.localIP().toString(); }
  cv.drawString(info, W / 2, H - 16);
}

void dessinerEcran(uint32_t now) {
  float t = now / 1000.f;
  float f = fractionOuverture();
  Rgb fond = mix(ROUGE_HAUT, VERT_HAUT, f);

  dessinerFond(f);
  dessinerEntete();
  dessinerGuirlande(t, fond);
  dessinerVoeux(t);
  dessinerPorte(f, t, mix(fond, mix(ROUGE_BAS, VERT_BAS, f), 0.3f));
  dessinerStatut(now);
  dessinerCarteEpi(mix(fond, mix(ROUGE_BAS, VERT_BAS, f), 0.7f), now);
  dessinerCarteUrgence(mix(fond, mix(ROUGE_BAS, VERT_BAS, f), 0.8f), t);
  dessinerPied(t);
  for (auto& fl : neige) cv.fillSmoothCircle((int)fl.x, (int)fl.y, fl.r, C({240, 246, 255}));

  cv.pushSprite(0, 0);
}

// =====================================================================
//  setup / loop
// =====================================================================
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Power.setExtOutput(true);                 // alimente le 5 V des ports d'extension

  if (M5.Display.width() > M5.Display.height())  // IHM concue en portrait
    M5.Display.setRotation((M5.Display.getRotation() + 1) % 4);
  M5.Display.setBrightness(200);
  W = M5.Display.width();
  H = M5.Display.height();

  cv.setColorDepth(16);
  cv.setPsram(true);
  if (!cv.createSprite(W, H)) Serial.println("[IHM] ERREUR : memoire insuffisante pour le tampon d'ecran");

  randomSeed(esp_random());
  initNeige();
  servoInit();                                 // porte fermee au demarrage (securite)
  wifiInit();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(surMessage);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(3);

  tMode = millis();
}

void loop() {
  M5.update();
  gererWifi();
  gererMqtt();

  uint32_t now = millis();
  static uint32_t tPrec = now, tImage = 0;
  float dt = (now - tPrec) / 1000.f; tPrec = now;
  if (dt > 0.2f) dt = 0.2f;

  majLogique(now);
  majServo(dt);
  publierDoorState(false);                     // publie uniquement si l'etat change

  if (now - tImage >= 33) {                    // ~30 images/s
    float dti = (now - tImage) / 1000.f; if (dti > 0.2f) dti = 0.2f;
    tImage = now;
    majNeige(dti, now / 1000.f);
    dessinerEcran(now);
  }
  delay(1);
}
