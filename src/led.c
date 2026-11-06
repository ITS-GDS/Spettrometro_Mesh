/* LED RGB di stato.
 * Provisionabile : blu, lampeggio breve ogni 1 s
 * Provisioning   : blu fisso (link PB-ADV/PB-GATT aperto)
 * Provisionato   : verde, impulso breve ogni 3 s
 * Reset          : rosso, lampeggio veloce, poi torna provisionabile
 * Attention      : 4 lampi bianchi + pausa (Health Server, ha priorita')
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "node.h"

#define LED_TICK_MS 100

/* Il rosso e' molto piu' efficiente degli altri canali: per ottenere un
 * bianco i tre canali usano un duty diverso (su 8 passi, timer a 250 us).
 */
#define WHITE_R 1
#define WHITE_G 8
#define WHITE_B 4
#define WHITE_STEP_US 250

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

static volatile bool attention;
static volatile bool attention_lit;
static uint32_t attention_t;

static void led_set(bool r, bool g, bool b)
{
	gpio_pin_set_dt(&led_r, r);
	gpio_pin_set_dt(&led_g, g);
	gpio_pin_set_dt(&led_b, b);
}

static void white_timer_fn(struct k_timer *t)
{
	static uint8_t ph;

	ph = (ph + 1) & 7;
	if (attention_lit) {
		gpio_pin_set_dt(&led_r, ph < WHITE_R);
		gpio_pin_set_dt(&led_g, ph < WHITE_G);
		gpio_pin_set_dt(&led_b, ph < WHITE_B);
	}
}
static K_TIMER_DEFINE(white_timer, white_timer_fn, NULL);

static void led_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(led_work, led_fn);

static void led_fn(struct k_work *w)
{
	static uint32_t t;

	t++;
	if (attention) {
		/* 100 ms on / 100 ms off x4, poi 800 ms di pausa. */
		uint32_t p = attention_t++ % 12;
		bool on = p < 8 && !(p & 1);

		attention_lit = on;
		if (!on) {
			led_set(false, false, false);
		}
	} else {
		switch (node_get_state()) {
		case ST_UNPROV:
			led_set(false, false, (t % 10) == 0);
			break;
		case ST_LINK:
			led_set(false, false, true);
			break;
		case ST_PROV:
			led_set(false, (t % 30) == 0, false);
			break;
		case ST_RESET:
			led_set(t & 1, false, false);
			break;
		}
	}
	k_work_reschedule(&led_work, K_MSEC(LED_TICK_MS));
}

void led_attention(bool on)
{
	if (on) {
		attention_t = 0;
		attention = true;
		k_timer_start(&white_timer, K_USEC(WHITE_STEP_US), K_USEC(WHITE_STEP_US));
		return;
	}
	attention = false;
	attention_lit = false;
	k_timer_stop(&white_timer);
	led_set(false, false, false);
}

void led_init(void)
{
	const struct gpio_dt_spec *leds[] = {&led_r, &led_g, &led_b};

	for (int i = 0; i < ARRAY_SIZE(leds); i++) {
		if (gpio_is_ready_dt(leds[i])) {
			gpio_pin_configure_dt(leds[i], GPIO_OUTPUT_INACTIVE);
		}
	}
	k_work_reschedule(&led_work, K_MSEC(LED_TICK_MS));
}
