#pragma once

#include <esp_err.h>
#include <cstdint>

namespace PRNM {

enum class LedAnimation : uint8_t {
  None = 0,
  // Regular animations (for random selection use 1..kNumRandomAnimations)
  Chase,          // One LED at a time, moving through sequence
  Twinkle,        // Random twinkling
  Wave,           // Alternating pattern wave
  // Special animations
  BlinkAll,          // Fast blink all - for error indication
  NumAnimations
};

// Number of regular animations available for random selection
constexpr uint8_t kNumRandomAnimations = 3;

class Leds {
public:
  static constexpr size_t kNumLeds = 6;

  static Leds& Instance();

  esp_err_t Initialize();

  // Start animation by enum
  esp_err_t StartAnimation(LedAnimation anim);

  // Start animation by id (0 = None, 1 = Chase, etc.)
  esp_err_t StartAnimation(uint8_t animId);

  // Stop current animation and turn off all LEDs
  esp_err_t Stop();

  // Check if animation is running
  bool IsRunning() const;

  // Get current animation
  LedAnimation CurrentAnimation() const;

  // Direct LED control (stops animation first)
  esp_err_t SetLed(size_t index, bool on);
  esp_err_t SetAllLeds(bool on);

private:
  Leds() = default;
  ~Leds() = default;

  Leds(const Leds&) = delete;
  Leds& operator=(const Leds&) = delete;

  void SetLedDirect(size_t index, bool on);
  void SetAllLedsDirect(bool on);

  static void AnimationTask(void* param);
  void RunAnimation();

  void AnimChase();
  void AnimTwinkle();
  void AnimWave();
  void AnimError();

private:
  bool initialized_ = false;
  volatile LedAnimation current_anim_ = LedAnimation::None;
  volatile bool running_ = false;
  void* task_handle_ = nullptr;
};

}  // namespace PRNM

