/*
 * Copyright 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "BTAudioCodecStatusAIDL"

#include "codec_status_aidl.h"

#include <aidl/vendor/qti/hardware/bluetooth/audio/classicVendorCapabilities.h>
#include <aidl/vendor/qti/hardware/bluetooth/audio/classicVendorConfiguration.h>
#include <bluetooth/log.h>

#include <optional>
#include <unordered_set>
#include <vector>

#include "a2dp_aac_constants.h"
#include "a2dp_sbc_constants.h"
#include "a2dp_vendor_aptx_adaptive.h"
#include "a2dp_vendor_aptx_adaptive_constants.h"
#include "a2dp_vendor_aptx_constants.h"
#include "a2dp_vendor_aptx_hd_constants.h"
#include "a2dp_vendor_lhdc_constants.h"
#include "a2dp_vendor_ldac_constants.h"
#include "bta/include/bta_av_api.h"
#include "btif/include/btif_av.h"
#include "client_interface_aidl.h"

namespace bluetooth::audio::aidl::a2dp::codec {

using ::aidl::android::hardware::bluetooth::audio::AacCapabilities;
using ::aidl::android::hardware::bluetooth::audio::AacConfiguration;
using ::aidl::android::hardware::bluetooth::audio::AacObjectType;
using ::aidl::android::hardware::bluetooth::audio::AptxCapabilities;
using ::aidl::android::hardware::bluetooth::audio::AptxConfiguration;
using ::aidl::android::hardware::bluetooth::audio::AptxAdaptiveCapabilities;
using ::aidl::android::hardware::bluetooth::audio::AptxAdaptiveChannelMode;
using ::aidl::android::hardware::bluetooth::audio::AptxAdaptiveConfiguration;
using ::aidl::android::hardware::bluetooth::audio::AptxAdaptiveInputMode;
using ::aidl::android::hardware::bluetooth::audio::AptxAdaptiveTimeToPlay;
using ::aidl::android::hardware::bluetooth::audio::AptxMode;
using ::aidl::android::hardware::bluetooth::audio::AudioCapabilities;
using ::aidl::android::hardware::bluetooth::audio::ChannelMode;
using ::aidl::android::hardware::bluetooth::audio::CodecCapabilities;
using ::aidl::android::hardware::bluetooth::audio::CodecType;
using ::aidl::android::hardware::bluetooth::audio::LdacCapabilities;
using ::aidl::android::hardware::bluetooth::audio::LdacChannelMode;
using ::aidl::android::hardware::bluetooth::audio::LdacConfiguration;
using ::aidl::android::hardware::bluetooth::audio::LdacQualityIndex;
using ::aidl::android::hardware::bluetooth::audio::OpusCapabilities;
using ::aidl::android::hardware::bluetooth::audio::OpusConfiguration;
using ::aidl::android::hardware::bluetooth::audio::SbcAllocMethod;
using ::aidl::android::hardware::bluetooth::audio::SbcCapabilities;
using ::aidl::android::hardware::bluetooth::audio::SbcChannelMode;
using ::aidl::android::hardware::bluetooth::audio::SbcConfiguration;
using ::aidl::vendor::qti::hardware::bluetooth::audio::classicVendorCapabilities;
using ::aidl::vendor::qti::hardware::bluetooth::audio::classicVendorConfiguration;
using ::aidl::vendor::qti::hardware::bluetooth::audio::LhdcChannelMode;
using ::aidl::vendor::qti::hardware::bluetooth::audio::LhdcChannelSplitMode;
using ::aidl::vendor::qti::hardware::bluetooth::audio::LhdcMaxTargetBitrate;
using ::aidl::vendor::qti::hardware::bluetooth::audio::LhdcMinTargetBitrate;
using ::aidl::vendor::qti::hardware::bluetooth::audio::LhdcMode;
using ::aidl::vendor::qti::hardware::bluetooth::audio::LhdcQualityIndex;
using ::aidl::vendor::qti::hardware::bluetooth::audio::LhdcVersion;
using ::aidl::vendor::qti::hardware::bluetooth::audio::VendorCodecType;

using ::bluetooth::audio::a2dp::ahal_codec_configuration;

using LhdcCapabilities = classicVendorCapabilities::LhdcCapabilities;
using LhdcConfiguration = classicVendorConfiguration::LhdcConfiguration;

namespace {

// capabilities from BluetoothAudioClientInterface::GetAudioCapabilities()
std::vector<AudioCapabilities> audio_hal_capabilities(0);
// capabilities that audio HAL supports and frameworks / Bluetooth SoC / runtime
// preference would like to use.
std::vector<AudioCapabilities> offloading_preference(0);

// The onyx QTI V4 provider serializes its standard aptX Adaptive capability
// with this invalid CodecType value and leaves the capability union at an
// empty/default SBC arm. Keep the compatibility handling tied to that exact
// observed signature rather than enabling Adaptive unconditionally.
constexpr int32_t kQtiMalformedAptxAdaptiveCodecType = 0x908;
bool qti_malformed_aptx_adaptive_capability = false;

static int32_t A2dpCodecToHalSampleRate(const btav_a2dp_codec_config_t& a2dp_codec_config) {
  switch (a2dp_codec_config.sample_rate) {
    case BTAV_A2DP_CODEC_SAMPLE_RATE_44100:
      return 44100;
    case BTAV_A2DP_CODEC_SAMPLE_RATE_48000:
      return 48000;
    case BTAV_A2DP_CODEC_SAMPLE_RATE_88200:
      return 88200;
    case BTAV_A2DP_CODEC_SAMPLE_RATE_96000:
      return 96000;
    case BTAV_A2DP_CODEC_SAMPLE_RATE_176400:
      return 176400;
    case BTAV_A2DP_CODEC_SAMPLE_RATE_192000:
      return 192000;
    case BTAV_A2DP_CODEC_SAMPLE_RATE_16000:
      return 16000;
    case BTAV_A2DP_CODEC_SAMPLE_RATE_24000:
      return 24000;
    default:
      return 0;
  }
}

static int8_t A2dpCodecToHalBitsPerSample(const btav_a2dp_codec_config_t& a2dp_codec_config) {
  switch (a2dp_codec_config.bits_per_sample) {
    case BTAV_A2DP_CODEC_BITS_PER_SAMPLE_16:
      return 16;
    case BTAV_A2DP_CODEC_BITS_PER_SAMPLE_24:
      return 24;
    case BTAV_A2DP_CODEC_BITS_PER_SAMPLE_32:
      return 32;
    default:
      return 0;
  }
}

static ChannelMode A2dpCodecToHalChannelMode(const btav_a2dp_codec_config_t& a2dp_codec_config) {
  switch (a2dp_codec_config.channel_mode) {
    case BTAV_A2DP_CODEC_CHANNEL_MODE_MONO:
      return ChannelMode::MONO;
    case BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO:
      return ChannelMode::STEREO;
    default:
      return ChannelMode::UNKNOWN;
  }
}

template <typename T>
struct identity {
  typedef T type;
};

template <class T>
bool ContainedInVector(const std::vector<T>& vector, const typename identity<T>::type& target) {
  return std::find(vector.begin(), vector.end(), target) != vector.end();
}

// The QTI AIDL extension identifies classic LHDC as vendor codec 3. Its
// nested parcelables are carried in standard VINTF ParcelableHolders.
constexpr int32_t kQtiBluetoothAudioVendorId = 1;
constexpr char16_t kQtiLhdcCodecId = 3;

// Full AVDTP vendor codec information element offsets. H0-H2 occupy bytes
// 0-2, followed by the four-byte vendor ID and two-byte codec ID.
constexpr size_t kLhdcP6 = 9;
constexpr size_t kLhdcP7 = 10;
constexpr size_t kLhdcP8 = 11;
constexpr size_t kLhdcV5P9 = 12;

std::optional<LhdcCapabilities> GetLhdcCapabilities(const CodecCapabilities& codec_capability) {
  if (codec_capability.codecType != CodecType::VENDOR ||
      codec_capability.capabilities.getTag() !=
              CodecCapabilities::Capabilities::vendorCapabilities) {
    return std::nullopt;
  }

  const auto& extension =
          codec_capability.capabilities.get<CodecCapabilities::Capabilities::vendorCapabilities>()
                  .extension;
  std::optional<classicVendorCapabilities> vendor_capability;
  const binder_status_t status = extension.getParcelable(&vendor_capability);
  if (status != STATUS_OK || !vendor_capability.has_value()) {
    log::warn("Unable to decode QTI vendor capability: status={} value={}", status,
              vendor_capability.has_value());
    return std::nullopt;
  }
  if (vendor_capability->vendorCodecType != VendorCodecType::LHDC) {
    log::warn("Unsupported QTI vendor capability: type={} tag={}",
              toString(vendor_capability->vendorCodecType),
              static_cast<int32_t>(vendor_capability->capabilities.getTag()));
    return std::nullopt;
  }

  if (vendor_capability->capabilities.getTag() ==
      classicVendorCapabilities::CodecSpecific::lhdcCapabilities) {
    return vendor_capability->capabilities
            .get<classicVendorCapabilities::CodecSpecific::lhdcCapabilities>();
  }

  // QTI's V4 provider on onyx identifies this capability as LHDC but leaves
  // the union at its default aptX Adaptive R4 arm. Mirror the capability set
  // advertised by BTAudioCodecsAidl while retaining normal validation of the
  // locally generated LHDC configuration below.
  if (vendor_capability->capabilities.getTag() !=
      classicVendorCapabilities::CodecSpecific::aptxAdaptiveR4Cap) {
    log::warn("Unexpected QTI LHDC capability tag={}",
              static_cast<int32_t>(vendor_capability->capabilities.getTag()));
    return std::nullopt;
  }

  log::warn("QTI LHDC capability uses default union arm; applying provider capability set");
  LhdcCapabilities capability;
  capability.sampleRateHz = {44100, 48000, 88200, 96000, 192000};
  capability.channelMode = {LhdcChannelMode::STEREO};
  capability.qualityIndex = {LhdcQualityIndex::QUALITY_HIGH, LhdcQualityIndex::QUALITY_MID,
                             LhdcQualityIndex::QUALITY_LOW, LhdcQualityIndex::QUALITY_LOW4,
                             LhdcQualityIndex::QUALITY_ABR,
                             static_cast<LhdcQualityIndex>(9),
                             static_cast<LhdcQualityIndex>(0)};
  capability.bitsPerSample = {16, 24, 32};
  capability.version = {LhdcVersion::LHDC_V2, LhdcVersion::LHDC_V3, LhdcVersion::LHDC_V4,
                        LhdcVersion::LHDC_V5};
  capability.mode = {LhdcMode::HQ, LhdcMode::LL};
  capability.maxTargetBitrate = {
          LhdcMaxTargetBitrate::MaxTargetBitrate_900kV4_noLimitV5,
          LhdcMaxTargetBitrate::MaxTargetBitrate_500kV4_400kV5,
          LhdcMaxTargetBitrate::MaxTargetBitrate_400kV4_600kV5,
          LhdcMaxTargetBitrate::MaxTargetBitrate_ReservedV4_900kV5};
  capability.minTargetBitrate = {
          LhdcMinTargetBitrate::MinTargetBitrate_defaultV4_noLimitV5,
          LhdcMinTargetBitrate::MinTargetBitrate_320kV4_128kV5,
          LhdcMinTargetBitrate::MinTargetBitrate_256kV5,
          LhdcMinTargetBitrate::MinTargetBitrate_400kV5};
  capability.channelSplitMode = {LhdcChannelSplitMode::LHDC_CH_SPLIT_NONE,
                                 LhdcChannelSplitMode::LHDC_CH_SPLIT_TWS,
                                 LhdcChannelSplitMode::LHDC_CH_SPLIT_TWS_PLUS};
  capability.llacEnable = false;
  capability.losslessEnable = false;
  capability.jasEnable = false;
  capability.xmsaEnable = false;
  return capability;
}

std::optional<LhdcConfiguration> GetLhdcConfiguration(
        const CodecConfiguration& codec_configuration) {
  if (codec_configuration.codecType != CodecType::VENDOR ||
      codec_configuration.config.getTag() != CodecConfiguration::CodecSpecific::vendorConfig) {
    return std::nullopt;
  }

  const auto& vendor_config =
          codec_configuration.config.get<CodecConfiguration::CodecSpecific::vendorConfig>();
  if (vendor_config.vendorId != kQtiBluetoothAudioVendorId ||
      vendor_config.codecId != kQtiLhdcCodecId) {
    return std::nullopt;
  }

  std::optional<classicVendorConfiguration> classic_config;
  if (vendor_config.codecConfig.getParcelable(&classic_config) != STATUS_OK ||
      !classic_config.has_value() || classic_config->vendorCodecType != VendorCodecType::LHDC ||
      classic_config->config.getTag() != classicVendorConfiguration::CodecSpecific::lhdcConfig) {
    return std::nullopt;
  }

  return classic_config->config.get<classicVendorConfiguration::CodecSpecific::lhdcConfig>();
}

bool sbc_offloading_capability_match(const SbcCapabilities& sbc_capability,
                                     const SbcConfiguration& sbc_config) {
  if (!ContainedInVector(sbc_capability.channelMode, sbc_config.channelMode) ||
      !ContainedInVector(sbc_capability.allocMethod, sbc_config.allocMethod) ||
      !ContainedInVector(sbc_capability.blockLength, sbc_config.blockLength) ||
      !ContainedInVector(sbc_capability.numSubbands, sbc_config.numSubbands) ||
      !ContainedInVector(sbc_capability.bitsPerSample, sbc_config.bitsPerSample) ||
      !ContainedInVector(sbc_capability.sampleRateHz, sbc_config.sampleRateHz) ||
      (sbc_config.minBitpool < sbc_capability.minBitpool ||
       sbc_config.maxBitpool < sbc_config.minBitpool ||
       sbc_capability.maxBitpool < sbc_config.maxBitpool)) {
    log::warn("software codec={} capability={}", sbc_config.toString(), sbc_capability.toString());
    return false;
  }
  log::info("offload codec={} capability={}", sbc_config.toString(), sbc_capability.toString());
  return true;
}

bool aac_offloading_capability_match(const AacCapabilities& aac_capability,
                                     const AacConfiguration& aac_config) {
  if (!ContainedInVector(aac_capability.channelMode, aac_config.channelMode) ||
      !ContainedInVector(aac_capability.objectType, aac_config.objectType) ||
      !ContainedInVector(aac_capability.bitsPerSample, aac_config.bitsPerSample) ||
      !ContainedInVector(aac_capability.sampleRateHz, aac_config.sampleRateHz) ||
      (!aac_capability.variableBitRateSupported && aac_config.variableBitRateEnabled)) {
    log::warn("software codec={} capability={}", aac_config.toString(), aac_capability.toString());
    return false;
  }
  log::info("offloading codec={} capability={}", aac_config.toString(), aac_capability.toString());
  return true;
}

bool aptx_offloading_capability_match(const AptxCapabilities& aptx_capability,
                                      const AptxConfiguration& aptx_config) {
  if (!ContainedInVector(aptx_capability.channelMode, aptx_config.channelMode) ||
      !ContainedInVector(aptx_capability.bitsPerSample, aptx_config.bitsPerSample) ||
      !ContainedInVector(aptx_capability.sampleRateHz, aptx_config.sampleRateHz)) {
    log::warn("software codec={} capability={}", aptx_config.toString(),
              aptx_capability.toString());
    return false;
  }
  log::info("offloading codec={} capability={}", aptx_config.toString(),
            aptx_capability.toString());
  return true;
}

bool aptx_adaptive_offloading_capability_match(
        const AptxAdaptiveCapabilities& aptx_adaptive_capability,
        const AptxAdaptiveConfiguration& aptx_adaptive_config) {
  if (!ContainedInVector(aptx_adaptive_capability.channelMode,
                         aptx_adaptive_config.channelMode) ||
      !ContainedInVector(aptx_adaptive_capability.bitsPerSample,
                         aptx_adaptive_config.bitsPerSample) ||
      !ContainedInVector(aptx_adaptive_capability.sampleRateHz,
                         aptx_adaptive_config.sampleRateHz)) {
    log::warn("software codec={} capability={}", aptx_adaptive_config.toString(),
              aptx_adaptive_capability.toString());
    return false;
  }
  log::info("offloading codec={} capability={}", aptx_adaptive_config.toString(),
            aptx_adaptive_capability.toString());
  return true;
}

bool ldac_offloading_capability_match(const LdacCapabilities& ldac_capability,
                                      const LdacConfiguration& ldac_config) {
  if (!ContainedInVector(ldac_capability.channelMode, ldac_config.channelMode) ||
      !ContainedInVector(ldac_capability.bitsPerSample, ldac_config.bitsPerSample) ||
      !ContainedInVector(ldac_capability.sampleRateHz, ldac_config.sampleRateHz)) {
    log::warn("software codec={} capability={}", ldac_config.toString(),
              ldac_capability.toString());
    return false;
  }
  log::info("offloading codec={} capability={}", ldac_config.toString(),
            ldac_capability.toString());
  return true;
}

bool lhdc_offloading_capability_match(const LhdcCapabilities& lhdc_capability,
                                      const LhdcConfiguration& lhdc_config) {
  const bool unsupported_feature =
          (lhdc_config.llacEnable && !lhdc_capability.llacEnable) ||
          (lhdc_config.losslessEnable && !lhdc_capability.losslessEnable) ||
          (lhdc_config.jasEnable && !lhdc_capability.jasEnable) ||
          (lhdc_config.xmsaEnable && !lhdc_capability.xmsaEnable);
  // Xiaomi's V5 bridge leaves channelSplitMode UNKNOWN because the field is
  // only meaningful for the legacy V2/V3/V4 TWS split modes.
  const bool channel_split_supported =
          (lhdc_config.version == LhdcVersion::LHDC_V5 &&
           lhdc_config.channelSplitMode == LhdcChannelSplitMode::UNKNOWN) ||
          ContainedInVector(lhdc_capability.channelSplitMode, lhdc_config.channelSplitMode);

  if (!ContainedInVector(lhdc_capability.channelMode, lhdc_config.channelMode) ||
      !ContainedInVector(lhdc_capability.qualityIndex, lhdc_config.qualityIndex) ||
      !ContainedInVector(lhdc_capability.bitsPerSample,
                         static_cast<uint8_t>(lhdc_config.bitsPerSample)) ||
      !ContainedInVector(lhdc_capability.sampleRateHz, lhdc_config.sampleRateHz) ||
      !ContainedInVector(lhdc_capability.version, lhdc_config.version) ||
      !ContainedInVector(lhdc_capability.mode, lhdc_config.mode) ||
      !ContainedInVector(lhdc_capability.maxTargetBitrate, lhdc_config.maxTargetBitrate) ||
      !ContainedInVector(lhdc_capability.minTargetBitrate, lhdc_config.minTargetBitrate) ||
      !channel_split_supported || unsupported_feature) {
    log::warn("software codec={} capability={}", lhdc_config.toString(),
              lhdc_capability.toString());
    return false;
  }
  log::info("offloading codec={} capability={}", lhdc_config.toString(),
            lhdc_capability.toString());
  return true;
}

bool opus_offloading_capability_match(const std::optional<OpusCapabilities>& opus_capability,
                                      const std::optional<OpusConfiguration>& opus_config) {
  if (!ContainedInVector(opus_capability->channelMode, opus_config->channelMode) ||
      !ContainedInVector(opus_capability->frameDurationUs, opus_config->frameDurationUs) ||
      !ContainedInVector(opus_capability->samplingFrequencyHz, opus_config->samplingFrequencyHz)) {
    log::warn("software codec={} capability={}", opus_config->toString(),
              opus_capability->toString());
    return false;
  }
  log::info("offloading codec={} capability={}", opus_config->toString(),
            opus_capability->toString());
  return true;
}

/***
 * Check whether this codec is supported by the audio HAL and is allowed to
 * use by preference of framework / Bluetooth SoC / runtime property.
 ***/
bool IsCodecOffloadingEnabled(const CodecConfiguration& codec_config) {
  for (auto preference : offloading_preference) {
    if (codec_config.codecType != preference.get<AudioCapabilities::a2dpCapabilities>().codecType) {
      continue;
    }
    auto codec_capability = preference.get<AudioCapabilities::a2dpCapabilities>();
    switch (codec_capability.codecType) {
      case CodecType::SBC: {
        auto sbc_capability = codec_capability.capabilities
                                      .get<CodecCapabilities::Capabilities::sbcCapabilities>();
        auto sbc_config = codec_config.config.get<CodecConfiguration::CodecSpecific::sbcConfig>();
        return sbc_offloading_capability_match(sbc_capability, sbc_config);
      }
      case CodecType::AAC: {
        auto aac_capability = codec_capability.capabilities
                                      .get<CodecCapabilities::Capabilities::aacCapabilities>();
        auto aac_config = codec_config.config.get<CodecConfiguration::CodecSpecific::aacConfig>();
        return aac_offloading_capability_match(aac_capability, aac_config);
      }
      case CodecType::APTX:
        [[fallthrough]];
      case CodecType::APTX_HD: {
        auto aptx_capability = codec_capability.capabilities
                                       .get<CodecCapabilities::Capabilities::aptxCapabilities>();
        auto aptx_config = codec_config.config.get<CodecConfiguration::CodecSpecific::aptxConfig>();
        return aptx_offloading_capability_match(aptx_capability, aptx_config);
      }
      case CodecType::APTX_ADAPTIVE: {
        auto aptx_adaptive_capability =
                codec_capability.capabilities
                        .get<CodecCapabilities::Capabilities::aptxAdaptiveCapabilities>();
        auto aptx_adaptive_config =
                codec_config.config.get<CodecConfiguration::CodecSpecific::aptxAdaptiveConfig>();
        return aptx_adaptive_offloading_capability_match(aptx_adaptive_capability,
                                                         aptx_adaptive_config);
      }
      case CodecType::LDAC: {
        auto ldac_capability = codec_capability.capabilities
                                       .get<CodecCapabilities::Capabilities::ldacCapabilities>();
        auto ldac_config = codec_config.config.get<CodecConfiguration::CodecSpecific::ldacConfig>();
        return ldac_offloading_capability_match(ldac_capability, ldac_config);
      }
      case CodecType::OPUS: {
        std::optional<OpusCapabilities> opus_capability =
                codec_capability.capabilities
                        .get<CodecCapabilities::Capabilities::opusCapabilities>();
        std::optional<OpusConfiguration> opus_config =
                codec_config.config.get<CodecConfiguration::CodecSpecific::opusConfig>();
        return opus_offloading_capability_match(opus_capability, opus_config);
      }
      case CodecType::VENDOR: {
        auto lhdc_capability = GetLhdcCapabilities(codec_capability);
        auto lhdc_config = GetLhdcConfiguration(codec_config);
        if (!lhdc_capability.has_value() || !lhdc_config.has_value()) {
          log::warn("Unsupported vendor offload codec={}", codec_config.toString());
          return false;
        }
        return lhdc_offloading_capability_match(*lhdc_capability, *lhdc_config);
      }
      case CodecType::UNKNOWN:
        [[fallthrough]];
      default:
        log::error("Unknown codecType={}", toString(codec_capability.codecType));
        return false;
    }
  }
  // QTI's onyx V4 provider validates AptxAdaptiveConfiguration correctly but
  // advertises the corresponding capability with a malformed wire CodecType.
  // Only use the provider as the final validator when that exact capability
  // signature was observed during capability enumeration.
  if (codec_config.codecType == CodecType::APTX_ADAPTIVE &&
      qti_malformed_aptx_adaptive_capability) {
    log::warn("Using QTI malformed-capability compatibility path for aptX Adaptive");
    return true;
  }
  log::info("software codec={}", codec_config.toString());
  return false;
}

bool A2dpSbcToHalConfig(const ahal_codec_configuration& config, CodecConfiguration* codec_config) {
  if (config.codec_config.codec_type != BTAV_A2DP_CODEC_INDEX_SOURCE_SBC &&
      config.codec_config.codec_type != BTAV_A2DP_CODEC_INDEX_SINK_SBC) {
    return false;
  }
  codec_config->codecType = CodecType::SBC;
  SbcConfiguration sbc_config = {};
  sbc_config.sampleRateHz = A2dpCodecToHalSampleRate(config.codec_config);
  if (sbc_config.sampleRateHz <= 0) {
    log::error("Unknown SBC sample_rate={}", config.codec_config.sample_rate);
    return false;
  }
  uint8_t channel_mode =
          config.codec_specific_information_elements[A2DP_SBC_IE_CH_MD_IDX] & A2DP_SBC_IE_CH_MD_MSK;
  switch (channel_mode) {
    case A2DP_SBC_IE_CH_MD_JOINT:
      sbc_config.channelMode = SbcChannelMode::JOINT_STEREO;
      break;
    case A2DP_SBC_IE_CH_MD_STEREO:
      sbc_config.channelMode = SbcChannelMode::STEREO;
      break;
    case A2DP_SBC_IE_CH_MD_DUAL:
      sbc_config.channelMode = SbcChannelMode::DUAL;
      break;
    case A2DP_SBC_IE_CH_MD_MONO:
      sbc_config.channelMode = SbcChannelMode::MONO;
      break;
    default:
      log::error("Unknown SBC channel_mode={}", channel_mode);
      sbc_config.channelMode = SbcChannelMode::UNKNOWN;
      return false;
  }
  uint8_t block_length = config.codec_specific_information_elements[A2DP_SBC_IE_BLOCKS_IDX] &
                         A2DP_SBC_IE_BLOCKS_MSK;
  switch (block_length) {
    case A2DP_SBC_IE_BLOCKS_4:
      sbc_config.blockLength = 4;
      break;
    case A2DP_SBC_IE_BLOCKS_8:
      sbc_config.blockLength = 8;
      break;
    case A2DP_SBC_IE_BLOCKS_12:
      sbc_config.blockLength = 12;
      break;
    case A2DP_SBC_IE_BLOCKS_16:
      sbc_config.blockLength = 16;
      break;
    default:
      log::error("Unknown SBC block_length={}", block_length);
      return false;
  }
  uint8_t sub_bands = config.codec_specific_information_elements[A2DP_SBC_IE_SUBBAND_IDX] &
                      A2DP_SBC_IE_SUBBAND_MSK;
  switch (sub_bands) {
    case A2DP_SBC_IE_SUBBAND_4:
      sbc_config.numSubbands = 4;
      break;
    case A2DP_SBC_IE_SUBBAND_8:
      sbc_config.numSubbands = 8;
      break;
    default:
      log::error("Unknown SBC Subbands={}", sub_bands);
      return false;
  }
  uint8_t alloc_method = config.codec_specific_information_elements[A2DP_SBC_IE_ALLOC_MD_IDX] &
                         A2DP_SBC_IE_ALLOC_MD_MSK;
  switch (alloc_method) {
    case A2DP_SBC_IE_ALLOC_MD_S:
      sbc_config.allocMethod = SbcAllocMethod::ALLOC_MD_S;
      break;
    case A2DP_SBC_IE_ALLOC_MD_L:
      sbc_config.allocMethod = SbcAllocMethod::ALLOC_MD_L;
      break;
    default:
      log::error("Unknown SBC alloc_method={}", alloc_method);
      return false;
  }
  sbc_config.minBitpool = config.codec_specific_information_elements[A2DP_SBC_IE_MIN_BITPOOL_IDX];
  sbc_config.maxBitpool = config.codec_specific_information_elements[A2DP_SBC_IE_MAX_BITPOOL_IDX];
  sbc_config.bitsPerSample = A2dpCodecToHalBitsPerSample(config.codec_config);
  if (sbc_config.bitsPerSample <= 0) {
    log::error("Unknown SBC bits_per_sample={}", config.codec_config.bits_per_sample);
    return false;
  }
  codec_config->config.set<CodecConfiguration::CodecSpecific::sbcConfig>(sbc_config);
  return true;
}

bool A2dpAacToHalConfig(const ahal_codec_configuration& config, CodecConfiguration* codec_config) {
  if (config.codec_config.codec_type != BTAV_A2DP_CODEC_INDEX_SOURCE_AAC &&
      config.codec_config.codec_type != BTAV_A2DP_CODEC_INDEX_SINK_AAC) {
    return false;
  }
  codec_config->codecType = CodecType::AAC;
  AacConfiguration aac_config = {};
  uint8_t object_type = config.codec_specific_information_elements[A2DP_AAC_OBJECT_TYPE_IDX];
  switch (object_type) {
    case A2DP_AAC_OBJECT_TYPE_MPEG2_LC:
      aac_config.objectType = AacObjectType::MPEG2_LC;
      break;
    case A2DP_AAC_OBJECT_TYPE_MPEG4_LC:
      aac_config.objectType = AacObjectType::MPEG4_LC;
      break;
    case A2DP_AAC_OBJECT_TYPE_MPEG4_LTP:
      aac_config.objectType = AacObjectType::MPEG4_LTP;
      break;
    case A2DP_AAC_OBJECT_TYPE_MPEG4_SCALABLE:
      aac_config.objectType = AacObjectType::MPEG4_SCALABLE;
      break;
    default:
      log::error("Unknown AAC object_type={}", object_type);
      return false;
  }
  aac_config.sampleRateHz = A2dpCodecToHalSampleRate(config.codec_config);
  if (aac_config.sampleRateHz <= 0) {
    log::error("Unknown AAC sample_rate={}", config.codec_config.sample_rate);
    return false;
  }
  aac_config.channelMode = A2dpCodecToHalChannelMode(config.codec_config);
  if (aac_config.channelMode == ChannelMode::UNKNOWN) {
    log::error("Unknown AAC channel_mode={}", config.codec_config.channel_mode);
    return false;
  }
  uint8_t vbr_enabled = config.codec_specific_information_elements[A2DP_AAC_VARIABLE_BIT_RATE_IDX] &
                        A2DP_AAC_VARIABLE_BIT_RATE_MASK;
  switch (vbr_enabled) {
    case A2DP_AAC_VARIABLE_BIT_RATE_ENABLED:
      aac_config.variableBitRateEnabled = true;
      break;
    case A2DP_AAC_VARIABLE_BIT_RATE_DISABLED:
      aac_config.variableBitRateEnabled = false;
      break;
    default:
      log::error("Unknown AAC VBR={}", vbr_enabled);
      return false;
  }
  aac_config.bitsPerSample = A2dpCodecToHalBitsPerSample(config.codec_config);
  if (aac_config.bitsPerSample <= 0) {
    log::error("Unknown AAC bits_per_sample={}", config.codec_config.bits_per_sample);
    return false;
  }
  codec_config->config.set<CodecConfiguration::CodecSpecific::aacConfig>(aac_config);
  return true;
}

bool A2dpAptxToHalConfig(const ahal_codec_configuration& config, CodecConfiguration* codec_config) {
  if (config.codec_config.codec_type != BTAV_A2DP_CODEC_INDEX_SOURCE_APTX &&
      config.codec_config.codec_type != BTAV_A2DP_CODEC_INDEX_SOURCE_APTX_HD) {
    return false;
  }
  if (config.codec_config.codec_type == BTAV_A2DP_CODEC_INDEX_SOURCE_APTX) {
    codec_config->codecType = CodecType::APTX;
  } else {
    codec_config->codecType = CodecType::APTX_HD;
  }
  AptxConfiguration aptx_config = {};
  aptx_config.sampleRateHz = A2dpCodecToHalSampleRate(config.codec_config);
  if (aptx_config.sampleRateHz <= 0) {
    log::error("Unknown aptX sample_rate={}", config.codec_config.sample_rate);
    return false;
  }
  aptx_config.channelMode = A2dpCodecToHalChannelMode(config.codec_config);
  if (aptx_config.channelMode == ChannelMode::UNKNOWN) {
    log::error("Unknown aptX channel_mode={}", config.codec_config.channel_mode);
    return false;
  }
  aptx_config.bitsPerSample = A2dpCodecToHalBitsPerSample(config.codec_config);
  if (aptx_config.bitsPerSample <= 0) {
    log::error("Unknown aptX bits_per_sample={}", config.codec_config.bits_per_sample);
    return false;
  }
  codec_config->config.set<CodecConfiguration::CodecSpecific::aptxConfig>(aptx_config);
  return true;
}

AptxAdaptiveChannelMode A2dpCodecToHalAptxAdaptiveChannelMode(
        const btav_a2dp_codec_config_t& codec_config) {
  switch (codec_config.channel_mode) {
    case BTAV_A2DP_CODEC_CHANNEL_MODE_MONO:
      return AptxAdaptiveChannelMode::MONO;
    case BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO:
      return AptxAdaptiveChannelMode::JOINT_STEREO;
    default:
      return AptxAdaptiveChannelMode::UNKNOWN;
  }
}

bool A2dpAptxAdaptiveToHalConfig(const ahal_codec_configuration& config,
                                 CodecConfiguration* codec_config) {
  if (config.codec_config.codec_type != BTAV_A2DP_CODEC_INDEX_SOURCE_APTX_ADAPTIVE) {
    return false;
  }

  AptxAdaptiveConfiguration adaptive_config = {};
  adaptive_config.sampleRateHz = A2dpCodecToHalSampleRate(config.codec_config);
  adaptive_config.channelMode =
          A2dpCodecToHalAptxAdaptiveChannelMode(config.codec_config);
  adaptive_config.bitsPerSample = A2dpCodecToHalBitsPerSample(config.codec_config);
  if (adaptive_config.sampleRateHz <= 0 ||
      adaptive_config.channelMode == AptxAdaptiveChannelMode::UNKNOWN ||
      adaptive_config.bitsPerSample <= 0) {
    log::error("Invalid aptX Adaptive PCM config rate={} mode={} bits={}",
               adaptive_config.sampleRateHz,
               static_cast<int32_t>(adaptive_config.channelMode),
               adaptive_config.bitsPerSample);
    return false;
  }

  tA2DP_APTX_ADAPTIVE_CIE adaptive_cie = {};
  if (!A2DP_GetAptxAdaptiveCIE(config.codec_specific_information_elements, &adaptive_cie)) {
    log::error("Unable to read aptX Adaptive OTA codec configuration");
    return false;
  }

  // The legacy Qualcomm provider expects the standard Adaptive stream
  // metadata in addition to the PCM fields.  It owns the encoder; the stack
  // only forwards this negotiated information.  Keep the negotiated TTP
  // values rather than rebuilding the local defaults: R2.2 peers use these
  // bytes when selecting the lossless profile.
  adaptive_config.aptxMode = static_cast<AptxMode>(btif_av_get_aptx_mode_info());
  adaptive_config.sinkBufferingMs = {20, 50, 20, 50, 20, 50};
  adaptive_config.ttp = {static_cast<int8_t>(adaptive_cie.aptx_data.ttp_ll_0 - 128),
                          static_cast<int8_t>(adaptive_cie.aptx_data.ttp_ll_1 - 128),
                          static_cast<int8_t>(adaptive_cie.aptx_data.ttp_hq_0 - 128),
                          static_cast<int8_t>(adaptive_cie.aptx_data.ttp_hq_1 - 128),
                          static_cast<int8_t>(adaptive_cie.aptx_data.ttp_tws_0 - 128),
                          static_cast<int8_t>(adaptive_cie.aptx_data.ttp_tws_1 - 128)};
  adaptive_config.inputMode = AptxAdaptiveInputMode::STEREO;
  adaptive_config.inputFadeDurationMs = 0xff;

  log::info("aptX Adaptive HAL config rate={} bits={} features=0x{:08x} ext_ver=0x{:02x}",
            adaptive_config.sampleRateHz, adaptive_config.bitsPerSample,
            adaptive_cie.aptx_data.aptx_adaptive_sup_features,
            adaptive_cie.aptx_data.cap_ext_ver_num);
  adaptive_config.aptxAdaptiveConfigStream = {
          adaptive_cie.aptx_data.cap_ext_ver_num,
          static_cast<uint8_t>(adaptive_cie.aptx_data.aptx_adaptive_sup_features & 0xff),
          static_cast<uint8_t>((adaptive_cie.aptx_data.aptx_adaptive_sup_features >> 8) & 0xff),
          static_cast<uint8_t>((adaptive_cie.aptx_data.aptx_adaptive_sup_features >> 16) & 0xff),
          static_cast<uint8_t>((adaptive_cie.aptx_data.aptx_adaptive_sup_features >> 24) & 0xff),
          adaptive_cie.aptx_data.first_setup_pref,
          adaptive_cie.aptx_data.second_setup_pref,
          adaptive_cie.aptx_data.third_setup_pref,
          adaptive_cie.aptx_data.fourth_setup_pref,
          adaptive_cie.aptx_data.eoc0,
          adaptive_cie.aptx_data.eoc1};

  codec_config->codecType = CodecType::APTX_ADAPTIVE;
  codec_config->config.set<CodecConfiguration::CodecSpecific::aptxAdaptiveConfig>(adaptive_config);
  return true;
}

bool A2dpLdacToHalConfig(const ahal_codec_configuration& config, CodecConfiguration* codec_config) {
  if (config.codec_config.codec_type != BTAV_A2DP_CODEC_INDEX_SOURCE_LDAC) {
    return false;
  }
  codec_config->codecType = CodecType::LDAC;
  LdacConfiguration ldac_config = {};
  ldac_config.sampleRateHz = A2dpCodecToHalSampleRate(config.codec_config);
  if (ldac_config.sampleRateHz <= 0) {
    log::error("Unknown LDAC sample_rate={}", config.codec_config.sample_rate);
    return false;
  }
  int channel_mode = config.codec_specific_information_elements[A2DP_LDAC_CHANNEL_MODE_IDX];
  switch (channel_mode) {
    case A2DP_LDAC_CHANNEL_MODE_STEREO:
      ldac_config.channelMode = LdacChannelMode::STEREO;
      break;
    case A2DP_LDAC_CHANNEL_MODE_DUAL:
      ldac_config.channelMode = LdacChannelMode::DUAL;
      break;
    case A2DP_LDAC_CHANNEL_MODE_MONO:
      ldac_config.channelMode = LdacChannelMode::MONO;
      break;
    default:
      log::error("Unknown LDAC channel_mode={}", channel_mode);
      ldac_config.channelMode = LdacChannelMode::UNKNOWN;
      return false;
  }
  int ldac_quality = A2DP_LDAC_QUALITY_ABR_OFFLOAD;
  if (config.codec_config.codec_specific_1 != 0) {
    switch (config.codec_config.codec_specific_1 % 10) {
      case 0:
        ldac_quality = A2DP_LDAC_QUALITY_HIGH;
        break;
      case 1:
        ldac_quality = A2DP_LDAC_QUALITY_MID;
        break;
      case 2:
        ldac_quality = A2DP_LDAC_QUALITY_LOW;
        break;
      default:
        ldac_quality = A2DP_LDAC_QUALITY_ABR_OFFLOAD;
        break;
    }
  }
  switch (ldac_quality) {
    case A2DP_LDAC_QUALITY_HIGH:
      ldac_config.qualityIndex = LdacQualityIndex::HIGH;
      break;
    case A2DP_LDAC_QUALITY_MID:
      ldac_config.qualityIndex = LdacQualityIndex::MID;
      break;
    case A2DP_LDAC_QUALITY_LOW:
      ldac_config.qualityIndex = LdacQualityIndex::LOW;
      break;
    case A2DP_LDAC_QUALITY_ABR_OFFLOAD:
      ldac_config.qualityIndex = LdacQualityIndex::ABR;
      break;
    default:
      log::error("Unknown LDAC QualityIndex={}", ldac_quality);
      return false;
  }
  ldac_config.bitsPerSample = A2dpCodecToHalBitsPerSample(config.codec_config);
  if (ldac_config.bitsPerSample <= 0) {
    log::error("Unknown LDAC bits_per_sample={}", config.codec_config.bits_per_sample);
    return false;
  }
  codec_config->config.set<CodecConfiguration::CodecSpecific::ldacConfig>(ldac_config);
  return true;
}

bool A2dpOpusToHalConfig(const ahal_codec_configuration& config, CodecConfiguration* codec_config) {
  if (config.codec_config.codec_type != BTAV_A2DP_CODEC_INDEX_SOURCE_OPUS) {
    codec_config = {};
    return false;
  }
  codec_config->codecType = CodecType::OPUS;
  OpusConfiguration opus_config = {};

  opus_config.pcmBitDepth = A2dpCodecToHalBitsPerSample(config.codec_config);
  if (opus_config.pcmBitDepth <= 0) {
    log::error("Unknown Opus bits_per_sample={}", config.codec_config.bits_per_sample);
    return false;
  }
  opus_config.samplingFrequencyHz = A2dpCodecToHalSampleRate(config.codec_config);
  if (opus_config.samplingFrequencyHz <= 0) {
    log::error("Unknown Opus sample_rate={}", config.codec_config.sample_rate);
    return false;
  }
  opus_config.channelMode = A2dpCodecToHalChannelMode(config.codec_config);
  if (opus_config.channelMode == ChannelMode::UNKNOWN) {
    log::error("Unknown Opus channel_mode={}", config.codec_config.channel_mode);
    return false;
  }

  opus_config.frameDurationUs = 20000;

  if (opus_config.channelMode == ChannelMode::STEREO) {
    opus_config.octetsPerFrame = 640;
  } else {
    opus_config.octetsPerFrame = 320;
  }

  codec_config->config.set<CodecConfiguration::CodecSpecific::opusConfig>(opus_config);
  return true;
}

std::optional<LhdcQualityIndex> A2dpCodecToHalLhdcQualityIndex(
        const btav_a2dp_codec_config_t& codec_config) {
  const uint8_t quality = static_cast<uint8_t>(codec_config.codec_specific_1 & 0xff);

  if (codec_config.codec_type == BTAV_A2DP_CODEC_INDEX_SOURCE_LHDCV5) {
    // Qualcomm's public QTI enum has no fixed 320-kbps or 1000-kbps V5
    // values. On onyx we extend the provider capability vector with raw 0
    // for fixed 320 kbps and raw 9 for fixed 1000 kbps. QTI8 remains the
    // stock ABR path.
    switch (quality) {
      case 8:
        return static_cast<LhdcQualityIndex>(9);  // HIGH1 / 1000 kbps
      case 7:
        return LhdcQualityIndex::QUALITY_HIGH;   // 900 kbps
      case 6:
        return LhdcQualityIndex::QUALITY_MID;    // 500 kbps
      case 5:
        return LhdcQualityIndex::QUALITY_LOW;    // 400 kbps
      case 9:
        return LhdcQualityIndex::QUALITY_LOW4;   // raw QTI8 / real ABR
      case 4:
        return static_cast<LhdcQualityIndex>(0); // raw QTI0 / fixed 320 kbps
      case 3:
        return LhdcQualityIndex::QUALITY_ABR;    // raw QTI4 / fixed 256 kbps
      case 0:
      case 1:
      case 2:
        // Preserve the existing low-mode compatibility mapping. QTI does not
        // expose distinct 64/128/192-kbps V5 quality enums.
        return static_cast<LhdcQualityIndex>(quality + 4);
      default:
        return std::nullopt;
    }
  }
  if (codec_config.codec_type == BTAV_A2DP_CODEC_INDEX_SOURCE_LHDCV2 && quality == 9) {
    return LhdcQualityIndex::QUALITY_ABR;
  }
  if (codec_config.codec_type == BTAV_A2DP_CODEC_INDEX_SOURCE_LHDCV3 && quality == 8) {
    return LhdcQualityIndex::QUALITY_ABR;
  }
  // The QTI extension has no distinct 1000-kbps quality enum. The frontend
  // already clamps this value for V2/V5, but map it to HIGH defensively.
  if ((codec_config.codec_type == BTAV_A2DP_CODEC_INDEX_SOURCE_LHDCV2 ||
       codec_config.codec_type == BTAV_A2DP_CODEC_INDEX_SOURCE_LHDCV5) &&
      quality == 8) {
    return LhdcQualityIndex::QUALITY_HIGH;
  }

  switch (quality) {
    case 7:
      return LhdcQualityIndex::QUALITY_HIGH;
    case 6:
      return LhdcQualityIndex::QUALITY_MID;
    case 5:
      return LhdcQualityIndex::QUALITY_LOW;
    case 4:
      return LhdcQualityIndex::QUALITY_LOW4;
    default:
      return std::nullopt;
  }
}

bool A2dpLhdcToHalConfig(const ahal_codec_configuration& config, CodecConfiguration* codec_config) {
  const auto codec_index = config.codec_config.codec_type;
  if (codec_index != BTAV_A2DP_CODEC_INDEX_SOURCE_LHDCV2 &&
      codec_index != BTAV_A2DP_CODEC_INDEX_SOURCE_LHDCV3 &&
      codec_index != BTAV_A2DP_CODEC_INDEX_SOURCE_LHDCV5) {
    return false;
  }

  LhdcConfiguration lhdc_config = {};
  lhdc_config.sampleRateHz = A2dpCodecToHalSampleRate(config.codec_config);
  if (lhdc_config.sampleRateHz <= 0) {
    log::error("Unknown LHDC sample_rate={}", config.codec_config.sample_rate);
    return false;
  }
  if (config.codec_config.channel_mode != BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO) {
    log::error("Unknown LHDC channel_mode={}", config.codec_config.channel_mode);
    return false;
  }
  lhdc_config.channelMode = LhdcChannelMode::STEREO;

  auto quality = A2dpCodecToHalLhdcQualityIndex(config.codec_config);
  if (!quality.has_value()) {
    log::warn("LHDC quality={} is not supported by QTI offload",
              config.codec_config.codec_specific_1 & 0xff);
    return false;
  }
  lhdc_config.qualityIndex = *quality;

  lhdc_config.bitsPerSample = A2dpCodecToHalBitsPerSample(config.codec_config);
  if (lhdc_config.bitsPerSample <= 0) {
    log::error("Unknown LHDC bits_per_sample={}", config.codec_config.bits_per_sample);
    return false;
  }

  const auto& codec_info = config.codec_specific_information_elements;
  const bool low_latency = (config.codec_config.codec_specific_2 & 0x1) != 0;
  const bool jas_enabled = (config.codec_config.codec_specific_3 & 0x1) != 0;
  lhdc_config.mode = low_latency ? LhdcMode::LL : LhdcMode::HQ;
  lhdc_config.jasEnable = jas_enabled;
  lhdc_config.xmsaEnable = false;

  if (codec_index == BTAV_A2DP_CODEC_INDEX_SOURCE_LHDCV5) {
    lhdc_config.version = LhdcVersion::LHDC_V5;
    // Xiaomi's A16 bridge leaves channel split unspecified for V5. The field
    // only describes the older V2/V3 TWS split modes.
    lhdc_config.channelSplitMode = LhdcChannelSplitMode::UNKNOWN;
    lhdc_config.maxTargetBitrate = static_cast<LhdcMaxTargetBitrate>(codec_info[kLhdcP7] & 0x30);
    lhdc_config.minTargetBitrate =
            static_cast<LhdcMinTargetBitrate>((codec_info[kLhdcP7] & 0xc0) >> 6);
    lhdc_config.llacEnable = false;
    lhdc_config.jasEnable = jas_enabled || (codec_info[kLhdcV5P9] & 0x02) != 0;
    lhdc_config.losslessEnable = false;
    if (!low_latency && (codec_info[kLhdcV5P9] & 0x40) != 0) {
      lhdc_config.mode = LhdcMode::LL;
    }
  } else {
    lhdc_config.version = LhdcVersion::LHDC_V2;
    if (codec_index == BTAV_A2DP_CODEC_INDEX_SOURCE_LHDCV3) {
      lhdc_config.version =
              ((codec_info[kLhdcP7] & 0x0f) == 0x02) ? LhdcVersion::LHDC_V4 : LhdcVersion::LHDC_V3;
    }

    const uint8_t channel_split = codec_info[kLhdcP8] & 0x0f;
    // A negotiated configuration should contain exactly one mode. Prefer
    // normal stereo defensively if an older peer/front-end supplied a
    // capability bitmask instead.
    if (channel_split & A2DP_LHDC_CH_SPLIT_NONE) {
      lhdc_config.channelSplitMode = LhdcChannelSplitMode::LHDC_CH_SPLIT_NONE;
    } else if (channel_split & A2DP_LHDC_CH_SPLIT_TWS_PLUS) {
      lhdc_config.channelSplitMode = LhdcChannelSplitMode::LHDC_CH_SPLIT_TWS_PLUS;
    } else if (channel_split & A2DP_LHDC_CH_SPLIT_TWS) {
      lhdc_config.channelSplitMode = LhdcChannelSplitMode::LHDC_CH_SPLIT_TWS;
    } else {
      log::error("Unknown LHDC channel_split={}", channel_split);
      return false;
    }
    lhdc_config.maxTargetBitrate = static_cast<LhdcMaxTargetBitrate>(codec_info[kLhdcP7] & 0x30);
    lhdc_config.minTargetBitrate =
            (codec_info[kLhdcP8] & 0x20) != 0
                    ? LhdcMinTargetBitrate::MinTargetBitrate_320kV4_128kV5
                    : LhdcMinTargetBitrate::MinTargetBitrate_defaultV4_noLimitV5;
    // codec_specific_3 stores peer/UI feature capability state and can retain
    // LLAC even when final negotiation resolves an LLAC+V4 peer to V4-only.
    // The negotiated OTA P7 bit is authoritative for the HAL configuration.
    lhdc_config.llacEnable = (codec_info[kLhdcP7] & 0x80) != 0;
    lhdc_config.jasEnable = jas_enabled || (codec_info[kLhdcP6] & 0x40) != 0;
    lhdc_config.losslessEnable = false;
    if (!low_latency && (codec_info[kLhdcP7] & 0x40) != 0) {
      lhdc_config.mode = LhdcMode::LL;
    }
  }

  classicVendorConfiguration classic_config = {};
  classic_config.vendorCodecType = VendorCodecType::LHDC;
  classic_config.config.set<classicVendorConfiguration::CodecSpecific::lhdcConfig>(lhdc_config);

  CodecConfiguration::VendorConfiguration vendor_config = {};
  vendor_config.vendorId = kQtiBluetoothAudioVendorId;
  vendor_config.codecId = kQtiLhdcCodecId;
  if (vendor_config.codecConfig.setParcelable(classic_config) != STATUS_OK) {
    log::error("Failed to serialize QTI LHDC configuration");
    return false;
  }

  codec_config->codecType = CodecType::VENDOR;
  codec_config->config.set<CodecConfiguration::CodecSpecific::vendorConfig>(vendor_config);
  return true;
}

}  // namespace

bool UpdateOffloadingCapabilities(
        const std::vector<btav_a2dp_codec_config_t>& framework_preference) {
  audio_hal_capabilities = BluetoothAudioClientInterface::GetAudioCapabilities(
          SessionType::A2DP_HARDWARE_OFFLOAD_ENCODING_DATAPATH);
  std::unordered_set<CodecType> codec_type_set;
  bool lhdc_enabled = false;
  for (auto preference : framework_preference) {
    switch (preference.codec_type) {
      case BTAV_A2DP_CODEC_INDEX_SOURCE_SBC:
        codec_type_set.insert(CodecType::SBC);
        break;
      case BTAV_A2DP_CODEC_INDEX_SOURCE_AAC:
        codec_type_set.insert(CodecType::AAC);
        break;
      case BTAV_A2DP_CODEC_INDEX_SOURCE_APTX:
        codec_type_set.insert(CodecType::APTX);
        break;
      case BTAV_A2DP_CODEC_INDEX_SOURCE_APTX_HD:
        codec_type_set.insert(CodecType::APTX_HD);
        break;
      case BTAV_A2DP_CODEC_INDEX_SOURCE_APTX_ADAPTIVE:
        codec_type_set.insert(CodecType::APTX_ADAPTIVE);
        break;
      case BTAV_A2DP_CODEC_INDEX_SOURCE_LDAC:
        codec_type_set.insert(CodecType::LDAC);
        break;
      case BTAV_A2DP_CODEC_INDEX_SOURCE_LC3:
        log::warn("Ignore source codec_type={}, not implemented", preference.codec_type);
        break;
      case BTAV_A2DP_CODEC_INDEX_SOURCE_OPUS:
        codec_type_set.insert(CodecType::OPUS);
        break;
      case BTAV_A2DP_CODEC_INDEX_SOURCE_LHDCV2:
      case BTAV_A2DP_CODEC_INDEX_SOURCE_LHDCV3:
      case BTAV_A2DP_CODEC_INDEX_SOURCE_LHDCV5:
        lhdc_enabled = true;
        break;
      case BTAV_A2DP_CODEC_INDEX_SINK_SBC:
      case BTAV_A2DP_CODEC_INDEX_SINK_AAC:
      case BTAV_A2DP_CODEC_INDEX_SINK_OPUS:
        log::warn("Ignore sink codec_type={}", preference.codec_type);
        break;
      case BTAV_A2DP_CODEC_INDEX_MAX:
      default:
        log::error("Unknown codec_type={}", preference.codec_type);
        break;
    }
  }
  offloading_preference.clear();
  qti_malformed_aptx_adaptive_capability = false;
  for (auto capability : audio_hal_capabilities) {
    const auto& a2dp_capability = capability.get<AudioCapabilities::a2dpCapabilities>();
    auto codec_type = a2dp_capability.codecType;

    if (static_cast<int32_t>(codec_type) == kQtiMalformedAptxAdaptiveCodecType &&
        a2dp_capability.capabilities.getTag() ==
                CodecCapabilities::Capabilities::sbcCapabilities) {
      const auto& sbc = a2dp_capability.capabilities
                                .get<CodecCapabilities::Capabilities::sbcCapabilities>();
      const bool empty_default_sbc =
              sbc.sampleRateHz.empty() && sbc.channelMode.empty() && sbc.blockLength.empty() &&
              sbc.numSubbands.empty() && sbc.allocMethod.empty() && sbc.bitsPerSample.empty() &&
              sbc.minBitpool == 0 && sbc.maxBitpool == 0;
      if (empty_default_sbc) {
        qti_malformed_aptx_adaptive_capability = true;
        log::warn("Detected QTI malformed aptX Adaptive offload capability: codecType=0x{:x}",
                  static_cast<int32_t>(codec_type));
      }
    }

    const bool enable_lhdc =
            codec_type == CodecType::VENDOR && lhdc_enabled &&
            GetLhdcCapabilities(a2dp_capability).has_value();
    if (enable_lhdc || codec_type_set.find(codec_type) != codec_type_set.end()) {
      log::info("enabled offloading capability={}", capability.toString());
      offloading_preference.push_back(capability);
    } else {
      log::info("disabled offloading capability={}", capability.toString());
    }
  }

  // TODO: Bluetooth SoC and runtime property
  return true;
}

bool getHalPcmConfiguration(const ahal_codec_configuration& config,
                            PcmConfiguration* pcm_configuration) {
  if (pcm_configuration == nullptr) {
    return false;
  }

  pcm_configuration->sampleRateHz = A2dpCodecToHalSampleRate(config.codec_config);
  pcm_configuration->bitsPerSample = A2dpCodecToHalBitsPerSample(config.codec_config);
  pcm_configuration->channelMode = A2dpCodecToHalChannelMode(config.codec_config);
  pcm_configuration->dataIntervalUs = config.preferred_encoding_interval_us;

  return pcm_configuration->sampleRateHz > 0 && pcm_configuration->bitsPerSample > 0 &&
         pcm_configuration->channelMode != ChannelMode::UNKNOWN;
}

bool getHalCodecConfiguration(const ahal_codec_configuration& config,
                              CodecConfiguration* codec_configuration) {
  if (codec_configuration == nullptr) {
    return false;
  }

  codec_configuration->encodedAudioBitrate = config.codec_bitrate;
  codec_configuration->peerMtu = config.peer_mtu;

  switch (config.codec_config.codec_type) {
    case BTAV_A2DP_CODEC_INDEX_SOURCE_SBC:
    case BTAV_A2DP_CODEC_INDEX_SINK_SBC:
      return A2dpSbcToHalConfig(config, codec_configuration) &&
             IsCodecOffloadingEnabled(*codec_configuration);

    case BTAV_A2DP_CODEC_INDEX_SOURCE_AAC:
    case BTAV_A2DP_CODEC_INDEX_SINK_AAC:
      return A2dpAacToHalConfig(config, codec_configuration) &&
             IsCodecOffloadingEnabled(*codec_configuration);

    case BTAV_A2DP_CODEC_INDEX_SOURCE_APTX:
    case BTAV_A2DP_CODEC_INDEX_SOURCE_APTX_HD:
      return A2dpAptxToHalConfig(config, codec_configuration) &&
             IsCodecOffloadingEnabled(*codec_configuration);

    case BTAV_A2DP_CODEC_INDEX_SOURCE_APTX_ADAPTIVE:
      return A2dpAptxAdaptiveToHalConfig(config, codec_configuration) &&
             IsCodecOffloadingEnabled(*codec_configuration);

    case BTAV_A2DP_CODEC_INDEX_SOURCE_LDAC:
      return A2dpLdacToHalConfig(config, codec_configuration) &&
             IsCodecOffloadingEnabled(*codec_configuration);

    case BTAV_A2DP_CODEC_INDEX_SOURCE_LHDCV2:
    case BTAV_A2DP_CODEC_INDEX_SOURCE_LHDCV3:
    case BTAV_A2DP_CODEC_INDEX_SOURCE_LHDCV5:
      return A2dpLhdcToHalConfig(config, codec_configuration) &&
             IsCodecOffloadingEnabled(*codec_configuration);

    case BTAV_A2DP_CODEC_INDEX_SOURCE_OPUS:
      return A2dpOpusToHalConfig(config, codec_configuration) &&
             IsCodecOffloadingEnabled(*codec_configuration);

    case BTAV_A2DP_CODEC_INDEX_MAX:
    default:
      break;
  }

  return false;
}

}  // namespace bluetooth::audio::aidl::a2dp::codec
