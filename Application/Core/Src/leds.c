#include "leds.h"
#include "main.h"

void blink_led(void)
{
    // Turn OFF all LEDs first
    HAL_GPIO_WritePin(GPIOE, LD5_Pin | LD7_Pin |
                              LD9_Pin | LD10_Pin |
                              LD8_Pin | LD6_Pin |
                              LD4_Pin | LD3_Pin,
                              GPIO_PIN_RESET);

   led_toggle(LD5_Pin);
   led_toggle(LD7_Pin);
   led_toggle(LD9_Pin);
   led_toggle(LD10_Pin);
   led_toggle(LD8_Pin);
   led_toggle(LD6_Pin);
   led_toggle(LD4_Pin);
   led_toggle(LD3_Pin);


}

void led_toggle(int led)
{
    HAL_GPIO_WritePin(GPIOE, led, GPIO_PIN_SET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(GPIOE, led, GPIO_PIN_RESET);
}
