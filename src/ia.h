#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Charge le modele YOLO26 depuis la partition flash "model320".
// Journalise la forme d'entree, le nombre de classes et un temps d'inference a vide.
// Renvoie 0 si le modele est charge, -1 sinon.
int ia_init(void);

// Cree la tache d'inference. A appeler apres ia_init() (renvoie 0 si OK).
int ia_start(void);

// A appeler depuis le rappel camera a chaque image (RGB565 petit-boutiste, w x h).
// Ne bloque pas : si la tache d'inference est occupee, l'image est ignoree.
void ia_submit_frame(const uint8_t *rgb565, uint32_t w, uint32_t h);

#ifdef __cplusplus
}
#endif