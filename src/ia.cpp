// Etape 4 : inference YOLO26 sur le flux camera, dans une tache separee.
// Resultats dans le log serie uniquement (pas d'affichage).
#include "ia.h"
#include <atomic>
#include <cstring>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "dl_model_base.hpp"
#include "yolo26.hpp"
#include "ppe_classes.hpp"
#include "ppe_decision.h"
#include "ui.h"

static const char *TAG = "ia";
static const char *MODEL_PARTITION = "model320";
static const int PPE_NUM_CLASSES = sizeof(ppe_classes) / sizeof(ppe_classes[0]);

static dl::Model *s_model = nullptr;
static YOLO26 *s_yolo = nullptr;
static ppe::Decision s_decision;

// Instantane de l'image camera, rempli par le rappel video, consomme par la tache
static uint8_t *s_snap = nullptr;
static size_t s_snap_size = 0;
static uint32_t s_snap_w = 0, s_snap_h = 0;
static std::atomic<bool> s_busy{false};
static SemaphoreHandle_t s_frame_ready = nullptr;

static const char *item_name(ppe::Item it)
{
    static const char *names[ppe::IT_COUNT] = {"casque", "gants", "gilet", "chaussures", "lunettes"};
    return names[it];
}

static char state_char(ppe::State s)
{
    switch (s) {
    case ppe::State::PRESENT: return 'P';
    case ppe::State::ABSENT:  return 'A';
    default:                  return '?';
    }
}

static const char *verdict_name(ppe::Verdict v)
{
    switch (v)
    {
        case ppe::Verdict::NO_PERSON:    return "PERSONNE_ABSENTE";
        case ppe::Verdict::MULTI_PERSON: return "PLUSIEURS_PERSONNES";
        case ppe::Verdict::CHECKING:     return "VERIFICATION";
        case ppe::Verdict::DENIED:       return "REFUSE";
        case ppe::Verdict::GRANTED:      return "VALIDE";
    }
    return "?";
}

extern "C" int ia_init(void)
{
    ESP_LOGI(TAG, "PSRAM libre avant chargement : %u Ko",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    s_model = new dl::Model(MODEL_PARTITION, fbs::MODEL_LOCATION_IN_FLASH_PARTITION, 0,
                            dl::MEMORY_MANAGER_GREEDY, nullptr, true);
    auto inputs = s_model->get_inputs();
    if (inputs.empty())
    {
        ESP_LOGE(TAG, "Le modele n'a pas d'entree : partition vide ou corrompue ?");
        return -1;
    }
    auto *in = inputs.begin()->second;
    ESP_LOGI(TAG, "Entree '%s' : [%d, %d, %d, %d]", inputs.begin()->first.c_str(),
             in->shape[0], in->shape[1], in->shape[2], in->shape[3]);

    auto outputs = s_model->get_outputs();
    auto it = outputs.find("one2one_p3_cls");
    if (it == outputs.end())
    {
        ESP_LOGE(TAG, "Sortie one2one_p3_cls absente");
        return -1;
    }
    const int model_classes = it->second->shape[3];
    ESP_LOGI(TAG, "Classes dans le modele : %d, dans ppe_classes.hpp : %d", model_classes, PPE_NUM_CLASSES);
    if (model_classes != PPE_NUM_CLASSES)
    {
        ESP_LOGE(TAG, "DESACCORD : la liste de classes ne correspond pas au modele");
        return -1;
    }

    s_yolo = new YOLO26(s_model, YOLO_TARGET_K, YOLO_CONF_THRESH, ppe_classes);
    return 0;
}

static void ia_task(void *)
{
    while (true) {
        xSemaphoreTake(s_frame_ready, portMAX_DELAY);

        dl::image::img_t img = {};
        img.data = s_snap;
        img.width = (uint16_t)s_snap_w;
        img.height = (uint16_t)s_snap_h;
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;

        const int64_t t0 = esp_timer_get_time();
        s_yolo->preprocess(img);
        const int64_t t1 = esp_timer_get_time();
        s_model->run();
        const int64_t t2 = esp_timer_get_time();
        auto results = s_yolo->postprocess(s_model->get_outputs());
        const int64_t t3 = esp_timer_get_time();

        // Conversion vers la logique de decision (boites en coordonnees modele, 320x320)
        std::vector<ppe::Detection> dets;
        dets.reserve(results.size());
        for (const auto &r : results)
        {
            dets.push_back({r.category, r.score,
                            (float)r.box[0], (float)r.box[1], (float)r.box[2], (float)r.box[3]});
        }
        const ppe::Verdict v = s_decision.update(dets);

        // Journal : temps, detections, etats lisses, verdict
        char line[256];
        int n = snprintf(line, sizeof(line), "pre %lld | inf %lld | post %lld ms | %u det :",
                         (long long)((t1 - t0) / 1000), (long long)((t2 - t1) / 1000),
                         (long long)((t3 - t2) / 1000), (unsigned)results.size());
        for (const auto &r : results)
        {
            if (n >= (int)sizeof(line) - 24) break;
            n += snprintf(line + n, sizeof(line) - n, " %s %.0f%%", ppe_classes[r.category], r.score * 100);
        }
        ESP_LOGI(TAG, "%s", line);

        n = snprintf(line, sizeof(line), "%s |", verdict_name(v));
        for (int i = 0; i < ppe::IT_COUNT; i++)
        {
            const ppe::Item it = (ppe::Item)i;
            n += snprintf(line + n, sizeof(line) - n, " %s%s=%c", item_name(it),
                          s_decision.required(it) ? "*" : "", state_char(s_decision.state(it)));
        }
        ESP_LOGI(TAG, "%s", line);

        // Mise a jour de l'interface
        ui_status_t st = {};
        st.person = (v == ppe::Verdict::NO_PERSON) ? UI_UNKNOWN :
                    (v == ppe::Verdict::MULTI_PERSON) ? UI_WARN : UI_OK;
        for (int i = 0; i < ppe::IT_COUNT; i++)
        {
            const ppe::State ps = s_decision.state((ppe::Item)i);
            st.items[i] = (ps == ppe::State::PRESENT) ? UI_OK : (ps == ppe::State::ABSENT) ? UI_BAD : UI_UNKNOWN;
            st.required[i] = s_decision.required((ppe::Item)i);
        }
        st.verdict = (ui_verdict_t)v;   // meme ordre que ppe::Verdict
        st.infer_ms = (int)((t2 - t1) / 1000);
        ui_update(&st);

        s_busy = false;
    }
}

extern "C" int ia_start(void)
{
    if (s_yolo == nullptr) return -1;
    s_frame_ready = xSemaphoreCreateBinary();
    BaseType_t ok = xTaskCreatePinnedToCore(ia_task, "ia", 16 * 1024, nullptr, 3, nullptr, 1);
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "Creation de la tache d'inference impossible");
        return -1;
    }
    ESP_LOGI(TAG, "Tache d'inference demarree");
    return 0;
}

extern "C" void ia_submit_frame(const uint8_t *rgb565, uint32_t w, uint32_t h)
{
    if (s_frame_ready == nullptr || s_busy.load()) return;

    const size_t need = (size_t)w * h * 2;
    if (s_snap == nullptr || s_snap_size < need)
    {
        if (s_snap) heap_caps_free(s_snap);
        s_snap = (uint8_t *)heap_caps_aligned_alloc(64, need, MALLOC_CAP_SPIRAM);
        if (s_snap == nullptr)
        {
            ESP_LOGE(TAG, "Allocation de l'instantane impossible (%u octets)", (unsigned)need);
            return;
        }
        s_snap_size = need;
    }

    s_busy = true;            // reserve avant la copie : la tache ne lira qu'apres le give
    memcpy(s_snap, rgb565, need);
    s_snap_w = w;
    s_snap_h = h;
    xSemaphoreGive(s_frame_ready);
}