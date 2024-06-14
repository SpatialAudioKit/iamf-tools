/*
 * Copyright (c) 2023, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 3-Clause Clear License
 * and the Alliance for Open Media Patent License 1.0. If the BSD 3-Clause Clear
 * License was not distributed with this source code in the LICENSE file, you
 * can obtain it at www.aomedia.org/license/software-license/bsd-3-c-c. If the
 * Alliance for Open Media Patent License 1.0 was not distributed with this
 * source code in the PATENTS file, you can obtain it at
 * www.aomedia.org/license/patent.
 */
#include "iamf/cli/codec/opus_decoder.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "iamf/cli/codec/decoder_base.h"
#include "iamf/cli/codec/opus_utils.h"
#include "iamf/cli/proto/codec_config.pb.h"
#include "iamf/common/macros.h"
#include "iamf/common/obu_util.h"
#include "iamf/obu/codec_config.h"
#include "iamf/obu/decoder_config/opus_decoder_config.h"
#include "include/opus.h"
#include "include/opus_types.h"

namespace iamf_tools {

namespace {

// Performs validation for values that this implementation assumes are
// restricted because they are restricted in IAMF V1.
absl::Status ValidateDecoderConfig(
    const OpusDecoderConfig& opus_decoder_config) {
  // Validate the input. Reject values that would need to be added to this
  // function if they were ever supported.
  if (opus_decoder_config.output_gain_ != 0 ||
      opus_decoder_config.mapping_family_ != 0) {
    LOG(ERROR) << "IAMF V1 expects output_gain: "
               << opus_decoder_config.output_gain_ << " and mapping_family: "
               << absl::StrCat(opus_decoder_config.mapping_family_)
               << " to be 0.";
    return absl::InvalidArgumentError("");
  }

  return absl::OkStatus();
}

}  // namespace

OpusDecoder::OpusDecoder(const CodecConfigObu& codec_config_obu,
                         int num_channels)
    : DecoderBase(num_channels,
                  static_cast<int>(codec_config_obu.GetNumSamplesPerFrame())),
      opus_decoder_config_(std::get<OpusDecoderConfig>(
          codec_config_obu.GetCodecConfig().decoder_config)),
      output_sample_rate_(codec_config_obu.GetOutputSampleRate()) {}

OpusDecoder::~OpusDecoder() {
  if (decoder_ != nullptr) {
    opus_decoder_destroy(decoder_);
  }
}

absl::Status OpusDecoder::Initialize() {
  RETURN_IF_NOT_OK(ValidateDecoderConfig(opus_decoder_config_));

  // Initialize the decoder.
  int opus_error_code;
  decoder_ = opus_decoder_create(static_cast<opus_int32>(output_sample_rate_),
                                 num_channels_, &opus_error_code);
  RETURN_IF_NOT_OK(OpusErrorCodeToAbslStatus(
      opus_error_code, "Failed to initialize Opus decoder."));

  return absl::OkStatus();
}

absl::Status OpusDecoder::DecodeAudioFrame(
    const std::vector<uint8_t>& encoded_frame,
    std::vector<std::vector<int32_t>>& decoded_samples) {
  // `opus_decode_float` decodes to `float` samples with channels interlaced.
  // Typically these values are in the range of [-1, +1] (always for
  // `iamf_tools`-encoded data). Values outside of that range will be clipped in
  // `NormalizedFloatToInt32`.
  std::vector<float> output_pcm_float;
  output_pcm_float.resize(num_samples_per_channel_ * num_channels_);

  // Transform the data and feed it to the decoder.
  std::vector<unsigned char> input_data(encoded_frame.size());
  std::transform(encoded_frame.begin(), encoded_frame.end(), input_data.begin(),
                 [](uint8_t c) { return static_cast<unsigned char>(c); });

  const int num_output_samples = opus_decode_float(
      decoder_, input_data.data(), static_cast<opus_int32>(input_data.size()),
      output_pcm_float.data(),
      /*frame_size=*/num_samples_per_channel_,
      /*decode_fec=*/0);
  if (num_output_samples < 0) {
    // When `num_output_samples` is negative, it is a non-OK Opus error code.
    return OpusErrorCodeToAbslStatus(num_output_samples,
                                     "Failed to decode Opus frame.");
  }
  output_pcm_float.resize(num_output_samples * num_channels_);
  LOG_FIRST_N(INFO, 3) << "Opus decoded " << num_output_samples
                       << " samples per channel. With " << num_channels_
                       << " channels.";
  // Convert data to channels arranged in (time, channel) axes. There can only
  // be one or two channels.
  decoded_samples.reserve(decoded_samples.size() +
                          output_pcm_float.size() / num_channels_);
  for (int i = 0; i < output_pcm_float.size(); i += num_channels_) {
    std::vector<int32_t> time_sample(num_channels_, 0);
    // Grab samples in all channels associated with this time instant.
    for (int j = 0; j < num_channels_; ++j) {
      RETURN_IF_NOT_OK(
          NormalizedFloatToInt32(output_pcm_float[i + j], time_sample[j]));
    }
    decoded_samples.push_back(time_sample);
  }

  return absl::OkStatus();
}

}  // namespace iamf_tools
