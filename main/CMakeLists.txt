idf_component_register(
  SRCS "main.c" "battery.c"
  PRIV_REQUIRES spi_flash esp_driver_ledc
  INCLUDE_DIRS "."
)
