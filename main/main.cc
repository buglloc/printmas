#include <esp_check.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_random.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "helpers.h"

#include <nvs.h>
#include <nvs_flash.h>

#include "ble.h"
#include "leds.h"
#include "printer.h"
#include "touch.h"
#include "signs.h"

namespace {
const char* kLogTag = "prnm::main";

PRNM::NiimbotPrinter g_printer;

}

namespace {

void showError() {
  PRNM::Leds::Instance().StartAnimation(PRNM::LedAnimation::BlinkAll);
  vTaskDelay(pdMS_TO_TICKS(2000));
  PRNM::Leds::Instance().Stop();
}

}

extern "C" {

void app_main(void)
{
#if CONFIG_PRNM_DEVMODE
  esp_log_level_set("*", ESP_LOG_DEBUG);
#else
  esp_log_level_set("*", ESP_LOG_INFO);
#endif

  esp_err_t err = ESP_OK;

  ESP_LOGI(kLogTag, "initialize nvs");
  {
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      nvs_flash_erase();
      err = nvs_flash_init();
    }
    ESP_SHUTDOWN_ON_ERROR(err, kLogTag, "initialize nvs");
  }

  ESP_LOGI(kLogTag, "Initialize touch sensor");
  {
    err = PRNM::Touch::Instance().Initialize();
    ESP_SHUTDOWN_ON_ERROR(err, kLogTag, "initialize touch sensor");
  }

  ESP_LOGI(kLogTag, "Initialize LEDs");
  {
    err = PRNM::Leds::Instance().Initialize();
    ESP_SHUTDOWN_ON_ERROR(err, kLogTag, "initialize LEDs");
  }

  ESP_LOGI(kLogTag, "Initialize printer");
  {
    auto& ble = PRNM::BLEClient::Instance();

    // Set up printer callbacks
    g_printer.SetSendCallback([&ble](const uint8_t* data, size_t len, bool wait) {
      ble.SendData(data, len, wait);
    });

    // Set up BLE callbacks
    ble.SetDataReceivedCallback([](const uint8_t* data, size_t len) {
      g_printer.ProcessReceivedData(data, len);
    });

    ble.SetWriteCompleteCallback([]() {
      g_printer.OnWriteComplete();
    });

    ble.SetConnectedCallback([]() {
      ESP_LOGI(kLogTag, "BLE connected, querying printer...");
      esp_err_t err = g_printer.SendHeartbeat();
      if (err != ESP_OK) {
        ESP_LOGE(kLogTag, "failed to send heartbeat: %s", esp_err_to_name(err));
      }
    });
  }

  ESP_LOGI(kLogTag, "Initialize BLE");
  {
    err = PRNM::BLEClient::Instance().Initialize();
    ESP_SHUTDOWN_ON_ERROR(err, kLogTag, "initialize BLE");
  }

  ESP_LOGI(kLogTag, "Initialized, running main loop!");
  while (true) {
    bool touched = PRNM::Touch::Instance().Wait(CONFIG_PRNM_PRINTER_PING_MS);
    if (!touched) {
      if (g_printer.IsReady()) {
        esp_err_t err = g_printer.GetPrintStatus();
        if (err != ESP_OK) {
          ESP_LOGE(kLogTag, "failed to ping printer: %s", esp_err_to_name(err));
        }
      }

      continue;
    }

    ESP_LOGI(kLogTag, "Touch detected");
    if (!g_printer.IsReady()) {
      ESP_LOGE(kLogTag, "printer not ready");
      showError();
      continue;
    }

    ESP_LOGI(kLogTag, "Printer ready");

    uint8_t animId = 1 + (esp_random() % PRNM::kNumRandomAnimations);
    ESP_LOGI(kLogTag, "Starting LED animation %d", animId);
    PRNM::Leds::Instance().StartAnimation(animId);

    ESP_LOGI(kLogTag, "Printing next sign...");
    const PRNM::Signs::RleImage* sign = PRNM::Signs::Next();
    assert(sign);
    err = g_printer.Print(*sign);
    if (err != ESP_OK) {
      ESP_LOGE(kLogTag, "Failed to print sign: %s", esp_err_to_name(err));
      showError();
      continue;
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
    PRNM::Leds::Instance().Stop();
  }
}

}
