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

#include "iamf/cli/wav_writer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/functional/any_invocable.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "iamf/common/macros.h"
#include "iamf/common/obu_util.h"
#include "src/dsp/write_wav_file.h"

namespace iamf_tools {

namespace {
// Some audio to tactile functions return 0 on success and 1 on failure.
constexpr int kAudioToTactileResultFailure = 0;
constexpr int kAudioToTactileResultSuccess = 1;

// Write samples for all channels.
absl::Status WriteSamplesInternal(absl::Nullable<FILE*> file,
                                  size_t num_channels, int bit_depth,
                                  const std::vector<uint8_t>& buffer,
                                  size_t& total_samples_accumulator) {
  if (file == nullptr) {
    // Wav writer may have been aborted.
    return absl::FailedPreconditionError(
        "Wav writer is not accepting samples.");
  }

  const auto buffer_size = buffer.size();

  if (buffer_size == 0) {
    // Nothing to write.
    return absl::OkStatus();
  }

  if (buffer_size % (bit_depth * num_channels / 8) != 0) {
    return absl::InvalidArgumentError(
        "Must write an integer number of samples.");
  }

  // Calculate how many samples there are.
  const int bytes_per_sample = bit_depth / 8;
  const size_t num_total_samples = (buffer_size) / bytes_per_sample;

  int write_sample_result = kAudioToTactileResultFailure;
  if (bit_depth == 16) {
    // Arrange the input samples into an int16_t to match the expected input of
    // `WriteWavSamples`.
    std::vector<int16_t> samples(num_total_samples, 0);
    for (int i = 0; i < num_total_samples * bytes_per_sample;
         i += bytes_per_sample) {
      samples[i / bytes_per_sample] = (buffer[i + 1] << 8) | buffer[i];
    }

    write_sample_result = WriteWavSamples(file, samples.data(), samples.size());
  } else if (bit_depth == 24) {
    // Arrange the input samples into an int32_t to match the expected input of
    // `WriteWavSamples24Bit` with the lowest byte unused.
    std::vector<int32_t> samples(num_total_samples, 0);
    for (int i = 0; i < num_total_samples * bytes_per_sample;
         i += bytes_per_sample) {
      samples[i / bytes_per_sample] =
          (buffer[i + 2] << 24) | buffer[i + 1] << 16 | buffer[i] << 8;
    }
    write_sample_result =
        WriteWavSamples24Bit(file, samples.data(), samples.size());
  } else if (bit_depth == 32) {
    // Arrange the input samples into an int32_t to match the expected input of
    // `WriteWavSamples32Bit`.
    std::vector<int32_t> samples(num_total_samples, 0);
    for (int i = 0; i < num_total_samples * bytes_per_sample;
         i += bytes_per_sample) {
      samples[i / bytes_per_sample] = buffer[i + 3] << 24 |
                                      buffer[i + 2] << 16 | buffer[i + 1] << 8 |
                                      buffer[i];
    }
    write_sample_result =
        WriteWavSamples32Bit(file, samples.data(), samples.size());
  } else {
    // This should never happen because the factory method would never create
    // an object with disallowed `bit_depth_` values.
    LOG(FATAL) << "WavWriter only supports 16, 24, and 32-bit samples; got "
               << bit_depth;
  }

  if (write_sample_result == kAudioToTactileResultSuccess) {
    total_samples_accumulator += num_total_samples;
    return absl::OkStatus();
  }

  // It's not clear why this would happen.
  return absl::UnknownError(
      absl::StrCat("Error writing samples to wav file. write_sample_result= ",
                   write_sample_result));
}

}  // namespace

std::unique_ptr<WavWriter> WavWriter::Create(const std::string& wav_filename,
                                             int num_channels,
                                             int sample_rate_hz, int bit_depth,
                                             bool write_header) {
  // Open the file to write to.
  LOG(INFO) << "Writer \"" << wav_filename << "\"";
  auto* file = std::fopen(wav_filename.c_str(), "wb");
  if (file == nullptr) {
    LOG(ERROR).WithPerror() << "Error opening file \"" << wav_filename << "\"";
    return nullptr;
  }

  // Write a dummy header. This will be overwritten in the destructor.
  WavHeaderWriter wav_header_writer;
  switch (bit_depth) {
    case 16:
      wav_header_writer = WriteWavHeader;
      break;
    case 24:
      wav_header_writer = WriteWavHeader24Bit;
      break;
    case 32:
      wav_header_writer = WriteWavHeader32Bit;
      break;
    default:
      LOG(WARNING) << "This implementation does not support writing "
                   << bit_depth << "-bit wav files.";
      std::fclose(file);
      std::remove(wav_filename.c_str());
      return nullptr;
  }

  // Set to an empty writer. The emptiness can be checked to skip writing the
  // header.
  if (!write_header) {
    wav_header_writer = WavHeaderWriter();
  } else if (wav_header_writer(file, 0, sample_rate_hz, num_channels) ==
             kAudioToTactileResultFailure) {
    LOG(ERROR) << "Error writing header of file \"" << wav_filename << "\"";
    return nullptr;
  }

  return absl::WrapUnique(new WavWriter(wav_filename, num_channels,
                                        sample_rate_hz, bit_depth, file,
                                        std::move(wav_header_writer)));
}

WavWriter::~WavWriter() {
  if (file_ == nullptr) {
    return;
  }

  // Finalize the temporary header based on the total number of samples written
  // and close the file.
  if (wav_header_writer_) {
    std::fseek(file_, 0, SEEK_SET);
    wav_header_writer_(file_, total_samples_written_, sample_rate_hz_,
                       num_channels_);
  }
  std::fclose(file_);
}

absl::Status WavWriter::PushFrame(
    absl::Span<const std::vector<int32_t>> time_channel_samples) {
  // Flatten down the serialized PCM for compatibility with the internal
  // `WriteSamplesInternal` function.
  const size_t num_ticks = time_channel_samples.size();
  const size_t num_channels =
      time_channel_samples.empty() ? 0 : time_channel_samples[0].size();
  if (!std::all_of(
          time_channel_samples.begin(), time_channel_samples.end(),
          [&](const auto& tick) { return tick.size() == num_channels; })) {
    return absl::InvalidArgumentError(
        "All ticks must have the same number of channels.");
  }

  std::vector<uint8_t> samples_as_pcm(num_channels * num_ticks * bit_depth_ / 8,
                                      0);
  int write_position = 0;
  for (const auto& tick : time_channel_samples) {
    for (const auto& channel_sample : tick) {
      RETURN_IF_NOT_OK(WritePcmSample(channel_sample, bit_depth_,
                                      /*big_endian=*/false,
                                      samples_as_pcm.data(), write_position));
    }
  }

  return WriteSamplesInternal(file_, num_channels_, bit_depth_, samples_as_pcm,
                              total_samples_written_);
}

absl::Status WavWriter::WritePcmSamples(const std::vector<uint8_t>& buffer) {
  return WriteSamplesInternal(file_, num_channels_, bit_depth_, buffer,
                              total_samples_written_);
}

void WavWriter::Abort() {
  std::fclose(file_);
  std::remove(filename_to_remove_.c_str());
  file_ = nullptr;
}

WavWriter::WavWriter(const std::string& filename_to_remove, int num_channels,
                     int sample_rate_hz, int bit_depth, FILE* file,
                     WavHeaderWriter wav_header_writer)
    : num_channels_(num_channels),
      sample_rate_hz_(sample_rate_hz),
      bit_depth_(bit_depth),
      total_samples_written_(0),
      file_(file),
      filename_to_remove_(filename_to_remove),
      wav_header_writer_(std::move(wav_header_writer)) {}

}  // namespace iamf_tools
