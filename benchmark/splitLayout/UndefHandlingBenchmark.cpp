// Copyright 2026 The QLever Authors, in particular:
//
// 2026 Pascal Keßler <kesslerp@informatik.uni-freiburg.de>, UFR
//
// UFR = University of Freiburg, Chair of Algorithms and Data Structures
//
// You may not use this file except in compliance with the Apache 2.0 License,
// which can be found in the `LICENSE` file at the root of the QLever project.

// Benchmarks for the places where UNDEF values in join columns are handled:
// the `isCheap` check in `MultiColumnJoin::computeMultiColumnJoin` (with
// `Id::isUndefinedL` and with `&Id::isUndefined`) and the whole
// `MultiColumnJoin`, with and without UNDEF values in the join column (without
// UNDEF the join takes the cheap path).

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../test/util/AllocatorTestHelpers.h"
#include "../../test/util/IdTableHelpers.h"
#include "../../test/util/IndexTestHelpers.h"
#include "../infrastructure/Benchmark.h"
#include "engine/MultiColumnJoin.h"
#include "engine/idTable/IdTable.h"
#include "global/Id.h"
#include "util/Log.h"

namespace ad_benchmark {
namespace {
constexpr size_t numRows = 2'000'000;
constexpr size_t numJoinRows = 1'000'000;
using Row = std::array<Id, 2>;

Id makeId(const size_t i) {
  return Id::makeFromVocabIndex(VocabIndex::make(i));
}

// A table with two columns and `numRows` rows without UNDEF. If
// `posUndefined` is set, the first column of that row is UNDEF.
IdTable makeTable(std::optional<size_t> posUndefined) {
  IdTable table(2, ad_utility::testing::makeAllocator());
  table.reserve(numRows);
  for (size_t i = 0; i < numRows; ++i) {
    const bool isUndefined = posUndefined.has_value() && posUndefined == i;
    table.push_back({isUndefined ? Id::makeUndefined() : makeId(i), makeId(i)});
  }
  return table;
}

// A table with two columns and `numJoinRows` rows, sorted by both columns. If
// `withUndef` is set, the first column of every 100th row is UNDEF.
IdTable makeSortedTable(const bool withUndef) {
  std::vector<Row> rows;
  rows.reserve(numJoinRows);
  for (size_t i = 0; i < numJoinRows; ++i) {
    rows.push_back({withUndef && i % 100 == 0 ? Id::makeUndefined() : makeId(i),
                    makeId(i)});
  }
  ql::ranges::sort(rows);
  IdTable table(2, ad_utility::testing::makeAllocator());
  table.reserve(numJoinRows);
  for (const Row& row : rows) {
    table.push_back({row[0], row[1]});
  }
  return table;
}

class UndefHandlingBenchmark : public BenchmarkInterface {
 public:
  [[nodiscard]] std::string name() const final;

  BenchmarkResults runAllBenchmarks() final {
    BenchmarkResults results{};

    // isCheap Check
    const std::vector<std::array<ColumnIndex, 2> > joinColumns{{0, 0}, {1, 1}};
    const auto right = makeTable(std::nullopt);
    const std::array<std::pair<std::string, std::optional<size_t> >, 3>
        positions{{{"no UNDEF", std::nullopt},
                   {"UNDEF in the middle", numRows / 2},
                   {"UNDEF at the end", numRows - 1}}};
    for (const auto& [groupName, positionOfUndefined] : positions) {
      auto& group = results.addGroup("isCheap, " + groupName);
      const IdTable left = makeTable(positionOfUndefined);

      // Warm up caches to get a comparable testsetup
      AD_LOG_INFO << "Warm up "
                  << ql::ranges::none_of(
                         joinColumns,
                         [&](const auto& jcs) {
                           auto [leftCol, rightCol] = jcs;
                           return ql::ranges::any_of(right.getColumn(rightCol),
                                                     Id::isUndefinedL) ||
                                  ql::ranges::any_of(left.getColumn(leftCol),
                                                     Id::isUndefinedL);
                         })
                  << std::endl;

      group.addMeasurement("isCheap with Id::isUndefinedL", [&] {
        AD_LOG_INFO << "isCheap "
                    << ql::ranges::none_of(
                           joinColumns,
                           [&](const auto& jcs) {
                             auto [leftCol, rightCol] = jcs;
                             return ql::ranges::any_of(
                                        right.getColumn(rightCol),
                                        Id::isUndefinedL) ||
                                    ql::ranges::any_of(left.getColumn(leftCol),
                                                       Id::isUndefinedL);
                           })
                    << std::endl;
      });
      group.addMeasurement("isCheap with &Id::isUndefined", [&] {
        AD_LOG_INFO << "isCheap "
                    << ql::ranges::none_of(
                           joinColumns,
                           [&](const auto& jcs) {
                             auto [leftCol, rightCol] = jcs;
                             return ql::ranges::any_of(
                                        right.getColumn(rightCol),
                                        &Id::isUndefined) ||
                                    ql::ranges::any_of(left.getColumn(leftCol),
                                                       &Id::isUndefined);
                           })
                    << std::endl;
      });
    }

    // The whole join. Without UNDEF in the join columns, `isCheap` is true and
    // the join takes the cheap path, with UNDEF the generic path (which uses
    // `findSmallerUndefRanges`). Every left row finds exactly one partner.
    auto* qec = ad_utility::testing::getQec();
    const std::array<std::pair<std::string, bool>, 2> joinCases{
        {{"no UNDEF (cheap path)", false},
         {"1% UNDEF in the left join column", true}}};
    for (const auto& [groupName, withUndef] : joinCases) {
      auto& joinGroup = results.addGroup("MultiColumnJoin, " + groupName);
      const IdTable joinLeft = makeSortedTable(withUndef);
      const IdTable joinRight = makeSortedTable(false);
      MultiColumnJoin join{qec, idTableToExecutionTree(qec, joinLeft),
                           idTableToExecutionTree(qec, joinRight)};
      auto computeJoin = [&] {
        IdTable result(2, ad_utility::testing::makeAllocator());
        join.computeMultiColumnJoin(joinLeft.asStaticView<0>(),
                                    joinRight.asStaticView<0>(), joinColumns,
                                    &result);
        return result.size();
      };

      // Join once before measuring, see above.
      AD_LOG_INFO << "Warm up " << computeJoin() << std::endl;

      joinGroup.addMeasurement("computeMultiColumnJoin", [&] {
        AD_LOG_INFO << "Result rows " << computeJoin() << std::endl;
      });
    }
    return results;
  }
};
}  // namespace

std::string UndefHandlingBenchmark::name() const {
  return "Benchmarks for the UNDEF handling in joins";
}

AD_REGISTER_BENCHMARK(UndefHandlingBenchmark);
}  // namespace ad_benchmark
