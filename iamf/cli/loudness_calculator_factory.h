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
#ifndef CLI_LOUDNESS_CALCULATOR_FACTORY_H_
#define CLI_LOUDNESS_CALCULATOR_FACTORY_H_

#include <cstdint>
#include <memory>

#include "iamf/cli/loudness_calculator.h"
#include "iamf/obu/mix_presentation.h"

namespace iamf_tools {

/*!\brief Abstract class to create loudness calculators.
 *
 * This class will be used when calculating the loudness of a mix presentation
 * layout. The mix presentation finalizer will take in a factory (or factories)
 * and use them to create a loudness calculator for each stream. By taking in a
 * factory the finalizer can be agnostic to the type of loudness calculator
 * being used which may depend on implementation details, or it may depend on
 * the specific layouts.
 */
class LoudnessCalculatorFactoryBase {
 public:
  /*!\brief Creates a loudness calculator.
   *
   * \param layout Layout to measure loudness on.
   * \param rendered_sample_rate Sample rate of the rendered audio.
   * \param rendered_bit_depth Bit-depth of the rendered audio.
   * \return Unique pointer to a loudness calculator.
   */
  virtual std::unique_ptr<LoudnessCalculatorBase> CreateLoudnessCalculator(
      const MixPresentationLayout& layout, int32_t rendered_sample_rate,
      int32_t rendered_bit_depth) const = 0;

  /*!\brief Destructor. */
  virtual ~LoudnessCalculatorFactoryBase() = 0;
};

// TODO(b/302273947): Use this class to measure loudness when finalizing mix
//                    presentations.
/*!\brief Factory which always provides a fallback loudness calculator.
 *
 * This factory produces underlying loudness calculators which entirely ignore
 * all input samples. Those calculators are useful if the user does not wish to
 * provide samples to the calculator, or knows the samples they provide are
 * inaccurate or not valid for some reason.
 *
 * This factory is intended to be used when the user does not care about
 * "accurate" loudness measurement. One such case is if the user does not
 * support rendering to a layout that loudness should be measured on.
 *
 * This factory is also intended be used as a fallback when other loudness
 * factories fail to be created.
 */
class LoudnessCalculatorFactoryUserProvidedLoudness
    : public LoudnessCalculatorFactoryBase {
 public:
  /*!\brief Creates a fallback loudness calculator.
   *
   * \param layout Layout to use when echoing loudness back.
   * \param rendered_sample_rate Sample rate of the rendered audio to ignore.
   * \param rendered_bit_depth Bit-depth of the rendered audio to ignore.
   * \return Unique pointer to a loudness calculator.
   */
  std::unique_ptr<LoudnessCalculatorBase> CreateLoudnessCalculator(
      const MixPresentationLayout& layout, int32_t /*rendered_sample_rate*/,
      int32_t /*rendered_bit_depth*/) const override;

  /*!\brief Destructor. */
  ~LoudnessCalculatorFactoryUserProvidedLoudness() override = default;
};

}  // namespace iamf_tools

#endif  // CLI_LOUDNESS_CALCULATOR_H_
