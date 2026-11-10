/* Leitor de CHD para o hash do RetroAchievements (ver chd_hash.c) */
#ifndef CHD_HASH_H
#define CHD_HASH_H

#include <rc_hash.h>

/* Preenche r: .chd pelo libchdr, o resto pelo leitor padrão do rcheevos */
void chd_hash_cdreader(struct rc_hash_cdreader *r);

#endif
