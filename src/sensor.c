/* Sensor Server: lux, CCT, cromaticita' (standard SIG) e spettro (0xF341, custom).
 * La misura vive in una cache protetta da mutex: la scrive il work di
 * campionamento, la leggono lo stack mesh e la shell.
 * Calibrazione lux dall'app: vedi CALIBRAZIONE-LUX.md.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/mesh.h>
#include <bluetooth/mesh/sensor_types.h>
#include "node.h"

LOG_MODULE_REGISTER(sensor, LOG_LEVEL_INF);

#define SAMPLE_PERIOD_S  5
#define PUBLISH_PERIOD_S 30
#define SAMPLE_FAILS_MAX 3

#define SPEC_CH       10
#define SPEC_PROP_ID  0xF341
#define SPEC_SCALE    100.0f
#define SPEC_SAT      65534 /* 0xFFFF e' "non noto" nel formato count_16 */

#define LUX_CAL_UNKNOWN 0xFFFFFFu

static K_MUTEX_DEFINE(lock);
static struct {
	bool valid;
	struct as7341_reading r;
} cache;

/* L'app scrive il lux reale R in centilux (Sensor Setting 0x004E, metodo Silvair):
 * coefficiente = R / lux grezzo attuale, salvato in flash.
 */
static struct {
	float k;
	uint32_t ref_cl; /* ultimo lux reale scritto dall'app (centilux) */
} lux_cal = {.k = 1.0f, .ref_cl = LUX_CAL_UNKNOWN};

bool sensor_snapshot(struct sensor_snap *s)
{
	k_mutex_lock(&lock, K_FOREVER);
	bool ok = cache.valid;

	if (ok) {
		s->r = cache.r;
		s->lux_k = lux_cal.k;
	}
	k_mutex_unlock(&lock);
	return ok;
}

void sensor_invalidate(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	cache.valid = false;
	k_mutex_unlock(&lock);
}

/* ---- Calibrazione lux -------------------------------------------------- */

static int lux_cal_settings_set(const char *name, size_t len, settings_read_cb read_cb,
				void *cb_arg)
{
	if (!strcmp(name, "luxcal") && len == sizeof(lux_cal)) {
		read_cb(cb_arg, &lux_cal, sizeof(lux_cal));
		return 0;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(as7341, "as7341", NULL, lux_cal_settings_set, NULL, NULL);

static void lux_cal_get(struct bt_mesh_sensor_srv *srv, struct bt_mesh_sensor *sensor,
			const struct bt_mesh_sensor_setting *setting, struct bt_mesh_msg_ctx *ctx,
			struct bt_mesh_sensor_value *rsp)
{
	struct sensor_value v = {.val1 = lux_cal.ref_cl / 100,
				 .val2 = (lux_cal.ref_cl % 100) * 10000};

	if (lux_cal.ref_cl == LUX_CAL_UNKNOWN ||
	    bt_mesh_sensor_value_from_sensor_value(setting->type->channels[0].format, &v, rsp)) {
		rsp->format = setting->type->channels[0].format;
		memset(rsp->raw, 0xFF, 3);
	}
}

static int lux_cal_set(struct bt_mesh_sensor_srv *srv, struct bt_mesh_sensor *sensor,
		       const struct bt_mesh_sensor_setting *setting, struct bt_mesh_msg_ctx *ctx,
		       const struct bt_mesh_sensor_value *value)
{
	struct sensor_value v;
	int err = 0;

	k_mutex_lock(&lock, K_FOREVER);
	if (bt_mesh_sensor_value_to_sensor_value(value, &v) != BT_MESH_SENSOR_VALUE_NUMBER) {
		/* "non noto" (0xFFFFFF): azzera la calibrazione */
		lux_cal.k = 1.0f;
		lux_cal.ref_cl = LUX_CAL_UNKNOWN;
	} else {
		float real = sensor_value_to_float(&v);

		if (!cache.valid || cache.r.lux < 0.0005f || real <= 0.0f) {
			err = -EINVAL;
		} else {
			lux_cal.k = real / cache.r.lux;
			lux_cal.ref_cl = (uint32_t)(real * 100.0f + 0.5f);
			LOG_INF("calibrazione lux: reale %.2f, grezzo %.3f, k=%.3f", (double)real,
				(double)cache.r.lux, (double)lux_cal.k);
		}
	}
	if (!err) {
		err = settings_save_one("as7341/luxcal", &lux_cal, sizeof(lux_cal));
	}
	k_mutex_unlock(&lock);
	return err;
}

static const struct bt_mesh_sensor_setting lux_settings[] = {
	{
		.type = &bt_mesh_sensor_present_amb_light_level,
		.get = lux_cal_get,
		.set = lux_cal_set,
	},
};

/* ---- Sensori ----------------------------------------------------------- */

static const struct bt_mesh_sensor_descriptor desc = {
	.tolerance = {
		.negative = BT_MESH_SENSOR_TOLERANCE_ENCODE(0),
		.positive = BT_MESH_SENSOR_TOLERANCE_ENCODE(0),
	},
	.sampling_type = BT_MESH_SENSOR_SAMPLING_UNSPECIFIED,
	.period = 0,
	.update_interval = 0,
};

/* Fuori scala (-ERANGE) non e' un errore: il valore viene saturato. */
static int encode(const struct bt_mesh_sensor_format *fmt, const struct sensor_value *v,
		  struct bt_mesh_sensor_value *rsp)
{
	int err = bt_mesh_sensor_value_from_sensor_value(fmt, v, rsp);

	return (err && err != -ERANGE) ? err : 0;
}

static int lux_get(struct bt_mesh_sensor_srv *srv, struct bt_mesh_sensor *sensor,
		   struct bt_mesh_msg_ctx *ctx, struct bt_mesh_sensor_value *rsp)
{
	struct sensor_snap s;
	struct sensor_value v;

	if (!sensor_snapshot(&s)) {
		return -EAGAIN;
	}
	sensor_value_from_float(&v, s.r.lux * s.lux_k);
	return encode(sensor->type->channels[0].format, &v, rsp);
}

static int cct_get(struct bt_mesh_sensor_srv *srv, struct bt_mesh_sensor *sensor,
		   struct bt_mesh_msg_ctx *ctx, struct bt_mesh_sensor_value *rsp)
{
	struct sensor_snap s;
	struct sensor_value v = {0};

	if (!sensor_snapshot(&s)) {
		return -EAGAIN;
	}
	v.val1 = s.r.cct;
	return encode(sensor->type->channels[0].format, &v, rsp);
}

static int chrom_get(struct bt_mesh_sensor_srv *srv, struct bt_mesh_sensor *sensor,
		     struct bt_mesh_msg_ctx *ctx, struct bt_mesh_sensor_value *rsp)
{
	struct sensor_snap s;

	if (!sensor_snapshot(&s)) {
		return -EAGAIN;
	}
	const float xy[2] = {s.r.cx, s.r.cy};

	for (int i = 0; i < 2; i++) {
		struct sensor_value v;
		int err;

		sensor_value_from_float(&v, xy[i]);
		err = encode(sensor->type->channels[i].format, &v, &rsp[i]);
		if (err) {
			return err;
		}
	}
	return 0;
}

/* Spettro: 10 canali count_16 in ordine F1..F8, clear, NIR.
 * Valore = conteggi / (guadagno * ms) * 100, saturato a SPEC_SAT.
 */
#ifdef CONFIG_BT_MESH_SENSOR_LABELS
#define SPEC_CHAN(n) {.format = &bt_mesh_sensor_format_count_16, .name = n}
#else
#define SPEC_CHAN(n) {.format = &bt_mesh_sensor_format_count_16}
#endif

static const struct bt_mesh_sensor_channel spec_channels[SPEC_CH] = {
	SPEC_CHAN("F1 415nm"), SPEC_CHAN("F2 445nm"), SPEC_CHAN("F3 480nm"),
	SPEC_CHAN("F4 515nm"), SPEC_CHAN("F5 555nm"), SPEC_CHAN("F6 590nm"),
	SPEC_CHAN("F7 630nm"), SPEC_CHAN("F8 680nm"), SPEC_CHAN("Clear"), SPEC_CHAN("NIR"),
};

static const struct bt_mesh_sensor_type spec_type = {
	.id = SPEC_PROP_ID,
	.channel_count = SPEC_CH,
	.channels = spec_channels,
};

static int spec_get(struct bt_mesh_sensor_srv *srv, struct bt_mesh_sensor *sensor,
		    struct bt_mesh_msg_ctx *ctx, struct bt_mesh_sensor_value *rsp)
{
	struct sensor_snap s;
	uint16_t raw[SPEC_CH];

	if (!sensor_snapshot(&s)) {
		return -EAGAIN;
	}
	memcpy(raw, s.r.ch, sizeof(s.r.ch));
	raw[8] = s.r.clear;
	raw[9] = s.r.nir;
	for (int i = 0; i < SPEC_CH; i++) {
		float f = raw[i] * s.r.norm * SPEC_SCALE;
		struct sensor_value v = {.val1 = f > SPEC_SAT ? SPEC_SAT : (int32_t)f};
		int err = encode(spec_channels[i].format, &v, &rsp[i]);

		if (err) {
			return err;
		}
	}
	return 0;
}

static struct bt_mesh_sensor lux_sensor = {
	.type = &bt_mesh_sensor_present_amb_light_level,
	.get = lux_get,
	.descriptor = &desc,
	.settings = {.list = lux_settings, .count = ARRAY_SIZE(lux_settings)},
};
static struct bt_mesh_sensor cct_sensor = {
	.type = &bt_mesh_sensor_present_correlated_col_temp,
	.get = cct_get,
	.descriptor = &desc,
};
static struct bt_mesh_sensor chrom_sensor = {
	.type = &bt_mesh_sensor_present_cie_1931_chromaticity_coords,
	.get = chrom_get,
	.descriptor = &desc,
};
static struct bt_mesh_sensor spec_sensor = {
	.type = &spec_type,
	.get = spec_get,
	.descriptor = &desc,
};

static struct bt_mesh_sensor *const sensors[] = {&lux_sensor, &cct_sensor, &chrom_sensor,
						&spec_sensor};
struct bt_mesh_sensor_srv sensor_srv = BT_MESH_SENSOR_SRV_INIT(sensors, ARRAY_SIZE(sensors));

/* ---- Campionamento / pubblicazione ------------------------------------- */

static void publish_all(void)
{
	struct bt_mesh_sensor_value v[SPEC_CH];

	if (!bt_mesh_is_provisioned()) {
		return;
	}
	for (int i = 0; i < ARRAY_SIZE(sensors); i++) {
		if (sensors[i]->get(&sensor_srv, sensors[i], NULL, v)) {
			continue;
		}
		/* -EADDRNOTAVAIL = publicazione non ancora configurata: normale. */
		int err = bt_mesh_sensor_srv_pub(&sensor_srv, NULL, sensors[i], v);

		if (err && err != -EADDRNOTAVAIL) {
			LOG_WRN("pub sensore %d err=%d", i, err);
		}
	}
}

static void sample_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(sample_work, sample_fn);

static void sample_fn(struct k_work *w)
{
	static int ticks, fails;
	struct as7341_reading r;

	k_work_reschedule(&sample_work, K_SECONDS(SAMPLE_PERIOD_S));
	if (!as7341_present() && !as7341_probe()) {
		return;
	}
	int err = as7341_sample(&r);

	if (err) {
		LOG_WRN("as7341_sample err=%d", err);
		if (++fails >= SAMPLE_FAILS_MAX) { /* sensore sparito o bloccato: riparti da capo */
			fails = 0;
			sensor_invalidate();
			as7341_drop();
		}
		return;
	}
	fails = 0;
	k_mutex_lock(&lock, K_FOREVER);
	if (!r.cct && cache.valid) { /* buio totale: tieni l'ultimo colore noto */
		r.cx = cache.r.cx;
		r.cy = cache.r.cy;
		r.cct = cache.r.cct;
	}
	cache.r = r;
	cache.valid = true;
	float lux = r.lux * lux_cal.k;
	k_mutex_unlock(&lock);

	LOG_INF("%.2f lux  %u K  clear %u  gain %u", (double)lux, r.cct, r.clear, r.gain);
	if (++ticks >= PUBLISH_PERIOD_S / SAMPLE_PERIOD_S) {
		ticks = 0;
		publish_all();
	}
}

void sensor_start(void)
{
	if (!as7341_probe()) {
		LOG_ERR("AS7341 non trovato (controlla cablaggio, 0x39): riprovo ogni %d s",
			SAMPLE_PERIOD_S);
	}
	k_work_reschedule(&sample_work, K_SECONDS(SAMPLE_PERIOD_S));
}
