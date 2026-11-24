// Copyright 2026 The QLever Authors, in particular:
//
// 2026 Pascal Keßler <kesslerp@cs.uni-freiburg.de>, UFR
//
// UFR = University of Freiburg, Chair of Algorithms and Data Structures
//
// You may not use this file except in compliance with the Apache 2.0 License,
// which can be found in the `LICENSE` file at the root of the QLever project.

#include "index/SplitLayoutIdConverter.h"

#include <absl/strings/str_cat.h>

#include "global/MaterializedViewConstants.h"
#include "index/ConstantsIndexBuilding.h"
#include "index/FoldedIndexReader.h"
#include "index/IndexConverterBase.h"
#include "index/IndexFormatConverter.h"
#include "index/IndexImpl.h"
#include "index/PatternCreator.h"

namespace qlever::splitLayoutIdConverter {

using namespace qlever::indexConverterBase;
using namespace qlever::foldedIndexReader;
namespace fs = ql::filesystem;

namespace {
// Read the configuration of the index with the base name `basename` and check
// that it has the folded layout and a current format.
nlohmann::json readAndCheckConfiguration(const std::string& basename) {
  auto configuration = readConfiguration(basename);
  if (configuration.contains(std::string{SPLIT_ID_LAYOUT_KEY})) {
    throw std::runtime_error{absl::StrCat(
        "The index \"", basename,
        "\" already has the split layout of `Id`, so there is nothing to "
        "convert.")};
  }
  auto version =
      configuration.at("index-format-version").get<IndexFormatVersion>();
  // A format change is the job of the `IndexFormatConverter`, with a binary
  // without `USE_SPLIT_LAYOUT` for an index with the folded layout.
  if (!isLoadableIndexFormatVersion(version)) {
    throw std::runtime_error{absl::StrCat(
        "The index \"", basename,
        "\" is not in a current index format. Please first upgrade it with "
        "the `qlever-upgrade-index` of a QLever binary that was built without "
        "`USE_SPLIT_LAYOUT`, then convert it to the split layout.")};
  }
  return configuration;
}

// Convert the permutation in the file `oldFilename` to `newFilename` and return
// its metadata (see `writeConvertedPermutation`).
IndexMetaData convertPermutationFile(
    const std::string& oldFilename, const std::string& newFilename,
    size_t rowsPerBlock, const FoldedIdConverter& convert,
    const std::function<void(size_t)>& progress,
    const qlever::Executor& executor) {
  // The `Id`s of the metadata are already converted when it is read.
  auto oldMetaData = readMetaData(oldFilename, convert);
  return writeConvertedPermutation(
      oldMetaData,
      readBlocks(oldFilename, oldMetaData, convert, progress, executor),
      [](Id id) { return id; }, newFilename, rowsPerBlock, executor);
}

// Convert all permutations, like `indexFormatConverter::convertPermutations`.
void convertPermutations(const std::string& oldBasename,
                         const std::string& newBasename,
                         const Index::NumNormalAndInternal& numTriples,
                         size_t rowsPerBlock, const FoldedIdConverter& convert,
                         std::vector<fs::path>& handledFiles,
                         const qlever::Executor& executor) {
  convertPermutationPairs(
      oldBasename, newBasename, numTriples, handledFiles,
      [&](Permutation::Enum permutation, bool isInternal,
          const std::function<void(size_t)>& progress) {
        return convertPermutationFile(
            filenameForPermutation(oldBasename, permutation, isInternal),
            filenameForPermutation(newBasename, permutation, isInternal),
            rowsPerBlock, convert, progress, executor);
      });
}

// Convert the patterns, if the index has them.
void convertPatterns(const std::string& oldBasename,
                     const std::string& newBasename,
                     const FoldedIdConverter& convert,
                     std::vector<fs::path>& handledFiles) {
  std::string oldFilename = absl::StrCat(oldBasename, PATTERNS_FILE_SUFFIX);
  if (!fs::exists(oldFilename)) {
    return;
  }
  handledFiles.emplace_back(oldFilename);
  PatternStatistics statistics;
  auto patterns = readPatterns(oldFilename, convert, statistics);
  PatternCreator::writePatternsToFile(
      absl::StrCat(newBasename, PATTERNS_FILE_SUFFIX), patterns, statistics);
}

// Convert the materialized views. A view is a single `SPO` permutation without
// a twin.
void convertMaterializedViews(const std::string& oldBasename,
                              const std::string& newBasename,
                              size_t rowsPerBlock,
                              const FoldedIdConverter& convert,
                              const qlever::Executor& executor) {
  for (const auto& name : findMaterializedViews(oldBasename)) {
    AD_LOG_INFO << "Converting the materialized view \"" << name << "\" ..."
                << std::endl;
    std::string newViewBasename =
        materializedViewFilenameBase(newBasename, name);
    std::string newFilename = absl::StrCat(newViewBasename, VIEW_SPO_SUFFIX);
    auto newMetaData = convertPermutationFile(
        absl::StrCat(materializedViewFilenameBase(oldBasename, name),
                     VIEW_SPO_SUFFIX),
        newFilename, rowsPerBlock, convert, [](size_t) {}, executor);
    newMetaData.setName(newViewBasename);
    writeMetaData(newMetaData, newFilename);
    copyViewInfo(oldBasename, name, newBasename, MATERIALIZED_VIEWS_VERSION);
  }
}

// Write the `configuration` of the converted index, which only gets the split
// layout. This is the last step: an index without its configuration file
// cannot be loaded, so an interrupted conversion is not mistaken for a
// complete one.
void writeConfiguration(const std::string& newBasename,
                        nlohmann::json configuration) {
  configuration[std::string{SPLIT_ID_LAYOUT_KEY}] = true;
  ad_utility::makeOfstream(absl::StrCat(newBasename, CONFIGURATION_FILE))
      << configuration.dump(4) << std::endl;
}
}  // namespace

// _____________________________________________________________________________
std::string conversionDescription() {
  return absl::StrCat(
      "Convert an index with the folded layout of the IDs to the split layout "
      "of this binary (`USE_SPLIT_LAYOUT`) in place. The index has to be in a "
      "current index format. Integers and doubles keep their value, and "
      "encoded IRIs are re-encoded.\n\nThe converted "
      "index is first written to the subdirectory `",
      indexFormatConverter::stagingDirPrefix,
      "<current datetime>.tmp` and checked. Only then, the original index is "
      "moved to the subdirectory `",
      indexFormatConverter::retiredDirPrefix,
      "<datetime of its build>` and the converted index takes its place.");
}

// _____________________________________________________________________________
void convertIndexToSplitLayout(const std::string& oldBasename,
                               const std::string& newBasename,
                               qlever::Executor executor) {
  checkBasenames(oldBasename, newBasename);
  auto configuration = readAndCheckConfiguration(oldBasename);
  throwIfPersistedUpdatesExist(oldBasename);

  checkNewBasenameIsUnused(newBasename);
  size_t rowsPerBlock = IndexImpl::rowsPerBlock(configuration);
  auto convert = FoldedIdConverter::forIndex(oldBasename, configuration);
  AD_LOG_INFO << "Converting the index \"" << oldBasename << "\" to \""
              << newBasename << "\" ..." << std::endl;

  // The files of the old index that were converted or copied, see
  // `checkAllFilesWereHandled`.
  std::vector<fs::path> handledFiles{
      absl::StrCat(oldBasename, CONFIGURATION_FILE)};
  convertPermutations(
      oldBasename, newBasename,
      static_cast<Index::NumNormalAndInternal>(configuration.at("num-triples")),
      rowsPerBlock, convert, handledFiles, executor);
  convertPatterns(oldBasename, newBasename, convert, handledFiles);
  copyFilesThatNeedNoConversion(oldBasename, newBasename, handledFiles);
  checkAllFilesWereHandled(oldBasename, handledFiles);
  convertMaterializedViews(oldBasename, newBasename, rowsPerBlock, convert,
                           executor);
  writeConfiguration(newBasename, std::move(configuration));

  AD_LOG_INFO << "Conversion of the index completed, the converted index is \""
              << newBasename << "\"" << std::endl;
}

// _____________________________________________________________________________
void upgradeIndexInPlace(const std::string& basename,
                         qlever::Executor executor) {
  auto configuration = readAndCheckConfiguration(basename);
  indexConverterBase::upgradeIndexInPlace(
      basename, configuration, indexFormatConverter::stagingDirPrefix,
      indexFormatConverter::retiredDirPrefix,
      [&basename, &executor](const std::string& newBasename) {
        convertIndexToSplitLayout(basename, newBasename, executor);
      });
}

}  // namespace qlever::splitLayoutIdConverter
