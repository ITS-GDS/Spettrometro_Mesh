#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include "as7341.h"

LOG_MODULE_REGISTER(as7341, LOG_LEVEL_INF);

#define AS_ENABLE  0x80
#define AS_ATIME   0x81
#define AS_STATUS2 0xA3
#define AS_CFG1    0xAA
#define AS_CFG6    0xAF
#define AS_ASTEP_L 0xCA
#define AS_CH0_L   0x95

#define AS_ATIME_V 29
#define AS_ASTEP_V 599
/* tempo di integrazione = (ATIME+1)*(ASTEP+1)*2.78 us ~ 50 ms */
#define AS_TINT_MS ((AS_ATIME_V + 1) * (AS_ASTEP_V + 1) * 2.78f / 1000.0f)

#define AS_GAIN_DEFAULT 6 /* 32x */
#define AS_GAIN_MAX     9
#define CLEAR_HIGH      50000 /* sopra: riduci il guadagno */
#define CLEAR_LOW       3000  /* sotto: aumentalo */

/* Fattore di taratura conteggi-base -> lux: indicativo, da tarare con un luxmetro. */
#define LUX_K 0.35f

/* CIE 1931 xbar/ybar/zbar alle lunghezze d'onda centrali di F1..F8 (415..680 nm). */
static const float cie_x[8] = {0.044f, 0.336f, 0.096f, 0.036f, 0.514f, 1.026f, 0.642f, 0.047f};
static const float cie_y[8] = {0.001f, 0.030f, 0.139f, 0.607f, 0.995f, 0.757f, 0.265f, 0.017f};
static const float cie_z[8] = {0.207f, 1.772f, 0.813f, 0.060f, 0.006f, 0.001f, 0.000f, 0.000f};

/* SMUX: F1-F4 + clear + NIR, poi F5-F8 + clear + NIR (da datasheet/Adafruit). */
static const uint8_t smux_f1f4[20] = {0x30, 0x01, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x50, 0x00,
				      0x00, 0x00, 0x20, 0x04, 0x00, 0x30, 0x01, 0x50, 0x00, 0x06};
static const uint8_t smux_f5f8[20] = {0x00, 0x00, 0x00, 0x40, 0x02, 0x00, 0x10, 0x03, 0x50, 0x10,
				      0x03, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x50, 0x00, 0x06};

static const struct device *bus;
static uint8_t gain = AS_GAIN_DEFAULT;

static int wr(uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte(bus, AS7341_ADDR, reg, val);
}

static int rd(uint8_t reg, uint8_t *val)
{
	return i2c_reg_read_byte(bus, AS7341_ADDR, reg, val);
}

static int set_smux(const uint8_t *cfg)
{
	int err = wr(AS_ENABLE, 0x01); /* PON, spettrale off */

	if (!err) {
		err = wr(AS_CFG6, 0x10); /* scrittura SMUX */
	}
	for (int i = 0; !err && i < 20; i++) {
		err = wr(i, cfg[i]);
	}
	if (!err) {
		err = wr(AS_ENABLE, 0x11); /* PON | SMUXEN */
	}
	for (int i = 0; !err && i < 50; i++) {
		uint8_t e;

		k_msleep(2);
		err = rd(AS_ENABLE, &e);
		if (!err && !(e & 0x10)) {
			return 0;
		}
	}
	return err ? err : -ETIMEDOUT;
}

/* Attende AVALID (dato spettrale pronto). */
static int wait_data(void)
{
	for (int i = 0; i < 100; i++) {
		uint8_t st;
		int err;

		k_msleep(10);
		err = rd(AS_STATUS2, &st);
		if (err || (st & 0x40)) {
			return err;
		}
	}
	return -ETIMEDOUT;
}

/* Misura 6 canali con la mappa SMUX data; out = 4 filtri, clear, NIR. */
static int measure(const uint8_t *cfg, uint16_t out[6])
{
	uint8_t buf[12];
	int err = set_smux(cfg);

	if (!err) {
		err = wr(AS_ENABLE, 0x03); /* PON | SP_EN */
	}
	if (!err) {
		err = wait_data();
	}
	if (!err) {
		err = i2c_burst_read(bus, AS7341_ADDR, AS_CH0_L, buf, sizeof(buf));
	}
	wr(AS_ENABLE, 0x01);
	if (err) {
		return err;
	}
	for (int i = 0; i < 6; i++) {
		out[i] = buf[2 * i] | (buf[2 * i + 1] << 8);
	}
	return 0;
}

static int init_on(const struct device *dev)
{
	uint8_t id;

	bus = dev;
	if (!device_is_ready(dev) || rd(AS7341_REG_ID, &id) || (id >> 2) != 0x09) {
		return -ENODEV;
	}
	gain = AS_GAIN_DEFAULT;
	return wr(AS_ENABLE, 0x01) ?: wr(AS_ATIME, AS_ATIME_V) ?:
	       wr(AS_ASTEP_L, AS_ASTEP_V & 0xff) ?: wr(AS_ASTEP_L + 1, AS_ASTEP_V >> 8) ?:
	       wr(AS_CFG1, gain);
}

bool as7341_probe(void)
{
	if (init_on(DEVICE_DT_GET(DT_NODELABEL(i2c1))) == 0) {
		LOG_INF("AS7341 su i2c1 (D4/D5)");
	} else if (init_on(DEVICE_DT_GET(DT_NODELABEL(i2c0))) == 0) {
		LOG_INF("AS7341 su i2c0 (D9/D10)");
	} else {
		bus = NULL;
	}
	return bus != NULL;
}

bool as7341_present(void)
{
	return bus != NULL;
}

void as7341_drop(void)
{
	bus = NULL;
}

/* Scelta guadagno automatica sul canale clear. */
static void autogain(uint16_t clear)
{
	uint8_t g = gain;

	if (clear > CLEAR_HIGH && g > 0) {
		g--;
	} else if (clear < CLEAR_LOW && g < AS_GAIN_MAX) {
		g++;
	}
	if (g != gain) {
		gain = g;
		wr(AS_CFG1, gain);
	}
}

int as7341_sample(struct as7341_reading *out)
{
	uint16_t a[6], b[6];
	int err = measure(smux_f1f4, a) ?: measure(smux_f5f8, b);

	if (err) {
		return err;
	}
	for (int i = 0; i < 4; i++) {
		out->ch[i] = a[i];
		out->ch[4 + i] = b[i];
	}
	out->clear = a[4];
	out->nir = a[5];
	out->gain = gain;
	out->norm = 1.0f / ((gain ? (float)(1u << (gain - 1)) : 0.5f) * AS_TINT_MS);

	float X = 0, Y = 0, Z = 0;

	for (int i = 0; i < 8; i++) {
		float c = out->ch[i] * out->norm;

		X += c * cie_x[i];
		Y += c * cie_y[i];
		Z += c * cie_z[i];
	}
	out->lux = Y * LUX_K;
	out->cx = out->cy = 0;
	out->cct = 0;
	if (X + Y + Z > 0.0f) {
		float x = X / (X + Y + Z), y = Y / (X + Y + Z);
		float n = (x - 0.3320f) / (0.1858f - y); /* McCamy */
		float cct = 449.0f * n * n * n + 3525.0f * n * n + 6823.3f * n + 5520.33f;

		out->cx = x;
		out->cy = y;
		out->cct = cct < 800.0f ? 800 : cct > 65000.0f ? 65000 : (uint32_t)cct;
	}
	autogain(out->clear);
	return 0;
}
