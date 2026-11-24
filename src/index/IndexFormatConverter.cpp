// Copyright 2026 The QLever Authors, in particular:
//
// 2026 Johannes Kalmbach <kalmbach@cs.uni-freiburg.de>, UFR
//
// UFR = University of Freiburg, Chair of Algorithms and Data Structures
//
// You may not use this file except in compliance with the Apache 2.0 License,
// which can be found in the `LICENSE` file at the root of the QLever project.

#include "index/IndexFormatConverter.h"

#include <absl/strings/str_cat.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "backports/StartsWithAndEndsWith.h"
#include "backports/algorithm.h"
#include "backports/filesystem.h"
#include "backports/keywords.h"
#include "engine/idTable/IdTable.h"
#include "global/Constants.h"
#include "global/FileSuffixConstants.h"
#include "global/MaterializedViewConstants.h"
#include "index/ConstantsIndexBuilding.h"
#include "index/DeltaTriples.h"
#include "index/Index.h"
#include "index/IndexConverterBase.h"
#include "index/IndexFormatVersion.h"
#include "index/IndexImpl.h"
#include "index/IndexMetaData.h"
#include "index/LocalVocab.h"
#include "index/PatternCreator.h"
#include "index/Permutation.h"
#include "index/ScanSpecification.h"
#include "util/AllocatorWithLimit.h"
#include "util/BitUtils.h"
#include "util/CancellationHandle.h"
#include "util/CompactStringVector.h"
#include "util/Exception.h"
#include "util/File.h"
#include "util/InputRangeUtils.h"
#include "util/Log.h"
#include "util/ProgressBar.h"
#include "util/json.h"

namespace qlever::indexFormatConverter {

using namespace qlever::indexConverterBase;

namespace {

namespace fs = ql::filesystem;

// Return a human-readable representation of the given index format `version`.
std::string versionAsString(const IndexFormatVersion& version) {
  return absl::StrCat("PR = ", version.prNumber_,
                      ", Date = ", version.date_.toStringAndType().first);
}

// The `Datatype`s of the source format (see `sourceVersion`), in the order of
// the numeric values that they had in that format. In other words, an `Id`
// whose datatype bits are `i` in the source format is an `Id` of datatype
// `datatypesOfSourceFormat[i]` in the target format. The only difference
// between the two formats is that `Datatype::SecondaryVocabIndex` was inserted
// (see the note there). This array excludes `SecondaryVocabIndex` and the later
// `TripleTermIndex`, which cannot occur in the historical source format.
constexpr std::array<Datatype, 12> datatypesOfSourceFormat{
    Datatype::Undefined,
    Datatype::Bool,
    Datatype::Int,
    Datatype::Double,
    Datatype::VocabIndex,
    Datatype::LocalVocabIndex,
    Datatype::TextRecordIndex,
    Datatype::Date,
    Datatype::GeoPoint,
    Datatype::WordVocabIndex,
    Datatype::BlankNodeIndex,
    Datatype::EncodedVal};

// Return true iff `datatypes` is strictly ascending. NOTE: This is `consteval`,
// because it is only ever used in the `static_assert` below.
QL_CONSTEVAL bool isStrictlyAscending(
    const std::array<Datatype, 12>& datatypes) noexcept {
  for (size_t i = 1; i < datatypes.size(); ++i) {
    if (!(datatypes[i - 1] < datatypes[i])) {
      return false;
    }
  }
  return true;
}

// The conversion preserves the relative order of all datatypes of the source
// format. This is what makes it possible to convert a permutation by rewriting
// its `Id`s one by one: the result is still sorted, so it does not have to be
// sorted again. If a future change of the format violates this, then this
// converter is not applicable to it.
static_assert(isStrictlyAscending(datatypesOfSourceFormat));

// The historical conversion adds `SecondaryVocabIndex`. `TripleTermIndex` is
// appended without changing existing tags and cannot occur in the source
// format.
// TODO<Prashanth0703> Before persisting triple-term references, define index
// metadata and version handling that preserve compatibility for indexes without
// them.
static_assert(datatypesOfSourceFormat.size() + 1 ==
              static_cast<size_t>(Datatype::TripleTermIndex));
static_assert(static_cast<size_t>(Datatype::TripleTermIndex) + 1 ==
              qlever::enumSize<Datatype>);

// The version of the on-disk format of the materialized views (see
// `MATERIALIZED_VIEWS_VERSION`) that the views of an index in the source format
// have. That version was raised together with the index format, so the views
// have to be converted as well.
constexpr size_t materializedViewsVersionOfSourceFormat = 1;
static_assert(materializedViewsVersionOfSourceFormat + 1 ==
              MATERIALIZED_VIEWS_VERSION);

// The key of the index format version in the configuration of an index. NOTE:
// This is a `const char*` and not a `std::string_view`, because the latter
// cannot be used to look up a key in a `nlohmann::json` object.
constexpr const char* indexFormatVersionKey = "index-format-version";

// Check that the source format of this converter (see `sourceVersion`) still
// is the previous index format, and that its target format (see
// `targetVersion`) still is a format that the current version of QLever loads
// without conversion. If not, then the index format has changed again and this
// converter has to be updated (see the note at `qlever::indexFormatVersion`),
// so this is a programming error and not something that a user can fix.
void checkThatTheSupportedFormatsAreUpToDate() {
  AD_CORRECTNESS_CHECK(
      isLoadableIndexFormatVersion(targetVersion),
      "The index converter converts to the index format ",
      versionAsString(targetVersion), ", but the current index format is ",
      versionAsString(indexFormatVersion),
      ". The converter has to be updated to the current index format.");
  AD_CORRECTNESS_CHECK(
      sourceVersion == previousIndexFormatVersion,
      "The index converter converts from the index format ",
      versionAsString(sourceVersion),
      ", but the index format that precedes the current one is ",
      versionAsString(previousIndexFormatVersion),
      ". The converter has to be updated to that index format.");
}

// Read the configuration of the index with the base name `basename`, and check
// that it is in the source format (see the documentation of
// `convertIndexToCurrentFormat`).
nlohmann::json readAndCheckConfiguration(const std::string& basename) {
  auto configuration = readConfiguration(basename);
  auto version =
      configuration.at(indexFormatVersionKey).get<IndexFormatVersion>();
  if (isLoadableIndexFormatVersion(version)) {
    throw std::runtime_error{absl::StrCat(
        "The index \"", basename,
        "\" already is in an index format that the current version of QLever "
        "can use (",
        versionAsString(version), "), so there is nothing to convert.")};
  }
  if (version != sourceVersion) {
    throw std::runtime_error{absl::StrCat(
        "The index \"", basename, "\" is in the index format (",
        versionAsString(version),
        "), but this converter only converts indexes in the format (",
        versionAsString(sourceVersion), ") to the format (",
        versionAsString(targetVersion), "). Please rebuild the index.")};
  }
  return configuration;
}

// An empty `LocatedTriplesState` for the given `permutation`, which is what a
// scan of that permutation requires. It is empty because the index that is
// converted has no delta triples (see `throwIfPersistedUpdatesExist` above).
std::shared_ptr<LocatedTriplesState> makeEmptyLocatedTriplesState(
    const Permutation& permutation) {
  LocatedTriplesPerBlockAllPermutations<false> emptyLocatedTriples;
  emptyLocatedTriples.at(static_cast<size_t>(permutation.permutation()))
      .setOriginalMetadata(permutation.metaData().blockDataShared());
  // NOTE: The located triples of the internal permutations deliberately stay
  // untouched. `loadPermutation` below loads every permutation with
  // `Permutation::Type::NORMAL`, including the internal ones, so a scan always
  // looks up its located triples in the array above.
  LocatedTriplesPerBlockAllPermutations<true> emptyInternalLocatedTriples;
  // The converter doesn't run as part of a query, so there is no memory limit.
  LocalVocab emptyVocab{ad_utility::makeUnlimitedAllocator<Id>()};
  return std::make_shared<LocatedTriplesState>(
      LocatedTriplesState{emptyLocatedTriples, emptyInternalLocatedTriples,
                          emptyVocab.getLifetimeExtender(), 0});
}

// Return the columns of a permutation with `numColumns` columns that a scan has
// to request explicitly, that is, all columns except for the three columns of
// the (permuted) triple itself. These are the graph column and, for the
// permutations that store the patterns, the two pattern columns.
std::vector<ColumnIndex> getAdditionalColumns(size_t numColumns) {
  AD_CORRECTNESS_CHECK(numColumns >= NumColumnsIndexBuilding);
  std::vector<ColumnIndex> additionalColumns;
  for (size_t column = NumColumnsIndexBuilding - 1; column < numColumns;
       ++column) {
    additionalColumns.push_back(static_cast<ColumnIndex>(column));
  }
  AD_CORRECTNESS_CHECK(additionalColumns.at(0) == ADDITIONAL_COLUMN_GRAPH_ID);
  return additionalColumns;
}

// Return a lazy full scan of `permutation` in which all `Id`s are converted to
// the current index format, reporting the number of triples of each block to
// `progress`. The blocks are read on the `executor`. The returned range has to
// be consumed before `permutation` is destroyed.
ad_utility::InputRangeTypeErased<IdTableStatic<0>> scanAndConvertIds(
    const Permutation& permutation, std::function<void(size_t)> progress,
    qlever::Executor executor) {
  auto locatedTriplesState = makeEmptyLocatedTriplesState(permutation);
  auto scanSpecAndBlocks = permutation.getScanSpecAndBlocks(
      ScanSpecification{std::nullopt, std::nullopt, std::nullopt},
      *locatedTriplesState);
  auto additionalColumns =
      getAdditionalColumns(numColumns(permutation.metaData()));
  // The cancellation handle of the scan, which never cancels anything.
  auto cancellationHandle =
      std::make_shared<ad_utility::CancellationHandle<>>();
  // NOTE: Deliberately no structured binding, because the members are captured
  // by the lambda below, which is only valid in C++20.
  auto scanWithReader = permutation.lazyScanWithUnlimitedReader(
      scanSpecAndBlocks, additionalColumns, cancellationHandle,
      *locatedTriplesState, std::move(executor));

  // NOTE: The scan borrows the `reader` and the `locatedTriplesState`, so both
  // of them are moved into the transformation below to keep them alive for as
  // long as the returned range is.
  return ad_utility::InputRangeTypeErased{
      ad_utility::CachingTransformInputRange{
          std::move(scanWithReader.blocks_),
          [reader = std::move(scanWithReader.reader_),
           locatedTriplesState = std::move(locatedTriplesState),
           progress = std::move(progress)](IdTable& idTable) {
            for (auto column : idTable.getColumns()) {
              ql::ranges::for_each(column,
                                   [](IdRef id) { id = convertId(id); });
            }
            progress(idTable.numRows());
            return IdTableStatic<0>{std::move(idTable)};
          }}};
}

// Load the given `permutation` of the index with the base name `basename` from
// disk. Return `nullptr` if it does not exist.
std::unique_ptr<Permutation> loadPermutation(const std::string& basename,
                                             Permutation::Enum permutationEnum,
                                             bool isInternal) {
  if (!hasPermutation(basename, permutationEnum, isInternal)) {
    return nullptr;
  }
  auto permutation = std::make_unique<Permutation>(
      permutationEnum, ad_utility::makeUnlimitedAllocator<Id>());

  // NOTE: The "Registered ... permutation" message that `loadFromDisk` logs by
  // default is suppressed here, because it would interrupt the progress bar of
  // `convertPermutations` below. The statistics of each permutation are in the
  // log of the index build, and those of the upgraded index are logged when it
  // is checked (see `checkUpgradedIndex`).
  permutation->loadFromDisk(basenameForPermutations(basename, isInternal),
                            false, Permutation::Type::NORMAL, {},
                            /* logRegistration = */ false);
  return permutation;
}

// Convert a single permutation of an index and write the result to the index
// with the base name `newBasename`. Return the metadata of the new permutation,
// which still has to be written to disk, see `writePermutation` above.
//
// NOTE: The two permutations of a pair are converted concurrently (see
// `convertPermutations` below), so this must not touch any state that is
// shared between them. That is why the files of the old permutation are
// recorded by the caller and not here, and why the progress is reported to a
// threadsafe `progress` callback instead of being logged here.
IndexMetaData convertPermutation(const Permutation& oldPermutation,
                                 const std::string& newBasename,
                                 bool isInternal, size_t rowsPerBlock,
                                 const std::function<void(size_t)>& progress,
                                 const qlever::Executor& executor) {
  return writeConvertedPermutation(
      oldPermutation.metaData(),
      scanAndConvertIds(oldPermutation, progress, executor), &convertId,
      filenameForPermutation(newBasename, oldPermutation.permutation(),
                             isInternal),
      rowsPerBlock, executor);
}

// Convert all permutations of the index with the base name `oldBasename` and
// write them to the index with the base name `newBasename`. The `numTriples`
// are the numbers of triples from the configuration of that index, which are
// the total for the progress bar below, and `rowsPerBlock` is its block size.
// The permutations are read and written on the `executor`.
void convertPermutations(const std::string& oldBasename,
                         const std::string& newBasename,
                         const Index::NumNormalAndInternal& numTriples,
                         size_t rowsPerBlock,
                         std::vector<fs::path>& handledFiles,
                         const qlever::Executor& executor) {
  convertPermutationPairs(
      oldBasename, newBasename, numTriples, handledFiles,
      [&](Permutation::Enum permutationEnum, bool isInternal,
          const std::function<void(size_t)>& progress) {
        auto permutation =
            loadPermutation(oldBasename, permutationEnum, isInternal);
        return convertPermutation(*permutation, newBasename, isInternal,
                                  rowsPerBlock, progress, executor);
      });
}

// Convert the patterns of the index with the base name `oldBasename` (if it has
// them) and write them to the index with the base name `newBasename`.
void convertPatterns(const std::string& oldBasename,
                     const std::string& newBasename,
                     std::vector<fs::path>& handledFiles) {
  std::string oldFilename = absl::StrCat(oldBasename, PATTERNS_FILE_SUFFIX);
  if (!fs::exists(oldFilename)) {
    return;
  }
  handledFiles.emplace_back(oldFilename);
  AD_LOG_INFO << "Converting the patterns ..." << std::endl;
  PatternStatistics statistics;
  CompactVectorOfStrings<Id> patterns;
  PatternCreator::readPatternsFromFile(
      oldFilename, statistics.avgNumDistinctSubjectsPerPredicate_,
      statistics.avgNumDistinctPredicatesPerSubject_,
      statistics.numDistinctSubjectPredicatePairs_, patterns);
  PatternCreator::writePatternsToFile(
      absl::StrCat(newBasename, PATTERNS_FILE_SUFFIX),
      patterns.cloneAndRemap(&convertId), statistics);
}

// Convert the materialized view with the given `name` of the index with the
// base name `oldBasename` and write it to the index with the base name
// `newBasename`, with the block size `rowsPerBlock` of that index. The
// permutation of the view is read and written on the `executor`.
void convertMaterializedView(const std::string& oldBasename,
                             const std::string& newBasename,
                             const std::string& name, size_t rowsPerBlock,
                             const qlever::Executor& executor) {
  AD_LOG_INFO << "Converting the materialized view \"" << name << "\" ..."
              << std::endl;
  std::string oldViewBasename = materializedViewFilenameBase(oldBasename, name);
  std::string newViewBasename = materializedViewFilenameBase(newBasename, name);

  // Convert the permutation of the view. A view always is a single `SPO`
  // permutation and never has a twin, so unlike the permutations of the index
  // itself, its multiplicities are not exchanged.
  Permutation oldPermutation{Permutation::SPO,
                             ad_utility::makeUnlimitedAllocator<Id>(), name};
  oldPermutation.loadFromDisk(oldViewBasename, false,
                              Permutation::Type::MATERIALIZED_VIEW, {},
                              /* logRegistration = */ false);
  std::string newFilename = absl::StrCat(newViewBasename, VIEW_SPO_SUFFIX);
  size_t numTriples = oldPermutation.metaData().totalElements();
  ad_utility::ConcurrentProgressBar progressBar{
      "Triples converted: ", numTriples, batchSizeFor(numTriples)};
  auto newMetaData = writeConvertedPermutation(
      oldPermutation.metaData(),
      scanAndConvertIds(oldPermutation, progressCallbackFor(progressBar),
                        executor),
      &convertId, newFilename, rowsPerBlock, executor);
  progressBar.logFinalProgressString();
  newMetaData.setName(newViewBasename);
  writeMetaData(newMetaData, newFilename);

  // The view that is read has to be in the version that belongs to the source
  // format, see `materializedViewsVersionOfSourceFormat`.
  copyViewInfo(oldBasename, name, newBasename,
               materializedViewsVersionOfSourceFormat);
}

// Convert all materialized views of the index with the base name `oldBasename`
// and write them to the index with the base name `newBasename`, with the block
// size `rowsPerBlock` of that index, on the `executor`.
void convertMaterializedViews(const std::string& oldBasename,
                              const std::string& newBasename,
                              size_t rowsPerBlock,
                              const qlever::Executor& executor) {
  auto names = findMaterializedViews(oldBasename);
  for (const auto& name : names) {
    convertMaterializedView(oldBasename, newBasename, name, rowsPerBlock,
                            executor);
  }
}

}  // namespace

// _____________________________________________________________________________
std::string conversionDescription() {
  return absl::StrCat(
      "Upgrade an index in the index format (", versionAsString(sourceVersion),
      ") to an index in the index format (", versionAsString(targetVersion),
      ") in place. The only difference between the two formats is the "
      "numbering of the datatypes of the IDs: the datatype for the words of a "
      "secondary vocabulary was inserted in the middle, which renumbered the "
      "datatypes after it. The IDs of the index are therefore rewritten, and "
      "nothing else changes.\n\nThe upgraded index is first written to the "
      "subdirectory `",
      stagingDirPrefix,
      "<current datetime>.tmp` and checked. Only then, the index in the old "
      "format is moved to the subdirectory `",
      retiredDirPrefix,
      "<datetime of its build>` and the upgraded index takes its "
      "place.\n\nNote that rebuilding the index from its input files is still "
      "the recommended way to move to a new index format, because it also "
      "profits from all improvements to the index building since the index "
      "was built.");
}

// _____________________________________________________________________________
Id convertId(Id id) {
  auto datatypeBits = id.getDatatypeBits();
  if (datatypeBits >= datatypesOfSourceFormat.size()) {
    throw std::runtime_error{absl::StrCat(
        "Encountered an `Id` with the invalid datatype ", datatypeBits,
        ", the index that is converted is corrupted")};
  }
  auto datatype = datatypesOfSourceFormat.at(datatypeBits);
  if (datatype == Datatype::LocalVocabIndex) {
    throw std::runtime_error{
        "Encountered an `Id` of type `LocalVocabIndex`, which must never be "
        "stored on disk (it holds a pointer into the memory of the process "
        "that created it), so the index that is converted is corrupted"};
  }
  return Id::fromBits(datatype, id.getPayloadBits());
}

// _____________________________________________________________________________
void convertIndexToCurrentFormat(const std::string& oldBasename,
                                 const std::string& newBasename,
                                 qlever::Executor executor) {
  checkBasenames(oldBasename, newBasename);
  checkThatTheSupportedFormatsAreUpToDate();

  auto configuration = readAndCheckConfiguration(oldBasename);
  throwIfPersistedUpdatesExist(oldBasename);

  // The converted permutations must have the block size of the index that is
  // converted, because the configuration (which records that block size) is
  // copied unchanged, see below.
  size_t rowsPerBlock = IndexImpl::rowsPerBlock(configuration);

  // The converted index must not overwrite any existing file.
  checkNewBasenameIsUnused(newBasename);

  AD_LOG_INFO << "Converting the index \"" << oldBasename << "\" to \""
              << newBasename << "\" ..." << std::endl;

  // The files of the old index that have been converted or copied, see
  // `checkAllFilesWereHandled`. The configuration file is handled at the very
  // end, but is already listed here.
  std::vector<fs::path> handledFiles{
      absl::StrCat(oldBasename, CONFIGURATION_FILE)};

  convertPermutations(
      oldBasename, newBasename,
      static_cast<Index::NumNormalAndInternal>(configuration.at("num-triples")),
      rowsPerBlock, handledFiles, executor);
  convertPatterns(oldBasename, newBasename, handledFiles);
  copyFilesThatNeedNoConversion(oldBasename, newBasename, handledFiles);
  checkAllFilesWereHandled(oldBasename, handledFiles);
  convertMaterializedViews(oldBasename, newBasename, rowsPerBlock, executor);

  // Write the configuration last, with the version of the target format. An
  // index without its configuration file cannot be loaded at all, so if the
  // conversion is interrupted, the incomplete index is not mistaken for a
  // complete one.
  configuration[indexFormatVersionKey] = targetVersion;
  ad_utility::makeOfstream(absl::StrCat(newBasename, CONFIGURATION_FILE))
      << configuration.dump(4) << std::endl;

  AD_LOG_INFO << "Conversion of the index completed, the converted index is \""
              << newBasename << "\"" << std::endl;
}

// _____________________________________________________________________________
void upgradeIndexInPlace(const std::string& basename,
                         qlever::Executor executor) {
  // Read the configuration of the index that is to be upgraded; this also
  // checks that the index exists and is exactly in the source format.
  auto configuration = readAndCheckConfiguration(basename);

  indexConverterBase::upgradeIndexInPlace(
      basename, configuration, stagingDirPrefix, retiredDirPrefix,
      [&basename, &executor](const std::string& newBasename) {
        convertIndexToCurrentFormat(basename, newBasename, executor);
      });
}

}  // namespace qlever::indexFormatConverter
