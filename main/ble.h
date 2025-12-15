#pragma once

#include <cstdint>
#include <functional>

#include <esp_err.h>
#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>

namespace PRNM {

class BLEClient {
public:
  // Callbacks
  using DataReceivedCallback = std::function<void(const uint8_t* data, size_t len)>;
  using WriteCompleteCallback = std::function<void()>;
  using ConnectedCallback = std::function<void()>;

  static BLEClient& Instance();
  esp_err_t Initialize();

  // Set callbacks
  void SetDataReceivedCallback(DataReceivedCallback callback);
  void SetWriteCompleteCallback(WriteCompleteCallback callback);
  void SetConnectedCallback(ConnectedCallback callback);

  // Send data to the printer
  void SendData(const uint8_t* data, size_t len, bool wait_for_response);

  // Check connection status
  bool IsConnected() const { return connected_; }

  static void GapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
  static void GattcCallback(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                            esp_ble_gattc_cb_param_t* param);
  static void GattcProfileHandler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                  esp_ble_gattc_cb_param_t* param);

private:
  static constexpr int kProfileNum = 1;
  static constexpr int kProfileAppId = 0;
  static constexpr uint16_t kInvalidHandle = 0;

  struct GattcProfile {
    esp_gattc_cb_t callback;
    uint16_t gattc_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_start_handle;
    uint16_t service_end_handle;
    uint16_t char_handle;
    esp_bd_addr_t remote_bda;
  };

  BLEClient() = default;
  ~BLEClient() = default;

  // Non-copyable
  BLEClient(const BLEClient&) = delete;
  BLEClient& operator=(const BLEClient&) = delete;

  void HandleGapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
  void HandleGattcEvent(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                        esp_ble_gattc_cb_param_t* param);

  static const char* KeyTypeToStr(esp_ble_key_type_t key_type);
  static const char* AuthReqToStr(esp_ble_auth_req_t auth_req);

  DataReceivedCallback data_received_callback_;
  WriteCompleteCallback write_complete_callback_;
  ConnectedCallback connected_callback_;

  GattcProfile profiles_[kProfileNum] = {};
  esp_bd_addr_t target_bda_ = {};
  esp_bt_uuid_t service_uuid_ = {};

  bool connected_ = false;
  bool has_service_ = false;
  esp_gatt_if_t gattc_if_ = ESP_GATT_IF_NONE;
};

}
