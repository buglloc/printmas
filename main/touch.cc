#include "touch.h"

#include <sdkconfig.h>

#include <esp_log.h>
#include <esp_check.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/gpio.h>


using namespace PRNM;

namespace {
  const char* kLogTag = "prnm::touch";
  constexpr gpio_num_t kTouchGpio = static_cast<gpio_num_t>(CONFIG_PRNM_TOUCH_GPIO);
  constexpr TickType_t kDebounceMs = CONFIG_PRNM_TOUCH_DEBOUNCE;
  constexpr size_t kPollingTimeout = 10;
}

Touch& Touch::Instance()
{
  static Touch instance;
  return instance;
}

esp_err_t Touch::Initialize()
{
  ESP_LOGI(kLogTag, "init touch GPIO %d", kTouchGpio);

  gpio_config_t io_conf = {};
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pin_bit_mask = (1ULL << kTouchGpio);
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;

  esp_err_t err = gpio_config(&io_conf);
  ESP_RETURN_ON_ERROR(err, kLogTag, "configure touch GPIO");

  initialized_ = true;
  ESP_LOGI(kLogTag, "touch GPIO initialized");
  return ESP_OK;
}

bool Touch::Wait(int timeout_ms)
{
  if (!initialized_) {
    ESP_LOGE(kLogTag, "touch not initialized");
    return false;
  }

  int max = timeout_ms / kPollingTimeout;
  while (gpio_get_level(kTouchGpio) == 0) {
    vTaskDelay(pdMS_TO_TICKS(kPollingTimeout));

    if (--max <= 0) {
      return false;
    }
  }
  vTaskDelay(pdMS_TO_TICKS(kDebounceMs));

  ESP_LOGI(kLogTag, "waiting for touch...");

  while (gpio_get_level(kTouchGpio) != 0) {
    vTaskDelay(pdMS_TO_TICKS(kPollingTimeout));

    if (--max <= 0) {
      return false;
    }
  }

  vTaskDelay(pdMS_TO_TICKS(kDebounceMs));

  if (gpio_get_level(kTouchGpio) != 0) {
    ESP_LOGD(kLogTag, "touch bounced, retrying");
    return Wait(timeout_ms);
  }

  return true;
}
