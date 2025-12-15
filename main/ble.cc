#include "ble.h"

#include <sdkconfig.h>
#include <cstring>

#include <esp_check.h>
#include <esp_log.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gatt_defs.h>
#include <esp_gatt_common_api.h>

using namespace PRNM;

namespace {
  // Niimbot BLE UUIDs (128-bit, little-endian)
  // Service: e7810a71-73ae-499d-8c15-faa9aef0c3f2
  // Characteristic: bef8d6c9-9c21-4c9e-b632-bd58c1009f9f
  static constexpr uint8_t kServiceUuid[16] = {
    0xf2, 0xc3, 0xf0, 0xae, 0xa9, 0xfa, 0x15, 0x8c,
    0x9d, 0x49, 0xae, 0x73, 0x71, 0x0a, 0x81, 0xe7};

  static constexpr uint8_t kCharacteristicUuid[16] = {
    0x9f, 0x9f, 0x00, 0xc1, 0x58, 0xbd, 0x32, 0xb6,
    0x9e, 0x4c, 0x21, 0x9c, 0xc9, 0xd6, 0xf8, 0xbe};

  static constexpr const char* kLogTag = "prnm::ble";
}

namespace {

  // Security parameters
  esp_ble_auth_req_t gAuthReq = ESP_LE_AUTH_REQ_SC_MITM_BOND;
  esp_ble_io_cap_t gIoCap = ESP_IO_CAP_NONE;
  uint8_t gKeySize = 16;
  uint8_t gInitKey = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t gRspKey = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t gOobSupport = ESP_BLE_OOB_DISABLE;

  esp_ble_scan_params_t gScanParams = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_RPA_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
  };

}

BLEClient& BLEClient::Instance()
{
  static BLEClient instance;
  return instance;
}

const char* BLEClient::KeyTypeToStr(esp_ble_key_type_t key_type)
{
  switch (key_type) {
    case ESP_LE_KEY_NONE: return "ESP_LE_KEY_NONE";
    case ESP_LE_KEY_PENC: return "ESP_LE_KEY_PENC";
    case ESP_LE_KEY_PID: return "ESP_LE_KEY_PID";
    case ESP_LE_KEY_PCSRK: return "ESP_LE_KEY_PCSRK";
    case ESP_LE_KEY_PLK: return "ESP_LE_KEY_PLK";
    case ESP_LE_KEY_LLK: return "ESP_LE_KEY_LLK";
    case ESP_LE_KEY_LENC: return "ESP_LE_KEY_LENC";
    case ESP_LE_KEY_LID: return "ESP_LE_KEY_LID";
    case ESP_LE_KEY_LCSRK: return "ESP_LE_KEY_LCSRK";
    default: return "INVALID";
  }
}

const char* BLEClient::AuthReqToStr(esp_ble_auth_req_t auth_req)
{
  switch (auth_req) {
    case ESP_LE_AUTH_NO_BOND: return "ESP_LE_AUTH_NO_BOND";
    case ESP_LE_AUTH_BOND: return "ESP_LE_AUTH_BOND";
    case ESP_LE_AUTH_REQ_MITM: return "ESP_LE_AUTH_REQ_MITM";
    case ESP_LE_AUTH_REQ_BOND_MITM: return "ESP_LE_AUTH_REQ_BOND_MITM";
    case ESP_LE_AUTH_REQ_SC_ONLY: return "ESP_LE_AUTH_REQ_SC_ONLY";
    case ESP_LE_AUTH_REQ_SC_BOND: return "ESP_LE_AUTH_REQ_SC_BOND";
    case ESP_LE_AUTH_REQ_SC_MITM: return "ESP_LE_AUTH_REQ_SC_MITM";
    case ESP_LE_AUTH_REQ_SC_MITM_BOND: return "ESP_LE_AUTH_REQ_SC_MITM_BOND";
    default: return "INVALID";
  }
}

esp_err_t BLEClient::Initialize()
{
  esp_err_t err = ESP_OK;

  // Parse target BDA from config
  ESP_LOGI(kLogTag, "Parsing target BDA");
  {
    const char* str = CONFIG_PRNM_PRINTER_BDA;
    int parsed = sscanf(
      str,
      "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
      &target_bda_[0], &target_bda_[1], &target_bda_[2],
      &target_bda_[3], &target_bda_[4], &target_bda_[5]
    );
    if (parsed != 6) {
      ESP_LOGE(kLogTag, "Invalid BLE address format: %s", str);
      return ESP_ERR_INVALID_ARG;
    }

    // Initialize service UUID
    service_uuid_.len = ESP_UUID_LEN_128;
    memcpy(service_uuid_.uuid.uuid128, kServiceUuid, 16);
  }

  // Initialize profiles
  profiles_[kProfileAppId] = {
    .callback = GattcProfileHandler,
    .gattc_if = ESP_GATT_IF_NONE,
    .app_id = kProfileAppId,
    .conn_id = 0,
    .service_start_handle = 0,
    .service_end_handle = 0,
    .char_handle = 0,
    .remote_bda = {},
  };

  ESP_LOGI(kLogTag, "Initializing BT controller");
  {
    err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    ESP_RETURN_ON_ERROR(err, kLogTag, "release classic BT memory");

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    ESP_RETURN_ON_ERROR(err, kLogTag, "init BT controller");
  }

  ESP_LOGI(kLogTag, "Enabling BT controller");
  {
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    ESP_RETURN_ON_ERROR(err, kLogTag, "enable BT controller");

    err = esp_bluedroid_init();
    ESP_RETURN_ON_ERROR(err, kLogTag, "init bluedroid");

    err = esp_bluedroid_enable();
    ESP_RETURN_ON_ERROR(err, kLogTag, "enable bluedroid");
  }

  ESP_LOGI(kLogTag, "Setting up GAP");
  {
    err = esp_ble_gap_register_callback(GapCallback);
    ESP_RETURN_ON_ERROR(err, kLogTag, "register GAP callback");

    err = esp_ble_gatt_set_local_mtu(CONFIG_PRNM_BT_MTU);
    ESP_RETURN_ON_ERROR(err, kLogTag, "set MTU");

    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &gAuthReq, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &gIoCap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &gKeySize, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_OOB_SUPPORT, &gOobSupport, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &gInitKey, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &gRspKey, sizeof(uint8_t));
  }

  ESP_LOGI(kLogTag, "Setting up GATTC");
  {
    err = esp_ble_gattc_register_callback(GattcCallback);
    ESP_RETURN_ON_ERROR(err, kLogTag, "register GATTC callback");

    err = esp_ble_gattc_app_register(kProfileAppId);
    ESP_RETURN_ON_ERROR(err, kLogTag, "register GATTC app");
  }

  return ESP_OK;
}

void BLEClient::SetDataReceivedCallback(DataReceivedCallback callback)
{
  data_received_callback_ = std::move(callback);
}

void BLEClient::SetWriteCompleteCallback(WriteCompleteCallback callback)
{
  write_complete_callback_ = std::move(callback);
}

void BLEClient::SetConnectedCallback(ConnectedCallback callback)
{
  connected_callback_ = std::move(callback);
}

void BLEClient::SendData(const uint8_t* data, size_t len, bool wait_for_response)
{
  auto& profile = profiles_[kProfileAppId];
  if (profile.char_handle == kInvalidHandle) {
    ESP_LOGE(kLogTag, "Characteristic not available");
    return;
  }

  esp_err_t err = esp_ble_gattc_write_char(
    gattc_if_, profile.conn_id, profile.char_handle, len,
    const_cast<uint8_t*>(data),
    wait_for_response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP,
    ESP_GATT_AUTH_REQ_NONE);

  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Write failed: %s", esp_err_to_name(err));
  }
}

// Static callbacks that delegate to instance methods

void BLEClient::GapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param)
{
  Instance().HandleGapEvent(event, param);
}

void BLEClient::GattcCallback(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                              esp_ble_gattc_cb_param_t* param)
{
  auto& instance = Instance();

  ESP_LOGD(kLogTag, "GATTC event %d, if %d", event, gattc_if);

  if (event == ESP_GATTC_REG_EVT) {
    if (param->reg.status != ESP_GATT_OK) {
      ESP_LOGW(kLogTag, "App registration failed, status %d", param->reg.status);
      return;
    }
    instance.profiles_[param->reg.app_id].gattc_if = gattc_if;
  }

  for (int i = 0; i < kProfileNum; i++) {
    if (gattc_if == ESP_GATT_IF_NONE || gattc_if == instance.profiles_[i].gattc_if) {
      if (instance.profiles_[i].callback) {
        instance.profiles_[i].callback(event, gattc_if, param);
      }
    }
  }
}

void BLEClient::GattcProfileHandler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                    esp_ble_gattc_cb_param_t* param)
{
  Instance().HandleGattcEvent(event, gattc_if, param);
}

void BLEClient::HandleGapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param)
{
  switch (event) {
  case ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT: {
    if (param->local_privacy_cmpl.status != ESP_BT_STATUS_SUCCESS) {
      ESP_LOGE(kLogTag, "Privacy config failed, status %x", param->local_privacy_cmpl.status);
      break;
    }
    ESP_LOGI(kLogTag, "Privacy config successful");
    esp_ble_gap_set_scan_params(&gScanParams);
    break;
  }

  case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
    esp_ble_gap_start_scanning(30);
    break;
  }

  case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT: {
    if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
      ESP_LOGE(kLogTag, "Scan start failed, status %x", param->scan_start_cmpl.status);
      break;
    }
    ESP_LOGI(kLogTag, "Scanning started");
    break;
  }

  case ESP_GAP_BLE_PASSKEY_REQ_EVT: {
    ESP_LOGI(kLogTag, "Passkey request");
    break;
  }

  case ESP_GAP_BLE_OOB_REQ_EVT: {
    ESP_LOGI(kLogTag, "OOB request");
    uint8_t tk[16] = {1};
    esp_ble_oob_req_reply(param->ble_security.ble_req.bd_addr, tk, sizeof(tk));
    break;
  }

  case ESP_GAP_BLE_LOCAL_IR_EVT:
  case ESP_GAP_BLE_LOCAL_ER_EVT:
    break;

  case ESP_GAP_BLE_SEC_REQ_EVT: {
    esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
    break;
  }

  case ESP_GAP_BLE_NC_REQ_EVT: {
    esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
    ESP_LOGI(kLogTag, "Numeric comparison, passkey %" PRIu32, param->ble_security.key_notif.passkey);
    break;
  }

  case ESP_GAP_BLE_PASSKEY_NOTIF_EVT: {
    ESP_LOGI(kLogTag, "Passkey notify: %06" PRIu32, param->ble_security.key_notif.passkey);
    break;
  }

  case ESP_GAP_BLE_KEY_EVT: {
    ESP_LOGI(kLogTag, "Key exchanged: %s", KeyTypeToStr(param->ble_security.ble_key.key_type));
    break;
  }

  case ESP_GAP_BLE_AUTH_CMPL_EVT: {
    const auto& auth = param->ble_security.auth_cmpl;
    if (!auth.success) {
      ESP_LOGI(kLogTag, "Pairing failed, reason 0x%x", auth.fail_reason);
    } else {
      ESP_LOGI(kLogTag, "Pairing successful, mode %s", AuthReqToStr(auth.auth_mode));
    }
    break;
  }

  case ESP_GAP_BLE_SCAN_RESULT_EVT: {
    const auto& result = param->scan_rst;
    if (result.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
      if (connected_) break;
      if (memcmp(result.bda, target_bda_, ESP_BD_ADDR_LEN) != 0) break;

      ESP_LOGI(kLogTag, "Target device found, connecting...");
      connected_ = true;
      esp_ble_gap_stop_scanning();

      esp_ble_gatt_creat_conn_params_t conn_params = {
        .remote_bda = {},
        .remote_addr_type = result.ble_addr_type,
        .is_direct = true,
        .is_aux = false,
        .own_addr_type = BLE_ADDR_TYPE_RPA_PUBLIC,
        .phy_mask = 0x0,
        .phy_1m_conn_params = nullptr,
        .phy_2m_conn_params = nullptr,
        .phy_coded_conn_params = nullptr,
      };
      memcpy(conn_params.remote_bda, result.bda, ESP_BD_ADDR_LEN);
      esp_ble_gattc_enh_open(profiles_[kProfileAppId].gattc_if, &conn_params);
    }
    break;
  }

  case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT: {
    if (param->scan_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
      ESP_LOGE(kLogTag, "Scan stop failed, status %x", param->scan_stop_cmpl.status);
    } else {
      ESP_LOGI(kLogTag, "Scanning stopped");
    }
    break;
  }

  default:
    break;
  }
}

void BLEClient::HandleGattcEvent(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                 esp_ble_gattc_cb_param_t* param)
{
  auto& profile = profiles_[kProfileAppId];

  switch (event) {
  case ESP_GATTC_REG_EVT: {
    ESP_LOGI(kLogTag, "GATTC registered, app_id %u, if %d", param->reg.app_id, gattc_if);
    esp_ble_gap_config_local_privacy(true);
    break;
  }

  case ESP_GATTC_CONNECT_EVT: {
    ESP_LOGI(kLogTag, "Connected, conn_id %d", param->connect.conn_id);
    break;
  }

  case ESP_GATTC_OPEN_EVT: {
    if (param->open.status != ESP_GATT_OK) {
      ESP_LOGE(kLogTag, "Open failed, status %x", param->open.status);
      connected_ = false;
      esp_ble_gap_start_scanning(0);
      break;
    }

    ESP_LOGI(kLogTag, "Open successful, MTU %d", param->open.mtu);
    profile.conn_id = param->open.conn_id;
    memcpy(profile.remote_bda, param->open.remote_bda, sizeof(esp_bd_addr_t));
    esp_ble_gattc_send_mtu_req(gattc_if, param->open.conn_id);
    break;
  }

  case ESP_GATTC_CFG_MTU_EVT: {
    ESP_LOGI(kLogTag, "MTU configured: %d", param->cfg_mtu.mtu);
    esp_ble_gattc_search_service(gattc_if, param->cfg_mtu.conn_id, &service_uuid_);
    break;
  }

  case ESP_GATTC_SEARCH_RES_EVT: {
    if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_128) {
      if (memcmp(param->search_res.srvc_id.uuid.uuid.uuid128, kServiceUuid, 16) == 0) {
        ESP_LOGI(kLogTag, "Niimbot service found");
        has_service_ = true;
        profile.service_start_handle = param->search_res.start_handle;
        profile.service_end_handle = param->search_res.end_handle;
      }
    }
    break;
  }

  case ESP_GATTC_SEARCH_CMPL_EVT: {
    if (param->search_cmpl.status != ESP_GATT_OK) {
      ESP_LOGE(kLogTag, "Service search failed, status %x", param->search_cmpl.status);
      break;
    }

    ESP_LOGI(kLogTag, "Service search complete");
    if (!has_service_) {
      ESP_LOGE(kLogTag, "Niimbot service not found");
      break;
    }

    // Get characteristics
    uint16_t count = 0;
    esp_ble_gattc_get_attr_count(gattc_if, profile.conn_id, ESP_GATT_DB_CHARACTERISTIC,
                                 profile.service_start_handle, profile.service_end_handle,
                                 kInvalidHandle, &count);

    if (count == 0) {
      ESP_LOGE(kLogTag, "No characteristics found");
      break;
    }

    auto* chars = static_cast<esp_gattc_char_elem_t*>(
      malloc(sizeof(esp_gattc_char_elem_t) * count));
    if (!chars) break;

    esp_ble_gattc_get_all_char(gattc_if, profile.conn_id,
                               profile.service_start_handle, profile.service_end_handle,
                               chars, &count, 0);

    for (uint16_t i = 0; i < count; i++) {
      if (chars[i].uuid.len == ESP_UUID_LEN_128 &&
          memcmp(chars[i].uuid.uuid.uuid128, kCharacteristicUuid, 16) == 0) {
        ESP_LOGI(kLogTag, "Niimbot characteristic found, handle %d", chars[i].char_handle);
        profile.char_handle = chars[i].char_handle;

        if (chars[i].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) {
          esp_ble_gattc_register_for_notify(gattc_if, profile.remote_bda, chars[i].char_handle);
        }
        break;
      }
    }

    free(chars);
    break;
  }

  case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
    if (param->reg_for_notify.status != ESP_GATT_OK) {
      ESP_LOGE(kLogTag, "Notify registration failed, status %x", param->reg_for_notify.status);
      break;
    }

    ESP_LOGI(kLogTag, "Notify registration successful");

    // Get descriptors and enable notifications
    uint16_t count = 0;
    esp_ble_gattc_get_attr_count(gattc_if, profile.conn_id, ESP_GATT_DB_DESCRIPTOR,
                                 profile.service_start_handle, profile.service_end_handle,
                                 param->reg_for_notify.handle, &count);

    if (count > 0) {
      auto* descs = static_cast<esp_gattc_descr_elem_t*>(
        malloc(sizeof(esp_gattc_descr_elem_t) * count));
      if (descs) {
        esp_ble_gattc_get_all_descr(gattc_if, profile.conn_id,
                                    param->reg_for_notify.handle, descs, &count, 0);

        for (uint16_t i = 0; i < count; i++) {
          if (descs[i].uuid.len == ESP_UUID_LEN_16 &&
              descs[i].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG) {
            ESP_LOGI(kLogTag, "Enabling notifications via CCCD");
            uint16_t notify_en = 0x0001;
            esp_ble_gattc_write_char_descr(gattc_if, profile.conn_id, descs[i].handle,
                                           sizeof(notify_en), reinterpret_cast<uint8_t*>(&notify_en),
                                           ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
            break;
          }
        }
        free(descs);
      }
    }
    break;
  }

  case ESP_GATTC_NOTIFY_EVT: {
    ESP_LOGD(kLogTag, "Notification received (%d bytes)", param->notify.value_len);
    if (data_received_callback_) {
      data_received_callback_(param->notify.value, param->notify.value_len);
    }
    break;
  }

  case ESP_GATTC_WRITE_DESCR_EVT: {
    if (param->write.status != ESP_GATT_OK) {
      ESP_LOGE(kLogTag, "Descriptor write failed, status %x", param->write.status);
      break;
    }

    ESP_LOGI(kLogTag, "Notifications enabled");
    gattc_if_ = gattc_if;
    if (connected_callback_) {
      connected_callback_();
    }
    break;
  }

  case ESP_GATTC_WRITE_CHAR_EVT: {
    if (param->write.status != ESP_GATT_OK) {
      ESP_LOGE(kLogTag, "Char write failed, status %x", param->write.status);
    }
    if (write_complete_callback_) {
      write_complete_callback_();
    }
    break;
  }

  case ESP_GATTC_SRVC_CHG_EVT: {
    ESP_LOGI(kLogTag, "Service changed");
    break;
  }

  case ESP_GATTC_DISCONNECT_EVT: {
    ESP_LOGI(kLogTag, "Disconnected, reason 0x%02x", param->disconnect.reason);
    connected_ = false;
    has_service_ = false;
    gattc_if_ = ESP_GATT_IF_NONE;

    profile.service_start_handle = 0;
    profile.service_end_handle = 0;
    profile.char_handle = 0;

    ESP_LOGI(kLogTag, "Restarting scan...");
    esp_ble_gap_start_scanning(0);
    break;
  }

  default:
    break;
  }
}
