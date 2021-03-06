// Copyright 2017 The TensorFlow Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
#include "tensorflow/contrib/tensor_forest/kernels/v4/grow_stats.h"

#include <cfloat>
#include <queue>
#include "tensorflow/contrib/tensor_forest/kernels/tree_utils.h"
#include "tensorflow/contrib/tensor_forest/kernels/v4/stat_utils.h"
#include "tensorflow/core/lib/random/distribution_sampler.h"


namespace tensorflow {
namespace tensorforest {

// When creating evaluators for the split candidates, use these
// for the left and right return values.
static const int32 LEFT_INDEX = 0;
static const int32 RIGHT_INDEX = 1;

GrowStats::GrowStats(const TensorForestParams& params, int32 depth)
    : depth_(depth),
      params_(params),
      split_after_samples_(ResolveParam(params.split_after_samples(), depth)),
      num_splits_to_consider_(
          ResolveParam(params.num_splits_to_consider(), depth)),
      num_outputs_(params.num_outputs()) {}

void GrowStats::AddSplit(const decision_trees::BinaryNode& split) {
  splits_.push_back(split);
  evaluators_.emplace_back(
      CreateBinaryDecisionNodeEvaluator(split, LEFT_INDEX, RIGHT_INDEX));
  AddSplitStats();
}

void GrowStats::RemoveSplit(int split_num) {
  splits_.erase(splits_.begin() + split_num);
  evaluators_.erase(evaluators_.begin() + split_num);
  RemoveSplitStats(split_num);
}

// ------------------------ Classification --------------------------- //

ClassificationStats::ClassificationStats(const TensorForestParams& params,
                                         int32 depth)
    : GrowStats(params, depth), finish_early_(false) {
  // Early splitting params.
  if (params.finish_type().type() == SPLIT_FINISH_BASIC) {
    min_split_samples_ = split_after_samples_;
  } else {
    if (!params.has_dominate_fraction() || !params.has_min_split_samples()) {
      LOG(FATAL) << "dominate_fraction and min_split_samples "
                 << "required for early-finish strategy.";
    } else {
      min_split_samples_ = ResolveParam(params.min_split_samples(), depth);
      finish_check_every_ =
          ResolveParam(params.finish_type().check_every_steps(), depth);
      finish_sample_epoch_ = min_split_samples_ / finish_check_every_;

      dominate_fraction_ = ResolveParam(params.dominate_fraction(), depth_);
      if (dominate_fraction_ <= 0 || dominate_fraction_ > 1.0) {
        LOG(FATAL) << "Invalid dominate fraction " << dominate_fraction_;
      }
    }
  }

  // Pruning params.
  if (params.pruning_type().type() != SPLIT_PRUNE_NONE) {
    prune_check_every_ =
        ResolveParam(params.pruning_type().prune_every_samples(), depth);
    prune_sample_epoch_ = 1;
    prune_fraction_ = 0.0;
    switch (params_.pruning_type().type()) {
      case SPLIT_PRUNE_HALF:
        prune_fraction_ = 0.5;
        break;
      case SPLIT_PRUNE_QUARTER:
        prune_fraction_ = 0.25;
        break;
      case SPLIT_PRUNE_10_PERCENT:
        prune_fraction_ = 0.10;
        break;
      case SPLIT_PRUNE_HOEFFDING:
        dominate_fraction_ = ResolveParam(params.dominate_fraction(), depth_);
        half_ln_dominate_frac_ = 0.5 * log(1.0 / (1.0 - dominate_fraction_));
        break;
      default:
        LOG(WARNING) << "Unknown pruning type";
    }
  }

  if (params.use_running_stats_method()) {
    left_gini_.reset(new RunningGiniScores());
    right_gini_.reset(new RunningGiniScores());
  }

  uint64 time_seed = static_cast<uint64>(std::clock());
  single_rand_ = std::unique_ptr<random::PhiloxRandom>(
      new random::PhiloxRandom(time_seed));
  rng_ = std::unique_ptr<random::SimplePhilox>(
      new random::SimplePhilox(single_rand_.get()));
}

bool ClassificationStats::IsFinished() const {
  bool basic = weight_sum_ >= split_after_samples_ && num_outputs_seen() > 1;
  return basic || finish_early_;
}

float ClassificationStats::MaybeCachedGiniScore(int split, float* left_sum,
                                                float* right_sum) const {
  if (left_gini_ == nullptr) {
    return GiniScore(split, left_sum, right_sum);
  } else {
    *left_sum = left_gini_->sum(split);
    const float left = WeightedSmoothedGini(
        *left_sum, left_gini_->square(split), num_outputs_);

    *right_sum = right_gini_->sum(split);
    const float right = WeightedSmoothedGini(
        *right_sum, right_gini_->square(split), num_outputs_);

    return left + right;
  }
}

void ClassificationStats::AddExample(
    const std::unique_ptr<TensorDataSet>& input_data, const InputTarget* target,
    int example) {
  const int64 int_label = target->GetTargetAsClassIndex(example, 0);
  const float weight = target->GetTargetWeight(example);

  for (int i = 0; i < num_splits(); ++i) {
    auto& eval = evaluators_[i];
    if (eval->Decide(input_data, example) == LEFT_INDEX) {
      if (left_gini_ != nullptr) {
        left_gini_->update(i, left_count(i, int_label), weight);
      }
      ClassificationAddLeftExample(i, int_label, weight);
    } else if (right_gini_ != nullptr) {
      right_gini_->update(i, right_count(i, int_label), weight);
    }
  }

  ClassificationAddTotalExample(int_label, weight);

  weight_sum_ += weight;

  CheckFinishEarly();
  CheckPrune();
}

void ClassificationStats::CheckPrune() {
  if (IsFinished() || weight_sum_ < prune_sample_epoch_ * prune_check_every_) {
    return;
  }
  ++prune_sample_epoch_;

  if (params_.pruning_type().type() == SPLIT_PRUNE_HOEFFDING) {
    CheckPruneHoeffding();
    return;
  }

  const int to_remove = num_splits() * prune_fraction_;
  if (to_remove <= 0) {
    return;
  }

  // pair ordering is first-then-second by default, no need for custom
  // comparison.  Use std::greater to make it a min-heap.
  std::priority_queue<std::pair<float, int>, std::vector<std::pair<float, int>>,
                      std::greater<std::pair<float, int>>>
      worst;

  // Track indices that are in the heap so we can iterate over them
  // by largest-first later.
  std::set<int> indices;

  for (int i = 0; i < num_splits(); ++i) {
    float left, right;
    const float split_score = MaybeCachedGiniScore(i, &left, &right);
    if (worst.size() < to_remove) {
      worst.push(std::pair<float, int>(split_score, i));
      indices.insert(i);
    } else if (worst.top().first < split_score) {
      indices.erase(worst.top().second);
      worst.pop();
      worst.push(std::pair<float, int>(split_score, i));
      indices.insert(i);
    }
  }

  // traverse indices from the back so that they are removed correctly.
  for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
    RemoveSplit(*it);
  }
}

void ClassificationStats::CheckPruneHoeffding() {
  std::vector<float> split_scores(num_splits());
  // Find best split score
  float best_split_score = FLT_MAX;
  for (int i = 0; i < num_splits(); ++i) {
    float left, right;
    split_scores[i] = MaybeCachedGiniScore(i, &left, &right);
    if (split_scores[i] < best_split_score) {
      best_split_score = split_scores[i];
    }
  }

  // We apply the Hoeffding bound to the difference between the best split
  // score and the i-th split score.
  // Raw Gini ranges from 0 to 1 - (1/n), but our gini score is weighted.
  const float num_classes = params_.num_outputs();
  const float gini_diff_range = weight_sum_ * (1.0 - 1.0 / num_classes);
  float epsilon = gini_diff_range * sqrt(half_ln_dominate_frac_ / weight_sum_);
  for (int i = num_splits() - 1; i >= 0; i--) {
    if (split_scores[i] - best_split_score > epsilon) {
      RemoveSplit(i);
    }
  }
}

void ClassificationStats::CheckFinishEarly() {
  if (weight_sum_ < min_split_samples_ ||
      weight_sum_ < finish_sample_epoch_ * finish_check_every_) {
    return;
  }
  ++finish_sample_epoch_;

  if (params_.finish_type().type() == SPLIT_FINISH_DOMINATE_HOEFFDING) {
    CheckFinishEarlyHoeffding();
  } else if (params_.finish_type().type() == SPLIT_FINISH_DOMINATE_BOOTSTRAP) {
    CheckFinishEarlyBootstrap();
  }
}

void ClassificationStats::CheckFinishEarlyHoeffding() {
  // Each term in the Gini impurity can range from 0 to 0.5 * 0.5.
  float range = 0.25 * static_cast<float>(params_.num_outputs()) * weight_sum_;

  float hoeffding_bound =
      range * sqrt(log(1.0 / (1.0 - dominate_fraction_)) / (2.0 * weight_sum_));

  float unused_left_sum, unused_right_sum;
  std::function<float(int)> score_fn =
      std::bind(&ClassificationStats::MaybeCachedGiniScore, this,
                std::placeholders::_1, &unused_left_sum, &unused_right_sum);

  float best_score;
  int32 best_index;
  float second_best_score;
  int32 second_best_index;
  GetTwoBest(num_splits(), score_fn, &best_score, &best_index,
             &second_best_score, &second_best_index);

  finish_early_ = (second_best_score - best_score) > hoeffding_bound;
}

void ClassificationStats::MakeBootstrapWeights(int index,
                                               std::vector<float>* weights) {
  int n = weight_sum_;
  float denom = static_cast<float>(n) + static_cast<float>(num_outputs_);
  for (int i = 0; i < num_outputs_; ++i) {
    // Use the Laplace smoothed per-class probabilities when generating the
    // bootstrap samples.
    (*weights)[i] = (left_count(index, i) + 1.0) / denom;
    (*weights)[num_outputs_ + i] = (right_count(index, i) + 1.0) / denom;
  }
}

int ClassificationStats::NumBootstrapSamples() const {
  float p = 1.0 - dominate_fraction_;
  int bootstrap_samples = 1;
  while (p < 1.0) {
    ++bootstrap_samples;
    p = p * 2;
  }
  return bootstrap_samples;
}

void ClassificationStats::CheckFinishEarlyBootstrap() {
  float unused_left_sum, unused_right_sum;
  std::function<float(int)> score_fn =
      std::bind(&ClassificationStats::MaybeCachedGiniScore, this,
                std::placeholders::_1, &unused_left_sum, &unused_right_sum);

  float best_score;
  int32 best_index;
  float second_best_score;
  int32 second_best_index;
  GetTwoBest(num_splits(), score_fn, &best_score, &best_index,
             &second_best_score, &second_best_index);

  std::vector<float> weights1(num_outputs_ * 2);
  MakeBootstrapWeights(best_index, &weights1);
  random::DistributionSampler ds1(weights1);

  std::vector<float> weights2(num_outputs_ * 2);
  MakeBootstrapWeights(second_best_index, &weights2);
  random::DistributionSampler ds2(weights2);

  const int bootstrap_samples = NumBootstrapSamples();

  int worst_g1 = 0;
  for (int i = 0; i < bootstrap_samples; i++) {
    int g1 = BootstrapGini(weight_sum_, 2 * num_outputs_, ds1, rng_.get());
    worst_g1 = std::max(worst_g1, g1);
  }

  int best_g2 = 99;
  for (int i = 0; i < bootstrap_samples; i++) {
    int g2 = BootstrapGini(weight_sum_, 2 * num_outputs_, ds2, rng_.get());
    best_g2 = std::min(best_g2, g2);
  }

  finish_early_ = worst_g1 < best_g2;
}

// ------------------------ Dense Classification --------------------------- //
void DenseClassificationGrowStats::ExtractFromProto(const FertileSlot& slot) {
  Initialize();
  if (!slot.has_post_init_leaf_stats()) {
    return;
  }
  const int32 num_classes = params_.num_outputs();
  weight_sum_ = slot.post_init_leaf_stats().weight_sum();
  const auto& class_stats =
      slot.post_init_leaf_stats().classification().dense_counts();

  // Total counts.
  for (int i = 0; i < num_classes; ++i) {
    total_counts_[i] = class_stats.value(i).float_value();
    num_outputs_seen_ += total_counts_[i] != 0;
  }

  // Candidate counts and splits.
  int split_num = 0;
  for (const auto& cand : slot.candidates()) {
    AddSplit(cand.split());
    const auto& left_stats = cand.left_stats().classification().dense_counts();
    for (int i = 0; i < num_classes; ++i) {
      const float val = left_stats.value(i).float_value();
      mutable_left_count(split_num, i) = val;
      MaybeInitializeRunningCount(split_num, val);
    }
    ++split_num;
  }
}

void DenseClassificationGrowStats::PackToProto(FertileSlot* slot) const {
  auto* slot_stats = slot->mutable_post_init_leaf_stats();
  slot_stats->set_weight_sum(weight_sum_);

  auto* class_stats = slot->mutable_post_init_leaf_stats()
                          ->mutable_classification()
                          ->mutable_dense_counts();
  for (int i = 0; i < num_outputs_; ++i) {
    class_stats->add_value()->set_float_value(total_counts_[i]);
  }

  for (int split_num = 0;  split_num < num_splits(); ++split_num) {
    auto* cand = slot->add_candidates();
    *cand->mutable_split() = splits_[split_num];
    auto* left_stats = cand->mutable_left_stats()
                           ->mutable_classification()
                           ->mutable_dense_counts();
    for (int i = 0; i < num_outputs_; ++i) {
       left_stats->add_value()->set_float_value(left_count(split_num, i));
    }
  }
}

float DenseClassificationGrowStats::GiniScore(int split, float* left_sum,
                                              float* right_sum) const {
  float left_square = 0, right_square = 0;
  *left_sum = 0;
  *right_sum = 0;
  for (int j = 0; j < num_outputs_; ++j) {
    const float left = left_count(split, j);
    *left_sum += left;
    left_square += left * left;
    const float right = right_count(split, j);
    *right_sum += right;
    right_square += right * right;
  }

  const float left_score =
      WeightedSmoothedGini(*left_sum, left_square, num_outputs_);
  const float right_score =
      WeightedSmoothedGini(*right_sum, right_square, num_outputs_);
  return left_score + right_score;
}

bool DenseClassificationGrowStats::BestSplit(SplitCandidate* best) const {
  float min_score = FLT_MAX;
  int best_index = -1;
  float best_left_sum, best_right_sum;

  // Calculate sums.
  for (int i = 0; i < num_splits(); ++i) {
    float left_sum, right_sum;
    const float split_score = MaybeCachedGiniScore(i, &left_sum, &right_sum);
    // Find the lowest gini.
    if (left_sum > 0 && right_sum > 0 &&
        split_score < min_score) {  // useless check
      min_score = split_score;
      best_index = i;
      best_left_sum = left_sum;
      best_right_sum = right_sum;
    }
  }

  // This could happen if all the splits are useless.
  if (best_index < 0) {
    return false;
  }

  // Fill in stats to be used for leaf model.
  *best->mutable_split() = splits_[best_index];
  // Left
  auto* left = best->mutable_left_stats();
  auto* left_class_stats = left->mutable_classification();
  left->set_weight_sum(best_left_sum);
  auto* left_counts = left_class_stats->mutable_dense_counts();
  for (int i = 0; i < params_.num_outputs(); ++i) {
    left_counts->add_value()->set_float_value(
        left_count(best_index, i));
  }

  // Right
  auto* right = best->mutable_right_stats();
  auto* right_class_stats = right->mutable_classification();
  right->set_weight_sum(best_right_sum);
  auto* right_counts = right_class_stats->mutable_dense_counts();
  for (int i = 0; i < params_.num_outputs(); ++i) {
    right_counts->add_value()->set_float_value(
        total_counts_[i] - left_count(best_index, i));
  }
  return true;
}

// ------------------------ Sparse Classification --------------------------- //
void SparseClassificationGrowStats::ExtractFromProto(const FertileSlot& slot) {
  Initialize();
  if (!slot.has_post_init_leaf_stats()) {
    return;
  }
  weight_sum_ = slot.post_init_leaf_stats().weight_sum();
  const auto& class_stats =
      slot.post_init_leaf_stats().classification().sparse_counts();

  // Total counts.
  for (auto const& entry : class_stats.sparse_value()) {
    total_counts_[entry.first] = entry.second.float_value();
  }

  // Candidate counts and splits.
  int split_num = 0;
  for (const auto& cand : slot.candidates()) {
    AddSplit(cand.split());
    const auto& left_stats = cand.left_stats().classification().sparse_counts();
    for (auto const& entry : left_stats.sparse_value()) {
      const float val = entry.second.float_value();
      left_counts_[split_num][entry.first] = val;
      MaybeInitializeRunningCount(split_num, val);
    }
    ++split_num;
  }
}

void SparseClassificationGrowStats::PackToProto(FertileSlot* slot) const {
  auto* slot_stats = slot->mutable_post_init_leaf_stats();
  slot_stats->set_weight_sum(weight_sum_);

  auto* class_stats = slot->mutable_post_init_leaf_stats()
                          ->mutable_classification()
                          ->mutable_sparse_counts()
                          ->mutable_sparse_value();
  for (const auto& entry : total_counts_) {
    decision_trees::Value val;
    val.set_float_value(entry.second);
    (*class_stats)[entry.first] = val;
  }

  for (int split_num = 0;  split_num < num_splits(); ++split_num) {
    auto* cand = slot->add_candidates();
    *cand->mutable_split() = splits_[split_num];
    auto* left_stats = cand->mutable_left_stats()
                           ->mutable_classification()
                           ->mutable_sparse_counts()
                           ->mutable_sparse_value();
    for (const auto& entry : left_counts_[split_num]) {
      decision_trees::Value val;
      val.set_float_value(entry.second);
      (*left_stats)[entry.first] = val;
    }
  }
}

float SparseClassificationGrowStats::GiniScore(
    int split, float* left_sum, float* right_sum) const {
  float left_square = 0, right_square = 0;
  *left_sum = 0;
  *right_sum = 0;
  for (const auto& entry : total_counts_) {
    const int label = entry.first;
    float left = 0;
    float right = 0;
    auto it = left_counts_[split].find(label);
    if (it == left_counts_[split].end()) {
      right = entry.second;
    } else {
      left = it->second;
      right = entry.second - it->second;
    }
    *left_sum += left;
    left_square += left * left;
    *right_sum += right;
    right_square += right * right;
  }
  const int32 num_classes = params_.num_outputs();
  const float left_score =
      WeightedSmoothedGini(*left_sum, left_square, num_classes);
  const float right_score =
      WeightedSmoothedGini(*right_sum, right_square, num_classes);
  return left_score + right_score;
}

bool SparseClassificationGrowStats::BestSplit(SplitCandidate* best) const {
  float min_score = FLT_MAX;
  int best_index = -1;
  float best_left_sum = -1;
  float best_right_sum = -1;

  // Find the lowest gini.
  for (int i = 0; i < num_splits(); ++i) {
    float left_sum, right_sum;
    const float split_score = MaybeCachedGiniScore(i, &left_sum, &right_sum);
    if (left_sum > 0 && right_sum > 0 &&
        split_score < min_score) {  // useless check
      min_score = split_score;
      best_index = i;
      best_left_sum = left_sum;
      best_right_sum = right_sum;
    }
  }

  // This could happen if all the splits are useless.
  if (best_index < 0) {
    return false;
  }

  // Fill in stats to be used for leaf model.
  *best->mutable_split() = splits_[best_index];
  // Left
  auto* left = best->mutable_left_stats();
  auto* left_class_stats = left->mutable_classification();
  left->set_weight_sum(best_left_sum);
  auto* left_counts =
      left_class_stats->mutable_sparse_counts()->mutable_sparse_value();

  // Right
  auto* right = best->mutable_right_stats();
  auto* right_class_stats = right->mutable_classification();
  right->set_weight_sum(best_right_sum);
  auto* right_counts =
      right_class_stats->mutable_sparse_counts()->mutable_sparse_value();

  for (const auto& entry : total_counts_) {
    auto it = left_counts_[best_index].find(entry.first);
    if (it == left_counts_[best_index].end()) {
      (*right_counts)[entry.first].set_float_value(entry.second);
    } else {
      const float left = it->second;
      const float right = entry.second - it->second;
      (*left_counts)[entry.first].set_float_value(left);
      if (right > 0) {
        (*right_counts)[entry.first].set_float_value(right);
      }
    }
  }
  return true;
}

// --------------------- Least Squares Regression --------------------------- //
void LeastSquaresRegressionGrowStats::ExtractFromProto(
    const FertileSlot& slot) {
  const int32 num_outputs = params_.num_outputs();
  Initialize();
  if (!slot.has_post_init_leaf_stats()) {
    return;
  }
  weight_sum_ = slot.post_init_leaf_stats().weight_sum();
  const auto& total_sums =
      slot.post_init_leaf_stats().regression().mean_output();
  const auto& total_squares =
      slot.post_init_leaf_stats().regression().mean_output_squares();

  // Total counts.
  for (int i = 0; i < num_outputs; ++i) {
    total_sum_[i] = total_sums.value(i).float_value();
    total_sum_squares_[i] = total_squares.value(i).float_value();
  }

  // Candidate counts and splits.
  int split_num = 0;
  for (const auto& cand : slot.candidates()) {
    AddSplit(cand.split());
    const auto& sums = cand.left_stats().regression().mean_output();
    const auto& squares = cand.left_stats().regression().mean_output_squares();
    for (int i = 0; i < num_outputs; ++i) {
      left_sum(split_num, i) = sums.value(i).float_value();
      left_square(split_num, i) = squares.value(i).float_value();
    }
    left_counts_[split_num] = cand.left_stats().weight_sum();
    ++split_num;
  }
}

void LeastSquaresRegressionGrowStats::PackToProto(FertileSlot* slot) const {
  const int32 num_outputs = params_.num_outputs();
  auto* slot_stats = slot->mutable_post_init_leaf_stats();
  slot_stats->set_weight_sum(weight_sum_);

  auto* total_sums = slot->mutable_post_init_leaf_stats()
                         ->mutable_regression()
                         ->mutable_mean_output();
  auto* total_squares = slot->mutable_post_init_leaf_stats()
                            ->mutable_regression()
                            ->mutable_mean_output_squares();

  for (int i = 0; i < total_sum_.size(); ++i) {
    total_sums->add_value()->set_float_value(total_sum_[i]);
    total_squares->add_value()->set_float_value(total_sum_squares_[i]);
  }

  for (int split_num = 0;  split_num < num_splits(); ++split_num) {
    auto* cand = slot->add_candidates();
    *cand->mutable_split() = splits_[split_num];
    auto* sums = cand->mutable_left_stats()
                           ->mutable_regression()
                           ->mutable_mean_output();
    auto* squares = cand->mutable_left_stats()
                        ->mutable_regression()
                        ->mutable_mean_output_squares();
    for (int i = 0; i < num_outputs; ++i) {
      sums->add_value()->set_float_value(left_sum(split_num, i));
      squares->add_value()->set_float_value(left_square(split_num, i));
    }
    cand->mutable_left_stats()->set_weight_sum(left_counts_[split_num]);
  }
}

void LeastSquaresRegressionGrowStats::AddExample(
    const std::unique_ptr<TensorDataSet>& input_data, const InputTarget* target,
    int example) {
  const int32 num_outputs = params_.num_outputs();
  // Update splits.
  for (int i = 0; i < num_splits(); ++i) {
    auto& eval = evaluators_[i];
    if (eval->Decide(input_data, example) == LEFT_INDEX) {
      for (int j = 0; j < num_outputs; ++j) {
        const float output = target->GetTargetAsContinuous(example, j);
        left_sum(i, j) += output;
        left_square(i, j) += output * output;
      }
      ++left_counts_[i];
    }
  }

  // Update totals.
  for (int i = 0; i < num_outputs; ++i) {
    const float output = target->GetTargetAsContinuous(example, i);
    total_sum_[i] += output;
    total_sum_squares_[i] += output * output;
  }
  weight_sum_ += 1.0;
}

float LeastSquaresRegressionGrowStats::SplitVariance(int split) const {
  float total_variance = 0;
  for (int i = 0; i < params_.num_outputs(); ++i) {
    // Left side
    const float le_x =
        left_sum(split, i) / left_counts_[split];

    const float le_x2 =
        left_square(split, i) / left_counts_[split];
    total_variance += le_x2 - le_x * le_x;

    // Right side
    const float re_x = (total_sum_[i] - left_sum(split, i)) /
                       (weight_sum_ - left_counts_[split]);

    const float re_x2 =
        (total_sum_squares_[i] - left_square(split, i)) /
        (weight_sum_ - left_counts_[split]);
    total_variance += re_x2 - re_x * re_x;
  }
  return total_variance;
}

bool LeastSquaresRegressionGrowStats::BestSplit(SplitCandidate* best) const {
  float min_score = FLT_MAX;
  int best_index = -1;
  const int32 num_outputs = params_.num_outputs();
  for (int i = 0; i < num_splits(); ++i) {
    if (left_counts_[i] > 0 && weight_sum_ - left_counts_[i] > 0) {
      const float split_score = SplitVariance(i);
      if (split_score < min_score) {
        min_score = split_score;
        best_index = i;
      }
    }
  }

  // This could happen if all the splits are useless.
  if (best_index < 0) {
    return false;
  }

  // Fill in right stats to be used for leaf model.
  *best->mutable_split() = splits_[best_index];
  // Left
  auto* left = best->mutable_left_stats();
  auto* left_reg_stats = left->mutable_regression();
  left->set_weight_sum(left_counts_[best_index]);
  auto* left_output_sum = left_reg_stats->mutable_mean_output();
  for (int i = 0; i < num_outputs; ++i) {
    left_output_sum->add_value()->set_float_value(
        left_sum(best_index, i));
  }

  // Right
  auto* right = best->mutable_right_stats();
  auto* right_reg_stats = right->mutable_regression();
  right->set_weight_sum(weight_sum_ - left_counts_[best_index]);
  auto* right_output_sum = right_reg_stats->mutable_mean_output();
  for (int i = 0; i < num_outputs; ++i) {
    right_output_sum->add_value()->set_float_value(
        total_sum_[i] - left_sum(best_index, i));
  }
  return true;
}

bool LeastSquaresRegressionGrowStats::IsFinished() const {
  return weight_sum_ >= split_after_samples_;
}

}  // namespace tensorforest
}  // namespace tensorflow
