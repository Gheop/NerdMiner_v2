// race/gheop8 — sonde PIE : ee.vadds.s32 sature-t-il ou wrappe-t-il ?
// SHA-256 exige l'addition modulo 2^32 ; une addition saturante est inutilisable.
// void race_pie_add_probe(uint32_t *io);  io[0..3]=A, io[4..7]=B, io[8..11]=A+B
    .text
    .align 4
    .global race_pie_add_probe
    .type race_pie_add_probe, @function
race_pie_add_probe:
    entry a1, 32
    ee.vld.128.ip q0, a2, 16       // q0 = A, a2 += 16
    ee.vld.128.ip q1, a2, 16       // q1 = B, a2 += 16
    ee.vadds.s32  q2, q0, q1
    ee.vst.128.ip q2, a2, 16       // io[8..11] = résultat
    retw
    .size race_pie_add_probe, .-race_pie_add_probe
