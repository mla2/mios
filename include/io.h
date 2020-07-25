#pragma once

#include <stddef.h>
#include <stdint.h>

#include "error.h"

typedef struct i2c i2c_t;
typedef struct spi spi_t;


typedef enum {
  GPIO_PULL_NONE = 0,
  GPIO_PULL_UP = 1,
  GPIO_PULL_DOWN = 2,
} gpio_pull_t;

typedef enum {
  GPIO_PUSH_PULL = 0,
  GPIO_OPEN_DRAIN = 1,
} gpio_output_type_t;


typedef enum {
  GPIO_SPEED_LOW       = 0,
  GPIO_SPEED_MID       = 1,
  GPIO_SPEED_HIGH      = 2,
  GPIO_SPEED_VERY_HIGH = 3,
} gpio_output_speed_t;

typedef enum {
  GPIO_FALLING_EDGE    = 0x1,
  GPIO_RISING_EDGE     = 0x2,
  GPIO_BOTH_EDGES      = 0x3,
} gpio_edge_t;



#include "io_types.h"

// I2C

error_t i2c_rw(i2c_t *bus, uint8_t addr,
               const uint8_t *write, size_t write_len,
               uint8_t *read, size_t read_len);

// SPI

error_t spi_rw(spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len,
               gpio_t nss);


// I2C

void gpio_conf_input(gpio_t gpio, gpio_pull_t pull);

void gpio_conf_output(gpio_t gpio, gpio_output_type_t type,
                      gpio_output_speed_t speed, gpio_pull_t pull);

void gpio_set_output(gpio_t gpio, int on);

int gpio_get_input(gpio_t gpio);

void gpio_conf_af(gpio_t gpio, int af, gpio_output_type_t type,
                  gpio_output_speed_t speed, gpio_pull_t pull);

void gpio_conf_irq(gpio_t gpio, gpio_pull_t pull,
                   void (*cb)(void *arg), void *arg,
                   gpio_edge_t edge, int level);
