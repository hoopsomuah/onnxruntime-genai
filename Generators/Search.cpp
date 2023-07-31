#include "Generators.h"

Search::Search(Gpt& model, SearchParams params)
    : model_{model}, params_{params} {
  auto allocator = &Ort::Allocator::GetWithDefaultOptions();
  auto cpu_allocator = allocator;

  int64_t sequences_dims[] = {params_.batch_size, params_.max_length};
  output_sequences_ = OrtValue::CreateTensor<int32_t>(*allocator, sequences_dims, std::size(sequences_dims));

  // below buffers are on cpu
  search_state_.sequences_space = AllocateBuffer<int32_t>(cpu_allocator,
                                                          sequences_space_buffer_,
                                                          SafeInt<size_t>(2) * params_.batch_size * params_.max_length);
  memset(search_state_.sequences_space.data(), 0, search_state_.sequences_space.size_bytes());
  sequences_.Init(search_state_.sequences_space, static_cast<int>(params_.batch_size), params_.sequence_length, params_.max_length);

  search_state_.sequence_lengths = AllocateBuffer<int32_t>(cpu_allocator, sequence_lengths_buffer_, params_.batch_size);
  search_state_.eos_meet = AllocateBuffer<bool>(cpu_allocator, eos_meet_buffer_, params_.batch_size);
  memset(search_state_.eos_meet.data(), 0, search_state_.eos_meet.size_bytes());

  search_state_.next_tokens = AllocateBuffer<int32_t>(cpu_allocator, next_tokens_buffer_, SafeInt<size_t>(params_.batch_size));

  // below buffers are on cpu or cuda
  size_t next_token_size = SafeInt<size_t>(params_.batch_size) * params_.vocab_size;
  search_state_.next_token_scores = AllocateBuffer<ScoreType>(allocator, next_token_scores_buffer_, next_token_size);
  search_state_.next_positions = AllocateBuffer<int32_t>(allocator, next_positions_buffer_, params_.batch_size);
  int64_t position_shape[]={params_.batch_size, 1};
  position_ids_ = OrtValue::CreateTensor<int32_t>(allocator->GetInfo(), search_state_.next_positions.data(), search_state_.next_positions.size(), position_shape, std::size(position_shape));

  model_.CreateInputs(search_state_.sequence_lengths);

  {
    auto shape = model.expanded_input_ids_->GetTensorTypeAndShapeInfo()->GetShape();
    size_t shape_elements = std::accumulate(shape.begin(), shape.end(), 1LL, std::multiplies<int64_t>());

    gsl::span<const int32_t> input_ids{model.expanded_input_ids_->GetTensorMutableData<int32_t>(), shape_elements};
    SetSequence(input_ids);
  }

  memset(search_state_.next_token_scores.data(), 0, search_state_.next_token_scores.size_bytes());
  memset(search_state_.next_tokens.data(), 0, search_state_.next_tokens.size_bytes());
  memset(search_state_.next_positions.data(), 0, search_state_.next_positions.size_bytes());

  gsl::copy(search_state_.sequence_lengths, search_state_.next_positions);
}

void Search::SetSequence(gsl::span<const int32_t> input_ids_in_cpu) {
  auto batch_beam_size = params_.BatchBeamSize();
  gsl::span<int32_t> sequences_0 = search_state_.sequences_space;
  for (size_t i = 0; i < batch_beam_size; i++) {
    for (int j = 0; j < params_.sequence_length; j++) {
      sequences_0[SafeInt<gsl::index>(i) * params_.max_length + j] =
          static_cast<int32_t>(input_ids_in_cpu[SafeInt<gsl::index>(i) * params_.sequence_length + j]);
    }
  }
}

void Search::RunModel() {
  if(model_.first_run_) {
    model_.first_run_=false;
  }
  else
    model_.UpdateInputs(search_state_.next_tokens, position_ids_.get(), params_.num_beams, sequences_.GetSequenceLength());
  model_.Run();

  // Logits has shape (batch_size, input_length, vocab_size),
  // where input_length equals to parameters_->sequence_length for first subgraph call, and 1 for the remaining calls.
  auto logits_shape = model_.logits_->GetTensorTypeAndShapeInfo()->GetShape();
  assert(logits_shape.size() == 3);
  const ScoreType* logits_data = model_.logits_->GetTensorMutableData<ScoreType>();

  auto input_length = logits_shape[1];

  // Get logits for the last token:
  //    next_token_logits = logits[:, -1, :], and the result shape is (batch_size, vocab_size)
  // When input_length == 1, use logits directly in SoftmaxCPU below so it only need for input_length > 1.
  gsl::span<ScoreType> next_token_scores = search_state_.next_token_scores;
  const ScoreType* current_logits = logits_data + (input_length - 1) * params_.vocab_size;
  for (int i = 0; i < params_.batch_size; i++) {
    gsl::span<const ScoreType> source(current_logits, params_.vocab_size);
    gsl::span<ScoreType> target = next_token_scores.subspan(SafeInt<gsl::index>(i) * params_.vocab_size,
                                                        static_cast<gsl::index>(params_.vocab_size));
    gsl::copy(source, target);
    current_logits += input_length * params_.vocab_size;
  }
}

#if 0
    if (do_sampling) {
      ORT_RETURN_IF_ERROR(SamplingCpuHelper::Sample(allocator,
                                                    thread_pool,
                                                    next_token_scores,
                                                    sampling_state,
                                                    greedy_state,
                                                    parameters,
                                                    dumper));
}
#endif

void Search::NextTokensFromLogits() {
  const ScoreType* logits_data = model_.logits_->GetTensorMutableData<ScoreType>();

  // next_tokens = torch.argmax(scores, dim=-1)
  for (size_t i = 0; i < params_.batch_size; i++) {
    int32_t best_token = 0;
    ScoreType best_score = logits_data[0];
    for (int32_t token = 1; token < params_.vocab_size; token++) {
      if (logits_data[token] > best_score) {
        best_score = logits_data[token];
        best_token = token;
      }
    }
    search_state_.next_tokens[i] = best_token;
    logits_data += params_.vocab_size;
  }
}

void Search::CheckForEOS() {

  // Look for EOS tokens, if seen set EOS flag and replace with pad token
  gsl::span<int32_t> next_tokens = search_state_.next_tokens;
  gsl::span<bool> eos_meet = search_state_.eos_meet;
  for (size_t batch_id = 0; batch_id < next_tokens.size(); ++batch_id) {
    if (next_tokens[batch_id] == params_.eos_token_id || eos_meet[batch_id] == true) {
      eos_meet[batch_id] = true;
      next_tokens[batch_id] = params_.pad_token_id;
    }
  }

  // When all batches are finished, stop earlier to avoid wasting computation.
  // TODO: Merge this with the above so we don't have to double scan. Just keep track of 'batches left'
  {
    gsl::span<bool> eos_meet = search_state_.eos_meet;
    size_t batch_id = 0;
    while (batch_id < eos_meet.size()) {
      if (eos_meet[batch_id] == false) {
        break;
      }
      ++batch_id;
    }
    if (batch_id == eos_meet.size()) {
      done_ = true;
      return;
    }
  }
}

void Search::AppendNextTokensToSequences() {
  sequences_.AppendNextTokenToSequences(search_state_.next_tokens);
  if (sequences_.GetSequenceLength()==params_.max_length)
    done_=true;
}

void Search::Finalize() {

  auto shape=output_sequences_->GetTensorTypeAndShapeInfo()->GetShape();
  size_t shape_count = std::accumulate(shape.begin(), shape.end(), 1LL, std::multiplies<int64_t>());

  // Copy the sequences to output
  gsl::span<int32_t> output{ output_sequences_->GetTensorMutableData<int32_t>(), shape_count};
  for (int batch_id = 0; batch_id < params_.batch_size; ++batch_id) {
    auto batch_output = output.subspan(
        static_cast<size_t>(batch_id) * params_.max_length,
        params_.max_length);
    gsl::span<const int32_t> sequence_source = sequences_.GetSequence(batch_id);
    gsl::copy(sequence_source, batch_output);
  }
}

gsl::span<ScoreType> Search::GetScores(int batch_beam_index) {
  assert(batch_beam_index >= 0 && batch_beam_index < params_.BatchBeamSize());
  return search_state_.next_token_scores.subspan(batch_beam_index * params_.vocab_size, params_.vocab_size);
}

namespace Processors {

void MinLength(Search &search, int min_length) {
  if (search.sequences_.GetSequenceLength() >= min_length)
    return;

  const int batch_beam_size = search.params_.BatchBeamSize();
  for (int i = 0; i < batch_beam_size; i++) {
    gsl::span<ScoreType> beam_token_scores = search.GetScores(i);
    beam_token_scores[search.params_.eos_token_id] = std::numeric_limits<ScoreType>::lowest();
  }
}

void RepetitionPenalty(Search& search, ScoreType penalty) {
  const int batch_beam_size = search.params_.BatchBeamSize();
  for (int i = 0; i < batch_beam_size; i++) {
    gsl::span<ScoreType> beam_token_scores = search.GetScores(i);
    gsl::span<const int32_t> sequence = search.sequences_.GetSequence(i);

    // Find unique word IDs in sequence.
    std::unordered_set<int32_t> unique_word_ids;
    for (const auto& word_id : sequence) {
      unique_word_ids.insert(word_id);
    }

    for (const int32_t word_id : unique_word_ids) {
      ScoreType score = beam_token_scores[word_id];

      // If score < 0, then repetition penalty > 1.0 has to multiplied to reduce the previous token probability,
      // This assumes that scores are either positive (like ctrl) or negative (like GPT-2), but not a mixture.
      beam_token_scores[word_id] = (score < 0 ? score * penalty : score / penalty);
    }
  }
}

}