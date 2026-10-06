// Copyright 2026 The QLever Authors, in particular:
//
// 2026 Pascal Keßler <kesslerp@informatik.uni-freiburg.de>, UFR
//
// UFR = University of Freiburg, Chair of Algorithms and Data Structures
//
// You may not use this file except in compliance with the Apache 2.0 License,
// which can be found in the `LICENSE` file at the root of the QLever project.

// Compare the lambdas `Id::isUndefinedL`, `Id::getDatatypeL` and
// `Id::getBitsL` with the pointer-to-member functions that they replace.
// NOTE: `Id::getBits` returns an integer for the legacy `Id`, so this
// benchmark only works for the legacy layout.

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../infrastructure/Benchmark.h"
#include "engine/idTable/IdTable.h"
#include "global/Id.h"
#include "index/ConstantsIndexBuilding.h"
#include "util/Log.h"

namespace ad_benchmark {
namespace {
constexpr size_t numIds = 10000000;

std::vector<Id> makeIds(const size_t numDistinct = numIds) {
  std::vector<Id> ids;
  ids.reserve(numIds);
  for (size_t i = 0; i < numIds; ++i) {
    ids.push_back(Id::makeFromVocabIndex(VocabIndex::make(i % numDistinct)));
  }
  return ids;
}

class LambdaReplacementBenchmark : public BenchmarkInterface {
 public:
  [[nodiscard]] std::string name() const final;

  BenchmarkResults runAllBenchmarks() final {
    BenchmarkResults results{};

    // One group per position of the UNDEF value (an UNDEF value at the start
    // is not interesting, the search would end instant).
    const std::array<std::pair<std::string, std::optional<size_t> >, 3>
        positions{{{"no UNDEF", std::nullopt},
                   {"UNDEF in the middle", numIds / 2},
                   {"UNDEF at the end", numIds - 1}}};
    for (const auto& [groupName, positionOfUndefined] : positions) {
      auto& group = results.addGroup(groupName);
      auto ids = makeIds();
      if (positionOfUndefined.has_value()) {
        ids.at(positionOfUndefined.value()) = Id::makeUndefined();
      }
      const ConstIdColumnRef column{ids};
      // To ensure the first run is not influenced by cold caches
      AD_LOG_INFO << "Warm up " << ql::ranges::any_of(column, Id::isUndefinedL)
                  << std::endl;

      // Id::isUndefinedL vs &Id::isUndefined
      group.addMeasurement("any_of(column, Id::isUndefinedL)", [&column] {
        AD_LOG_INFO << "Found undefined "
                    << ql::ranges::any_of(column, Id::isUndefinedL)
                    << std::endl;
      });
      group.addMeasurement("any_of(column, &Id::isUndefined)", [&column] {
        AD_LOG_INFO << "Found undefined "
                    << ql::ranges::any_of(column, &Id::isUndefined)
                    << std::endl;
      });

      // Id::getDatatypeL vs &Id::getDatatype
      group.addMeasurement(
          "find(column, Datatype::Undefined, Id::getDatatypeL)", [&column] {
            AD_LOG_INFO << "Found undefined "
                        << (ql::ranges::find(column, Datatype::Undefined,
                                             Id::getDatatypeL) != column.end())
                        << std::endl;
          });
      group.addMeasurement(
          "find(column, Datatype::Undefined, &Id::getDatatype)", [&column] {
            AD_LOG_INFO << "Found undefined "
                        << (ql::ranges::find(column, Datatype::Undefined,
                                             &Id::getDatatype) != column.end())
                        << std::endl;
          });
    }

    // Id::getBitsL vs &Id::getBits, analog to the projection of the `find` in
    // `computeDistinctGraphs`: every `Id` of the column is searched in a
    // small array of known graphs.
    auto& graphGroup = results.addGroup("find in known graphs");
    const auto ids = makeIds(MAX_NUM_GRAPHS_STORED_IN_BLOCK_METADATA);

    // Warm up caches.
    AD_LOG_INFO << "Warm up " << ql::ranges::count(ids, ids.front())
                << std::endl;

    // A vector instead of an array, because in `computeDistinctGraphs` the
    // number of known graphs is not known at compile time either.
    std::vector<Id> graphs;
    graphs.reserve(MAX_NUM_GRAPHS_STORED_IN_BLOCK_METADATA);

    for (size_t i = 0; i < MAX_NUM_GRAPHS_STORED_IN_BLOCK_METADATA; ++i) {
      graphs.push_back(Id::makeFromVocabIndex(VocabIndex::make(i)));
    }
    graphGroup.addMeasurement(
        "find(graphs, id.getBits(), Id::getBitsL)", [&ids, &graphs] {
          size_t numFound = 0;
          for (const Id id : ids) {
            numFound += ql::ranges::find(graphs, id.getBits(), Id::getBitsL) !=
                        graphs.end();
          }
          AD_LOG_INFO << "Found " << numFound << std::endl;
        });
    graphGroup.addMeasurement(
        "find(graphs, id.getBits(), &Id::getBits)", [&ids, &graphs] {
          size_t numFound = 0;
          for (const Id id : ids) {
            numFound += ql::ranges::find(graphs, id.getBits(), &Id::getBits) !=
                        graphs.end();
          }
          AD_LOG_INFO << "Found " << numFound << std::endl;
        });
    return results;
  }
};
}  // namespace

std::string LambdaReplacementBenchmark::name() const {
  return "Benchmarks for the replaced pointer-to-member lambdas";
}

AD_REGISTER_BENCHMARK(LambdaReplacementBenchmark);
}  // namespace ad_benchmark
