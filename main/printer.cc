#include "printer.h"

#include <cstring>

#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "signs.h"

using namespace PRNM;

namespace {
  static constexpr const char* kLogTag = "prnm::printer";

  // Packet markers
  static constexpr uint8_t kPacketStart1 = 0x55;
  static constexpr uint8_t kPacketStart2 = 0x55;
  static constexpr uint8_t kPacketEnd1 = 0xAA;
  static constexpr uint8_t kPacketEnd2 = 0xAA;
}

NiimbotPrinter::NiimbotPrinter()
{
  write_semaphore_ = xSemaphoreCreateBinary();
}

NiimbotPrinter::~NiimbotPrinter()
{
  if (write_semaphore_) {
    vSemaphoreDelete(write_semaphore_);
  }
}

void NiimbotPrinter::SetSendCallback(SendPacketCallback callback)
{
  send_callback_ = std::move(callback);
}

void NiimbotPrinter::SetReadyCallback(ReadyCallback callback)
{
  ready_callback_ = std::move(callback);
}

void NiimbotPrinter::Reset()
{
  ready_ = false;
  packet_buf_len_ = 0;
  status_ = {};
}

void NiimbotPrinter::OnWriteComplete()
{
  if (write_semaphore_) {
    xSemaphoreGive(write_semaphore_);
  }
}

size_t NiimbotPrinter::BuildPacket(uint8_t* buf, size_t buf_size, uint8_t type, const uint8_t* data, size_t data_len)
{
  if (buf_size < data_len + 7) {
    ESP_LOGE(kLogTag, "Buffer too small for packet");
    return 0;
  }

  buf[0] = kPacketStart1;
  buf[1] = kPacketStart2;
  buf[2] = type;
  buf[3] = static_cast<uint8_t>(data_len);

  // Calculate checksum: XOR of type, length, and all data bytes
  uint8_t checksum = type ^ static_cast<uint8_t>(data_len);
  for (size_t i = 0; i < data_len; i++) {
    buf[4 + i] = data[i];
    checksum ^= data[i];
  }

  buf[4 + data_len] = checksum;
  buf[5 + data_len] = kPacketEnd1;
  buf[6 + data_len] = kPacketEnd2;

  return data_len + 7;
}

bool NiimbotPrinter::ParsePacket(const uint8_t* buf, size_t len, uint8_t* type,
                                 uint8_t* data, size_t* data_len)
{
  if (len < 7) {
    return false;
  }

  // Check start markers
  if (buf[0] != kPacketStart1 || buf[1] != kPacketStart2) {
    return false;
  }

  *type = buf[2];
  size_t pkt_data_len = buf[3];

  if (len < pkt_data_len + 7) {
    return false;  // Incomplete packet
  }

  // Check end markers
  if (buf[5 + pkt_data_len] != kPacketEnd1 || buf[6 + pkt_data_len] != kPacketEnd2) {
    ESP_LOGW(kLogTag, "Invalid packet end markers");
    return false;
  }

  // Verify checksum
  uint8_t checksum = *type ^ static_cast<uint8_t>(pkt_data_len);
  for (size_t i = 0; i < pkt_data_len; i++) {
    data[i] = buf[4 + i];
    checksum ^= buf[4 + i];
  }

  if (checksum != buf[4 + pkt_data_len]) {
    ESP_LOGW(kLogTag, "Packet checksum mismatch: expected 0x%02x, got 0x%02x",
             checksum, buf[4 + pkt_data_len]);
    return false;
  }

  *data_len = pkt_data_len;
  return true;
}

esp_err_t NiimbotPrinter::SendPacket(RequestCode code, const uint8_t* data, size_t data_len,
                                     bool wait_for_response)
{
  if (!send_callback_) {
    ESP_LOGE(kLogTag, "Send callback not set");
    return ESP_ERR_INVALID_STATE;
  }

  uint8_t type = static_cast<uint8_t>(code);
  uint8_t pkt[256];
  size_t pkt_len = BuildPacket(pkt, sizeof(pkt), type, data, data_len);
  if (pkt_len == 0) {
    return ESP_ERR_INVALID_SIZE;
  }

  ESP_LOGD(kLogTag, "Sending packet type=0x%02x len=%zu", type, data_len);
  ESP_LOG_BUFFER_HEX_LEVEL(kLogTag, pkt, pkt_len, ESP_LOG_DEBUG);

  // Clear any pending signals
  if (wait_for_response && write_semaphore_) {
    xSemaphoreTake(write_semaphore_, 0);
  }

  send_callback_(pkt, pkt_len, wait_for_response);

  // Wait for write completion
  if (wait_for_response && write_semaphore_) {
    if (xSemaphoreTake(write_semaphore_, kWriteTimeout) != pdTRUE) {
      ESP_LOGW(kLogTag, "Write timeout waiting for response");
      return ESP_ERR_TIMEOUT;
    }
  }

  return ESP_OK;
}

void NiimbotPrinter::HandleResponse(uint8_t type, const uint8_t* data, size_t data_len)
{
  ESP_LOGI(kLogTag, "Response type=0x%02x len=%zu", type, data_len);

  // Error response (0xDB = 219)
  if (type == 0xDB) {
    ESP_LOGE(kLogTag, "Printer error: 0x%02x", data_len > 0 ? data[0] : 0xFF);
    return;
  }

  // Heartbeat response (0xDC + 1 = 0xDD)
  if (type == 0xDD && data_len >= 9) {
    switch (data_len) {
      case 20:
        status_.paper_state = data[18];
        status_.rfid_read_state = data[19];
        break;
      case 13:
        status_.closing_state = data[9];
        status_.power_level = data[10];
        status_.paper_state = data[11];
        status_.rfid_read_state = data[12];
        break;
      case 19:
        status_.closing_state = data[15];
        status_.power_level = data[16];
        status_.paper_state = data[17];
        status_.rfid_read_state = data[18];
        break;
      case 10:
        status_.closing_state = data[8];
        status_.power_level = data[9];
        break;
      case 9:
        status_.closing_state = data[8];
        break;
    }
    ESP_LOGI(kLogTag, "Heartbeat: closing=%d power=%d paper=%d rfid=%d",
             status_.closing_state, status_.power_level,
             status_.paper_state, status_.rfid_read_state);

    if (!ready_) {
      ready_ = true;
      ESP_LOGI(kLogTag, "Printer ready!");
      if (ready_callback_) {
        ready_callback_();
      }
    }
    return;
  }

  // Info responses (key + 0x40)
  if (type == static_cast<uint8_t>(InfoKey::BATTERY) + 0x40) {
    if (data_len > 0) {
      ESP_LOGI(kLogTag, "Battery: %d%%", data[0]);
    }
    return;
  }

  if (type == static_cast<uint8_t>(InfoKey::DEVICETYPE) + 0x40) {
    if (data_len >= 2) {
      uint16_t device_type = (data[0] << 8) | data[1];
      ESP_LOGI(kLogTag, "Device type: %d (B1 = 4096)", device_type);
    }
    return;
  }

  // Print status response (0xA3 + 16 = 0xB3)
  if (type == 0xB3 && data_len >= 4) {
    uint16_t page = (data[0] << 8) | data[1];
    uint8_t progress1 = data[2];
    uint8_t progress2 = data[3];
    ESP_LOGI(kLogTag, "Print status: page=%d progress=%d/%d", page, progress1, progress2);
    return;
  }

  // Config responses (SET_LABEL_TYPE, SET_LABEL_DENSITY)
  if ((type == 0x31 || type == 0x33) && data_len > 0) {
    ESP_LOGI(kLogTag, "Config response (0x%02x): success=%d", type, data[0]);
    return;
  }

  // START_PRINT response (0x01 + 1 = 0x02)
  if (type == 0x02 && data_len > 0) {
    ESP_LOGI(kLogTag, "Start print: success=%d", data[0]);
    return;
  }

  // START_PAGE_PRINT response (0x03 + 1 = 0x04)
  if (type == 0x04 && data_len > 0) {
    ESP_LOGI(kLogTag, "Start page: success=%d", data[0]);
    return;
  }

  // SET_DIMENSION response (0x13 + 1 = 0x14)
  if (type == 0x14 && data_len > 0) {
    ESP_LOGI(kLogTag, "Set dimension: success=%d", data[0]);
    return;
  }

  // END_PAGE_PRINT response (0xE3 + 1 = 0xE4)
  if (type == 0xE4 && data_len > 0) {
    ESP_LOGI(kLogTag, "End page: success=%d", data[0]);
    return;
  }

  // END_PRINT response (0xF3 + 1 = 0xF4)
  if (type == 0xF4 && data_len > 0) {
    ESP_LOGI(kLogTag, "End print: success=%d", data[0]);
    return;
  }

  ESP_LOGI(kLogTag, "Unknown response type 0x%02x", type);
  ESP_LOG_BUFFER_HEX_LEVEL(kLogTag, data, data_len, ESP_LOG_INFO);
}

void NiimbotPrinter::ProcessReceivedData(const uint8_t* data, size_t len)
{
  // Append to buffer
  if (packet_buf_len_ + len > sizeof(packet_buf_)) {
    ESP_LOGW(kLogTag, "Packet buffer overflow, resetting");
    packet_buf_len_ = 0;
  }
  memcpy(packet_buf_ + packet_buf_len_, data, len);
  packet_buf_len_ += len;

  // Try to parse packets
  while (packet_buf_len_ >= 7) {
    uint8_t type;
    uint8_t pkt_data[256];
    size_t pkt_data_len;

    // Find start markers
    size_t start_idx = 0;
    while (start_idx < packet_buf_len_ - 1) {
      if (packet_buf_[start_idx] == kPacketStart1 &&
          packet_buf_[start_idx + 1] == kPacketStart2) {
        break;
      }
      start_idx++;
    }

    if (start_idx > 0) {
      // Remove garbage before start markers
      memmove(packet_buf_, packet_buf_ + start_idx, packet_buf_len_ - start_idx);
      packet_buf_len_ -= start_idx;
    }

    if (packet_buf_len_ < 7) {
      break;  // Not enough data
    }

    if (ParsePacket(packet_buf_, packet_buf_len_, &type, pkt_data, &pkt_data_len)) {
      HandleResponse(type, pkt_data, pkt_data_len);

      // Remove processed packet from buffer
      size_t pkt_total_len = pkt_data_len + 7;
      memmove(packet_buf_, packet_buf_ + pkt_total_len, packet_buf_len_ - pkt_total_len);
      packet_buf_len_ -= pkt_total_len;
    } else {
      // Check if we have the length byte and know the expected packet size
      if (packet_buf_len_ >= 4) {
        size_t expected_len = packet_buf_[3] + 7;
        if (packet_buf_len_ < expected_len) {
          break;  // Wait for more data
        }
        // Packet complete but invalid, skip first byte and try again
        memmove(packet_buf_, packet_buf_ + 1, packet_buf_len_ - 1);
        packet_buf_len_--;
      } else {
        break;  // Wait for more data
      }
    }
  }
}

// Commands

esp_err_t NiimbotPrinter::SendHeartbeat()
{
  uint8_t data[] = {0x01};
  ESP_LOGI(kLogTag, "Sending heartbeat...");
  return SendPacket(RequestCode::HEARTBEAT, data, sizeof(data));
}

esp_err_t NiimbotPrinter::GetDeviceInfo(InfoKey key)
{
  uint8_t data[] = {static_cast<uint8_t>(key)};
  ESP_LOGI(kLogTag, "Requesting info key=%d", static_cast<int>(key));
  return SendPacket(RequestCode::GET_INFO, data, sizeof(data));
}

esp_err_t NiimbotPrinter::SetLabelDensity(uint8_t density)
{
  uint8_t data[] = {density};
  ESP_LOGI(kLogTag, "Setting label density to %d", density);
  return SendPacket(RequestCode::SET_LABEL_DENSITY, data, sizeof(data));
}

esp_err_t NiimbotPrinter::SetLabelType(uint8_t type)
{
  uint8_t data[] = {type};
  ESP_LOGI(kLogTag, "Setting label type to %d", type);
  return SendPacket(RequestCode::SET_LABEL_TYPE, data, sizeof(data));
}

esp_err_t NiimbotPrinter::StartPrint(uint16_t total_pages, uint8_t page_color)
{
  uint8_t data[] = {
      static_cast<uint8_t>(total_pages >> 8),
      static_cast<uint8_t>(total_pages & 0xFF),
      0x00, 0x00, 0x00, 0x00,  // Reserved zeros
      page_color};
  ESP_LOGI(kLogTag, "Starting print (pages=%d, color=%d)", total_pages, page_color);
  return SendPacket(RequestCode::START_PRINT, data, sizeof(data));
}

esp_err_t NiimbotPrinter::StartPagePrint()
{
  uint8_t data[] = {0x01};
  ESP_LOGI(kLogTag, "Starting page...");
  return SendPacket(RequestCode::START_PAGE_PRINT, data, sizeof(data));
}

esp_err_t NiimbotPrinter::SetPageSize(uint16_t rows, uint16_t cols, uint16_t copies)
{
  uint8_t data[] = {
      static_cast<uint8_t>(rows >> 8),
      static_cast<uint8_t>(rows & 0xFF),
      static_cast<uint8_t>(cols >> 8),
      static_cast<uint8_t>(cols & 0xFF),
      static_cast<uint8_t>(copies >> 8),
      static_cast<uint8_t>(copies & 0xFF),
  };
  ESP_LOGI(kLogTag, "Setting page size: %dx%d, copies=%d", rows, cols, copies);
  return SendPacket(RequestCode::SET_DIMENSION, data, sizeof(data));
}

esp_err_t NiimbotPrinter::SendBitmapRow(uint16_t row_num, const uint8_t* row_data, size_t row_len)
{
  uint8_t pkt_data[256];

  // Row number (big endian)
  pkt_data[0] = static_cast<uint8_t>(row_num >> 8);
  pkt_data[1] = static_cast<uint8_t>(row_num & 0xFF);

  // Bit counts (can be 0,0,0)
  pkt_data[2] = 0;
  pkt_data[3] = 0;
  pkt_data[4] = 0;

  // Repeat count
  pkt_data[5] = 1;

  // Copy row data
  memcpy(pkt_data + 6, row_data, row_len);

  return SendPacket(RequestCode::PRINT_BITMAP_ROW, pkt_data, 6 + row_len);
}

esp_err_t NiimbotPrinter::SendEmptyRow(uint16_t row_num, uint8_t count)
{
  uint8_t data[] = {
      static_cast<uint8_t>(row_num >> 8),
      static_cast<uint8_t>(row_num & 0xFF),
      count};
  return SendPacket(RequestCode::PRINT_EMPTY_ROW, data, sizeof(data));
}

esp_err_t NiimbotPrinter::EndPagePrint()
{
  uint8_t data[] = {0x01};
  ESP_LOGI(kLogTag, "Ending page...");
  return SendPacket(RequestCode::END_PAGE_PRINT, data, sizeof(data));
}

esp_err_t NiimbotPrinter::EndPrint()
{
  uint8_t data[] = {0x01};
  ESP_LOGI(kLogTag, "Ending print...");
  return SendPacket(RequestCode::END_PRINT, data, sizeof(data));
}

esp_err_t NiimbotPrinter::GetPrintStatus()
{
  uint8_t data[] = {0x01};
  return SendPacket(RequestCode::GET_PRINT_STATUS, data, sizeof(data));
}

esp_err_t NiimbotPrinter::Print(const Signs::RleImage& image)
{
  if (!ready_) {
    ESP_LOGW(kLogTag, "Printer not ready");
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(kLogTag, "Starting print...");
  ESP_LOGI(kLogTag, "   Image: %dx%d dots", image.w, image.h);

  // Row data buffer: 384 pixels = 48 bytes
  constexpr size_t kRowBytes = kPaperWidthDots / 8;
  uint8_t row_data[kRowBytes];

  // Use image dimensions (capped to paper size)
  uint16_t print_height = image.h < kPaperHeightDots ? image.h : kPaperHeightDots;

  // Step 1: Set density (3 = medium)
  ESP_RETURN_ON_ERROR(SetLabelDensity(3), kLogTag, "failed to set label density");
  vTaskDelay(pdMS_TO_TICKS(10));

  // Step 2: Set label type (1 = with gaps)
  ESP_RETURN_ON_ERROR(SetLabelType(1), kLogTag, "failed to set label type");
  vTaskDelay(pdMS_TO_TICKS(10));

  // Step 3: Start print
  ESP_RETURN_ON_ERROR(StartPrint(1, 0), kLogTag, "failed to start print");
  vTaskDelay(pdMS_TO_TICKS(10));

  // Step 4: Start page
  ESP_RETURN_ON_ERROR(StartPagePrint(), kLogTag, "failed to start page print");
  vTaskDelay(pdMS_TO_TICKS(10));

  // Step 5: Set page size (height, width)
  ESP_RETURN_ON_ERROR(SetPageSize(print_height, kPaperWidthDots, 1), kLogTag, "failed to set page size");
  vTaskDelay(pdMS_TO_TICKS(10));

  // Step 6: Send image data
  ESP_LOGI(kLogTag, "Sending %d rows of image data...", print_height);

  for (uint16_t y = 0; y < print_height; y++) {
    // Decode the RLE row into 1bpp format
    Signs::decode_rle_row_1bpp(image, y, row_data, kRowBytes);
    ESP_RETURN_ON_ERROR(SendBitmapRow(y, row_data, kRowBytes), kLogTag, "failed to send bitmap row");

    // Progress logging every 60 rows
    if ((y % 60) == 59) {
      ESP_LOGI(kLogTag, "   Progress: %d/%d rows", y + 1, print_height);
    }
  }

  ESP_LOGI(kLogTag, "Image data sent!");

  // Step 7: End page
  vTaskDelay(pdMS_TO_TICKS(100));
  ESP_RETURN_ON_ERROR(EndPagePrint(), kLogTag, "failed to end page print");

  // Step 8: Wait for printer to finish and end print
  vTaskDelay(pdMS_TO_TICKS(2000));
  ESP_RETURN_ON_ERROR(EndPrint(), kLogTag, "failed to end print");

  ESP_LOGI(kLogTag, "Print complete!");
  return ESP_OK;
}
