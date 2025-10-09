#pragma once

#include <stdint.h>

// Define basic types used by the functions to avoid including more headers
typedef int esp_err_t;
typedef int gpio_num_t;

#define GPIO_NUM_5 5

#ifdef __cplusplus
extern "C" {
#endif

void gpio_hold_en(gpio_num_t gpio_num);
void gpio_hold_dis(gpio_num_t gpio_num);

#ifdef __cplusplus
}
#endif