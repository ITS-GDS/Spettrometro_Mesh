/* Driver minimale AS7341 (Zephyr non ne ha uno): 11 canali spettrali, I2C 0x39. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AS7341_ADDR   0x39
#define AS7341_REG_ID 0x92

struct as7341_reading {
	uint16_t ch[8];		/* F1..F8 (415..680 nm), conteggi grezzi */
	uint16_t clear, nir;
	uint8_t gain;		/* codice CFG1: guadagno = 2^(gain-1), 0 => 0.5x */
	float norm;		/* 1 / (guadagno * tempo di integrazione in ms) */
	float lux;		/* grezzo, prima della calibrazione utente */
	float cx, cy;		/* cromaticita' CIE 1931 */
	uint32_t cct;		/* kelvin */
};

/* Cerca il sensore su i2c1 (D4/D5) poi i2c0 (D9/D10). */
bool as7341_probe(void);
bool as7341_present(void);
/* Dimentica il bus: la prossima as7341_probe() riparte da zero. */
void as7341_drop(void);
/* Misura F1..F8 + clear + NIR e calcola lux/CCT/xy. Adatta il guadagno per la prossima. */
int as7341_sample(struct as7341_reading *out);
