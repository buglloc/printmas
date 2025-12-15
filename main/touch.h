#pragma once

#include <esp_err.h>

namespace PRNM {

class Touch {
public:
  static Touch& Instance();

  esp_err_t Initialize();

  // Wait for first touch (blocking)
  bool Wait(int timeout_ms);

private:
  Touch() = default;
  ~Touch() = default;

  Touch(const Touch&) = delete;
  Touch& operator=(const Touch&) = delete;

private:
  bool initialized_ = false;
};

}

