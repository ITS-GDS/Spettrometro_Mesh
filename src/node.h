/* Interfaccia condivisa tra i moduli del nodo. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <zephyr/drivers/sensor.h>
#include <bluetooth/mesh/models.h>
#include "as7341.h"

enum node_state { ST_UNPROV, ST_LINK, ST_PROV, ST_RESET };

/* main.c */
enum node_state node_get_state(void);
void node_reset_begin(void);

/* led.c */
void led_init(void);
void led_attention(bool on);

/* sensor.c */
struct sensor_snap {
	struct as7341_reading r;
	float lux_k; /* coefficiente di calibrazione utente */
};

extern struct bt_mesh_sensor_srv sensor_srv;
void sensor_start(void);
void sensor_invalidate(void);
/* Copia l'ultima misura. Ritorna false se non ce n'e' una valida. */
bool sensor_snapshot(struct sensor_snap *s);
