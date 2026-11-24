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
#include "backports/filesystem.h"
#include "engine/MaterializedViews.h"
#include "global/Constants.h"
#include "global/MaterializedViewConstants.h"
#include "index/ConstantsIndexBuilding.h"
#include "index/IndexConverterBase.h"
#include "index/IndexFormatVersion.h"
#include "util/File.h"
#include "util/FilesystemHelpers.h"
#include "util/json.h"

// The steps that both index converters share. They only check and move files
// and do not read any `Id`, so they are tested with an index of this binary,
// in both layouts.
namespace {

namespace fs = ql::filesystem;
using namespace qlever::indexConverterBase;
using ::testing::HasSubstr;

// A fixture that builds a small index in a fresh directory.
class IndexConverterBaseTest : public ::testing::Test {
 protected:
  static constexpr std::string_view indexName_ = "index";
  fs::path directory_;
  std::string oldBasename_;
  std::string newBasename_;

  void SetUp() override {
    directory_ = fs::path{gtestCurrentTestName()};
    fs::remove_all(directory_);
    fs::create_directories(directory_);
    oldBasename_ = (directory_ / indexName_).string();
    newBasename_ = (directory_ / "converted" / indexName_).string();
    ad_utility::testing::makeTestIndex(
        oldBasename_, ad_utility::testing::TestIndexConfig{"<a> <b> <c> ."});
  }

  void TearDown() override { fs::remove_all(directory_); }

  // The names of the files of the index in the given `directory`.
  static std::vector<std::string> filenames(const fs::path& directory) {
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator{directory}) {
      if (entry.is_regular_file()) {
        names.push_back(entry.path().filename().string());
      }
    }
    return names;
  }
};

// _____________________________________________________________________________
TEST_F(IndexConverterBaseTest, checkBasenames) {
  checkBasenames(oldBasename_, newBasename_);
  AD_EXPECT_THROW_WITH_MESSAGE(checkBasenames("", newBasename_),
                               HasSubstr("must not be empty"));
  AD_EXPECT_THROW_WITH_MESSAGE(checkBasenames(oldBasename_, ""),
                               HasSubstr("must not be empty"));
  // The comparison normalizes the paths, so also a base name of the same index
  // that is spelled differently is caught.
  for (const auto& newBasename :
       {oldBasename_, absl::StrCat("./", oldBasename_)}) {
    AD_EXPECT_THROW_WITH_MESSAGE(checkBasenames(oldBasename_, newBasename),
                                 HasSubstr("has to differ from the base name"));
  }
}

// _____________________________________________________________________________
TEST_F(IndexConverterBaseTest, checkNewBasenameIsUnused) {
  // The directory of the converted index is created.
  checkNewBasenameIsUnused(newBasename_);
  EXPECT_TRUE(fs::is_directory(fs::path{newBasename_}.parent_path()));
  AD_EXPECT_THROW_WITH_MESSAGE(checkNewBasenameIsUnused(oldBasename_),
                               HasSubstr("must not overwrite"));
}

// _____________________________________________________________________________
TEST_F(IndexConverterBaseTest, readConfiguration) {
  EXPECT_EQ(readConfiguration(oldBasename_)
                .at("index-format-version")
                .get<qlever::IndexFormatVersion>(),
            qlever::indexFormatVersion);

  AD_EXPECT_THROW_WITH_MESSAGE(
      readConfiguration((directory_ / "doesNotExist").string()),
      HasSubstr("is not the base name of a QLever index"));

  // An index without a version.
  auto configuration = readConfiguration(oldBasename_);
  configuration.erase("index-format-version");
  ad_utility::makeOfstream(absl::StrCat(oldBasename_, CONFIGURATION_FILE))
      << configuration.dump();
  AD_EXPECT_THROW_WITH_MESSAGE(readConfiguration(oldBasename_),
                               HasSubstr("before versioning was introduced"));
}

// _____________________________________________________________________________
TEST_F(IndexConverterBaseTest, throwIfPersistedUpdatesExist) {
  throwIfPersistedUpdatesExist(oldBasename_);
  ad_utility::makeOfstream(absl::StrCat(oldBasename_, UPDATE_TRIPLES_SUFFIX))
      << "irrelevant content";
  AD_EXPECT_THROW_WITH_MESSAGE(throwIfPersistedUpdatesExist(oldBasename_),
                               HasSubstr("has persisted updates"));
}

// _____________________________________________________________________________
TEST_F(IndexConverterBaseTest, indexWithOnlyOnePermutationOfAPair) {
  // `PSO` and `POS` are the first pair, so no permutation is converted.
  ad_utility::deleteFile(
      filenameForPermutation(oldBasename_, Permutation::POS, false));
  std::vector<fs::path> handledFiles;
  AD_EXPECT_THROW_WITH_MESSAGE(
      convertPermutationPairs(
          oldBasename_, newBasename_, {}, handledFiles,
          [](Permutation::Enum, bool, const std::function<void(size_t)>&) {
            ADD_FAILURE() << "No permutation must be converted";
            return IndexMetaData{};
          }),
      HasSubstr("has only one of the permutations"));
}

// _____________________________________________________________________________
TEST_F(IndexConverterBaseTest, materializedViews) {
  // The files of a view, with only the version in its info file, which is all
  // that is read here.
  std::string viewBasename = materializedViewFilenameBase(oldBasename_, "view");
  for (auto suffix : VIEW_ALL_SUFFIXES) {
    ad_utility::makeOfstream(absl::StrCat(viewBasename, suffix))
        << nlohmann::json{{"version", MATERIALIZED_VIEWS_VERSION}}.dump();
  }
  EXPECT_THAT(findMaterializedViews(oldBasename_),
              ::testing::ElementsAre("view"));

  checkNewBasenameIsUnused(newBasename_);
  copyViewInfo(oldBasename_, "view", newBasename_, MATERIALIZED_VIEWS_VERSION);
  EXPECT_TRUE(fs::exists(absl::StrCat(
      materializedViewFilenameBase(newBasename_, "view"), VIEW_INFO_SUFFIX)));
  AD_EXPECT_THROW_WITH_MESSAGE(
      copyViewInfo(oldBasename_, "view", newBasename_,
                   MATERIALIZED_VIEWS_VERSION + 1),
      HasSubstr("only converts views in the format version"));

  // A view of which only some of its files exist. The info file must stay,
  // because the views are found by exactly those files.
  ad_utility::deleteFile(absl::StrCat(viewBasename, VIEW_SPO_SUFFIX));
  AD_EXPECT_THROW_WITH_MESSAGE(
      findMaterializedViews(oldBasename_),
      HasSubstr("files that belong to a materialized view"));
}

// _____________________________________________________________________________
// The in-place upgrade stages the converted index in a subdirectory, checks it,
// and only then retires the original index to another subdirectory and moves
// the converted index to its base name.
TEST_F(IndexConverterBaseTest, upgradeIndexInPlace) {
  auto filesBefore = filenames(directory_);
  // A conversion that copies all files of the index.
  auto copyIndex = [this](const std::string& stagingBasename) {
    checkNewBasenameIsUnused(stagingBasename);
    for (const auto& filename : filenames(directory_)) {
      fs::copy_file(
          directory_ / filename,
          absl::StrCat(stagingBasename,
                       std::string_view{filename}.substr(indexName_.size())));
    }
  };
  upgradeIndexInPlace(oldBasename_, readConfiguration(oldBasename_), "staging.",
                      "retired.", copyIndex);

  EXPECT_THAT(filenames(directory_),
              ::testing::UnorderedElementsAreArray(filesBefore));
  auto retiredDirs =
      qlever::util::directoriesWithPrefix(directory_, "retired.");
  ASSERT_EQ(retiredDirs.size(), 1u);
  EXPECT_THAT(filenames(retiredDirs.front()),
              ::testing::UnorderedElementsAreArray(filesBefore));
  EXPECT_TRUE(
      qlever::util::directoriesWithPrefix(directory_, "staging.").empty());
}

}  // namespace
