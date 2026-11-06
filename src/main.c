/* Nodo BLE Mesh AS7341: composition, provisioning e avvio.
 * Sensore in sensor.c, LED di stato in led.c, shell USB in shell.c.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/bluetooth/mesh/rpr_srv.h>
#include "node.h"

LOG_MODULE_REGISTER(as7341_node, LOG_LEVEL_INF);

#define PROV_BEARERS  (BT_MESH_PROV_ADV | BT_MESH_PROV_GATT)
#define RESET_SHOW_MS 2000

static volatile enum node_state state = ST_UNPROV;
static uint8_t dev_uuid[16];

enum node_state node_get_state(void)
{
	return state;
}

/* ---- Reset ------------------------------------------------------------ */

static void rearm_work_fn(struct k_work *w)
{
	/* Dopo un reset lo stack non riabilita da solo i bearer di provisioning. */
	int err = bt_mesh_prov_enable(PROV_BEARERS);

	if (err) {
		LOG_ERR("bt_mesh_prov_enable err=%d", err);
	}
	state = ST_UNPROV;
	LOG_INF("reset completato: in attesa di provisioning");
}
static K_WORK_DELAYABLE_DEFINE(rearm_work, rearm_work_fn);

void node_reset_begin(void)
{
	state = ST_RESET;
	sensor_invalidate();
	k_work_reschedule(&rearm_work, K_MSEC(RESET_SHOW_MS));
}

/* ---- Composition ------------------------------------------------------ */

BT_MESH_HEALTH_PUB_DEFINE(health_pub, 0);

static void attention_on(const struct bt_mesh_model *mod)
{
	led_attention(true);
}

static void attention_off(const struct bt_mesh_model *mod)
{
	led_attention(false);
}

static const struct bt_mesh_health_srv_cb health_cb = {
	.attn_on = attention_on,
	.attn_off = attention_off,
};

static struct bt_mesh_health_srv health_srv = {
	.cb = &health_cb,
};

static struct bt_mesh_elem elements[] = {
	BT_MESH_ELEM(0,
		     BT_MESH_MODEL_LIST(BT_MESH_MODEL_CFG_SRV,
					BT_MESH_MODEL_HEALTH_SRV(&health_srv, &health_pub),
					BT_MESH_MODEL_SENSOR_SRV(&sensor_srv),
					BT_MESH_MODEL_RPR_SRV),
		     BT_MESH_MODEL_NONE),
};

static const struct bt_mesh_comp comp = {
	.cid = CONFIG_BT_COMPANY_ID,
	.elem = elements,
	.elem_count = ARRAY_SIZE(elements),
};

/* ---- Provisioning ----------------------------------------------------- */

static void prov_link_open(bt_mesh_prov_bearer_t bearer)
{
	LOG_INF("provisioning: link aperto (bearer=%d)", bearer);
	state = ST_LINK;
}

static void prov_link_close(bt_mesh_prov_bearer_t bearer)
{
	LOG_INF("provisioning: link chiuso (bearer=%d)", bearer);
	if (state == ST_LINK) {
		state = bt_mesh_is_provisioned() ? ST_PROV : ST_UNPROV;
	}
}

static void prov_complete(uint16_t net_idx, uint16_t addr)
{
	LOG_INF("provisionato: net_idx=%u addr=0x%04x", net_idx, addr);
	state = ST_PROV;
}

/* Node Reset dalla mesh: lo stack ha gia' cancellato le chiavi. */
static void prov_reset(void)
{
	LOG_INF("reset da mesh (Node Reset)");
	node_reset_begin();
}

static const struct bt_mesh_prov prov = {
	.uuid = dev_uuid,
	.link_open = prov_link_open,
	.link_close = prov_link_close,
	.complete = prov_complete,
	.reset = prov_reset,
};

/* ---- Avvio ------------------------------------------------------------ */

static void bt_ready(int err)
{
	if (err) {
		LOG_ERR("bt init err=%d", err);
		return;
	}
	err = bt_mesh_init(&prov, &comp);
	if (err) {
		LOG_ERR("mesh init err=%d", err);
		return;
	}
	settings_load();
	/* No-op se settings_load() ha ripristinato un provisioning. */
	bt_mesh_prov_enable(PROV_BEARERS);
	state = bt_mesh_is_provisioned() ? ST_PROV : ST_UNPROV;
	LOG_INF("mesh pronta, %s",
		bt_mesh_is_provisioned() ? "provisionato" : "in attesa di provisioning");
}

int main(void)
{
	uint8_t id[8];

	led_init();
	sensor_start();

	/* UUID dal Device ID hardware: stabile tra i riavvii. */
	hwinfo_get_device_id(id, sizeof(id));
	memcpy(dev_uuid, id, sizeof(id));
	memcpy(dev_uuid + 8, id, sizeof(id));

	int err = bt_enable(bt_ready);

	if (err) {
		LOG_ERR("bt_enable err=%d", err);
	}
	return 0;
}
