// Copyright 2026 The QLever Authors, in particular:
//
// 2026 Johannes Kalmbach <kalmbach@cs.uni-freiburg.de>, UFR
// 2026 Pascal Keßler <kesslerp@cs.uni-freiburg.de>, UFR
//
// UFR = University of Freiburg, Chair of Algorithms and Data Structures
//
// You may not use this file except in compliance with the Apache 2.0 License,
// which can be found in the `LICENSE` file at the root of the QLever project.

#include "index/IndexConverterBase.h"

#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>

#include <future>

#include "backports/StartsWithAndEndsWith.h"
#include "global/Constants.h"
#include "global/FileSuffixConstants.h"
#include "global/MaterializedViewConstants.h"
#include "index/Index.h"
#include "index/IndexImpl.h"
#include "index/IndexSwap.h"
#include "index/compressedRelationWriter/CompressedRelationWriter.h"
#include "util/Algorithm.h"
#include "util/File.h"
#include "util/FilesystemHelpers.h"
#include "util/Log.h"

namespace qlever::indexConverterBase {

namespace fs = ql::filesystem;

// The permutations of an index, as pairs of "twins" (like `PSO` and `POS`),
// together with the information whether the pair is the pair of internal
// permutations. The multiplicities of the last column of a permutation are
// stored in the metadata of its twin (see
// `IndexMetaData::exchangeMultiplicities`), which is why the permutations have
// to be converted pairwise.
using PermutationPair = std::pair<Permutation::Enum, Permutation::Enum>;
const std::array<std::pair<PermutationPair, bool>, 4> permutationPairs{
    std::pair{PermutationPair{Permutation::PSO, Permutation::POS}, false},
    std::pair{PermutationPair{Permutation::PSO, Permutation::POS}, true},
    std::pair{PermutationPair{Permutation::SPO, Permutation::SOP}, false},
    std::pair{PermutationPair{Permutation::OPS, Permutation::OSP}, false}};

// Return the base name that the permutation files of the index with the base
// name `basename` share. The internal permutations have an additional infix.
std::string basenameForPermutations(std::string_view basename,
                                    bool isInternal) {
  return absl::StrCat(basename, isInternal ? QLEVER_INTERNAL_INDEX_INFIX : "");
}

// Return the name of the file that stores the given `permutation` of the index
// with the base name `basename`.
std::string filenameForPermutation(std::string_view basename,
                                   Permutation::Enum permutation,
                                   bool isInternal) {
  return absl::StrCat(
      basenameForPermutations(basename, isInternal), PERMUTATION_FILE_INFIX,
      Permutation{permutation, ad_utility::makeUnlimitedAllocator<Id>()}
          .fileSuffix());
}

// Throw if the index with the base name `basename` has persisted updates. Those
// contain `Id`s as well, but converting them is deliberately not supported (see
// the documentation of `convertIndexToCurrentFormat`).
void throwIfPersistedUpdatesExist(const std::string& basename) {
  std::string filename = absl::StrCat(basename, UPDATE_TRIPLES_SUFFIX);
  if (fs::exists(filename)) {
    throw std::runtime_error{absl::StrCat(
        "The index \"", basename, "\" has persisted updates (the file \"",
        filename,
        "\"), which this converter does not convert. Please either materialize "
        "them into the index (by rebuilding it) or delete the file (which "
        "discards the updates) before converting the index.")};
  }
}

// Return a callback for the given `progressBar`, which reports that `numSteps`
// steps have been processed and displays an update when one is due. The
// callback is threadsafe (see `ConcurrentProgressBar`), which matters because
// the two permutations of a pair are converted concurrently, see
// `convertPermutations` below.
//
// NOTE: The rebuild reports its progress in exactly the same way, see
// `IndexRebuilder.cpp`.
std::function<void(size_t)> progressCallbackFor(
    ad_utility::ConcurrentProgressBar& progressBar) {
  return [&progressBar](size_t numSteps) {
    progressBar.add(numSteps);
    if (auto update = progressBar.update()) {
      AD_LOG_INFO << update->getProgressString() << std::flush;
    }
  };
}

// The batch size for a progress bar with the given `total`, chosen such that
// about 1000 progress lines are written, that is, one line per mille of the
// total. Each line costs a formatted string and a lock, which is nothing
// compared to converting a per mille of an index. The lower bound keeps tiny
// indexes (in particular, those of the unit tests) from producing a progress
// line for almost every block.
//
// NOTE: The rebuild computes its batch size in the same way, but with about 50
// lines per phase (`IndexRebuilder.cpp`).
size_t batchSizeFor(size_t total) {
  return std::max<size_t>(total / 1000, 100'000);
}

namespace {
// Write the given `blocks` as a single permutation to the file `filename`, and
// return its metadata. The metadata is not yet written to disk, because it is
// only complete once the multiplicities have been exchanged with the twin
// permutation (see `permutationPairs` above).
//
// NOTE: The permutation is written with `rowsPerBlock`, the block size of the
// index that is converted (see `convertIndexToCurrentFormat`), so it has
// exactly the blocks of a fresh build of that index. These blocks can still
// differ from the blocks of the permutation that it was converted from, which
// is irrelevant for its content, but not for its metadata: a relation that is
// large enough to occupy blocks of its own in the permutation that is converted
// can be small enough to share a block with other relations in the converted
// permutation. Such a relation has no `CompressedRelationMetadata` of its own
// anymore, that metadata is derived from its block instead (see
// `CompressedRelationReader::getMetadataForSmallRelation`). The number of
// blocks, the `numRows_` and the multiplicities of the converted permutation
// can therefore differ from those of the permutation that it was converted
// from; they are exactly those that a freshly built index would have.
//
// The blocks are compressed and written on the `executor`.
IndexMetaData writePermutation(
    const std::string& filename, size_t numColumns, size_t rowsPerBlock,
    ad_utility::InputRangeTypeErased<IdTableStatic<0>> blocks,
    qlever::Executor executor) {
  auto writer = std::make_unique<CompressedRelationWriter>(
      numColumns, ad_utility::File{filename, "w"}, rowsPerBlock,
      std::move(executor));
  IndexMetaData metaData;
  auto callback =
      [&metaData](ql::span<const CompressedRelationMetadata> metadata) {
        for (const auto& relationMetadata : metadata) {
          metaData.add(relationMetadata);
        }
      };
  // The blocks already are in the correct order (the conversion does not change
  // the order of the `Id`s), so the identity is the correct key order here.
  auto [numDistinctCol0, blockMetadata] =
      CompressedRelationWriter::createPermutation(
          {std::move(writer), callback}, std::move(blocks),
          KeyOrder{0, 1, 2, 3}, {}, /* showProgressBar = */ false);
  metaData.blockData() = std::move(blockMetadata);
  metaData.calculateStatistics(numDistinctCol0);
  return metaData;
}
}  // namespace

// Append the `metaData` of the permutation that was written to `filename` to
// that file, and to the corresponding metadata file.
void writeMetaData(IndexMetaData& metaData, const std::string& filename) {
  ad_utility::File permutationFile{filename, "r+"};
  ad_utility::File metaFile{absl::StrCat(filename, META_FILE_SUFFIX), "w"};
  metaData.appendToFile(permutationFile, metaFile);
}

namespace {
// Check that the permutation that was written (`newMetaData`) has the same
// content as the permutation that it was converted from (`oldMetaData`). Only
// the number of triples and the first and last triple are compared, which is
// cheap because it only looks at the metadata that is in memory anyway.
void verifyConvertedPermutation(const IndexMetaData& oldMetaData,
                                const IndexMetaData& newMetaData,
                                const std::function<Id(Id)>& convertId,
                                const std::string& filename) {
  // NOTE: This can only fail if the converter itself is broken, hence a
  // correctness check and not an exception with a user-facing message.
  auto check = [&filename](bool condition) {
    AD_CORRECTNESS_CHECK(
        condition, "The converted permutation \"", filename,
        "\" does not have the same content as the permutation it was converted "
        "from. The converted index is incomplete and has to be deleted.");
  };
  check(oldMetaData.totalElements() == newMetaData.totalElements());
  const auto& oldBlocks = oldMetaData.blockData();
  const auto& newBlocks = newMetaData.blockData();
  check(oldBlocks.empty() == newBlocks.empty());
  if (oldBlocks.empty()) {
    return;
  }
  auto convertTriple =
      [&convertId](CompressedBlockMetadata::PermutedTriple triple) {
        return CompressedBlockMetadata::PermutedTriple{
            convertId(triple.col0Id_), convertId(triple.col1Id_),
            convertId(triple.col2Id_), convertId(triple.graphId_)};
      };
  check(convertTriple(oldBlocks.front().firstTriple_) ==
        newBlocks.front().firstTriple_);
  check(convertTriple(oldBlocks.back().lastTriple_) ==
        newBlocks.back().lastTriple_);
}
}  // namespace

// Whether the index with the base name `basename` has the given permutation.
// An index built with `--only-pso-and-pos-permutations` has neither `SPO` and
// `SOP` nor `OPS` and `OSP`.
bool hasPermutation(const std::string& basename,
                    Permutation::Enum permutationEnum, bool isInternal) {
  return fs::exists(
      filenameForPermutation(basename, permutationEnum, isInternal));
}

namespace {
// The number of normal and of internal permutations that the index with the
// base name `basename` has, in the same way in which `convertPermutations`
// below determines which permutations it converts.
Index::NumNormalAndInternal numPermutationsOfIndex(
    const std::string& basename) {
  Index::NumNormalAndInternal numPermutations{};
  for (const auto& permutationPair : permutationPairs) {
    auto [enumA, enumB] = permutationPair.first;
    bool isInternal = permutationPair.second;
    for (auto permutationEnum : {enumA, enumB}) {
      if (hasPermutation(basename, permutationEnum, isInternal)) {
        ++(isInternal ? numPermutations.internal : numPermutations.normal);
      }
    }
  }
  return numPermutations;
}
}  // namespace

// Copy all files of the index with the base name `oldBasename` that need no
// conversion to the index with the base name `newBasename`. These are the files
// that contain no `Id`s at all: the vocabulary, the text index (which stores
// plain integers and reconstructs its `Id`s when it is read), the settings, and
// the state of the graph-name allocation.
void copyFilesThatNeedNoConversion(const std::string& oldBasename,
                                   const std::string& newBasename,
                                   std::vector<fs::path>& handledFiles) {
  auto copy = [&handledFiles, &oldBasename,
               &newBasename](const fs::path& oldFile) {
    std::string_view filename = oldFile.native();
    AD_CORRECTNESS_CHECK(ql::starts_with(filename, oldBasename));
    filename.remove_prefix(oldBasename.size());
    handledFiles.emplace_back(oldFile);
    fs::copy_file(oldFile, absl::StrCat(newBasename, filename));
  };
  AD_LOG_INFO << "Copying the files that need no conversion ..." << std::endl;
  for (auto suffix :
       {SETTINGS_FILE_SUFFIX, ALLOCATED_GRAPHS_SUFFIX, TEXT_INDEX_FILE_SUFFIX,
        TEXT_VOCAB_FILE_SUFFIX, TEXT_DOCS_DB_FILE_SUFFIX}) {
    fs::path oldFile = absl::StrCat(oldBasename, suffix);
    if (fs::exists(oldFile)) {
      copy(oldFile);
    }
  }
  // The set of files of the vocabulary depends on its type, so enumerate them
  // via their common prefix (the same mechanism as in
  // `IndexImpl::allIndexFiles`).
  ql::ranges::for_each(
      qlever::util::filesWithBaseNameAndSuffix(oldBasename, VOCAB_SUFFIX),
      copy);
}

// Check that every file of the index with the base name `oldBasename` was
// either converted or copied. This makes sure that a file type that is added to
// an index in the future is not silently dropped by this converter.
//
// NOTE: Such a file can only appear if `IndexImpl::allIndexFiles` was extended
// without extending this converter, hence a correctness check and not an
// exception with a user-facing message.
void checkAllFilesWereHandled(const std::string& oldBasename,
                              const std::vector<fs::path>& handledFiles) {
  std::vector<std::string> missingFiles;
  for (const auto& file : IndexImpl::allIndexFiles(oldBasename)) {
    if (!ad_utility::contains(handledFiles, file)) {
      missingFiles.push_back(file.native());
    }
  }
  AD_CORRECTNESS_CHECK(missingFiles.empty(),
                       "The following files of the index \"", oldBasename,
                       "\" were neither converted nor copied: ",
                       absl::StrJoin(missingFiles, ", "),
                       ". The index converter has to be extended for them. The "
                       "converted index is incomplete and has to be deleted.");
}

namespace {
// Check that the upgraded index with the base name `newBasename` can be
// loaded, and that the number of triples of each of its permutations matches
// `expectedNumTriples`, the number of triples from the configuration of the
// index that was upgraded. The number of triples of a permutation comes from
// the metadata of that permutation (the configuration is copied unchanged by
// the conversion, so comparing configurations would check nothing), so a
// conversion bug that loses or duplicates rows of a permutation is caught
// here, before the upgraded index replaces the original one.
void checkUpgradedIndex(const std::string& newBasename,
                        const Index::NumNormalAndInternal& expectedNumTriples) {
  AD_LOG_INFO << "Checking that the upgraded index can be loaded ..."
              << std::endl;
  // Loading an index logs the vocabulary, every permutation, and the patterns,
  // and unloading it logs one more message. All of that is noise here, only a
  // warning or an error of the load is of interest.
  //
  // NOTE: Declared before `index`, so that it is destroyed after it and hence
  // also covers the message that the destruction of `index` logs.
  ad_utility::ScopedLogLevel scopedLogLevel{LogLevel::Enum::WARN};
  Index index{ad_utility::makeUnlimitedAllocator<Id>()};
  // Load what the index has: the patterns and the permutations beyond `PSO`
  // and `POS` are optional.
  index.usePatterns() =
      fs::exists(absl::StrCat(newBasename, PATTERNS_FILE_SUFFIX));
  index.loadAllPermutations() =
      fs::exists(absl::StrCat(newBasename, PERMUTATION_FILE_INFIX, ".ops"));
  index.createFromOnDiskIndex(newBasename, false);

  auto checkNumTriples = [](const Permutation& permutation, size_t expected) {
    size_t actual = permutation.numTriples();
    if (actual != expected) {
      throw std::runtime_error{absl::StrCat(
          "The ", permutation.readableName(),
          " permutation of the upgraded index has ", actual,
          " triples, but the index that was upgraded has ", expected,
          " according to its configuration. This is a bug in the conversion; "
          "the original index was not modified.")};
    }
  };
  std::vector<Permutation::Enum> permutations{Permutation::Enum::PSO,
                                              Permutation::Enum::POS};
  if (index.loadAllPermutations()) {
    permutations.insert(permutations.end(),
                        {Permutation::Enum::SPO, Permutation::Enum::SOP,
                         Permutation::Enum::OSP, Permutation::Enum::OPS});
  }
  for (auto p : permutations) {
    checkNumTriples(index.getImpl().getPermutation(p),
                    expectedNumTriples.normal);
  }
  // The triples with QLever-internal predicates live in a nested permutation
  // of `PSO` and `POS` (see `IndexImpl::createFromOnDiskIndex`).
  for (auto p : {Permutation::Enum::PSO, Permutation::Enum::POS}) {
    checkNumTriples(index.getImpl().getPermutation(p).internalPermutation(),
                    expectedNumTriples.internal);
  }
}
}  // namespace

// _____________________________________________________________________________
void checkBasenames(const std::string& oldBasename,
                    const std::string& newBasename) {
  AD_CONTRACT_CHECK(!oldBasename.empty() && !newBasename.empty(),
                    "The base names of the indexes must not be empty");
  // NOTE: This is a user-facing error and not a requirement violation, because
  // passing the same base name twice is an easy mistake to make on the command
  // line of `qlever-upgrade-index`.
  if (fs::path{oldBasename}.lexically_normal() ==
      fs::path{newBasename}.lexically_normal()) {
    throw std::runtime_error{
        "The base name of the converted index has to differ from the base "
        "name of the index that is converted, because the index that is "
        "converted is not modified"};
  }
}

// _____________________________________________________________________________
size_t numColumns(const IndexMetaData& metaData) {
  // Every block knows the offsets of its columns. An empty permutation has the
  // minimal number of columns, like in the index builder.
  const auto& blocks = metaData.blockData();
  return blocks.empty() ? NumColumnsIndexBuilding
                        : blocks.front().offsetsAndCompressedSize_->size();
}

// _____________________________________________________________________________
IndexMetaData writeConvertedPermutation(
    const IndexMetaData& oldMetaData,
    ad_utility::InputRangeTypeErased<IdTableStatic<0>> blocks,
    const std::function<Id(Id)>& convertId, const std::string& newFilename,
    size_t rowsPerBlock, qlever::Executor executor) {
  auto newMetaData =
      writePermutation(newFilename, numColumns(oldMetaData), rowsPerBlock,
                       std::move(blocks), std::move(executor));
  newMetaData.setName(oldMetaData.getName());
  verifyConvertedPermutation(oldMetaData, newMetaData, convertId, newFilename);
  return newMetaData;
}

// _____________________________________________________________________________
nlohmann::json readConfiguration(const std::string& basename) {
  std::string filename = absl::StrCat(basename, CONFIGURATION_FILE);
  if (!fs::exists(filename)) {
    throw std::runtime_error{absl::StrCat(
        "The file \"", filename, "\" does not exist, so \"", basename,
        "\" is not the base name of a QLever index. Note that the base name "
        "includes the name of the index, for example `index-dir/wikidata`.")};
  }
  nlohmann::json configuration;
  ad_utility::makeIfstream(filename) >> configuration;

  if (!configuration.contains("index-format-version")) {
    throw std::runtime_error{absl::StrCat(
        "The index \"", basename,
        "\" was built before versioning was introduced for QLever's index "
        "format, it is much too old to be converted. Please rebuild it.")};
  }
  return configuration;
}

// _____________________________________________________________________________
void checkNewBasenameIsUnused(const std::string& newBasename) {
  fs::path newDirectory = fs::path{newBasename}.parent_path();
  if (!newDirectory.empty()) {
    fs::create_directories(newDirectory);
  }
  if (qlever::util::doesDirectoryContainFileWithBasename(newBasename)) {
    throw std::runtime_error{absl::StrCat(
        "There already are files with the base name \"", newBasename,
        "\", but the converted index must not overwrite any of them. Please "
        "choose a base name that is not in use yet.")};
  }
}

// _____________________________________________________________________________
void convertPermutationPairs(const std::string& oldBasename,
                             const std::string& newBasename,
                             const Index::NumNormalAndInternal& numTriples,
                             std::vector<fs::path>& handledFiles,
                             const ConvertPermutation& convertPermutation) {
  // Each triple is written once per permutation, which gives the total number
  // of triples that the conversion of the permutations writes.
  auto numPermutations = numPermutationsOfIndex(oldBasename);
  size_t numTriplesTotal = numPermutations.normal * numTriples.normal +
                           numPermutations.internal * numTriples.internal;
  AD_LOG_INFO << "Converting " << numPermutations.normalAndInternal_()
              << " permutations (" << numPermutations.normal << " normal and "
              << numPermutations.internal << " internal, "
              << ad_utility::withThousandSeparators(numTriplesTotal)
              << " triples in total) ..." << std::endl;
  ad_utility::ConcurrentProgressBar progressBar{
      "Triples converted: ", numTriplesTotal, batchSizeFor(numTriplesTotal)};
  auto progress = progressCallbackFor(progressBar);

  for (const auto& [pair, isInternal] : permutationPairs) {
    auto [enumA, enumB] = pair;
    bool hasA = hasPermutation(oldBasename, enumA, isInternal);
    bool hasB = hasPermutation(oldBasename, enumB, isInternal);
    if (!hasA && !hasB) {
      // The index does not have this pair of permutations at all, which is the
      // case for `SPO`, `SOP`, `OPS`, and `OSP` if the index was built with
      // `--only-pso-and-pos-permutations`.
      continue;
    }
    if (!hasA || !hasB) {
      throw std::runtime_error{absl::StrCat(
          "The index \"", oldBasename, "\" has only one of the permutations ",
          Permutation::toString(enumA), " and ", Permutation::toString(enumB),
          ", so it is incomplete and cannot be converted")};
    }
    for (auto permutationEnum : {enumA, enumB}) {
      std::string oldFilename =
          filenameForPermutation(oldBasename, permutationEnum, isInternal);
      handledFiles.emplace_back(oldFilename);
      handledFiles.emplace_back(absl::StrCat(oldFilename, META_FILE_SUFFIX));
    }

    // Convert the two permutations of the pair concurrently. They are
    // independent of each other (each has its own reader, its own writer, and
    // its own metadata). One of the two conversions runs on this thread, so
    // that only one additional thread is needed.
    //
    // NOTE: If the conversion on this thread throws, the destructor of
    // `futureB` waits for the other conversion to finish before the exception
    // leaves this function, so no thread is still writing to the incomplete
    // index when the caller handles the error.
    auto futureB = std::async(
        std::launch::async, [&, enumB = enumB, isInternal = isInternal]() {
          return convertPermutation(enumB, isInternal, progress);
        });
    auto newMetaA = convertPermutation(enumA, isInternal, progress);
    auto newMetaB = futureB.get();
    // The multiplicities of the last column of a permutation are stored in the
    // metadata of its twin, so they have to be exchanged before the metadata is
    // written.
    newMetaA.exchangeMultiplicities(newMetaB);
    writeMetaData(newMetaA,
                  filenameForPermutation(newBasename, enumA, isInternal));
    writeMetaData(newMetaB,
                  filenameForPermutation(newBasename, enumB, isInternal));
  }
  progressBar.logFinalProgressString();
}

// _____________________________________________________________________________
std::vector<std::string> findMaterializedViews(const std::string& oldBasename) {
  // Each view has exactly one info file, so the names of the views are exactly
  // the infixes of those files (`<basename>.view.<name><suffix>`).
  auto viewFiles =
      qlever::util::filesWithBaseNameAndSuffix(oldBasename, VIEW_FILE_INFIX);
  std::string prefix = materializedViewFilenameBase(oldBasename, "");
  std::vector<std::string> names;
  for (const auto& file : viewFiles) {
    std::string_view filename = file.native();
    if (!ql::ends_with(filename, VIEW_INFO_SUFFIX)) {
      continue;
    }
    AD_CORRECTNESS_CHECK(ql::starts_with(filename, prefix));
    filename.remove_prefix(prefix.size());
    filename.remove_suffix(VIEW_INFO_SUFFIX.size());
    names.emplace_back(filename);
  }
  // Every file of every view has to be handled by the conversion of that view,
  // else the converted view would be incomplete.
  size_t numExpectedFiles = names.size() * VIEW_ALL_SUFFIXES.size();
  if (viewFiles.size() != numExpectedFiles) {
    throw std::runtime_error{absl::StrCat(
        "The index \"", oldBasename, "\" has ", viewFiles.size(),
        " files that belong to a materialized view, but its ", names.size(),
        " views consist of ", numExpectedFiles,
        " files. Please delete the files that do not belong to a complete "
        "view, or delete the incomplete views.")};
  }
  return names;
}

// _____________________________________________________________________________
void copyViewInfo(const std::string& oldBasename, const std::string& name,
                  const std::string& newBasename, size_t expectedVersion) {
  // Copy the metadata of the view, with the version of the on-disk format of
  // the views raised to the current one (an unconverted view has to be rejected
  // by the engine, see `MATERIALIZED_VIEWS_VERSION`).
  nlohmann::json viewInfo;
  ad_utility::makeIfstream(absl::StrCat(
      materializedViewFilenameBase(oldBasename, name), VIEW_INFO_SUFFIX)) >>
      viewInfo;
  auto viewsVersion = viewInfo.at("version").get<size_t>();
  if (viewsVersion != expectedVersion) {
    throw std::runtime_error{absl::StrCat(
        "The materialized view \"", name, "\" of the index \"", oldBasename,
        "\" is stored in the format version ", viewsVersion,
        ", but this converter only converts views in the format version ",
        expectedVersion,
        ". Please delete the view and create it again after the conversion.")};
  }
  viewInfo["version"] = MATERIALIZED_VIEWS_VERSION;
  ad_utility::makeOfstream(absl::StrCat(
      materializedViewFilenameBase(newBasename, name), VIEW_INFO_SUFFIX))
      << viewInfo.dump() << std::endl;
}

// _____________________________________________________________________________
void upgradeIndexInPlace(
    const std::string& basename, const nlohmann::json& configuration,
    std::string_view stagingDirPrefix, std::string_view retiredDirPrefix,
    const std::function<void(const std::string&)>& convertTo) {
  // Derive the staging and retirement directories. There is no command-line
  // option for choosing these directories, hence no hint for the (unlikely)
  // error that the default names are all taken.
  IndexSwapNaming naming{std::string{stagingDirPrefix},
                         std::string{retiredDirPrefix},
                         IndexImpl::dateOfIndexBuild(configuration, basename),
                         /* retiredDirConflictHint_ */ ""};
  IndexSwapConfig config =
      makeIndexSwapConfig(basename, naming, std::nullopt, std::nullopt);

  convertTo(config.newIndexSource());
  checkUpgradedIndex(config.newIndexSource(),
                     static_cast<Index::NumNormalAndInternal>(
                         configuration.at("num-triples")));
  moveIndexIntoPlace(config);
  AD_LOG_INFO << "The upgrade was successful: the upgraded index is at \""
              << config.newIndexTarget() << "\"" << std::endl;
  AD_LOG_INFO << "The index in the old format was moved to the directory \""
              << fs::path{config.oldIndexTarget()}.parent_path().string()
              << "\"" << std::endl;
}

}  // namespace qlever::indexConverterBase
