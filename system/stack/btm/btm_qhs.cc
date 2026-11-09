/*
 * Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "stack/btm/btm_qhs.h"

#include <bluetooth/log.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "osi/include/properties.h"
#include "btif/include/btif_config.h"
#include "hci/hci_interface.h"
#include "main/shim/entry.h"
#include "packet/raw_builder.h"
#include "stack/acl/acl.h"
#include "stack/btm/internal/btm_api.h"
#include "stack/include/acl_api.h"
#include "stack/include/hcidefs.h"
#include "stack/include/main_thread.h"

using namespace bluetooth;

namespace {

constexpr uint16_t kQtiGetAddonFeaturesOcf = 0x001d;
constexpr uint16_t kQtiQbceOcf = 0x0051;

constexpr uint8_t kQbceSetQhsHostMode = 0x01;
constexpr uint8_t kQbceQllConnectionComplete = 0x09;
constexpr uint8_t kQbceReadLocalQllFeatures = 0x0b;
constexpr uint8_t kQbceReadRemoteQllFeatures = 0x0c;
constexpr uint8_t kQbceSetQlmEventMask = 0x0f;
constexpr uint8_t kQbceSetQllEventMask = 0x10;

constexpr uint8_t kQhsTransportBrEdr = 0;
constexpr uint8_t kQhsTransportLe = 1;
constexpr uint8_t kQhsTransportLeIso = 2;
constexpr uint8_t kQhsBrEdrMask = 0x01;
constexpr uint8_t kQhsLeMask = 0x02;
constexpr uint8_t kQhsHostAware = 3;
constexpr uint8_t kQhsDisableAll = 4;

constexpr uint8_t kQcmPhyStateEvent = 0x01;
constexpr uint8_t kRemoteQllFeaturesComplete = 0x06;
constexpr uint8_t kQllStateConnected = 1;
constexpr uint8_t kQllStateFeaturesReady = 2;
constexpr uint8_t kQllStateFailed = 3;
constexpr uint64_t kRemoteQhsPhyFeature = 1ULL << 22;
constexpr char kQcmPhyStateKey[] = "QCM_PHY_STATE";
constexpr char kQhsTestStateProperty[] = "persist.bluetooth.qhs_test_state";

// Qualcomm stores this mask most-significant-byte first and serializes it in
// reverse. These are the bytes actually sent on HCI by stock HyperOS.
constexpr std::array<uint8_t, 8> kQllEventMaskWire = {
        0x4a, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

std::array<uint8_t, 245> qti_addon_features{};
size_t qti_addon_features_len = 0;
std::array<uint8_t, 8> qti_local_qll_features{};
size_t qti_local_qll_features_len = 0;
bool qti_qle_hci_supported = false;
bool qti_qcm_hci_supported = false;

void qhs_command_complete(tBTM_VSC_CMPL* complete) {
  if (complete == nullptr || complete->p_param_buf == nullptr || complete->param_len == 0) {
    log::warn("QHS command complete without return parameters");
    return;
  }
  const uint8_t status = complete->p_param_buf[0];
  const uint8_t subcmd = complete->param_len > 1 ? complete->p_param_buf[1] : 0xff;
  log::info("QHS FC51 complete status=0x{:02x} subcmd=0x{:02x} len={}", status, subcmd,
            complete->param_len);
}

void remote_qll_command_status_or_complete(hci::CommandStatusOrCompleteView result) {
  if (std::holds_alternative<hci::CommandStatusView>(result)) {
    const auto view = std::get<hci::CommandStatusView>(result);
    log::info("Remote QLL feature read Command Status=0x{:02x}",
              static_cast<uint8_t>(view.GetStatus()));
    return;
  }
  const auto view = std::get<hci::CommandCompleteView>(result);
  const auto payload_view = view.GetPayload();
  const std::vector<uint8_t> payload(payload_view.begin(), payload_view.end());
  const uint8_t status = payload.empty() ? 0xff : payload[0];
  log::info("Remote QLL feature read Command Complete status=0x{:02x}", status);
}

void send_remote_qll_feature_read(uint16_t handle) {
  std::vector<uint8_t> parameters = {
          kQbceReadRemoteQllFeatures,
          static_cast<uint8_t>(handle & 0xff),
          static_cast<uint8_t>((handle >> 8) & 0xff),
  };
  constexpr uint16_t kVendorOgf = 0x3f;
  const auto opcode = static_cast<hci::OpCode>((kVendorOgf << 10) | kQtiQbceOcf);
  bluetooth::shim::GetHciLayer()->EnqueueCommand(
          hci::CommandBuilder::Create(
                  opcode, std::make_unique<packet::RawBuilder>(std::move(parameters))),
          get_main()->BindOnce(remote_qll_command_status_or_complete));
}

void read_local_qll_features_complete(tBTM_VSC_CMPL* complete) {
  qti_local_qll_features.fill(0);
  qti_local_qll_features_len = 0;
  if (complete == nullptr || complete->p_param_buf == nullptr || complete->param_len < 2) {
    log::warn("Read local QLL features returned no usable payload");
    return;
  }
  const uint8_t* p = complete->p_param_buf;
  const uint8_t status = p[0];
  const uint8_t subcmd = p[1];
  if (status != HCI_SUCCESS || subcmd != kQbceReadLocalQllFeatures) {
    log::warn("Read local QLL features failed status=0x{:02x} subcmd=0x{:02x}", status, subcmd);
    return;
  }
  const size_t available = complete->param_len - 2;
  qti_local_qll_features_len = std::min(available, qti_local_qll_features.size());
  std::copy_n(p + 2, qti_local_qll_features_len, qti_local_qll_features.begin());
  log::info("Local QLL features={:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
            qti_local_qll_features[0], qti_local_qll_features[1], qti_local_qll_features[2],
            qti_local_qll_features[3], qti_local_qll_features[4], qti_local_qll_features[5],
            qti_local_qll_features[6], qti_local_qll_features[7]);
}

void send_qhs_host_mode(uint8_t transport, uint8_t mode) {
  uint8_t cmd[] = {kQbceSetQhsHostMode, transport, mode};
  BTM_VendorSpecificCommand(kQtiQbceOcf, sizeof(cmd), cmd, qhs_command_complete);
}

void send_qll_event_mask(uint8_t subcmd) {
  std::array<uint8_t, 9> cmd{};
  cmd[0] = subcmd;
  std::copy(kQllEventMaskWire.begin(), kQllEventMaskWire.end(), cmd.begin() + 1);
  BTM_VendorSpecificCommand(kQtiQbceOcf, cmd.size(), cmd.data(), qhs_command_complete);
}

void addon_features_complete(tBTM_VSC_CMPL* complete) {
  qti_addon_features.fill(0);
  qti_addon_features_len = 0;
  qti_qle_hci_supported = false;
  qti_qcm_hci_supported = false;
  if (complete == nullptr || complete->p_param_buf == nullptr || complete->param_len < 5) {
    log::warn("QTI addon feature query returned no usable payload");
    return;
  }

  const uint8_t* p = complete->p_param_buf;
  const uint8_t status = *p++;
  if (status != HCI_SUCCESS) {
    log::warn("QTI addon feature query failed status=0x{:02x}", status);
    return;
  }
  const uint16_t product_id = static_cast<uint16_t>(p[0] | (p[1] << 8));
  p += 2;
  const uint16_t response_version = static_cast<uint16_t>(p[0] | (p[1] << 8));
  p += 2;
  const size_t available = complete->param_len - 5;
  qti_addon_features_len = std::min(available, qti_addon_features.size());
  std::copy_n(p, qti_addon_features_len, qti_addon_features.begin());
  if (qti_addon_features_len > 3) {
    qti_qle_hci_supported = (qti_addon_features[3] & 0x10) != 0;
    qti_qcm_hci_supported = (qti_addon_features[3] & 0x20) != 0;
  }
  log::info("QTI addon features product=0x{:04x} version=0x{:04x} len={} qle={} qcm={}",
            product_id, response_version, qti_addon_features_len, qti_qle_hci_supported,
            qti_qcm_hci_supported);
  BTM_ConfigQHS();
}

tACL_CONN* acl_from_qll_handle(uint16_t handle) {
  handle &= 0x0fff;
  const RawAddress address = acl_address_from_handle(handle);
  if (address.IsEmpty()) {
    return nullptr;
  }
  tACL_CONN* acl = btm_acl_for_bda(address, BT_TRANSPORT_LE);
  if (acl == nullptr) {
    acl = btm_acl_for_bda(address, BT_TRANSPORT_BR_EDR);
  }
  return acl;
}

}  // namespace

void BTM_QhsInit() {
  char soc[PROPERTY_VALUE_MAX] = {};
  if (osi_property_get("persist.vendor.qcom.bluetooth.soc", soc, "") <= 0 || soc[0] == '\0') {
    log::info("QHS init skipped: Qualcomm Bluetooth SoC property is absent");
    return;
  }
  qti_addon_features.fill(0);
  qti_addon_features_len = 0;
  qti_local_qll_features.fill(0);
  qti_local_qll_features_len = 0;
  qti_qle_hci_supported = false;
  qti_qcm_hci_supported = false;
  log::info("QHS init for Qualcomm Bluetooth SoC {}", soc);
  BTM_VendorSpecificCommand(kQtiGetAddonFeaturesOcf, 0, nullptr, addon_features_complete);
}

void BTM_ConfigQHS() {
  if (qti_qle_hci_supported) {
    char qhs_value[PROPERTY_VALUE_MAX] = {};
    osi_property_get("persist.vendor.qcom.bluetooth.qhs_enable", qhs_value, "true");
    const bool qhs_enabled = std::strncmp(qhs_value, "true", 4) == 0;
    send_qhs_host_mode(kQhsTransportLeIso, qhs_enabled ? kQhsHostAware : kQhsDisableAll);

    char qhs_support_value[PROPERTY_VALUE_MAX] = {};
    osi_property_get("persist.vendor.qcom.bluetooth.qhs_support", qhs_support_value, "255");
    const uint8_t support_mask = static_cast<uint8_t>(std::atoi(qhs_support_value));
    if (support_mask != 0xff) {
      send_qhs_host_mode(kQhsTransportBrEdr,
                         (support_mask & kQhsBrEdrMask) ? kQhsHostAware : kQhsDisableAll);
      send_qhs_host_mode(kQhsTransportLe,
                         (support_mask & kQhsLeMask) ? kQhsHostAware : kQhsDisableAll);
      send_qhs_host_mode(kQhsTransportLeIso,
                         (support_mask & kQhsLeMask) ? kQhsHostAware : kQhsDisableAll);
    }
    send_qll_event_mask(kQbceSetQllEventMask);
  }

  if (qti_qcm_hci_supported) {
    send_qll_event_mask(kQbceSetQlmEventMask);
  }

  if (qti_qle_hci_supported) {
    uint8_t cmd[] = {kQbceReadLocalQllFeatures};
    BTM_VendorSpecificCommand(kQtiQbceOcf, sizeof(cmd), cmd, read_local_qll_features_complete);
  }
}

void BTM_GetQllLocalSupportedFeatures(uint8_t* features) {
  if (features == nullptr) {
    return;
  }
  std::copy(qti_local_qll_features.begin(), qti_local_qll_features.end(), features);
}

bool BTM_IsQHSPhySupported(const RawAddress& bd_addr, tBT_TRANSPORT transport) {
  if (transport == BT_TRANSPORT_BR_EDR) {
    int qcm_phy_state = 0;
    const std::string section = bd_addr.ToString();
    if (!btif_config_get_int(section, kQcmPhyStateKey, &qcm_phy_state)) {
      log::debug("No QCM PHY state for peer {}", bd_addr);
      return false;
    }
    return qcm_phy_state == 1;
  }

  if (transport != BT_TRANSPORT_LE) {
    return false;
  }

  // Stock only evaluates the remote QLL feature vector when the controller
  // advertised QBCE/QLE HCI support (SoC add-on feature byte 3, bit 4).
  if (!qti_qle_hci_supported) {
    return false;
  }

  tACL_CONN* acl = btm_acl_for_bda(bd_addr, transport);
  if (acl == nullptr || acl->qll_features_state != kQllStateFeaturesReady) {
    return false;
  }
  uint64_t features = 0;
  for (size_t i = 0; i < acl->remote_qll_features.size(); i++) {
    features |= static_cast<uint64_t>(acl->remote_qll_features[i]) << (i * 8);
  }
  return (features & kRemoteQhsPhyFeature) != 0;
}

void BTM_QllVendorSpecificEvent(uint8_t subevent, const uint8_t* payload, size_t length) {
  if (payload == nullptr) {
    return;
  }

  // Stock QBCE subevent 0x01 reports the classic BR/EDR QCM PHY state.
  // VendorSpecificEventView removes the subevent byte, so payload is:
  // status, handle_lo, handle_hi, qcm_phy_state, ...
  if (subevent == kQcmPhyStateEvent) {
    if (length < 4) {
      log::warn("Short QCM PHY-state event len={}", length);
      return;
    }
    const uint8_t status = payload[0];
    const uint16_t handle = static_cast<uint16_t>(payload[1] | (payload[2] << 8)) & 0x0fff;
    if (status != HCI_SUCCESS) {
      log::warn("QCM PHY-state event failed handle=0x{:04x} status=0x{:02x}", handle, status);
      return;
    }
    const RawAddress address = acl_address_from_handle(handle);
    if (address.IsEmpty()) {
      log::warn("QCM PHY-state event for unknown handle=0x{:04x}", handle);
      return;
    }
    const int qcm_phy_state = payload[3];
    const std::string section = address.ToString();
    if (!btif_config_set_int(section, kQcmPhyStateKey, qcm_phy_state)) {
      log::warn("Failed to persist QCM PHY state={} for peer {}", qcm_phy_state, address);
      return;
    }
    log::info("QCM PHY state handle=0x{:04x} state={} qhs={}", handle, qcm_phy_state,
              qcm_phy_state == 1);
    return;
  }

  if (subevent == kQbceQllConnectionComplete) {
    if (length < 3) {
      log::warn("Short QLL connection-complete event len={}", length);
      return;
    }
    const uint8_t status = payload[0];
    const uint16_t handle = static_cast<uint16_t>(payload[1] | (payload[2] << 8)) & 0x0fff;
    tACL_CONN* acl = acl_from_qll_handle(handle);
    if (acl == nullptr) {
      log::warn("QLL connection-complete for unknown handle=0x{:04x}", handle);
      return;
    }
    if (status != HCI_SUCCESS) {
      acl->qll_features_state = kQllStateFailed;
      acl->remote_qll_features.fill(0);
      log::warn("QLL connection failed handle=0x{:04x} status=0x{:02x}", handle, status);
      return;
    }

    acl->qll_features_state = kQllStateConnected;
    acl->remote_qll_features.fill(0);
    send_remote_qll_feature_read(handle);
    log::info("QLL connected handle=0x{:04x}; requested remote QLL features", handle);
    return;
  }

  if (subevent == kRemoteQllFeaturesComplete) {
    if (length < 3) {
      log::warn("Short remote QLL feature event len={}", length);
      return;
    }
    const uint8_t status = payload[0];
    const uint16_t handle = static_cast<uint16_t>(payload[1] | (payload[2] << 8)) & 0x0fff;
    tACL_CONN* acl = acl_from_qll_handle(handle);
    if (acl == nullptr) {
      log::warn("Remote QLL features for unknown handle=0x{:04x}", handle);
      return;
    }
    if (status != HCI_SUCCESS) {
      acl->qll_features_state = kQllStateFailed;
      acl->remote_qll_features.fill(0);
      log::warn("Remote QLL feature read failed handle=0x{:04x} status=0x{:02x}", handle,
                status);
      return;
    }

    acl->qll_features_state = kQllStateFeaturesReady;
    acl->remote_qll_features.fill(0);
    const size_t feature_len = std::min<size_t>(8, length - 3);
    std::copy_n(payload + 3, feature_len, acl->remote_qll_features.begin());
    const bool qhs = BTM_IsQHSPhySupported(acl->RemoteAddress(), acl->link_spec.transport);
    log::info("Remote QLL features ready handle=0x{:04x} qhs={}", handle, qhs);
  }
}

void BTM_QhsTestMaybeInjectBrEdrState(const RawAddress& bd_addr) {
#if !defined(__ANDROID_DEBUGGABLE__)
  (void)bd_addr;
  return;
#else
  char value[PROPERTY_VALUE_MAX] = {};
  osi_property_get(kQhsTestStateProperty, value, "-1");
  char* end = nullptr;
  const long requested_state = std::strtol(value, &end, 10);
  if (end == value || *end != '\0' || (requested_state != 0 && requested_state != 1)) return;
  tACL_CONN* acl = btm_acl_for_bda(bd_addr, BT_TRANSPORT_BR_EDR);
  if (acl == nullptr) { log::warn("QHS TEST: no BR/EDR ACL for peer {}", bd_addr); return; }
  const uint16_t handle = acl->Handle() & 0x0fff;
  const uint8_t payload[] = {HCI_SUCCESS, static_cast<uint8_t>(handle & 0xff), static_cast<uint8_t>((handle >> 8) & 0xff), static_cast<uint8_t>(requested_state)};
  log::warn("QHS TEST ONLY: injecting stock-format QCM VSE 0x01 handle=0x{:04x} state={} peer={}", handle, requested_state, bd_addr);
  BTM_QllVendorSpecificEvent(kQcmPhyStateEvent, payload, sizeof(payload));
  log::warn("QHS TEST ONLY: post-injection BR/EDR qhs={}", BTM_IsQHSPhySupported(bd_addr, BT_TRANSPORT_BR_EDR));
#endif
}

