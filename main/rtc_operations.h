#ifndef RTC_OPERATIONS_H
#define RTC_OPERATIONS_H

#include <time.h>
#include <stdbool.h>
// #include "driver/i2c.h"
#include <driver/i2c_master.h>
#include "esp_err.h"

#define PCF8563T_ADDRESS 0x51 // I2C address of the RTC (this is correct)

// Register addresses
#define PCF8563T_CONTROL_STATUS1 0x00
#define PCF8563T_CONTROL_STATUS2 0x01
#define PCF8563T_SEC 0x02
#define PCF8563T_MIN 0x03
#define PCF8563T_HOUR 0x04
#define PCF8563T_DAY 0x05
#define PCF8563T_DATE 0x05
#define PCF8563T_WEEKDAY 0x06
#define PCF8563T_MONTH 0x07
#define PCF8563T_YEAR 0x08

// #define PCF8563_ADDR 0x51 // I2C address

// // Register addresses
// #define PCF8563_ADDR_STATUS1 0x00
// #define PCF8563_ADDR_STATUS2 0x01
#define PCF8563_ADDR_TIME 0x02
// #define PCF8563_ADDR_ALARM   0x09
// #define PCF8563_ADDR_CONTROL 0x0d
// #define PCF8563_ADDR_TIMER   0x0e

// Define CHECK_ARG macro
#define CHECK_ARG(ARG)                  \
    do                                  \
    {                                   \
        if (!(ARG))                     \
            return ESP_ERR_INVALID_ARG; \
    } while (0)

// Function prototypes
uint8_t bcd2dec(uint8_t val);
uint8_t dec2bcd(uint8_t val);
esp_err_t pcf8563_init_desc(i2c_dev_t *dev, i2c_port_t port, gpio_num_t sda_gpio, gpio_num_t scl_gpio);
esp_err_t pcf8563_reset(i2c_dev_t *dev);
esp_err_t pcf8563_set_time(i2c_dev_t *dev, struct tm *time);
esp_err_t pcf8563_get_time(i2c_dev_t *dev, struct tm *time);

// Additional functions from RTC_PCF8563
uint8_t pcf8563_read_byte(uint8_t reg);
void pcf8563_write_byte(uint8_t reg, uint8_t data);
void read_rtc_time(struct tm *timeinfo);
void set_rtc_time(struct tm *timeinfo);

// New function to fetch formatted time string
char *fetchTime();

#endif // RTC_OPERATIONS_H