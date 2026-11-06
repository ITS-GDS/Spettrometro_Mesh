/* Shell USB CDC: comandi "node ...". */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/bluetooth/mesh.h>
#include <hal/nrf_power.h>
#include "node.h"

/* Non esposta nell'header pubblico ma esportata dallo stack. */
extern uint16_t bt_mesh_primary_addr(void);

/* Il bootloader Adafruit entra in DFU seriale se GPREGRET = 0x4e al reset. */
#define DFU_MAGIC 0x4e

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "reset locale: cancello provisioning e chiavi mesh");
	node_reset_begin();
	bt_mesh_reset();
	return 0;
}

static int cmd_dfu(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "riavvio nel bootloader DFU...");
	k_msleep(100);
	nrf_power_gpregret_set(NRF_POWER, 0, DFU_MAGIC);
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const names[] = {"provisionabile", "link provisioning", "provisionato",
					    "reset"};
	struct sensor_snap s;

	shell_print(sh, "stato: %s", names[node_get_state()]);
	if (bt_mesh_is_provisioned()) {
		shell_print(sh, "indirizzo: 0x%04x", bt_mesh_primary_addr());
	}
	if (!sensor_snapshot(&s)) {
		shell_print(sh, "nessuna misura valida");
		return 0;
	}
	const struct as7341_reading *r = &s.r;

	shell_print(sh, "%.2f lux (grezzo %.3f, k %.3f)  %u K  gain %u",
		    (double)(r->lux * s.lux_k), (double)r->lux, (double)s.lux_k, r->cct, r->gain);
	shell_print(sh, "F1-F8: %u %u %u %u %u %u %u %u", r->ch[0], r->ch[1], r->ch[2], r->ch[3],
		    r->ch[4], r->ch[5], r->ch[6], r->ch[7]);
	shell_print(sh, "clear %u  NIR %u", r->clear, r->nir);
	return 0;
}

/* Scansione indirizzi 0x08..0x77 su entrambi i bus. */
static int cmd_scan(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *bus[] = {DEVICE_DT_GET(DT_NODELABEL(i2c1)),
				      DEVICE_DT_GET(DT_NODELABEL(i2c0))};
	const char *name[] = {"i2c1 (D4/D5)", "i2c0 (D9/D10)"};

	for (int b = 0; b < ARRAY_SIZE(bus); b++) {
		bool ready = device_is_ready(bus[b]);
		uint8_t id = 0;
		int n = 0;

		shell_print(sh, "%s%s", name[b], ready ? "" : " non pronto");
		if (!ready) {
			continue;
		}
		int rc = i2c_reg_read_byte(bus[b], AS7341_ADDR, AS7341_REG_ID, &id);

		shell_print(sh, "  ID@0x%02x rc=%d id=0x%02x", AS7341_ADDR, rc, id);
		for (uint8_t a = 0x08; a < 0x78; a++) {
			uint8_t d;

			if (i2c_read(bus[b], &d, 1, a) == 0) {
				shell_print(sh, "  0x%02x", a);
				n++;
			}
		}
		shell_print(sh, "  %d dispositivi", n);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(node_cmds,
	SHELL_CMD(reset, NULL, "Reset mesh locale (torna provisionabile)", cmd_reset),
	SHELL_CMD(dfu, NULL, "Riavvia nel bootloader (aggiornamento firmware)", cmd_dfu),
	SHELL_CMD(status, NULL, "Stato nodo e ultima misura", cmd_status),
	SHELL_CMD(scan, NULL, "Scansione I2C", cmd_scan),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(node, &node_cmds, "Comandi nodo AS7341", NULL);
