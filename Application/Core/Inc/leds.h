#ifndef __LEDS_H
#define __LEDS_H

#ifdef __cplusplus
extern "C" {
#endif

void blink_led(void);

void led_toggle(int led);

#ifdef __cplusplus
}
#endif

#endif /* __LEDS_H */