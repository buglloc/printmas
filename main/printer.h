#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>

#include <esp_err.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "signs.h"

namespace PRNM {

// Niimbot printer protocol and commands
class NiimbotPrinter {
public:
  // Niimbot B1 specifications
  static constexpr uint16_t kPaperWidthDots = 384;   // Limited by printhead
  static constexpr uint16_t kPaperHeightDots = 240;  // 30mm @ 203 DPI
  static constexpr uint16_t kPrinterDpi = 203;

  // Niimbot request codes
  enum class RequestCode : uint8_t {
    GET_INFO = 0x40,
    GET_RFID = 0x1A,
    HEARTBEAT = 0xDC,
    SET_LABEL_TYPE = 0x23,
    SET_LABEL_DENSITY = 0x21,
    START_PRINT = 0x01,
    END_PRINT = 0xF3,
    START_PAGE_PRINT = 0x03,
    END_PAGE_PRINT = 0xE3,
    ALLOW_PRINT_CLEAR = 0x20,
    SET_DIMENSION = 0x13,
    SET_QUANTITY = 0x15,
    GET_PRINT_STATUS = 0xA3,
    PRINT_BITMAP_ROW_INDEXED = 0x83,
    PRINT_EMPTY_ROW = 0x84,
    PRINT_BITMAP_ROW = 0x85,
  };

  // Info request keys
  enum class InfoKey : uint8_t {
    DENSITY = 1,
    PRINTSPEED = 2,
    LABELTYPE = 3,
    LANGUAGETYPE = 6,
    AUTOSHUTDOWNTIME = 7,
    DEVICETYPE = 8,
    SOFTVERSION = 9,
    BATTERY = 10,
    DEVICESERIAL = 11,
    HARDVERSION = 12,
  };

  // Printer status from heartbeat
  struct Status {
    uint8_t closing_state = 0;
    uint8_t power_level = 0;
    uint8_t paper_state = 0;
    uint8_t rfid_read_state = 0;
  };

  // Callback type for sending packets over BLE
  using SendPacketCallback = std::function<void(const uint8_t* data, size_t len, bool wait_for_response)>;
  // Callback when printer becomes ready
  using ReadyCallback = std::function<void()>;

  NiimbotPrinter();
  ~NiimbotPrinter();

  // Set the callback for sending packets
  void SetSendCallback(SendPacketCallback callback);
  // Set callback for when printer is ready
  void SetReadyCallback(ReadyCallback callback);

  // Process received data from BLE
  void ProcessReceivedData(const uint8_t* data, size_t len);

  // Signal that a write operation completed
  void OnWriteComplete();

  // Check if printer is ready
  bool IsReady() const { return ready_; }

  // Get printer status
  const Status& GetStatus() const { return status_; }

  // Commands
  esp_err_t SendHeartbeat();
  esp_err_t GetDeviceInfo(InfoKey key);
  esp_err_t SetLabelDensity(uint8_t density);
  esp_err_t SetLabelType(uint8_t type);
  esp_err_t StartPrint(uint16_t total_pages = 1, uint8_t page_color = 0);
  esp_err_t StartPagePrint();
  esp_err_t SetPageSize(uint16_t rows, uint16_t cols, uint16_t copies = 1);
  esp_err_t SendBitmapRow(uint16_t row_num, const uint8_t* row_data, size_t row_len);
  esp_err_t SendEmptyRow(uint16_t row_num, uint8_t count);
  esp_err_t EndPagePrint();
  esp_err_t EndPrint();
  esp_err_t GetPrintStatus();

  // Print an RLE-encoded image
  esp_err_t Print(const Signs::RleImage& image);

  // Reset state (e.g., on disconnect)
  void Reset();

  // Packet building utilities
  static size_t BuildPacket(uint8_t* buf, size_t buf_size, uint8_t type,
                           const uint8_t* data, size_t data_len);
  static bool ParsePacket(const uint8_t* buf, size_t len, uint8_t* type,
                         uint8_t* data, size_t* data_len);

private:
  // Non-copyable
  NiimbotPrinter(const NiimbotPrinter&) = delete;
  NiimbotPrinter& operator=(const NiimbotPrinter&) = delete;

  // Timeout for write operations
  static constexpr TickType_t kWriteTimeout = pdMS_TO_TICKS(1000);

  esp_err_t SendPacket(RequestCode code, const uint8_t* data, size_t data_len, bool wait_for_response = true);
  void HandleResponse(uint8_t type, const uint8_t* data, size_t data_len);

  SendPacketCallback send_callback_;
  ReadyCallback ready_callback_;
  SemaphoreHandle_t write_semaphore_;

  // Packet receive buffer
  uint8_t packet_buf_[512];
  size_t packet_buf_len_ = 0;

  Status status_;
  bool ready_ = false;
};

}
