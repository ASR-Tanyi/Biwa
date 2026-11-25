/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Biwa contributors */

/* biwa/a64/a64_sem.h compiled as C17, the way generated code uses it. */

#include "biwa/a64/a64_sem.h"

int biwa_test_sem_in_c(void);

/* 0 when a few semantics give the expected results in C. */
int biwa_test_sem_in_c(void) {
    BiwaCpu cpu = {0};
    cpu.x[1] = 5;
    sem_addsub_imm(&cpu, true, false, false, 0, 1, 7);
    if (cpu.x[0] != 12) {
        return 1;
    }
    uint64_t wmask = 0;
    uint64_t tmask = 0;
    if (!sem_decode_bit_masks(1, 7, 0, true, 64, &wmask, &tmask) || wmask != 0xFF) {
        return 2;
    }
    sem_move_wide(&cpu, false, 0, 2, 0, 0); /* movn w2, #0 */
    if (cpu.x[2] != 0xFFFFFFFFu) {
        return 3;
    }
    return 0;
}
