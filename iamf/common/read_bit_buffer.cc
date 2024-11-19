/*
 * Copyright (c) 2024, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 3-Clause Clear License
 * and the Alliance for Open Media Patent License 1.0. If the BSD 3-Clause Clear
 * License was not distributed with this source code in the LICENSE file, you
 * can obtain it at www.aomedia.org/license/software-license/bsd-3-c-c. If the
 * Alliance for Open Media Patent License 1.0 was not distributed with this
 * source code in the PATENTS file, you can obtain it at
 * www.aomedia.org/license/patent.
 */

#include "iamf/common/read_bit_buffer.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "iamf/common/bit_buffer_util.h"
#include "iamf/common/macros.h"
#include "iamf/obu/types.h"

namespace iamf_tools {

namespace {

bool ShouldRead(const int64_t& source_offset,
                const std::vector<uint8_t>& source,
                const int32_t& remaining_bits_to_read) {
  const bool valid_bit_offset = (source_offset / 8) < source.size();
  const bool bits_to_read = remaining_bits_to_read > 0;
  return valid_bit_offset && bits_to_read;
}

bool CanReadByteAligned(const int64_t& buffer_bit_offset,
                        const int32_t& num_bits) {
  const bool buffer_bit_offset_is_aligned = (buffer_bit_offset % 8 == 0);
  const bool num_bits_to_read_is_aligned = (num_bits % 8 == 0);
  return buffer_bit_offset_is_aligned && num_bits_to_read_is_aligned;
}

// Reads one bit from source_data at position `offset`. Reads in order of most
// significant to least significant - that is, offset = 0 refers to the bit in
// position 2^7, offset = 1 refers to the bit in position 2^6, etc. Caller
// should ensure that offset/8 is < data.size().
uint8_t GetUpperBit(const int64_t& offset,
                    const std::vector<uint8_t>& source_data) {
  int64_t byteIndex = offset / 8;
  uint8_t bitIndex = 7 - (offset % 8);
  return (source_data.at(byteIndex) >> bitIndex) & 0x01;
}

// Read unsigned literal bit by bit. Data is read into the lower
// `remaining_bits_to_read` of `output` from the upper `remaining_bits_to_read`
// of bit_offer[buffer_bit_offset].
//
// Ex: Input: bit_buffer = 10000111, buffer_bit_offset = 0,
//        remaining_bits_to_read = 5, output = 0
//     Output: output = {59 leading zeroes} + 10000, buffer_bit_offset = 5,
//        remaining_bits_to_read = 0.
void ReadUnsignedLiteralBits(int64_t& buffer_bit_offset,
                             const std::vector<uint8_t>& bit_buffer,
                             const int64_t& buffer_size,
                             int& remaining_bits_to_read, uint64_t& output) {
  while (((buffer_bit_offset / 8) < bit_buffer.size()) &&
         remaining_bits_to_read > 0 && (buffer_bit_offset < buffer_size)) {
    uint8_t upper_bit = GetUpperBit(buffer_bit_offset, bit_buffer);
    output |= (uint64_t)(upper_bit) << (remaining_bits_to_read - 1);
    remaining_bits_to_read--;
    buffer_bit_offset++;
  }
}

// Read unsigned literal byte by byte.
void ReadUnsignedLiteralBytes(int64_t& buffer_bit_offset,
                              const std::vector<uint8_t>& bit_buffer,
                              int& remaining_bits_to_read, uint64_t& output) {
  while (((buffer_bit_offset / 8) < bit_buffer.size()) &&
         remaining_bits_to_read > 0) {
    output = output << 8;
    output |= (uint64_t)(bit_buffer.at(buffer_bit_offset / 8));
    remaining_bits_to_read -= 8;
    buffer_bit_offset += 8;
  }
}

typedef absl::AnyInvocable<void(uint64_t, int, uint64_t&) const>
    ByteAccumulator;

absl::Status AccumulateUleb128Byte(const ByteAccumulator& accumulator,
                                   uint32_t max_output, const uint64_t& byte,
                                   const int index, bool& is_terminal_block,
                                   uint64_t& accumulated_value) {
  accumulator(byte, index, accumulated_value);
  is_terminal_block = ((byte & 0x80) == 0);
  if ((index == (kMaxLeb128Size - 1)) && !is_terminal_block) {
    return absl::InvalidArgumentError(
        "Have read the max allowable bytes for a uleb128, but bitstream "
        "says to keep reading.");
  }
  if (accumulated_value > max_output) {
    return absl::InvalidArgumentError(
        absl::StrCat("Overflow - data is larger than max_output=", max_output));
  }
  return absl::OkStatus();
}

// Common internal function for reading uleb128 and iso14496_1 expanded. They
// have similar logic except the bytes are accumulated in different orders, and
// they have different max output values.
absl::Status AccumulateUleb128OrIso14496_1Internal(
    const ByteAccumulator& accumulator, const uint32_t max_output,
    ReadBitBuffer& rb, uint32_t& output, int8_t& encoded_size) {
  uint64_t accumulated_value = 0;
  uint64_t byte = 0;
  bool terminal_block = false;
  encoded_size = 0;
  for (int i = 0; i < kMaxLeb128Size; ++i) {
    RETURN_IF_NOT_OK(rb.ReadUnsignedLiteral(8, byte));
    encoded_size++;
    RETURN_IF_NOT_OK(AccumulateUleb128Byte(accumulator, max_output, byte, i,
                                           terminal_block, accumulated_value));

    if (terminal_block) {
      break;
    }
  }
  // Accumulated value is guaranteed to fit into a `uint32_t` at this
  // stage.
  output = static_cast<uint32_t>(accumulated_value);
  return absl::OkStatus();
}

}  // namespace

ReadBitBuffer::ReadBitBuffer(int64_t capacity, std::vector<uint8_t>* source)
    : source_(source) {
  bit_buffer_.reserve(capacity);
}

absl::Status ReadBitBuffer::ReadUnsignedLiteralInternal(const int num_bits,
                                                        const int max_num_bits,
                                                        uint64_t& output) {
  if (num_bits > max_num_bits) {
    return absl::InvalidArgumentError("num_bits must be <= max_num_bits.");
  }
  if (buffer_bit_offset_ < 0) {
    return absl::UnknownError("buffer_bit_offset_ must be >= 0.");
  }
  output = 0;
  int remaining_bits_to_read = num_bits;
  if (CanReadByteAligned(buffer_bit_offset_, num_bits)) {
    ReadUnsignedLiteralBytes(buffer_bit_offset_, bit_buffer_,
                             remaining_bits_to_read, output);
  } else {
    ReadUnsignedLiteralBits(buffer_bit_offset_, bit_buffer_, buffer_size_,
                            remaining_bits_to_read, output);
  }
  if (remaining_bits_to_read != 0) {
    RETURN_IF_NOT_OK(LoadBits(remaining_bits_to_read));
    // Guaranteed to have enough bits to read the unsigned literal at this
    // point.
    if (CanReadByteAligned(buffer_bit_offset_, num_bits)) {
      ReadUnsignedLiteralBytes(buffer_bit_offset_, bit_buffer_,
                               remaining_bits_to_read, output);
    } else {
      ReadUnsignedLiteralBits(buffer_bit_offset_, bit_buffer_, buffer_size_,
                              remaining_bits_to_read, output);
    }
  }
  return absl::OkStatus();
}

// Reads n = `num_bits` bits from the buffer. These are the upper n bits of
// `bit_buffer_`. n must be <= 64. The read data is consumed, meaning
// `buffer_bit_offset_` is incremented by n as a side effect of this fxn.
absl::Status ReadBitBuffer::ReadUnsignedLiteral(const int num_bits,
                                                uint64_t& output) {
  return ReadUnsignedLiteralInternal(num_bits, 64, output);
}

absl::Status ReadBitBuffer::ReadUnsignedLiteral(const int num_bits,
                                                uint32_t& output) {
  uint64_t value;
  RETURN_IF_NOT_OK(ReadUnsignedLiteralInternal(num_bits, 32, value));
  output = static_cast<uint32_t>(value);
  return absl::OkStatus();
}

absl::Status ReadBitBuffer::ReadUnsignedLiteral(const int num_bits,
                                                uint16_t& output) {
  uint64_t value;
  RETURN_IF_NOT_OK(ReadUnsignedLiteralInternal(num_bits, 16, value));
  output = static_cast<uint16_t>(value);
  return absl::OkStatus();
}

absl::Status ReadBitBuffer::ReadUnsignedLiteral(const int num_bits,
                                                uint8_t& output) {
  uint64_t value;
  RETURN_IF_NOT_OK(ReadUnsignedLiteralInternal(num_bits, 8, value));
  output = static_cast<uint8_t>(value);
  return absl::OkStatus();
}

// Reads a standard int16_t in two's complement form from the read buffer.
absl::Status ReadBitBuffer::ReadSigned16(int16_t& output) {
  uint64_t value;
  RETURN_IF_NOT_OK(ReadUnsignedLiteral(16, value));
  output = static_cast<int16_t>(value) & 0xffff;
  return absl::OkStatus();
}

// Reads a null terminated C-style string from the buffer.
absl::Status ReadBitBuffer::ReadString(std::string& output) {
  // Read up to the first `kIamfMaxStringSize` characters. Exit after seeing the
  // null terminator. Override anything in `output`.
  output = "";
  for (int i = 0; i < kIamfMaxStringSize; i++) {
    uint8_t byte;
    RETURN_IF_NOT_OK(ReadUnsignedLiteral(8, byte));
    if (byte == '\0') {
      return absl::OkStatus();
    }
    output.push_back(byte);
  }

  // Failed to find the null terminator within `kIamfMaxStringSize` bytes.
  return absl::InvalidArgumentError(
      "Failed to find the null terminator for data= ");
}

absl::Status ReadBitBuffer::ReadULeb128(DecodedUleb128& uleb128) {
  int8_t unused_size;
  return ReadULeb128(uleb128, unused_size);
}

absl::Status ReadBitBuffer::ReadULeb128(DecodedUleb128& uleb128,
                                        int8_t& encoded_uleb128_size) {
  static const ByteAccumulator little_endian_accumulator =
      [&](uint64_t byte, int index, uint64_t& accumulated_value) {
        accumulated_value |= (byte & 0x7f) << (7 * index);
      };
  // IAMF requires all `leb128`s to decode to a value that fits in 32 bits.
  const uint32_t kMaxUleb128 = std::numeric_limits<uint32_t>::max();
  return AccumulateUleb128OrIso14496_1Internal(little_endian_accumulator,
                                               kMaxUleb128, *this, uleb128,
                                               encoded_uleb128_size);
}

absl::Status ReadBitBuffer::ReadIso14496_1Expanded(uint32_t max_class_size,
                                                   uint32_t& size_of_instance) {
  static const ByteAccumulator big_endian_accumulator =
      [](uint64_t byte, int /*index*/, uint64_t& accumulated_value) {
        accumulated_value = accumulated_value << 7 | (byte & 0x7f);
      };
  int8_t unused_encoded_size = 0;
  return AccumulateUleb128OrIso14496_1Internal(
      big_endian_accumulator, max_class_size, *this, size_of_instance,
      unused_encoded_size);
}

absl::Status ReadBitBuffer::ReadUint8Span(absl::Span<uint8_t> output) {
  for (auto& byte : output) {
    RETURN_IF_NOT_OK(ReadUnsignedLiteral(8, byte));
  }
  return absl::OkStatus();
}

absl::Status ReadBitBuffer::ReadBoolean(bool& output) {
  uint64_t bit;
  RETURN_IF_NOT_OK(ReadUnsignedLiteral(1, bit));
  output = static_cast<bool>(bit);
  return absl::OkStatus();
}

// Loads enough bits from source such that there are at least n =
// `required_num_bits` in `bit_buffer_` after completion. Returns an error if
// there are not enough bits in `source_` to fulfill this request. If `source_`
// contains enough data, this function will fill the read buffer completely.
absl::Status ReadBitBuffer::LoadBits(const int32_t required_num_bits,
                                     const bool fill_to_capacity) {
  DiscardAllBits();
  int num_bits_to_load = required_num_bits;
  if (fill_to_capacity) {
    int bit_capacity = bit_buffer_.capacity() * 8;
    if (required_num_bits > bit_capacity) {
      return absl::InvalidArgumentError(
          "required_num_bits must be <= capacity.");
    } else {
      num_bits_to_load = bit_capacity;
    }
  }
  int bits_loaded = 0;
  int original_source_offset = source_bit_offset_;
  int64_t bit_buffer_write_offset = 0;
  while (ShouldRead(source_bit_offset_, *source_,
                    (num_bits_to_load - bits_loaded)) &&
         (bit_buffer_.size() != bit_buffer_.capacity())) {
    if ((num_bits_to_load - bits_loaded) % 8 != 0 ||
        source_bit_offset_ % 8 != 0 || bit_buffer_write_offset % 8 != 0) {
      // Load bit by bit
      uint8_t loaded_bit = GetUpperBit(source_bit_offset_, *source_);
      RETURN_IF_NOT_OK(
          CanWriteBits(true, 1, bit_buffer_write_offset, bit_buffer_));
      RETURN_IF_NOT_OK(
          WriteBit(loaded_bit, bit_buffer_write_offset, bit_buffer_));
      source_bit_offset_++;
      buffer_size_++;
      bits_loaded++;
    } else {
      // Load byte by byte
      bit_buffer_.push_back(source_->at(source_bit_offset_ / 8));
      source_bit_offset_ += 8;
      buffer_size_ += 8;
      bits_loaded += 8;
    }
  }
  if (bits_loaded < required_num_bits) {
    source_bit_offset_ = original_source_offset;
    DiscardAllBits();
    return absl::ResourceExhaustedError("Not enough bits in source.");
  }
  return absl::OkStatus();
}

bool ReadBitBuffer::IsDataAvailable() {
  bool valid_data_in_buffer =
      (buffer_bit_offset_ >= 0 && buffer_bit_offset_ < buffer_size_);
  bool valid_data_in_source =
      (source_bit_offset_ >= 0 && (source_bit_offset_ / 8) < source_->size());
  return valid_data_in_buffer || valid_data_in_source;
}

void ReadBitBuffer::DiscardAllBits() {
  buffer_bit_offset_ = 0;
  buffer_size_ = 0;
  bit_buffer_.clear();
}

}  // namespace iamf_tools
