#include <driver/i2c_master.h>

#include"rtc_operations.h"

#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_TIMEOUT_MS 1000
#define I2C_MASTER_ADDR_BIT_LENGTH I2C_ADDR_BIT_LEN_7

typedef struct {
  i2c_master_dev_handle_t dev_handle;
  i2c_port_t port;
  uint8_t address;
  gpio_num_t sda_io_num;
  gpio_num_t scl_io_num;
  uint32_t clk_speed;
} i2c_dev_t;

esp_err_t i2c_master_init_(i2c_master_bus_handle_t *bus_handle) {
  i2c_master_bus_config_t i2c_mst_config = {
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .i2c_port = I2C_MASTER_NUM,
      .scl_io_num = I2C_MASTER_SCL_IO,
      .sda_io_num = I2C_MASTER_SDA_IO,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };

  esp_err_t ret = i2c_new_master_bus(&i2c_mst_config, bus_handle);
  return ret;
}

esp_err_t i2c_device_add(i2c_master_bus_handle_t *bus_handle,
                         i2c_dev_t *i2c_dev, uint8_t address, uint32_t speed) {
  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_MASTER_ADDR_BIT_LENGTH,
      .device_address = address,
      .scl_speed_hz = speed,
  };

  esp_err_t ret =
      i2c_master_bus_add_device(*bus_handle, &dev_cfg, &(i2c_dev->dev_handle));
  if (ret != ESP_OK) {
    return ret;
  }

  ret = i2c_master_probe(*bus_handle, address, I2C_MASTER_TIMEOUT_MS);
  return ret;
}