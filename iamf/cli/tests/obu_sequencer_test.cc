/*
 * Copyright (c) 2023, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 3-Clause Clear
 * License and the Alliance for Open Media Patent License 1.0. If the BSD
 * 3-Clause Clear License was not distributed with this source code in the
 * LICENSE file, you can obtain it at
 * www.aomedia.org/license/software-license/bsd-3-c-c. If the Alliance for
 * Open Media Patent License 1.0 was not distributed with this source code
 * in the PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */
#include "iamf/cli/obu_sequencer.h"

#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status_matchers.h"
#include "absl/strings/string_view.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "iamf/cli/audio_element_with_data.h"
#include "iamf/cli/audio_frame_with_data.h"
#include "iamf/cli/leb_generator.h"
#include "iamf/cli/parameter_block_with_data.h"
#include "iamf/cli/tests/cli_test_utils.h"
#include "iamf/common/write_bit_buffer.h"
#include "iamf/obu/arbitrary_obu.h"
#include "iamf/obu/audio_frame.h"
#include "iamf/obu/codec_config.h"
#include "iamf/obu/demixing_info_parameter_data.h"
#include "iamf/obu/demixing_param_definition.h"
#include "iamf/obu/ia_sequence_header.h"
#include "iamf/obu/mix_presentation.h"
#include "iamf/obu/obu_base.h"
#include "iamf/obu/obu_header.h"
#include "iamf/obu/param_definitions.h"
#include "iamf/obu/parameter_block.h"
#include "iamf/obu/temporal_delimiter.h"
#include "iamf/obu/types.h"

namespace iamf_tools {
namespace {

using ::absl_testing::IsOk;

constexpr DecodedUleb128 kCodecConfigId = 1;
constexpr uint32_t kNumSamplesPerFrame = 8;
constexpr uint32_t kSampleRate = 48000;
constexpr DecodedUleb128 kFirstAudioElementId = 1;
constexpr DecodedUleb128 kSecondAudioElementId = 2;
constexpr DecodedUleb128 kFirstSubstreamId = 1;
constexpr DecodedUleb128 kSecondSubstreamId = 2;
constexpr DecodedUleb128 kFirstMixPresentationId = 100;
constexpr DecodedUleb128 kFirstDemixingParameterId = 998;
constexpr DecodedUleb128 kCommonMixGainParameterId = 999;
constexpr uint32_t kCommonMixGainParameterRate = kSampleRate;

constexpr absl::string_view kOmitOutputIamfFile = "";
constexpr bool kIncludeTemporalDelimiters = true;
constexpr bool kDoNotIncludeTemporalDelimiters = false;

// TODO(b/302470464): Add test coverage `ObuSequencer::WriteTemporalUnit()` and
//                    `ObuSequencer::PickAndPlace()` configured with minimal and
//                    fixed-size leb generators.

void AddEmptyAudioFrameWithAudioElementIdSubstreamIdAndTimestamps(
    uint32_t audio_element_id, uint32_t substream_id, int32_t start_timestamp,
    int32_t end_timestamp,
    const absl::flat_hash_map<uint32_t, AudioElementWithData>& audio_elements,
    std::list<AudioFrameWithData>& audio_frames) {
  ASSERT_TRUE(audio_elements.contains(audio_element_id));

  audio_frames.emplace_back(AudioFrameWithData{
      .obu = AudioFrameObu(ObuHeader(), substream_id, {}),
      .start_timestamp = start_timestamp,
      .end_timestamp = end_timestamp,
      .raw_samples = {},
      .down_mixing_params = {.in_bitstream = false},
      .audio_element_with_data = &audio_elements.at(audio_element_id)});
}

TEST(GenerateTemporalUnitMap, SubstreamsOrderedByAudioElementIdSubstreamId) {
  const std::list<ParameterBlockWithData> kNoParameterBlocks;
  const std::list<ArbitraryObu> kNoArbitraryObus;
  // Initialize two audio elements each with two substreams.
  absl::flat_hash_map<uint32_t, CodecConfigObu> codec_config_obus = {};
  absl::flat_hash_map<uint32_t, AudioElementWithData> audio_elements = {};
  const uint32_t kCodecConfigId = 0;
  AddLpcmCodecConfigWithIdAndSampleRate(kCodecConfigId, 48000,
                                        codec_config_obus);
  AddAmbisonicsMonoAudioElementWithSubstreamIds(
      /*audio_element_id=*/100, kCodecConfigId, {2000, 4000}, codec_config_obus,
      audio_elements);
  AddAmbisonicsMonoAudioElementWithSubstreamIds(
      /*audio_element_id=*/200, kCodecConfigId, {3000, 5000}, codec_config_obus,
      audio_elements);

  // Add some audio frames in an arbitrary order.
  std::list<AudioFrameWithData> audio_frames;
  AddEmptyAudioFrameWithAudioElementIdSubstreamIdAndTimestamps(
      200, 5000, 0, 16, audio_elements, audio_frames);
  AddEmptyAudioFrameWithAudioElementIdSubstreamIdAndTimestamps(
      100, 2000, 0, 16, audio_elements, audio_frames);
  AddEmptyAudioFrameWithAudioElementIdSubstreamIdAndTimestamps(
      200, 3000, 0, 16, audio_elements, audio_frames);
  AddEmptyAudioFrameWithAudioElementIdSubstreamIdAndTimestamps(
      100, 4000, 0, 16, audio_elements, audio_frames);

  // By default the results are expected to be sorted by audio element ID then
  // by substream ID.
  struct ExpectedAudioElementIdAndSubstreamId {
    uint32_t audio_element_id;
    uint32_t substream_id;
  };
  std::vector<ExpectedAudioElementIdAndSubstreamId> expected_results = {
      {100, 2000}, {100, 4000}, {200, 3000}, {200, 5000}};

  // Generate the temporal unit map.
  TemporalUnitMap temporal_unit_map;
  EXPECT_THAT(ObuSequencerBase::GenerateTemporalUnitMap(
                  audio_frames, kNoParameterBlocks, kNoArbitraryObus,
                  temporal_unit_map),
              IsOk());

  // The test is hard-coded with one temporal unit and four frames.
  ASSERT_EQ(temporal_unit_map.size(), 1);
  ASSERT_TRUE(temporal_unit_map.contains(0));
  ASSERT_EQ(temporal_unit_map[0].audio_frames.size(), 4);

  // Validate the order of the output frames matches the expected order.
  auto expected_results_iter = expected_results.begin();
  for (const auto& audio_frame : temporal_unit_map[0].audio_frames) {
    EXPECT_EQ(audio_frame->audio_element_with_data->obu.GetAudioElementId(),
              expected_results_iter->audio_element_id);
    EXPECT_EQ(audio_frame->obu.GetSubstreamId(),
              expected_results_iter->substream_id);

    expected_results_iter++;
  }
}

PerIdParameterMetadata CreatePerIdMetadataForDemixing(
    DecodedUleb128 parameter_id) {
  DemixingParamDefinition expected_demixing_param_definition;
  expected_demixing_param_definition.parameter_id_ = parameter_id;
  expected_demixing_param_definition.parameter_rate_ = 48000;
  expected_demixing_param_definition.param_definition_mode_ = 0;
  expected_demixing_param_definition.duration_ = 8;
  expected_demixing_param_definition.constant_subblock_duration_ = 8;
  expected_demixing_param_definition.reserved_ = 10;

  return PerIdParameterMetadata{
      .param_definition_type = ParamDefinition::kParameterDefinitionDemixing,
      .param_definition = expected_demixing_param_definition};
}

TEST(GenerateTemporalUnitMap, ParameterBlocksAreOrderedByAscendingParameterId) {
  constexpr DecodedUleb128 kLowerParameterId = 9;
  constexpr DecodedUleb128 kHigherParameterId = 9000;
  constexpr int32_t kStartTimestamp = 0;
  constexpr int32_t kEndTimestamp = 16;
  constexpr DecodedUleb128 kSecondParameterId = kCommonMixGainParameterId + 1;
  std::list<ParameterBlockWithData> parameter_blocks;
  const std::list<ArbitraryObu> kNoArbitraryObus;
  absl::flat_hash_map<uint32_t, CodecConfigObu> codec_config_obus = {};
  absl::flat_hash_map<uint32_t, AudioElementWithData> audio_elements = {};
  AddLpcmCodecConfigWithIdAndSampleRate(kCodecConfigId, 48000,
                                        codec_config_obus);
  AddAmbisonicsMonoAudioElementWithSubstreamIds(
      kFirstAudioElementId, kCodecConfigId, {kFirstSubstreamId},
      codec_config_obus, audio_elements);
  std::list<AudioFrameWithData> audio_frames;
  AddEmptyAudioFrameWithAudioElementIdSubstreamIdAndTimestamps(
      kFirstAudioElementId, kFirstSubstreamId, kStartTimestamp, kEndTimestamp,
      audio_elements, audio_frames);
  PerIdParameterMetadata lower_per_id_metadata =
      CreatePerIdMetadataForDemixing(kLowerParameterId);
  PerIdParameterMetadata higher_per_id_metadata =
      CreatePerIdMetadataForDemixing(kHigherParameterId);
  auto common_demixing_info_parameter_data =
      std::make_unique<DemixingInfoParameterData>();
  common_demixing_info_parameter_data->dmixp_mode =
      DemixingInfoParameterData::kDMixPMode1;
  common_demixing_info_parameter_data->reserved = 0;
  auto higher_id_parameter_block = std::make_unique<ParameterBlockObu>(
      ObuHeader(), higher_per_id_metadata.param_definition.parameter_id_,
      higher_per_id_metadata);
  ASSERT_THAT(higher_id_parameter_block->InitializeSubblocks(), IsOk());
  higher_id_parameter_block->subblocks_[0].param_data =
      std::move(common_demixing_info_parameter_data);
  auto lower_id_parameter_block = std::make_unique<ParameterBlockObu>(
      ObuHeader(), kSecondParameterId, lower_per_id_metadata);
  ASSERT_THAT(lower_id_parameter_block->InitializeSubblocks(), IsOk());
  const auto& higher_id__parameter_block_with_data =
      parameter_blocks.emplace_back(ParameterBlockWithData{
          .obu = std::move(higher_id_parameter_block),
          .start_timestamp = 0,
          .end_timestamp = 16,
      });
  const auto& lower_id_parameter_block_with_data =
      parameter_blocks.emplace_back(ParameterBlockWithData{
          .obu = std::move(lower_id_parameter_block),
          .start_timestamp = 0,
          .end_timestamp = 16,
      });
  const std::vector<const ParameterBlockWithData*>
      expected_output_in_ascending_parameter_id_order = {
          &lower_id_parameter_block_with_data,
          &higher_id__parameter_block_with_data};

  // Generate the temporal unit map.
  TemporalUnitMap temporal_unit_map;
  EXPECT_THAT(
      ObuSequencerBase::GenerateTemporalUnitMap(
          audio_frames, parameter_blocks, kNoArbitraryObus, temporal_unit_map),
      IsOk());

  ASSERT_TRUE(temporal_unit_map.contains(0));
  EXPECT_EQ(temporal_unit_map[0].parameter_blocks,
            expected_output_in_ascending_parameter_id_order);
}

TEST(GenerateTemporalUnitMap, OmitsArbitraryObusWithNoInsertionTick) {
  const std::list<AudioFrameWithData> kNoAudioFrames;
  const std::list<ParameterBlockWithData> kNoParameterBlocks;
  const std::optional<int64_t> kNoInsertionTick = std::nullopt;
  std::list<ArbitraryObu> arbitrary_obus;
  arbitrary_obus.emplace_back(ArbitraryObu(
      kObuIaReserved25, ObuHeader(), {},
      ArbitraryObu::kInsertionHookAfterIaSequenceHeader, kNoInsertionTick));

  // Generate the temporal unit map.
  TemporalUnitMap temporal_unit_map;
  EXPECT_THAT(ObuSequencerBase::GenerateTemporalUnitMap(
                  kNoAudioFrames, kNoParameterBlocks, arbitrary_obus,
                  temporal_unit_map),
              IsOk());
  EXPECT_TRUE(temporal_unit_map.empty());
}

TEST(GenerateTemporalUnitMap, CreatesTemporalUnitsForEachInsertionTick) {
  const std::list<AudioFrameWithData> kNoAudioFrames;
  const std::list<ParameterBlockWithData> kNoParameterBlocks;
  const int64_t kFirstInsertionTick = 99;
  const int kNumberOfObusAtFirstInsertionTick = 2;
  const int64_t kSecondInsertionTick = 1999;
  const int kNumberOfObusAtSecondInsertionTick = 1;
  std::list<ArbitraryObu> arbitrary_obus;
  arbitrary_obus.emplace_back(
      ArbitraryObu(kObuIaReserved25, ObuHeader(), {},
                   ArbitraryObu::kInsertionHookAfterParameterBlocksAtTick,
                   kFirstInsertionTick));
  arbitrary_obus.emplace_back(ArbitraryObu(
      kObuIaReserved25, ObuHeader(), {},
      ArbitraryObu::kInsertionHookAfterIaSequenceHeader, kFirstInsertionTick));
  arbitrary_obus.emplace_back(
      ArbitraryObu(kObuIaReserved25, ObuHeader(), {},
                   ArbitraryObu::kInsertionHookAfterParameterBlocksAtTick,
                   kSecondInsertionTick));

  // Generate the temporal unit map.
  TemporalUnitMap temporal_unit_map;
  EXPECT_THAT(ObuSequencerBase::GenerateTemporalUnitMap(
                  kNoAudioFrames, kNoParameterBlocks, arbitrary_obus,
                  temporal_unit_map),
              IsOk());

  EXPECT_EQ(temporal_unit_map.size(), 2);
  ASSERT_TRUE(temporal_unit_map.contains(kFirstInsertionTick));
  EXPECT_EQ(temporal_unit_map.at(kFirstInsertionTick).arbitrary_obus.size(),
            kNumberOfObusAtFirstInsertionTick);
  ASSERT_TRUE(temporal_unit_map.contains(kSecondInsertionTick));
  EXPECT_EQ(temporal_unit_map.at(kSecondInsertionTick).arbitrary_obus.size(),
            kNumberOfObusAtSecondInsertionTick);
}

void ValidateWriteTemporalUnitSequence(
    bool include_temporal_delimiters, const TemporalUnit& temporal_unit,
    const std::list<const ObuBase*>& expected_sequence) {
  WriteBitBuffer expected_wb(128);
  for (const auto* expected_obu : expected_sequence) {
    ASSERT_NE(expected_obu, nullptr);
    EXPECT_THAT(expected_obu->ValidateAndWriteObu(expected_wb), IsOk());
  }

  WriteBitBuffer result_wb(128);
  int unused_num_samples;
  EXPECT_THAT(ObuSequencerBase::WriteTemporalUnit(include_temporal_delimiters,
                                                  temporal_unit, result_wb,
                                                  unused_num_samples),
              IsOk());

  EXPECT_EQ(result_wb.bit_buffer(), expected_wb.bit_buffer());
}

void InitializeOneParameterBlockAndOneAudioFrame(
    PerIdParameterMetadata& per_id_metadata,
    std::list<ParameterBlockWithData>& parameter_blocks,
    std::list<AudioFrameWithData>& audio_frames,
    absl::flat_hash_map<uint32_t, CodecConfigObu>& codec_config_obus,
    absl::flat_hash_map<uint32_t, AudioElementWithData>& audio_elements) {
  const int32_t kStartTimestamp = 0;
  const int32_t kEndTimestamp = 16;
  AddLpcmCodecConfigWithIdAndSampleRate(kCodecConfigId, kSampleRate,
                                        codec_config_obus);
  AddAmbisonicsMonoAudioElementWithSubstreamIds(
      kFirstAudioElementId, kCodecConfigId, {kFirstSubstreamId},
      codec_config_obus, audio_elements);
  AddEmptyAudioFrameWithAudioElementIdSubstreamIdAndTimestamps(
      kFirstAudioElementId, kFirstSubstreamId, kStartTimestamp, kEndTimestamp,
      audio_elements, audio_frames);
  auto data = std::make_unique<DemixingInfoParameterData>();
  data->dmixp_mode = DemixingInfoParameterData::kDMixPMode1;
  data->reserved = 0;
  auto parameter_block = std::make_unique<ParameterBlockObu>(
      ObuHeader(), per_id_metadata.param_definition.parameter_id_,
      per_id_metadata);
  ASSERT_THAT(parameter_block->InitializeSubblocks(), IsOk());
  parameter_block->subblocks_[0].param_data = std::move(data);
  parameter_blocks.emplace_back(ParameterBlockWithData{
      .obu = std::move(parameter_block),
      .start_timestamp = 0,
      .end_timestamp = 16,
  });
}

TEST(WriteTemporalUnit, WritesArbitraryObuBeforeParameterBlocksAtTime) {
  std::list<ParameterBlockWithData> parameter_blocks;
  std::list<AudioFrameWithData> audio_frames;
  absl::flat_hash_map<uint32_t, CodecConfigObu> codec_config_obus;
  absl::flat_hash_map<uint32_t, AudioElementWithData> audio_elements;
  PerIdParameterMetadata per_id_metadata =
      CreatePerIdMetadataForDemixing(kFirstDemixingParameterId);
  InitializeOneParameterBlockAndOneAudioFrame(per_id_metadata, parameter_blocks,
                                              audio_frames, codec_config_obus,
                                              audio_elements);
  std::list<ArbitraryObu> arbitrary_obus;
  arbitrary_obus.emplace_back(
      ArbitraryObu(kObuIaReserved25, ObuHeader(), {},
                   ArbitraryObu::kInsertionHookBeforeParameterBlocksAtTick));
  TemporalUnit temporal_unit = {
      .audio_frames = {&audio_frames.front()},
      .parameter_blocks = {&parameter_blocks.front()},
      .arbitrary_obus = {&arbitrary_obus.front()},
  };
  const TemporalDelimiterObu temporal_delimiter_obu(ObuHeader{});

  const std::list<const ObuBase*>
      expected_arbitrary_obu_between_temporal_delimiter_and_parameter_block = {
          &temporal_delimiter_obu, &arbitrary_obus.front(),
          parameter_blocks.front().obu.get(), &audio_frames.front().obu};

  ValidateWriteTemporalUnitSequence(
      kIncludeTemporalDelimiters, temporal_unit,
      expected_arbitrary_obu_between_temporal_delimiter_and_parameter_block);
}

TEST(WriteTemporalUnit, WritesArbitraryObuAfterParameterBlocksAtTime) {
  std::list<ParameterBlockWithData> parameter_blocks;
  std::list<AudioFrameWithData> audio_frames;
  absl::flat_hash_map<uint32_t, CodecConfigObu> codec_config_obus;
  absl::flat_hash_map<uint32_t, AudioElementWithData> audio_elements;
  PerIdParameterMetadata per_id_metadata =
      CreatePerIdMetadataForDemixing(kFirstDemixingParameterId);
  InitializeOneParameterBlockAndOneAudioFrame(per_id_metadata, parameter_blocks,
                                              audio_frames, codec_config_obus,
                                              audio_elements);
  std::list<ArbitraryObu> arbitrary_obus;
  arbitrary_obus.emplace_back(
      ArbitraryObu(kObuIaReserved25, ObuHeader(), {},
                   ArbitraryObu::kInsertionHookAfterParameterBlocksAtTick));
  TemporalUnit temporal_unit = {
      .audio_frames = {&audio_frames.front()},
      .parameter_blocks = {&parameter_blocks.front()},
      .arbitrary_obus = {&arbitrary_obus.front()},
  };

  const std::list<const ObuBase*>
      expected_arbitrary_obu_between_parameter_block_and_audio_frame = {
          parameter_blocks.front().obu.get(),
          &arbitrary_obus.front(),
          &audio_frames.front().obu,
      };

  ValidateWriteTemporalUnitSequence(
      kDoNotIncludeTemporalDelimiters, temporal_unit,
      expected_arbitrary_obu_between_parameter_block_and_audio_frame);
}

TEST(WriteTemporalUnit, WritesArbitraryObuAfterAudioFramesAtTime) {
  std::list<ParameterBlockWithData> parameter_blocks;
  std::list<AudioFrameWithData> audio_frames;
  absl::flat_hash_map<uint32_t, CodecConfigObu> codec_config_obus;
  absl::flat_hash_map<uint32_t, AudioElementWithData> audio_elements;
  PerIdParameterMetadata per_id_metadata =
      CreatePerIdMetadataForDemixing(kFirstDemixingParameterId);
  InitializeOneParameterBlockAndOneAudioFrame(per_id_metadata, parameter_blocks,
                                              audio_frames, codec_config_obus,
                                              audio_elements);
  std::list<ArbitraryObu> arbitrary_obus;
  arbitrary_obus.emplace_back(
      ArbitraryObu(kObuIaReserved25, ObuHeader(), {},
                   ArbitraryObu::kInsertionHookAfterAudioFramesAtTick));
  TemporalUnit temporal_unit = {
      .audio_frames = {&audio_frames.front()},
      .parameter_blocks = {&parameter_blocks.front()},
      .arbitrary_obus = {&arbitrary_obus.front()},
  };

  const std::list<const ObuBase*> expected_arbitrary_obu_after_audio_frame = {
      parameter_blocks.front().obu.get(),
      &audio_frames.front().obu,
      &arbitrary_obus.front(),
  };

  ValidateWriteTemporalUnitSequence(kDoNotIncludeTemporalDelimiters,
                                    temporal_unit,
                                    expected_arbitrary_obu_after_audio_frame);
}

TEST(WriteTemporalUnit, AddsNumberOfUntrimmedSamplesToNumSamples) {
  std::list<ParameterBlockWithData> parameter_blocks;
  std::list<AudioFrameWithData> audio_frames;
  absl::flat_hash_map<uint32_t, CodecConfigObu> codec_config_obus;
  absl::flat_hash_map<uint32_t, AudioElementWithData> audio_elements;
  PerIdParameterMetadata per_id_metadata =
      CreatePerIdMetadataForDemixing(kFirstDemixingParameterId);
  InitializeOneParameterBlockAndOneAudioFrame(per_id_metadata, parameter_blocks,
                                              audio_frames, codec_config_obus,
                                              audio_elements);
  audio_frames.front().obu.header_.num_samples_to_trim_at_end = 2;
  audio_frames.front().obu.header_.num_samples_to_trim_at_start = 1;
  constexpr uint32_t kNumUntrimmedSamples = kNumSamplesPerFrame - 1 - 2;

  TemporalUnit temporal_unit = {
      .audio_frames = {&audio_frames.front()},
      .parameter_blocks = {&parameter_blocks.front()},
  };

  WriteBitBuffer undefined_wb(128);
  int num_samples = 0;
  EXPECT_THAT(ObuSequencerBase::WriteTemporalUnit(
                  kDoNotIncludeTemporalDelimiters, temporal_unit, undefined_wb,
                  num_samples),
              IsOk());
  EXPECT_EQ(num_samples, kNumUntrimmedSamples);
  // Another write keeps adding to the number of samples.
  EXPECT_THAT(ObuSequencerBase::WriteTemporalUnit(
                  kDoNotIncludeTemporalDelimiters, temporal_unit, undefined_wb,
                  num_samples),
              IsOk());
  EXPECT_EQ(num_samples, kNumUntrimmedSamples * 2);
}

TEST(WriteTemporalUnit, FailsWhenAudioFrameHasNoAudioElement) {
  std::list<ParameterBlockWithData> parameter_blocks;
  std::list<AudioFrameWithData> audio_frames;
  absl::flat_hash_map<uint32_t, CodecConfigObu> codec_config_obus;
  absl::flat_hash_map<uint32_t, AudioElementWithData> audio_elements;
  PerIdParameterMetadata per_id_metadata =
      CreatePerIdMetadataForDemixing(kFirstDemixingParameterId);
  InitializeOneParameterBlockAndOneAudioFrame(per_id_metadata, parameter_blocks,
                                              audio_frames, codec_config_obus,
                                              audio_elements);
  // Corrupt the audio frame by disassociating the audio element.
  audio_frames.front().audio_element_with_data = nullptr;

  TemporalUnit temporal_unit = {
      .audio_frames = {&audio_frames.front()},
      .parameter_blocks = {&parameter_blocks.front()},
  };

  WriteBitBuffer undefined_wb(128);
  int unused_num_samples;
  EXPECT_FALSE(ObuSequencerBase::WriteTemporalUnit(
                   kDoNotIncludeTemporalDelimiters, temporal_unit, undefined_wb,
                   unused_num_samples)
                   .ok());
}

TEST(WriteTemporalUnit, FailsWhenAudioElementHasNoCodecConfig) {
  std::list<ParameterBlockWithData> parameter_blocks;
  std::list<AudioFrameWithData> audio_frames;
  absl::flat_hash_map<uint32_t, CodecConfigObu> codec_config_obus;
  absl::flat_hash_map<uint32_t, AudioElementWithData> audio_elements;
  PerIdParameterMetadata per_id_metadata =
      CreatePerIdMetadataForDemixing(kFirstDemixingParameterId);
  InitializeOneParameterBlockAndOneAudioFrame(per_id_metadata, parameter_blocks,
                                              audio_frames, codec_config_obus,
                                              audio_elements);
  // Corrupt the audio element by disassociating the codec config.
  audio_elements.at(kFirstAudioElementId).codec_config = nullptr;
  std::list<ArbitraryObu> arbitrary_obus;

  TemporalUnit temporal_unit = {
      .audio_frames = {&audio_frames.front()},
      .parameter_blocks = {&parameter_blocks.front()},
  };

  WriteBitBuffer undefined_wb(128);
  int unused_num_samples;
  EXPECT_FALSE(ObuSequencerBase::WriteTemporalUnit(
                   kDoNotIncludeTemporalDelimiters, temporal_unit, undefined_wb,
                   unused_num_samples)
                   .ok());
}

class ObuSequencerTest : public ::testing::Test {
 public:
  void InitializeDescriptorObus() {
    ia_sequence_header_obu_.emplace(ObuHeader(), IASequenceHeaderObu::kIaCode,
                                    ProfileVersion::kIamfSimpleProfile,
                                    ProfileVersion::kIamfSimpleProfile);
    AddLpcmCodecConfigWithIdAndSampleRate(kCodecConfigId, kSampleRate,
                                          codec_config_obus_);
    AddAmbisonicsMonoAudioElementWithSubstreamIds(
        kFirstAudioElementId, kCodecConfigId, {kFirstSubstreamId},
        codec_config_obus_, audio_elements_);
    AddMixPresentationObuWithAudioElementIds(
        kFirstMixPresentationId, {kFirstAudioElementId},
        kCommonMixGainParameterId, kCommonMixGainParameterRate,
        mix_presentation_obus_);

    ASSERT_TRUE(ia_sequence_header_obu_.has_value());
    ASSERT_TRUE(codec_config_obus_.contains(kCodecConfigId));
    ASSERT_TRUE(audio_elements_.contains(kFirstAudioElementId));
    ASSERT_FALSE(mix_presentation_obus_.empty());
  }

  void InitObusForOneFrameIaSequence() {
    ia_sequence_header_obu_.emplace(ObuHeader(), IASequenceHeaderObu::kIaCode,
                                    ProfileVersion::kIamfSimpleProfile,
                                    ProfileVersion::kIamfSimpleProfile);
    per_id_metadata_ =
        CreatePerIdMetadataForDemixing(kFirstDemixingParameterId);
    InitializeOneParameterBlockAndOneAudioFrame(
        per_id_metadata_, parameter_blocks_, audio_frames_, codec_config_obus_,
        audio_elements_);
    AddMixPresentationObuWithAudioElementIds(
        kFirstMixPresentationId, {audio_elements_.begin()->first},
        kCommonMixGainParameterId, kCommonMixGainParameterRate,
        mix_presentation_obus_);
  }

  void ValidateWriteDescriptorObuSequence(
      const std::list<const ObuBase*>& expected_sequence) {
    WriteBitBuffer expected_wb(128);
    for (const auto* expected_obu : expected_sequence) {
      ASSERT_NE(expected_obu, nullptr);
      EXPECT_THAT(expected_obu->ValidateAndWriteObu(expected_wb), IsOk());
    }

    WriteBitBuffer result_wb(128);
    EXPECT_THAT(ObuSequencerBase::WriteDescriptorObus(
                    ia_sequence_header_obu_.value(), codec_config_obus_,
                    audio_elements_, mix_presentation_obus_, arbitrary_obus_,
                    result_wb),
                IsOk());

    EXPECT_EQ(result_wb.bit_buffer(), expected_wb.bit_buffer());
  }

 protected:
  std::optional<IASequenceHeaderObu> ia_sequence_header_obu_;
  absl::flat_hash_map<uint32_t, CodecConfigObu> codec_config_obus_;
  absl::flat_hash_map<uint32_t, AudioElementWithData> audio_elements_;
  std::list<MixPresentationObu> mix_presentation_obus_;

  PerIdParameterMetadata per_id_metadata_;
  std::list<ParameterBlockWithData> parameter_blocks_;
  std::list<AudioFrameWithData> audio_frames_;

  std::list<ArbitraryObu> arbitrary_obus_;
};

TEST_F(ObuSequencerTest, OrdersByAParticularObuType) {
  InitializeDescriptorObus();
  // The IAMF spec REQUIRES descriptor OBUs to be ordered by `obu_type` in a
  // particular order (i.e. IA Sequence Header, Codec Config Audio Element, Mix
  // Presentation).
  const std::list<const ObuBase*> expected_sequence = {
      &ia_sequence_header_obu_.value(), &codec_config_obus_.at(kCodecConfigId),
      &audio_elements_.at(kFirstAudioElementId).obu,
      &mix_presentation_obus_.back()};

  ValidateWriteDescriptorObuSequence(expected_sequence);
}

TEST_F(ObuSequencerTest, ArbitraryObuAfterIaSequenceHeader) {
  InitializeDescriptorObus();

  arbitrary_obus_.emplace_back(
      ArbitraryObu(kObuIaReserved25, ObuHeader(), {},
                   ArbitraryObu::kInsertionHookAfterIaSequenceHeader));

  const std::list<const ObuBase*> expected_sequence = {
      &ia_sequence_header_obu_.value(),
      &arbitrary_obus_.back(),
      &codec_config_obus_.at(kCodecConfigId),
      &audio_elements_.at(kFirstAudioElementId).obu,
      &mix_presentation_obus_.back(),
  };

  ValidateWriteDescriptorObuSequence(expected_sequence);
}

TEST_F(ObuSequencerTest, ArbitraryObuAfterCodecConfigs) {
  InitializeDescriptorObus();

  arbitrary_obus_.emplace_back(
      ArbitraryObu(kObuIaReserved25, ObuHeader(), {},
                   ArbitraryObu::kInsertionHookAfterCodecConfigs));

  const std::list<const ObuBase*> expected_sequence = {
      &ia_sequence_header_obu_.value(),
      &codec_config_obus_.at(kCodecConfigId),
      &arbitrary_obus_.back(),
      &audio_elements_.at(kFirstAudioElementId).obu,
      &mix_presentation_obus_.back(),
  };

  ValidateWriteDescriptorObuSequence(expected_sequence);
}

TEST_F(ObuSequencerTest, ArbitraryObuAfterAudioElements) {
  InitializeDescriptorObus();

  arbitrary_obus_.emplace_back(
      ArbitraryObu(kObuIaReserved25, ObuHeader(), {},
                   ArbitraryObu::kInsertionHookAfterAudioElements));

  const std::list<const ObuBase*> expected_sequence = {
      &ia_sequence_header_obu_.value(),
      &codec_config_obus_.at(kCodecConfigId),
      &audio_elements_.at(kFirstAudioElementId).obu,
      &arbitrary_obus_.back(),
      &mix_presentation_obus_.back(),
  };

  ValidateWriteDescriptorObuSequence(expected_sequence);
}

TEST_F(ObuSequencerTest, ArbitraryObuAfterMixPresentations) {
  InitializeDescriptorObus();

  arbitrary_obus_.emplace_back(
      ArbitraryObu(kObuIaReserved25, ObuHeader(), {},
                   ArbitraryObu::kInsertionHookAfterMixPresentations));

  const std::list<const ObuBase*> expected_sequence = {
      &ia_sequence_header_obu_.value(),
      &codec_config_obus_.at(kCodecConfigId),
      &audio_elements_.at(kFirstAudioElementId).obu,
      &mix_presentation_obus_.back(),
      &arbitrary_obus_.back(),
  };

  ValidateWriteDescriptorObuSequence(expected_sequence);
}

// This behavior helps ensure that "after descriptors" are not written in the
// "IACB" box in MP4.
TEST_F(ObuSequencerTest, DoesNotWriteArbitraryObuAfterDescriptors) {
  InitializeDescriptorObus();

  arbitrary_obus_.emplace_back(
      ArbitraryObu(kObuIaReserved25, ObuHeader(), {},
                   ArbitraryObu::kInsertionHookAfterDescriptors));

  const std::list<const ObuBase*> expected_sequence = {
      &ia_sequence_header_obu_.value(), &codec_config_obus_.at(kCodecConfigId),
      &audio_elements_.at(kFirstAudioElementId).obu,
      &mix_presentation_obus_.back(),
      // &arbitrary_obus_.back(),
  };

  ValidateWriteDescriptorObuSequence(expected_sequence);
}

TEST_F(ObuSequencerTest, CodecConfigAreAscendingOrderByDefault) {
  InitializeDescriptorObus();

  // Initialize a second Codec Config OBU.
  const DecodedUleb128 kSecondCodecConfigId = 101;
  AddLpcmCodecConfigWithIdAndSampleRate(kSecondCodecConfigId, kSampleRate,
                                        codec_config_obus_);

  // IAMF makes no recommendation for the ordering between multiple descriptor
  // OBUs of the same type. By default `WriteDescriptorObus` orders them in
  // ascending order.
  ASSERT_LT(kCodecConfigId, kSecondCodecConfigId);
  const std::list<const ObuBase*> expected_sequence = {
      &ia_sequence_header_obu_.value(), &codec_config_obus_.at(kCodecConfigId),
      &codec_config_obus_.at(kSecondCodecConfigId),
      &audio_elements_.at(kFirstAudioElementId).obu,
      &mix_presentation_obus_.back()};

  ValidateWriteDescriptorObuSequence(expected_sequence);
}

TEST_F(ObuSequencerTest, AudioElementAreAscendingOrderByDefault) {
  InitializeDescriptorObus();

  // Initialize a second Audio Element OBU.
  const DecodedUleb128 kSecondAudioElementId = 101;
  AddAmbisonicsMonoAudioElementWithSubstreamIds(
      kSecondAudioElementId, kCodecConfigId, {kFirstSubstreamId},
      codec_config_obus_, audio_elements_);

  // IAMF makes no recommendation for the ordering between multiple descriptor
  // OBUs of the same type. By default `WriteDescriptorObus` orders them in
  // ascending order.
  ASSERT_LT(kFirstAudioElementId, kSecondAudioElementId);
  const std::list<const ObuBase*> expected_sequence = {
      &ia_sequence_header_obu_.value(), &codec_config_obus_.at(kCodecConfigId),
      &audio_elements_.at(kFirstAudioElementId).obu,
      &audio_elements_.at(kSecondAudioElementId).obu,
      &mix_presentation_obus_.back()};

  ValidateWriteDescriptorObuSequence(expected_sequence);
}

TEST_F(ObuSequencerTest, MixPresentationsAreAscendingOrderByDefault) {
  InitializeDescriptorObus();

  // Initialize a second Mix Presentation OBU.
  const DecodedUleb128 kSecondMixPresentationId = 99;
  AddMixPresentationObuWithAudioElementIds(
      kSecondMixPresentationId, {kFirstAudioElementId},
      kCommonMixGainParameterId, kCommonMixGainParameterRate,
      mix_presentation_obus_);

  // IAMF makes no recommendation for the ordering between multiple descriptor
  // OBUs of the same type. By default `WriteDescriptorObus` orders them in
  // ascending order regardless of their order in the input list.
  ASSERT_LT(kSecondMixPresentationId, kFirstMixPresentationId);
  ASSERT_EQ(mix_presentation_obus_.back().GetMixPresentationId(),
            kSecondMixPresentationId);
  ASSERT_EQ(mix_presentation_obus_.front().GetMixPresentationId(),
            kFirstMixPresentationId);
  const std::list<const ObuBase*> expected_sequence = {
      &ia_sequence_header_obu_.value(), &codec_config_obus_.at(kCodecConfigId),
      &audio_elements_.at(kFirstAudioElementId).obu,
      &mix_presentation_obus_.back(), &mix_presentation_obus_.front()};

  ValidateWriteDescriptorObuSequence(expected_sequence);
}

void InitializeDescriptorObusForTwoMonoAmbisonicsAudioElement(
    absl::flat_hash_map<DecodedUleb128, CodecConfigObu>& codec_config_obus,
    absl::flat_hash_map<uint32_t, AudioElementWithData>& audio_elements,
    std::list<MixPresentationObu>& mix_presentation_obus) {
  AddLpcmCodecConfigWithIdAndSampleRate(kCodecConfigId, kSampleRate,
                                        codec_config_obus);
  AddAmbisonicsMonoAudioElementWithSubstreamIds(
      kFirstAudioElementId, kCodecConfigId, {kFirstSubstreamId},
      codec_config_obus, audio_elements);
  AddAmbisonicsMonoAudioElementWithSubstreamIds(
      kSecondAudioElementId, kCodecConfigId, {kSecondSubstreamId},
      codec_config_obus, audio_elements);
  AddMixPresentationObuWithAudioElementIds(
      kFirstMixPresentationId, {kFirstAudioElementId, kSecondAudioElementId},
      kCommonMixGainParameterId, kCommonMixGainParameterRate,
      mix_presentation_obus);
}

TEST(WriteDescriptorObus,
     InvalidWhenMixPresentationDoesNotComplyWithIaSequenceHeader) {
  IASequenceHeaderObu ia_sequence_header_obu(
      ObuHeader(), IASequenceHeaderObu::kIaCode,
      ProfileVersion::kIamfSimpleProfile, ProfileVersion::kIamfSimpleProfile);
  absl::flat_hash_map<DecodedUleb128, CodecConfigObu> codec_config_obus;
  absl::flat_hash_map<uint32_t, AudioElementWithData> audio_elements;
  std::list<MixPresentationObu> mix_presentation_obus;
  InitializeDescriptorObusForTwoMonoAmbisonicsAudioElement(
      codec_config_obus, audio_elements, mix_presentation_obus);

  WriteBitBuffer unused_wb(0);
  EXPECT_FALSE(ObuSequencerBase::WriteDescriptorObus(
                   ia_sequence_header_obu, codec_config_obus, audio_elements,
                   mix_presentation_obus, /*arbitrary_obus=*/{}, unused_wb)
                   .ok());
}

TEST(WriteDescriptorObus,
     ValidWhenMixPresentationCompliesWithIaSequenceHeader) {
  IASequenceHeaderObu ia_sequence_header_obu(
      ObuHeader(), IASequenceHeaderObu::kIaCode,
      ProfileVersion::kIamfSimpleProfile, ProfileVersion::kIamfBaseProfile);
  absl::flat_hash_map<DecodedUleb128, CodecConfigObu> codec_config_obus;
  absl::flat_hash_map<uint32_t, AudioElementWithData> audio_elements;
  std::list<MixPresentationObu> mix_presentation_obus;
  InitializeDescriptorObusForTwoMonoAmbisonicsAudioElement(
      codec_config_obus, audio_elements, mix_presentation_obus);

  WriteBitBuffer unused_wb(0);
  EXPECT_THAT(ObuSequencerBase::WriteDescriptorObus(
                  ia_sequence_header_obu, codec_config_obus, audio_elements,
                  mix_presentation_obus, /*arbitrary_obus=*/{}, unused_wb),
              IsOk());
}

TEST(ObuSequencerIamf, PickAndPlaceWritesFileWithOnlyIaSequenceHeader) {
  const std::string kOutputIamfFilename = GetAndCleanupOutputFileName(".iamf");
  {
    const IASequenceHeaderObu ia_sequence_header_obu(
        ObuHeader(), IASequenceHeaderObu::kIaCode,
        ProfileVersion::kIamfSimpleProfile, ProfileVersion::kIamfBaseProfile);
    ObuSequencerIamf sequencer(kOutputIamfFilename,
                               kDoNotIncludeTemporalDelimiters,
                               *LebGenerator::Create());

    EXPECT_THAT(sequencer.PickAndPlace(
                    ia_sequence_header_obu, /*codec_config_obus=*/{},
                    /*audio_elements=*/{}, /*mix_presentation_obus=*/{},
                    /*audio_frames=*/{}, /*parameter_blocks=*/{},
                    /*arbitrary_obus=*/{}),
                IsOk());

    // `ObuSequencerIamf` goes out of scope and closes the file.
  }

  EXPECT_TRUE(std::filesystem::exists(kOutputIamfFilename));
}

TEST(ObuSequencerIamf, PickAndPlaceSucceedsWithEmptyOutputFile) {
  const IASequenceHeaderObu ia_sequence_header_obu(
      ObuHeader(), IASequenceHeaderObu::kIaCode,
      ProfileVersion::kIamfSimpleProfile, ProfileVersion::kIamfBaseProfile);

  ObuSequencerIamf sequencer(std::string(kOmitOutputIamfFile),
                             kDoNotIncludeTemporalDelimiters,
                             *LebGenerator::Create());

  EXPECT_THAT(sequencer.PickAndPlace(
                  ia_sequence_header_obu, /*codec_config_obus=*/{},
                  /*audio_elements=*/{}, /*mix_presentation_obus=*/{},
                  /*audio_frames=*/{}, /*parameter_blocks=*/{},
                  /*arbitrary_obus=*/{}),
              IsOk());
}

TEST_F(ObuSequencerTest, PickAndPlaceCreatesFileWithOneFrameIaSequence) {
  const std::string kOutputIamfFilename = GetAndCleanupOutputFileName(".iamf");
  InitObusForOneFrameIaSequence();
  ObuSequencerIamf sequencer(kOutputIamfFilename,
                             kDoNotIncludeTemporalDelimiters,
                             *LebGenerator::Create());

  ASSERT_THAT(
      sequencer.PickAndPlace(*ia_sequence_header_obu_, codec_config_obus_,
                             audio_elements_, mix_presentation_obus_,
                             audio_frames_, parameter_blocks_, arbitrary_obus_),
      IsOk());

  EXPECT_TRUE(std::filesystem::exists(kOutputIamfFilename));
}

TEST_F(ObuSequencerTest, PickAndPlaceLeavesNoFileWhenDescriptorsAreInvalid) {
  constexpr uint32_t kInvalidIaCode = IASequenceHeaderObu::kIaCode + 1;
  const std::string kOutputIamfFilename = GetAndCleanupOutputFileName(".iamf");
  InitObusForOneFrameIaSequence();
  // Overwrite the IA Sequence Header with an invalid one.
  ia_sequence_header_obu_ = IASequenceHeaderObu(
      ObuHeader(), kInvalidIaCode, ProfileVersion::kIamfSimpleProfile,
      ProfileVersion::kIamfSimpleProfile);
  ObuSequencerIamf sequencer(kOutputIamfFilename,
                             kDoNotIncludeTemporalDelimiters,
                             *LebGenerator::Create());

  ASSERT_FALSE(sequencer
                   .PickAndPlace(*ia_sequence_header_obu_, codec_config_obus_,
                                 audio_elements_, mix_presentation_obus_,
                                 audio_frames_, parameter_blocks_,
                                 arbitrary_obus_)
                   .ok());

  EXPECT_FALSE(std::filesystem::exists(kOutputIamfFilename));
}

TEST_F(ObuSequencerTest, PickAndPlaceLeavesNoFileWhenTemporalUnitsAreInvalid) {
  constexpr bool kInvalidateTemporalUnit = true;
  const std::string kOutputIamfFilename = GetAndCleanupOutputFileName(".iamf");
  InitObusForOneFrameIaSequence();
  arbitrary_obus_.emplace_back(
      ArbitraryObu(kObuIaReserved25, ObuHeader(), {},
                   ArbitraryObu::kInsertionHookAfterAudioFramesAtTick, 0,
                   kInvalidateTemporalUnit));
  ObuSequencerIamf sequencer(kOutputIamfFilename,
                             kDoNotIncludeTemporalDelimiters,
                             *LebGenerator::Create());

  ASSERT_FALSE(sequencer
                   .PickAndPlace(*ia_sequence_header_obu_, codec_config_obus_,
                                 audio_elements_, mix_presentation_obus_,
                                 audio_frames_, parameter_blocks_,
                                 arbitrary_obus_)
                   .ok());

  EXPECT_FALSE(std::filesystem::exists(kOutputIamfFilename));
}

TEST_F(ObuSequencerTest,
       PickAndPlaceOnInvalidTemporalUnitFailsWhenOutputFileIsOmitted) {
  constexpr bool kInvalidateTemporalUnit = true;
  InitObusForOneFrameIaSequence();
  arbitrary_obus_.emplace_back(
      ArbitraryObu(kObuIaReserved25, ObuHeader(), {},
                   ArbitraryObu::kInsertionHookAfterAudioFramesAtTick, 0,
                   kInvalidateTemporalUnit));
  ObuSequencerIamf sequencer(std::string(kOmitOutputIamfFile),
                             kDoNotIncludeTemporalDelimiters,
                             *LebGenerator::Create());

  ASSERT_FALSE(sequencer
                   .PickAndPlace(*ia_sequence_header_obu_, codec_config_obus_,
                                 audio_elements_, mix_presentation_obus_,
                                 audio_frames_, parameter_blocks_,
                                 arbitrary_obus_)
                   .ok());
}

}  // namespace
}  // namespace iamf_tools
