// Copyright 2026 The QLever Authors, in particular:
//
// 2026 Johannes Kalmbach <kalmbach@cs.uni-freiburg.de>, UFR
// 2026 Pascal Keßler <kesslerp@cs.uni-freiburg.de>, UFR
//
// UFR = University of Freiburg, Chair of Algorithms and Data Structures
//
// You may not use this file except in compliance with the Apache 2.0 License,
// which can be found in the `LICENSE` file at the root of the QLever project.

#ifndef QLEVER_SRC_INDEX_INDEXCONVERTERBASE_H
#define QLEVER_SRC_INDEX_INDEXCONVERTERBASE_H

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "backports/filesystem.h"
#include "engine/idTable/IdTable.h"
#include "global/Id.h"
#include "index/Index.h"
#include "index/IndexMetaData.h"
#include "index/Permutation.h"
#include "util/Executor.h"
#include "util/Iterators.h"
#include "util/ProgressBar.h"
#include "util/json.h"

// The steps that the index converters share: the one that upgrades an index
// to the current format (see `index/IndexFormatConverter.h`) and the one that
// converts an index to the split layout of the `Id` (see
// `index/SplitLayoutIdConverter.h`). A converter only provides how it reads
// and converts the `Id`s of a permutation, of the patterns, and of a view.
namespace qlever::indexConverterBase {

// The permutations of an index, as pairs of "twins" (like `PSO` and `POS`),
// together with the information whether the pair is the pair of internal
// permutations. The multiplicities of the last column of a permutation are
// stored in the metadata of its twin (see
// `IndexMetaData::exchangeMultiplicities`), which is why the permutations have
// to be converted pairwise.
using PermutationPair = std::pair<Permutation::Enum, Permutation::Enum>;
extern const std::array<std::pair<PermutationPair, bool>, 4> permutationPairs;

// The base name that the permutation files of the index with the base name
// `basename` share, and the file of the given `permutation`.
std::string basenameForPermutations(std::string_view basename, bool isInternal);
std::string filenameForPermutation(std::string_view basename,
                                   Permutation::Enum permutation,
                                   bool isInternal);

// Whether the index with the base name `basename` has the given permutation.
bool hasPermutation(const std::string& basename,
                    Permutation::Enum permutationEnum, bool isInternal);

// Throw if the two base names are equal or empty.
void checkBasenames(const std::string& oldBasename,
                    const std::string& newBasename);

// Throw if there already are files with the base name `newBasename` (the
// converted index must not overwrite anything). Create its directory.
void checkNewBasenameIsUnused(const std::string& newBasename);

// Read the configuration of the index with the base name `basename`. Throw if
// the index does not exist or if its configuration has no index format
// version. The converters check the version and the layout themselves.
nlohmann::json readConfiguration(const std::string& basename);

// Throw if the index has persisted updates, which are not converted.
void throwIfPersistedUpdatesExist(const std::string& basename);

// A threadsafe callback that reports progress to the `progressBar`, and the
// batch size for a progress bar with the given `total`.
std::function<void(size_t)> progressCallbackFor(
    ad_utility::ConcurrentProgressBar& progressBar);
size_t batchSizeFor(size_t total);

// The number of columns of a permutation with the given `metaData`.
size_t numColumns(const IndexMetaData& metaData);

// Write the converted `blocks` of the permutation with the metadata
// `oldMetaData` (whose `Id`s are converted with `convertId`) as a single
// permutation to `newFilename`, with the block size `rowsPerBlock`. The blocks
// are sorted, because the conversion preserves the order of the `Id`s. Check
// that the result has the same number of triples and the same first and last
// triple, and return its metadata. It is only written by `writeMetaData`,
// after the multiplicities were exchanged with the twin permutation. The
// blocks are compressed and written on the `executor`.
IndexMetaData writeConvertedPermutation(
    const IndexMetaData& oldMetaData,
    ad_utility::InputRangeTypeErased<IdTableStatic<0>> blocks,
    const std::function<Id(Id)>& convertId, const std::string& newFilename,
    size_t rowsPerBlock, qlever::Executor executor);
void writeMetaData(IndexMetaData& metaData, const std::string& filename);

// Convert all permutations of the index with the base name `oldBasename` to
// the index with the base name `newBasename`, the two of a pair concurrently.
// `convertPermutation` converts and writes one permutation and returns its
// metadata (see `writeConvertedPermutation`), reporting its progress to the
// given callback. The `numTriples` (from the configuration) are the total of
// the progress bar. The files of the old permutations are added to
// `handledFiles`.
using ConvertPermutation = std::function<IndexMetaData(
    Permutation::Enum, bool isInternal, const std::function<void(size_t)>&)>;
void convertPermutationPairs(const std::string& oldBasename,
                             const std::string& newBasename,
                             const Index::NumNormalAndInternal& numTriples,
                             std::vector<ql::filesystem::path>& handledFiles,
                             const ConvertPermutation& convertPermutation);

// The names of the materialized views of the index. Throw if a view is
// incomplete.
std::vector<std::string> findMaterializedViews(const std::string& oldBasename);

// Copy the info file of the view `name` to the converted index, with the
// current version of the views. Throw if the view has another version than
// `expectedVersion`.
void copyViewInfo(const std::string& oldBasename, const std::string& name,
                  const std::string& newBasename, size_t expectedVersion);

// Copy all files that contain no `Id`s, and check that all files of the index
// were either converted or copied.
void copyFilesThatNeedNoConversion(
    const std::string& oldBasename, const std::string& newBasename,
    std::vector<ql::filesystem::path>& handledFiles);
void checkAllFilesWereHandled(
    const std::string& oldBasename,
    const std::vector<ql::filesystem::path>& handledFiles);

// Upgrade the index with the base name `basename` and the given
// `configuration` in place: `convertTo` writes the converted index to the
// given staging base name, which is then checked and moved into place (see
// `upgradeIndexInPlace` in `index/IndexFormatConverter.h`).
void upgradeIndexInPlace(
    const std::string& basename, const nlohmann::json& configuration,
    std::string_view stagingDirPrefix, std::string_view retiredDirPrefix,
    const std::function<void(const std::string&)>& convertTo);

}  // namespace qlever::indexConverterBase

#endif  // QLEVER_SRC_INDEX_INDEXCONVERTERBASE_H
