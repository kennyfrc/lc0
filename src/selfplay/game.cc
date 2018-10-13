/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2018 The LCZero Authors

  Leela Chess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Leela Chess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.

  Additional permission under GNU GPL version 3 section 7

  If you modify this Program, or any covered work, by linking or
  combining it with NVIDIA Corporation's libraries from the NVIDIA CUDA
  Toolkit and the NVIDIA CUDA Deep Neural Network library (or a
  modified version of those libraries), containing parts covered by the
  terms of the respective license agreement, the licensors of this
  Program grant you additional permission to convey the resulting work.
*/

#include "selfplay/game.h"
#include <algorithm>

#include "neural/writer.h"
#include "utils/epdqueue.h"

namespace lczero {

namespace {
const char* kReuseTreeStr = "Reuse the node statistics between moves";
const char* kResignPercentageStr = "Resign when win percentage drops below n";
const char* kSyzygyTablebaseStr = "List of Syzygy tablebase directories";

}  // namespace

    SyzygyTablebase* SelfPlayGame::GetTB(std::string path) {
        static SyzygyTablebase tb;
        static bool done = false;

        if (!path.empty() && !done) {
            done = true;
            std::cerr << "Loading Syzygy tablebases from " << path << std::endl;
            if (!tb.init(path)) {
                std::cerr << "Failed to load Syzygy tablebases!" << std::endl;
            } else {
                std::cerr << "Loaded Syzygy tablebases!" << std::endl;
            }
        }

        return &tb;
    }

void SelfPlayGame::PopulateUciParams(OptionsParser* options) {
  options->Add<BoolOption>(kReuseTreeStr, "reuse-tree") = false;
  options->Add<FloatOption>(kResignPercentageStr, 0.0f, 100.0f,
                            "resign-percentage", 'r') = 0.0f;
}

SelfPlayGame::SelfPlayGame(PlayerOptions player1, PlayerOptions player2,
                           bool shared_tree)
    : options_{player1, player2} {

  // init TB

  std::string tb_paths = options_[0].uci_options->Get<std::string>(kSyzygyTablebaseStr);
  if (!tb_paths.empty()) {
    syzygy_tb_ = GetTB(tb_paths);
  }

  tree_[0] = std::make_shared<NodeTree>();

  std::string epd = epdqueue::Pop();
  if (epd == "") {
    // out of epd's
    abort_ = true;
    return;
  } else {
    epd.append(" 0 1");
  }

  tree_[0]->ResetToPosition(epd, {});

  if (shared_tree) {
    tree_[1] = tree_[0];
  } else {
    tree_[1] = std::make_shared<NodeTree>();
    tree_[1]->ResetToPosition(epd, {});
  }
}

void SelfPlayGame::Play(int white_threads, int black_threads,
                        bool training, bool enable_resign) {
  bool blacks_move = false;

  // Do moves while not end of the game. (And while not abort_)
  while (!abort_) {
    game_result_ = tree_[0]->GetPositionHistory().ComputeGameResult();

    // If endgame, stop.
    if (game_result_ != GameResult::UNDECIDED) break;

    // Initialize search.
    const int idx = blacks_move ? 1 : 0;
    if (!options_[idx].uci_options->Get<bool>(kReuseTreeStr)) {
      tree_[idx]->TrimTreeAtHead();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (abort_) break;
      search_ = std::make_unique<Search>(
          *tree_[idx], options_[idx].network, options_[idx].best_move_callback,
          options_[idx].info_callback, options_[idx].search_limits,
          *options_[idx].uci_options, options_[idx].cache, syzygy_tb_);
    }

    // Do search.
    search_->RunBlocking(blacks_move ? black_threads : white_threads);
    if (abort_) break;

    if (training) {
      // Append training data. The GameResult is later overwritten.
      training_data_.push_back(tree_[idx]->GetCurrentHead()->GetV3TrainingData(
          GameResult::UNDECIDED, tree_[idx]->GetPositionHistory()));
    }

    float eval = search_->GetBestEval();
    eval = (eval + 1) / 2;
    if (eval < min_eval_[idx]) min_eval_[idx] = eval;
    if (enable_resign) {
      const float resignpct =
          options_[idx].uci_options->Get<float>(kResignPercentageStr) / 100;
      if (eval < resignpct) {  // always false when resignpct == 0
        game_result_ =
            blacks_move ? GameResult::WHITE_WON : GameResult::BLACK_WON;
        break;
      }
    }

    // Add best move to the tree.
    Move move = search_->GetBestMove().first;
    tree_[0]->MakeMove(move);
    if (tree_[0] != tree_[1]) tree_[1]->MakeMove(move);
    blacks_move = !blacks_move;
  }
}

std::vector<Move> SelfPlayGame::GetMoves() const {
  std::vector<Move> moves;
  bool flip = !tree_[0]->IsBlackToMove();
  for (Node* node = tree_[0]->GetCurrentHead();
       node != tree_[0]->GetGameBeginNode(); node = node->GetParent()) {
    moves.push_back(node->GetParent()->GetEdgeToNode(node)->GetMove(flip));
    flip = !flip;
  }
  std::reverse(moves.begin(), moves.end());
  return moves;
}

float SelfPlayGame::GetWorstEvalForWinnerOrDraw() const {
  if (game_result_ == GameResult::WHITE_WON) return min_eval_[0];
  if (game_result_ == GameResult::BLACK_WON) return min_eval_[1];
  return std::min(min_eval_[0], min_eval_[1]);
}

void SelfPlayGame::Abort() {
  std::lock_guard<std::mutex> lock(mutex_);
  abort_ = true;
  if (search_) search_->Abort();
}

void SelfPlayGame::WriteTrainingData(TrainingDataWriter* writer) const {
  assert(!training_data_.empty());
  bool black_to_move =
      tree_[0]->GetPositionHistory().Starting().IsBlackToMove();
  for (auto chunk : training_data_) {
    if (game_result_ == GameResult::WHITE_WON) {
      chunk.result = black_to_move ? -1 : 1;
    } else if (game_result_ == GameResult::BLACK_WON) {
      chunk.result = black_to_move ? 1 : -1;
    } else {
      chunk.result = 0;
    }
    writer->WriteChunk(chunk);
    black_to_move = !black_to_move;
  }
}

}  // namespace lczero
