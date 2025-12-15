#include "leds.h"

#include <sdkconfig.h>

#include <esp_log.h>
#include <esp_check.h>
#include <esp_random.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/gpio.h>


using namespace PRNM;

namespace {
  const char* kLogTag = "prnm::leds";

  constexpr gpio_num_t kLedGpios[Leds::kNumLeds] = {
    static_cast<gpio_num_t>(CONFIG_PRNM_LED_1_GPIO),
    static_cast<gpio_num_t>(CONFIG_PRNM_LED_2_GPIO),
    static_cast<gpio_num_t>(CONFIG_PRNM_LED_3_GPIO),
    static_cast<gpio_num_t>(CONFIG_PRNM_LED_4_GPIO),
    static_cast<gpio_num_t>(CONFIG_PRNM_LED_5_GPIO),
    static_cast<gpio_num_t>(CONFIG_PRNM_LED_6_GPIO),
  };

  constexpr TickType_t kAnimationDelayMs = 100;
  constexpr size_t kAnimationStackSize = 2048;
  constexpr UBaseType_t kAnimationTaskPriority = 5;
}

Leds& Leds::Instance()
{
  static Leds instance;
  return instance;
}

esp_err_t Leds::Initialize()
{
  ESP_LOGI(kLogTag, "initializing %zu LEDs", kNumLeds);

  uint64_t pin_mask = 0;
  for (size_t i = 0; i < kNumLeds; ++i) {
    pin_mask |= (1ULL << kLedGpios[i]);
    ESP_LOGI(kLogTag, "  LED %zu: GPIO %d", i + 1, kLedGpios[i]);
  }

  gpio_config_t io_conf = {};
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = pin_mask;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;

  esp_err_t err = gpio_config(&io_conf);
  ESP_RETURN_ON_ERROR(err, kLogTag, "configure LED GPIOs");

  // Turn off all LEDs initially
  SetAllLedsDirect(false);

  initialized_ = true;
  ESP_LOGI(kLogTag, "LEDs initialized");
  return ESP_OK;
}

esp_err_t Leds::StartAnimation(LedAnimation anim)
{
  if (!initialized_) {
    ESP_LOGE(kLogTag, "LEDs not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (anim == LedAnimation::None || anim >= LedAnimation::NumAnimations) {
    return Stop();
  }

  // Stop current animation if running
  if (running_) {
    Stop();
  }

  ESP_LOGI(kLogTag, "starting animation %d", static_cast<int>(anim));
  current_anim_ = anim;
  running_ = true;

  BaseType_t ret = xTaskCreate(
    AnimationTask,
    "led_anim",
    kAnimationStackSize,
    this,
    kAnimationTaskPriority,
    reinterpret_cast<TaskHandle_t*>(&task_handle_)
  );

  if (ret != pdPASS) {
    ESP_LOGE(kLogTag, "failed to create animation task");
    running_ = false;
    current_anim_ = LedAnimation::None;
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

esp_err_t Leds::StartAnimation(uint8_t animId)
{
  if (animId >= static_cast<uint8_t>(LedAnimation::NumAnimations)) {
    ESP_LOGW(kLogTag, "invalid animation id %d", animId);
    return ESP_ERR_INVALID_ARG;
  }
  return StartAnimation(static_cast<LedAnimation>(animId));
}

esp_err_t Leds::Stop()
{
  if (!initialized_) {
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(kLogTag, "stopping animation");
  running_ = false;

  if (task_handle_) {
    // Give task time to exit gracefully
    vTaskDelay(pdMS_TO_TICKS(kAnimationDelayMs * 2));
    task_handle_ = nullptr;
  }

  current_anim_ = LedAnimation::None;
  SetAllLedsDirect(false);

  return ESP_OK;
}

bool Leds::IsRunning() const
{
  return running_;
}

LedAnimation Leds::CurrentAnimation() const
{
  return current_anim_;
}

esp_err_t Leds::SetLed(size_t index, bool on)
{
  if (!initialized_) {
    return ESP_ERR_INVALID_STATE;
  }
  if (index >= kNumLeds) {
    return ESP_ERR_INVALID_ARG;
  }

  if (running_) {
    Stop();
  }

  SetLedDirect(index, on);
  return ESP_OK;
}

esp_err_t Leds::SetAllLeds(bool on)
{
  if (!initialized_) {
    return ESP_ERR_INVALID_STATE;
  }

  if (running_) {
    Stop();
  }

  SetAllLedsDirect(on);
  return ESP_OK;
}

void Leds::SetLedDirect(size_t index, bool on)
{
  if (index < kNumLeds) {
    gpio_set_level(kLedGpios[index], on ? 1 : 0);
  }
}

void Leds::SetAllLedsDirect(bool on)
{
  for (size_t i = 0; i < kNumLeds; ++i) {
    gpio_set_level(kLedGpios[i], on ? 1 : 0);
  }
}

void Leds::AnimationTask(void* param)
{
  Leds* self = static_cast<Leds*>(param);
  self->RunAnimation();
  self->task_handle_ = nullptr;
  vTaskDelete(nullptr);
}

void Leds::RunAnimation()
{
  while (running_) {
    switch (current_anim_) {
      case LedAnimation::Chase:
        AnimChase();
        break;
      case LedAnimation::Twinkle:
        AnimTwinkle();
        break;
      case LedAnimation::Wave:
        AnimWave();
        break;
      case LedAnimation::BlinkAll:
        AnimError();
        break;
      default:
        running_ = false;
        break;
    }
  }
}

void Leds::AnimChase()
{
  // One LED lights up at a time, moving through the sequence
  for (size_t i = 0; i < kNumLeds && running_; ++i) {
    SetAllLedsDirect(false);
    SetLedDirect(i, true);
    vTaskDelay(pdMS_TO_TICKS(150));
  }

  // Then back
  for (size_t i = kNumLeds - 1; i > 0 && running_; --i) {
    SetAllLedsDirect(false);
    SetLedDirect(i - 1, true);
    vTaskDelay(pdMS_TO_TICKS(150));
  }
}

void Leds::AnimTwinkle()
{
  // Random LEDs turn on/off creating a twinkling effect
  for (int cycle = 0; cycle < 10 && running_; ++cycle) {
    size_t led = esp_random() % kNumLeds;
    bool state = (esp_random() % 2) == 0;
    SetLedDirect(led, state);
    vTaskDelay(pdMS_TO_TICKS(80));
  }
}

void Leds::AnimWave()
{
  // Alternating pattern - even LEDs on, odd off, then swap
  for (size_t i = 0; i < kNumLeds; ++i) {
    SetLedDirect(i, (i % 2) == 0);
  }
  vTaskDelay(pdMS_TO_TICKS(300));
  if (!running_) return;

  for (size_t i = 0; i < kNumLeds; ++i) {
    SetLedDirect(i, (i % 2) == 1);
  }
  vTaskDelay(pdMS_TO_TICKS(300));
}

void Leds::AnimError()
{
  // Fast blinking all LEDs - error indication
  SetAllLedsDirect(true);
  vTaskDelay(pdMS_TO_TICKS(150));
  if (!running_) return;

  SetAllLedsDirect(false);
  vTaskDelay(pdMS_TO_TICKS(150));
}

