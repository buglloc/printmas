idf_component_register(
SRCS
  "main.cc"
  "ble.cc"
  "printer.cc"
  "touch.cc"
  "leds.cc"

INCLUDE_DIRS
  "."
REQUIRES
  bt
  esp_timer
  driver
  soc
  nvs_flash

  signs
)