// ppe_decision.h - Logique de decision EPI, independante d'ESP-DL et de LVGL.
// Entree : la liste des detections d'une inference. Sortie : etat lisse par EPI + verdict.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace ppe
{

// Indices de classes du modele yolo26n_ppe (11 classes, ordre du data.yaml d'entrainement)
enum ClassId : int
{
    HELMET = 0, GLOVES = 1, VEST = 2, BOOTS = 3, GOGGLES = 4, NONE = 5, PERSON = 6,
    NO_HELMET = 7, NO_GOGGLE = 8, NO_GLOVES = 9, NO_BOOTS = 10,
    NO_CLASS = -1,   // pas de classe negative pour cet EPI (gilet)
};

struct Detection
{
    int cls;            // indice de classe
    float score;        // confiance 0..1
    float x1, y1, x2, y2;
};

enum Item : int { IT_HELMET, IT_GLOVES, IT_VEST, IT_BOOTS, IT_GOGGLES, IT_COUNT };
enum class Obs : uint8_t { UNKNOWN, PRESENT, ABSENT };                    // une inference
enum class State : uint8_t { CHECKING, PRESENT, ABSENT };                 // apres lissage
enum class Verdict : uint8_t { NO_PERSON, MULTI_PERSON, CHECKING, DENIED, GRANTED };

static constexpr int WINDOW_SIZE = 4;

struct Config
{
    std::array<bool, IT_COUNT> required{true, false, true, false, false}; // regle metier : a adapter
    std::array<float, IT_COUNT> thr_present{0.40f, 0.40f, 0.40f, 0.40f, 0.40f};
    std::array<float, IT_COUNT> thr_absent{0.30f, 0.30f, 0.30f, 0.30f, 0.30f};
    float thr_person = 0.40f;
    int need_present = 3;   // nb d'observations PRESENT sur la fenetre pour valider
    int need_absent = 2;    // nb d'observations ABSENT sur la fenetre pour refuser
    // Pour le gilet (sans classe negative), ABSENT = non vu alors que la personne l'est :
    // plus bruyant, donc on exige la fenetre entiere avant de refuser.
    int need_absent_no_neg = WINDOW_SIZE;
};

class Decision
{
public:
    static constexpr int WINDOW = WINDOW_SIZE;  // fenetre de lissage, en inferences

    explicit Decision(const Config &cfg = Config()) : cfg_(cfg) { reset(); }

    void reset()
    {
        for (auto &h : hist_) h.fill(Obs::UNKNOWN);
        pos_ = 0;
        verdict_ = Verdict::NO_PERSON;
    }

    // A appeler une fois par inference terminee.
    Verdict update(const std::vector<Detection> &dets)
    {
        static const int pos_cls[IT_COUNT] = {HELMET, GLOVES, VEST, BOOTS, GOGGLES};
        static const int neg_cls[IT_COUNT] = {NO_HELMET, NO_GLOVES, NO_CLASS, NO_BOOTS, NO_GOGGLE};

        int persons = 0;
        float best_pos[IT_COUNT] = {0}, best_neg[IT_COUNT] = {0};
        for (const auto &d : dets)
        {
            if (d.cls == PERSON && d.score >= cfg_.thr_person) persons++;
            for (int i = 0; i < IT_COUNT; i++)
            {
                if (d.cls == pos_cls[i] && d.score > best_pos[i]) best_pos[i] = d.score;
                if (d.cls == neg_cls[i] && d.score > best_neg[i]) best_neg[i] = d.score;
            }
        }

        // Sans exactement une personne, on ne valide jamais : on vide l'historique.
        if (persons != 1)
        {
            for (auto &h : hist_) h.fill(Obs::UNKNOWN);
            verdict_ = (persons == 0) ? Verdict::NO_PERSON : Verdict::MULTI_PERSON;
            return verdict_;
        }

        for (int i = 0; i < IT_COUNT; i++)
        {
            Obs o = Obs::UNKNOWN;
            const bool p = best_pos[i] >= cfg_.thr_present[i];
            const bool n = best_neg[i] >= cfg_.thr_absent[i];
            if (p && n) o = (best_pos[i] > best_neg[i]) ? Obs::PRESENT : Obs::ABSENT;
            else if (p) o = Obs::PRESENT;
            else if (n) o = Obs::ABSENT;
            else if (neg_cls[i] == NO_CLASS) o = Obs::ABSENT;  // sans classe negative : personne vue, EPI non vu
            hist_[i][pos_] = o;
        }
        pos_ = (pos_ + 1) % WINDOW;

        bool all_present = true, any_absent = false;
        for (int i = 0; i < IT_COUNT; i++)
        {
            if (!cfg_.required[i]) continue;
            const State s = state(static_cast<Item>(i));
            if (s != State::PRESENT) all_present = false;
            if (s == State::ABSENT) any_absent = true;
        }
        verdict_ = any_absent ? Verdict::DENIED : (all_present ? Verdict::GRANTED : Verdict::CHECKING);
        return verdict_;
    }

    State state(Item it) const
    {
        int present = 0, absent = 0;
        for (Obs o : hist_[it])
        {
            if (o == Obs::PRESENT) present++;
            else if (o == Obs::ABSENT) absent++;
        }
        const int need_abs = has_neg_class(it) ? cfg_.need_absent : cfg_.need_absent_no_neg;
        if (absent >= need_abs) return State::ABSENT;                // le refus l'emporte
        if (present >= cfg_.need_present) return State::PRESENT;
        return State::CHECKING;
    }

    static bool has_neg_class(Item it) { return it != IT_VEST; }
    Verdict verdict() const { return verdict_; }
    bool required(Item it) const { return cfg_.required[it]; }

private:
    Config cfg_;
    std::array<std::array<Obs, WINDOW>, IT_COUNT> hist_;
    int pos_ = 0;
    Verdict verdict_ = Verdict::NO_PERSON;
};

}  // namespace ppe