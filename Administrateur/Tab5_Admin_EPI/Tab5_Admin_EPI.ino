/*
 * ============================================================================
 *  Tab5_Admin_EPI.ino  —  Tablette ADMINISTRATEUR (M5Stack Tab5 / ESP32-P4)
 * ============================================================================
 *  Rôle :
 *   - Lit (subscriber) les topics "Detection" et "Demande_critique"
 *     publiés par les Tab5 postes
 *   - Affiche un widget par poste (jusqu'à 6) avec ses EPI en temps réel
 *   - Quand un poste envoie Bouton = true, affiche AUTORISER / REFUSER
 *     et publie (publisher) la décision sur "Autorisation_critique"
 *
 *  Arduino IDE :
 *   - Carte : M5Stack > M5Tab5   (board manager M5Stack >= 3.2.2)
 *   - Bibliothèques : M5Unified, M5GFX, PubSubClient (Nick O'Leary),
 *                     ArduinoJson (Benoit Blanchon, v7)
 *
 *  ---------------------------------------------------------------------------
 *  TOPICS ET MESSAGES MQTT  (l'id du poste est DANS le message)
 *  ---------------------------------------------------------------------------
 *  1) Detection               -> LU
 *       { "id": 1,
 *         "Detection": { "Casque": true, "Gilet": false, "Chaussures": true } }
 *
 *  2) Demande_critique        -> LU
 *       { "id": 1, "Bouton": true }
 *
 *  3) Autorisation_critique   -> ÉCRIT par l'admin
 *       { "id": 1, "Autorisation": true }
 * ============================================================================
 */

#include <M5Unified.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ============================ CONFIGURATION ==================================
const char* WIFI_SSID   = "MON_WIFI";
const char* WIFI_PASS   = "MON_MOT_DE_PASSE";

const char* MQTT_HOST   = "172.20.10.2";    // IP du broker (PC avec Mosquitto)
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER   = "";               // laisser "" si pas d'authentification
const char* MQTT_PASSW  = "";

const char* TOPIC_DETECTION    = "Detection";              // lu
const char* TOPIC_DEMANDE      = "Demande_critique";       // lu
const char* TOPIC_AUTORISATION = "Autorisation_critique";  // publié

// Postes affichés dès le démarrage, même sans aucun message du broker
const char* POSTES_PREVUS[] = { "1", "2", "3", "4", "5", "6" };

// true = fausses données générées localement pour voir toute l'interface
//        (EPI qui changent, demandes critiques...) sans broker ni autres tabs
const bool DEMO_MODE = false;

const uint32_t OFFLINE_MS = 15000;          // poste "hors ligne" si muet > 15 s
// =============================================================================

// Liaison SDIO vers l'ESP32-C6 (Wi-Fi) de la Tab5 — broches officielles M5Stack
#define SDIO2_CLK GPIO_NUM_12
#define SDIO2_CMD GPIO_NUM_13
#define SDIO2_D0  GPIO_NUM_11
#define SDIO2_D1  GPIO_NUM_10
#define SDIO2_D2  GPIO_NUM_9
#define SDIO2_D3  GPIO_NUM_8
#define SDIO2_RST GPIO_NUM_15

// ------------------------------- Couleurs (RGB888) ---------------------------
constexpr uint32_t COL_BG       = 0x0F1720;
constexpr uint32_t COL_HEADER   = 0x1E293B;
constexpr uint32_t COL_CARD     = 0x1F2A37;
constexpr uint32_t COL_GREEN    = 0x22C55E;
constexpr uint32_t COL_RED      = 0xEF4444;
constexpr uint32_t COL_ORANGE   = 0xF59E0B;
constexpr uint32_t COL_GREY     = 0x64748B;
constexpr uint32_t COL_TEXT     = 0xF1F5F9;
constexpr uint32_t COL_SUBTEXT  = 0x94A3B8;

// ------------------------------- Données -------------------------------------
#define MAX_TABS 6

struct TabState {
  char     id[24]     = "";
  bool     used       = false;   // emplacement occupé
  bool     hasData    = false;   // au moins un message "Detection" reçu
  bool     casque     = false;
  bool     gilet      = false;
  bool     chaussures = false;
  bool     critique   = false;   // Bouton = true reçu, décision en attente
  uint32_t lastSeen   = 0;
  uint32_t nbDetect   = 0;
  int8_t   decision   = -1;      // -1 aucune, 0 refusé, 1 autorisé
  bool     dirty      = true;
};

TabState tabs[MAX_TABS];

WiFiClient   net;
PubSubClient mqtt(net);

M5Canvas cardSpr(&M5.Display);
M5Canvas headerSpr(&M5.Display);

// ------------------------------- Mise en page --------------------------------
int SCR_W, SCR_H;
const int HEADER_H = 70;
const int MARGIN   = 16;
int COLS = 3, ROWS = 2;
int CARD_W, CARD_H;

struct Rect { int x, y, w, h; bool contains(int px, int py) const {
  return px >= x && px < x + w && py >= y && py < y + h; } };
Rect btnOk, btnNo;

bool     blinkState = false;
uint32_t lastBlink  = 0;
uint32_t lastSecond = 0;
String   lastLog    = "Demarrage...";

// =============================================================================
//  Gestion des postes
// =============================================================================
TabState* getTab(const char* id) {
  for (auto& t : tabs) if (t.used && strcmp(t.id, id) == 0) return &t;
  for (auto& t : tabs) if (!t.used) {
    t = TabState();
    strlcpy(t.id, id, sizeof(t.id));
    t.used = true;
    return &t;
  }
  return nullptr;
}

void cardOrigin(int i, int& x, int& y) {
  int c = i % COLS, r = i / COLS;
  x = MARGIN + c * (CARD_W + MARGIN);
  y = HEADER_H + MARGIN + r * (CARD_H + MARGIN);
}

// =============================================================================
//  Lecture JSON (tolérante : "id" nombre ou texte, majuscules ou minuscules)
// =============================================================================
String readId(JsonDocument& d) {
  JsonVariant v = d["id"];
  if (v.isNull()) v = d["ID"];
  if (v.is<int>())         return String(v.as<int>());
  if (v.is<const char*>()) return String(v.as<const char*>());
  return "";
}

bool readBool(JsonVariant obj, const char* k1, const char* k2) {
  if (obj[k1].is<bool>()) return obj[k1].as<bool>();
  if (obj[k2].is<bool>()) return obj[k2].as<bool>();
  return false;
}

// =============================================================================
//  Traitement d'un message (vient du broker OU du mode démo)
// =============================================================================
void handleDetection(TabState* t, JsonVariant det) {
  t->casque     = readBool(det, "Casque",     "casque");
  t->gilet      = readBool(det, "Gilet",      "gilet");
  t->chaussures = readBool(det, "Chaussures", "chaussures");
  t->hasData    = true;
  t->nbDetect++;
}

void handleDemande(TabState* t, bool bouton) {
  if (bouton && !t->critique) {               // nouvelle demande -> bip
    t->decision = -1;
    M5.Speaker.tone(2500, 120);
  }
  t->critique = bouton;
}

void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) {
    Serial.printf("[MQTT] JSON invalide sur %s\n", topic);
    lastLog = String("JSON invalide sur ") + topic;
    return;
  }

  String id = readId(doc);
  if (id.isEmpty()) {
    Serial.printf("[MQTT] Pas de champ \"id\" sur %s\n", topic);
    lastLog = String("Pas d'id sur ") + topic;
    return;
  }

  TabState* t = getTab(id.c_str());
  if (!t) { Serial.println("[MQTT] Trop de postes (MAX_TABS)"); return; }

  if (strcmp(topic, TOPIC_DETECTION) == 0) {
    // Format attendu : { "id":1, "Detection": { "Casque":.., "Gilet":.., "Chaussures":.. } }
    JsonVariant det = doc["Detection"];
    if (det.isNull()) det = doc.as<JsonVariant>();   // accepte aussi les clés à la racine
    handleDetection(t, det);
    Serial.printf("[MQTT] Detection id=%s casque=%d gilet=%d chaussures=%d\n",
                  id.c_str(), t->casque, t->gilet, t->chaussures);
  }
  else if (strcmp(topic, TOPIC_DEMANDE) == 0) {
    // Format attendu : { "id":1, "Bouton": true }
    bool bouton = readBool(doc.as<JsonVariant>(), "Bouton", "bouton");
    handleDemande(t, bouton);
    Serial.printf("[MQTT] Demande id=%s bouton=%d\n", id.c_str(), bouton);
  }
  else return;

  t->lastSeen = millis();
  t->dirty    = true;
  lastLog = String("Recu ") + topic + " id=" + id;
}

// Publie sur Autorisation_critique : { "id": 1, "Autorisation": true/false }
void sendCommand(TabState& t, bool autoriser) {
  JsonDocument doc;
  char* end;
  long n = strtol(t.id, &end, 10);
  if (*end == '\0') doc["id"] = n;            // id numérique -> 1
  else              doc["id"] = t.id;         // sinon texte
  doc["Autorisation"] = autoriser;

  char buf[96];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  bool ok = DEMO_MODE ? true
                      : mqtt.publish(TOPIC_AUTORISATION, (const uint8_t*)buf, len, false);

  if (ok) {
    t.decision = autoriser ? 1 : 0;
    t.critique = false;
    lastLog = String(autoriser ? "AUTORISE -> poste " : "REFUSE -> poste ") + t.id;
    M5.Speaker.tone(autoriser ? 1800 : 600, 80);
  } else {
    lastLog = "Echec envoi (MQTT deconnecte ?)";
  }
  t.dirty = true;
  Serial.printf("[CMD] %s %s %s\n", TOPIC_AUTORISATION, buf, ok ? "OK" : "ECHEC");
}

void ensureConnections() {
  static uint32_t lastWifiTry = 0, lastMqttTry = 0;
  static bool ipShown = false;

  if (WiFi.status() == WL_CONNECTED && !ipShown) {
    Serial.print("[WiFi] IP Tab5 : "); Serial.println(WiFi.localIP());
    ipShown = true;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiTry > 8000) {
      lastWifiTry = millis();
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      Serial.println("[WiFi] Connexion...");
    }
    return;
  }

  if (!mqtt.connected() && millis() - lastMqttTry > 3000) {
    lastMqttTry = millis();
    String cid = "tab5-admin-" + WiFi.macAddress();
    cid.replace(":", "");
    bool ok = strlen(MQTT_USER)
              ? mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASSW)
              : mqtt.connect(cid.c_str());
    if (ok) {
      mqtt.subscribe(TOPIC_DETECTION);
      mqtt.subscribe(TOPIC_DEMANDE);
      lastLog = String("Abonne a ") + TOPIC_DETECTION + " et " + TOPIC_DEMANDE;
    } else {
      lastLog = "MQTT erreur rc=" + String(mqtt.state());
    }
    Serial.println("[MQTT] " + lastLog);
  }
}

// =============================================================================
//  Mode démo : fausses données toutes les 4 s
// =============================================================================
void demoTick() {
  static uint32_t last = 0;
  if (millis() - last < 4000) return;
  last = millis();

  int i = random(0, MAX_TABS);
  TabState& t = tabs[i];
  if (!t.used) return;

  JsonDocument det;
  det["Casque"]     = random(100) > 25;
  det["Gilet"]      = random(100) > 25;
  det["Chaussures"] = random(100) > 25;
  handleDetection(&t, det.as<JsonVariant>());

  bool complet = t.casque && t.gilet && t.chaussures;
  if (!complet && random(100) > 50) handleDemande(&t, true);

  t.lastSeen = millis();
  t.dirty = true;
  lastLog = String("DEMO : poste ") + t.id;
}

// =============================================================================
//  Dessin
// =============================================================================
void drawPill(M5Canvas& s, int x, int y, int w, int h, const char* label,
              bool ok, bool known) {
  uint32_t col = !known ? COL_GREY : (ok ? COL_GREEN : COL_RED);
  s.fillRoundRect(x, y, w, h, h / 2, col);
  s.setTextColor(known ? 0x000000 : COL_TEXT);
  s.setFont(&fonts::FreeSansBold9pt7b);
  s.setTextDatum(middle_center);
  String lab = label;
  if (w < 120 && lab == "Chaussures") lab = "Chauss.";   // widgets étroits (portrait)
  String txt = String(!known ? "? " : (ok ? "+ " : "x ")) + lab;
  s.drawString(txt, x + w / 2, y + h / 2);
}

void drawButton(M5Canvas& s, const Rect& r, const char* label, uint32_t col) {
  s.fillRoundRect(r.x, r.y, r.w, r.h, 10, col);
  s.setTextColor(0xFFFFFF);
  s.setFont(r.w < 170 ? &fonts::FreeSansBold9pt7b : &fonts::FreeSansBold12pt7b);
  s.setTextDatum(middle_center);
  s.drawString(label, r.x + r.w / 2, r.y + r.h / 2);
}

void drawCard(int i) {
  TabState& t = tabs[i];
  M5Canvas& s = cardSpr;
  uint32_t now = millis();
  bool online = t.hasData && (now - t.lastSeen < OFFLINE_MS);
  bool epiOk  = t.casque && t.gilet && t.chaussures;

  uint32_t border;
  if (!online)          border = COL_GREY;
  else if (t.critique)  border = blinkState ? COL_ORANGE : COL_RED;
  else                  border = epiOk ? COL_GREEN : COL_RED;

  s.fillSprite(COL_BG);
  s.fillRoundRect(0, 0, CARD_W, CARD_H, 14, border);
  s.fillRoundRect(5, 5, CARD_W - 10, CARD_H - 10, 10, COL_CARD);

  // --- Titre + état ---
  s.setTextDatum(top_left);
  s.setFont(&fonts::FreeSansBold18pt7b);
  s.setTextColor(t.used ? COL_TEXT : COL_GREY);
  s.drawString(t.used ? String("Poste ") + t.id : String("Poste --"), 20, 16);

  s.fillCircle(CARD_W - 28, 32, 9, online ? COL_GREEN : COL_GREY);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextColor(COL_SUBTEXT);
  s.setTextDatum(middle_right);
  s.drawString(online ? "en ligne" : (t.hasData ? "hors ligne" : "aucune donnee"),
               CARD_W - 44, 32);

  // --- Résumé EPI ---
  s.setTextDatum(top_left);
  s.setFont(&fonts::FreeSansBold12pt7b);
  if (!t.hasData) { s.setTextColor(COL_GREY);  s.drawString("EPI : en attente", 20, 64); }
  else if (epiOk) { s.setTextColor(COL_GREEN); s.drawString("EPI : COMPLETS",   20, 64); }
  else            { s.setTextColor(COL_RED);   s.drawString("EPI : MANQUANTS",  20, 64); }

  // --- Pastilles EPI (grises "?" tant qu'aucune donnée) ---
  int pillY = 104, pillH = 42, gap = 10;
  int pillW = (CARD_W - 40 - 2 * gap) / 3;
  drawPill(s, 20,                     pillY, pillW, pillH, "Casque",     t.casque,     t.hasData);
  drawPill(s, 20 + pillW + gap,       pillY, pillW, pillH, "Gilet",      t.gilet,      t.hasData);
  drawPill(s, 20 + 2 * (pillW + gap), pillY, pillW, pillH, "Chaussures", t.chaussures, t.hasData);

  // --- Zone basse ---
  if (t.critique) {
    s.setFont(&fonts::FreeSansBold12pt7b);
    s.setTextColor(blinkState ? COL_ORANGE : COL_TEXT);
    s.setTextDatum(top_center);
    s.drawString("! DEMANDE CRITIQUE", CARD_W / 2, 162);
    drawButton(s, btnOk, "AUTORISER", COL_GREEN);
    drawButton(s, btnNo, "REFUSER",   COL_RED);
  } else {
    s.setFont(&fonts::FreeSans12pt7b);
    s.setTextColor(COL_SUBTEXT);
    s.setTextDatum(top_left);
    s.drawString("Detections : " + String(t.nbDetect), 20, 166);
    if (t.hasData)
      s.drawString("Derniere info : il y a " + String((now - t.lastSeen) / 1000) + " s", 20, 198);
    else
      s.drawString("Derniere info : --", 20, 198);
    s.drawString("Demande critique : non", 20, 230);
    if (t.decision >= 0) {
      s.setTextColor(t.decision ? COL_GREEN : COL_RED);
      s.drawString(t.decision ? "Dernier ordre : AUTORISE"
                              : "Dernier ordre : REFUSE", 20, 262);
    }
  }

  int x, y; cardOrigin(i, x, y);
  s.pushSprite(x, y);
  t.dirty = false;
}

void drawHeader() {
  M5Canvas& s = headerSpr;
  s.fillSprite(COL_HEADER);

  s.setTextDatum(middle_left);
  s.setFont(&fonts::FreeSansBold18pt7b);
  s.setTextColor(COL_TEXT);
  s.drawString(DEMO_MODE ? "Supervision EPI - DEMO" : "Supervision EPI - Admin",
               MARGIN, HEADER_H / 2);

  int online = 0, alerts = 0;
  for (auto& t : tabs) if (t.hasData && millis() - t.lastSeen < OFFLINE_MS) {
    online++; if (t.critique) alerts++;
  }

  bool wifiOk = WiFi.status() == WL_CONNECTED;
  bool mqttOk = mqtt.connected();

  s.setFont(&fonts::FreeSans12pt7b);
  s.setTextDatum(middle_right);
  int x = SCR_W - MARGIN;

  s.setTextColor(alerts ? COL_ORANGE : COL_TEXT);
  s.drawString("En ligne : " + String(online) + "   Alertes : " + String(alerts), x, 22);

  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextColor(wifiOk && mqttOk ? COL_GREEN : COL_RED);
  String net = String("WiFi ") + (wifiOk ? "OK" : "--") + " | MQTT " + (mqttOk ? "OK" : "--")
             + " | " + lastLog;
  s.drawString(net, x, 52);

  s.pushSprite(0, 0);
}

// =============================================================================
//  Tactile
// =============================================================================
void handleTouch(int px, int py) {
  for (int i = 0; i < MAX_TABS; i++) {
    int cx, cy; cardOrigin(i, cx, cy);
    if (px < cx || px >= cx + CARD_W || py < cy || py >= cy + CARD_H) continue;
    TabState& t = tabs[i];
    if (!t.used || !t.critique) return;
    int lx = px - cx, ly = py - cy;
    if (btnOk.contains(lx, ly))      sendCommand(t, true);
    else if (btnNo.contains(lx, ly)) sendCommand(t, false);
    return;
  }
}

// =============================================================================
//  Setup / Loop
// =============================================================================
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);

  M5.Display.setRotation(3);
  SCR_W = M5.Display.width();
  SCR_H = M5.Display.height();
  if (SCR_W < SCR_H) { COLS = 2; ROWS = 3; }

  CARD_W = (SCR_W - (COLS + 1) * MARGIN) / COLS;
  CARD_H = (SCR_H - HEADER_H - (ROWS + 1) * MARGIN) / ROWS;

  int bw = (CARD_W - 40 - 14) / 2, bh = 56;
  btnOk = { 20,           CARD_H - bh - 18, bw, bh };
  btnNo = { 20 + bw + 14, CARD_H - bh - 18, bw, bh };

  cardSpr.setColorDepth(16);
  cardSpr.setPsram(true);
  cardSpr.createSprite(CARD_W, CARD_H);
  headerSpr.setColorDepth(16);
  headerSpr.setPsram(true);
  headerSpr.createSprite(SCR_W, HEADER_H);

  M5.Display.fillScreen(COL_BG);
  M5.Speaker.setVolume(120);

  // Postes prévus : affichés tout de suite, même sans broker
  for (const char* id : POSTES_PREVUS) getTab(id);

  if (!DEMO_MODE) {
    WiFi.setPins(SDIO2_CLK, SDIO2_CMD, SDIO2_D0, SDIO2_D1, SDIO2_D2, SDIO2_D3, SDIO2_RST);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setBufferSize(512);
    mqtt.setSocketTimeout(2);
    mqtt.setKeepAlive(15);
  } else {
    randomSeed(esp_random());
    lastLog = "Mode DEMO (pas de MQTT)";
  }

  drawHeader();
  for (int i = 0; i < MAX_TABS; i++) drawCard(i);
}

void loop() {
  M5.update();

  if (DEMO_MODE) demoTick();
  else { ensureConnections(); mqtt.loop(); }

  auto d = M5.Touch.getDetail();
  if (d.wasPressed()) handleTouch(d.x, d.y);

  uint32_t now = millis();

  if (now - lastBlink > 500) {
    lastBlink = now;
    blinkState = !blinkState;
    for (auto& t : tabs) if (t.critique) t.dirty = true;
  }

  if (now - lastSecond > 1000) {
    lastSecond = now;
    for (auto& t : tabs) if (t.used) t.dirty = true;
    drawHeader();
  }

  for (int i = 0; i < MAX_TABS; i++) if (tabs[i].dirty) drawCard(i);

  delay(5);
}
