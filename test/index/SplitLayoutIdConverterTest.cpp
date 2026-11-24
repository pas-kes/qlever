// Copyright 2026 The QLever Authors, in particular:
//
// 2026 Pascal Keßler <kesslerp@cs.uni-freiburg.de>, UFR
//
// UFR = University of Freiburg, Chair of Algorithms and Data Structures
//
// You may not use this file except in compliance with the Apache 2.0 License,
// which can be found in the `LICENSE` file at the root of the QLever project.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../util/GTestHelpers.h"
#include "../util/IndexTestHelpers.h"
#include "backports/algorithm.h"
#include "backports/filesystem.h"
#include "global/Constants.h"
#include "index/ConstantsIndexBuilding.h"
#include "index/FoldedIndexReader.h"
#include "index/Index.h"
#include "index/IndexFormatConverter.h"
#include "index/IndexImpl.h"
#include "index/SplitLayoutIdConverter.h"
#include "util/FilesystemHelpers.h"
#include "util/GlobalExecutor.h"
#include "util/json.h"

// These tests only run with `USE_SPLIT_LAYOUT` (see `CMakeLists.txt`).
namespace {

namespace fs = ql::filesystem;
using namespace qlever::splitLayoutIdConverter;
using namespace qlever::foldedIndexReader;
using FoldedId = foldedValueId::ValueId;
using ::testing::HasSubstr;

// The converter with the default encoded IRIs, which is what the tests of
// single `Id`s use.
Id convertId(uint64_t bits) {
  return FoldedIdConverter{nlohmann::json(EncodedIriManager{})}(bits);
}

// _____________________________________________________________________________
TEST(SplitLayoutIdConverter, convertIdKeepsTheValue) {
  // Integers and doubles keep their value, although their payload differs.
  for (int64_t value : {int64_t{0}, int64_t{17}, int64_t{-1}, int64_t{-17},
                        FoldedId::maxInt, -FoldedId::maxInt - 1}) {
    EXPECT_EQ(convertId(FoldedId::makeFromInt(value).getBits()),
              Id::makeFromInt(value));
  }
  for (double value : {0.0, 3.5, -0.125, 1e300, -1e-300}) {
    auto foldedId = FoldedId::makeFromDouble(value);
    EXPECT_EQ(convertId(foldedId.getBits()).getDouble(), foldedId.getDouble());
  }
  // An `Id` of type `LocalVocabIndex` (datatype bits 5) is never stored.
  AD_EXPECT_THROW_WITH_MESSAGE(
      convertId(FoldedId::fromBits(uint64_t{5}, 17).getBits()),
      HasSubstr("must never be stored on disk"));
}

// _____________________________________________________________________________
TEST(SplitLayoutIdConverter, convertEncodedIris) {
  // A plain prefix and a general pattern. With a general pattern, the plain
  // prefix is stored as a pattern as well (see `FoldedIdConverter`), so both
  // configurations are tested.
  using FoldedManager = FoldedIdConverter::FoldedEncodedIriManager;
  encodedIri::Pattern generalPattern{
      "https://example.org/p/",
      {encodedIri::Part{20, {}, "", encodedIri::NumberEncoding::Binary}}};
  for (const auto& foldedManager :
       {FoldedManager{{"https://example.org/id/"}},
        FoldedManager{{"https://example.org/id/"}, {generalPattern}}}) {
    nlohmann::json encodedIriPrefixes = foldedManager;
    FoldedIdConverter convert{encodedIriPrefixes};
    auto manager = encodedIriPrefixes.get<EncodedIriManager>();
    std::vector<Id> foldedIds;
    std::vector<Id> convertedIds;
    for (std::string_view iri :
         {"<https://example.org/id/1>", "<https://example.org/id/10>",
          "<https://example.org/id/2>",
          "<https://example.org/id/1234567890123>", "<https://example.org/p/5>",
          "<https://example.org/p/1000000>"}) {
      SCOPED_TRACE(iri);
      auto foldedId = foldedManager.encode(iri);
      if (!foldedId.has_value()) {
        // The general pattern is only part of the second configuration.
        continue;
      }
      auto bits = FoldedId::fromBits(Datatype::EncodedVal,
                                     foldedId.value().getEncodedVal())
                      .getBits();
      Id converted = convert(bits);
      EXPECT_EQ(converted, manager.encode(iri).value());
      foldedIds.push_back(foldedId.value());
      convertedIds.push_back(converted);
    }
    // The order of the encoded IRIs is preserved.
    for (size_t i = 0; i < foldedIds.size(); ++i) {
      for (size_t j = 0; j < foldedIds.size(); ++j) {
        EXPECT_EQ(foldedIds[i] < foldedIds[j],
                  convertedIds[i] < convertedIds[j]);
      }
    }
  }
}

// A fixture that copies a checked-in index with the folded layout into a fresh
// directory, converts it, and loads the converted index.
class SplitLayoutIdConverterTest : public ::testing::Test {
 protected:
  fs::path directory_;
  std::string oldBasename_;
  std::string newBasename_;

  void TearDown() override { fs::remove_all(directory_); }

  // Copy the index with the given `basename` from the subdirectory
  // `sourceDirectory` of the test data.
  void copyIndex(std::string_view sourceDirectory, std::string_view basename) {
    directory_ = fs::path{gtestCurrentTestName()};
    fs::remove_all(directory_);
    fs::create_directories(directory_ / "old");
    oldBasename_ = (directory_ / "old" / basename).string();
    newBasename_ = (directory_ / "converted" / basename).string();
    for (const auto& entry : fs::directory_iterator{
             fs::path{QLEVER_TEST_DATA_DIR} / sourceDirectory}) {
      std::string filename = entry.path().filename().string();
      if (ql::starts_with(filename, absl::StrCat(basename, "."))) {
        fs::copy_file(
            entry.path(),
            absl::StrCat(oldBasename_,
                         std::string_view{filename}.substr(basename.size())));
      }
    }
  }

  nlohmann::json convertedConfiguration() const {
    nlohmann::json configuration;
    ad_utility::makeIfstream(absl::StrCat(newBasename_, CONFIGURATION_FILE)) >>
        configuration;
    return configuration;
  }

  // Load the converted index and return all triples of the `permutation`.
  static std::vector<std::array<Id, 3>> scanAll(
      const Index& index, Permutation::Enum permutationEnum) {
    auto locatedTriples =
        index.deltaTriplesManager().getCurrentLocatedTriplesSharedState();
    const auto& permutation = index.getImpl().getPermutation(permutationEnum);
    IdTable table = permutation.scan(
        permutation.getScanSpecAndBlocks(
            ScanSpecification{std::nullopt, std::nullopt, std::nullopt},
            *locatedTriples),
        {}, std::make_shared<ad_utility::CancellationHandle<>>(),
        *locatedTriples, ad_utility::globalExecutor());
    std::vector<std::array<Id, 3>> triples;
    for (const auto& row : table) {
      triples.push_back({row[0], row[1], row[2]});
    }
    return triples;
  }

  Index loadConvertedIndex() const {
    Index index{ad_utility::makeUnlimitedAllocator<Id>()};
    index.usePatterns() = true;
    index.loadAllPermutations() = true;
    index.createFromOnDiskIndex(newBasename_, false);
    return index;
  }
};

// _____________________________________________________________________________
TEST_F(SplitLayoutIdConverterTest, convertedIndexHasTheSameContent) {
  // An index with all datatypes, patterns, a text index, and a materialized
  // view, see the `README.md` of the index.
  copyIndex("foldedIndex", "folded");
  convertIndexToSplitLayout(oldBasename_, newBasename_,
                            ad_utility::globalExecutor());
  auto configuration = convertedConfiguration();
  EXPECT_EQ(configuration.at("index-format-version")
                .get<qlever::IndexFormatVersion>(),
            qlever::indexFormatVersion);
  EXPECT_TRUE(configuration.contains(std::string{SPLIT_ID_LAYOUT_KEY}));

  auto index = loadConvertedIndex();
  auto spo = scanAll(index, Permutation::SPO);
  EXPECT_EQ(spo.size(), 20);
  ad_utility::HashSet<Datatype> datatypes;
  for (const auto& triple : spo) {
    for (Id id : triple) {
      datatypes.insert(id.getDatatype());
    }
  }
  EXPECT_THAT(datatypes,
              ::testing::UnorderedElementsAre(
                  Datatype::VocabIndex, Datatype::BlankNodeIndex,
                  Datatype::EncodedVal, Datatype::Int, Datatype::Double,
                  Datatype::Bool, Datatype::Date, Datatype::GeoPoint));
  // The encoded IRIs are the last subjects of `SPO`.
  EXPECT_EQ(spo.back()[0], index.getImpl()
                               .encodedIriManager()
                               .encode("<https://example.org/id/456>")
                               .value());
  for (auto permutation : Permutation::ALL) {
    EXPECT_TRUE(ql::ranges::is_sorted(scanAll(index, permutation)));
  }

  // A converted index already has the split layout.
  AD_EXPECT_THROW_WITH_MESSAGE(
      convertIndexToSplitLayout(newBasename_, (directory_ / "again").string(),
                                ad_utility::globalExecutor()),
      HasSubstr("nothing to convert"));
}

// _____________________________________________________________________________
TEST_F(SplitLayoutIdConverterTest, indexInThePreviousFormat) {
  // A change of the format is the job of the `IndexFormatConverter`.
  copyIndex("oldIndexFormat", "oldFormat");
  AD_EXPECT_THROW_WITH_MESSAGE(
      convertIndexToSplitLayout(oldBasename_, newBasename_,
                                ad_utility::globalExecutor()),
      HasSubstr("is not in a current index format"));
}

// _____________________________________________________________________________
TEST_F(SplitLayoutIdConverterTest, upgradeIndexInPlace) {
  copyIndex("foldedIndex", "folded");
  upgradeIndexInPlace(oldBasename_, ad_utility::globalExecutor());
  nlohmann::json configuration;
  ad_utility::makeIfstream(absl::StrCat(oldBasename_, CONFIGURATION_FILE)) >>
      configuration;
  EXPECT_TRUE(configuration.contains(std::string{SPLIT_ID_LAYOUT_KEY}));

  // The index in place now has the split layout.
  AD_EXPECT_THROW_WITH_MESSAGE(
      upgradeIndexInPlace(oldBasename_, ad_utility::globalExecutor()),
      HasSubstr("nothing to convert"));
}

// _____________________________________________________________________________
TEST_F(SplitLayoutIdConverterTest, irisThatTheSplitLayoutEncodes) {
  // The IRIs with 14 digits are words of the vocabulary of this index, but the
  // split layout encodes them (see the `README.md` of the index), so the index
  // cannot be converted.
  copyIndex("foldedIndexWithLongIris", "longIris");
  AD_EXPECT_THROW_WITH_MESSAGE(
      convertIndexToSplitLayout(oldBasename_, newBasename_,
                                ad_utility::globalExecutor()),
      ::testing::AllOf(HasSubstr("12345678901234"),
                       HasSubstr("please rebuild it")));
}
}  // namespace
