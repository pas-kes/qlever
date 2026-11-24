// Copyright 2026 The QLever Authors, in particular:
//
// 2026 Pascal Keßler <kesslerp@cs.uni-freiburg.de>, UFR
//
// UFR = University of Freiburg, Chair of Algorithms and Data Structures
//
// You may not use this file except in compliance with the Apache 2.0 License,
// which can be found in the `LICENSE` file at the root of the QLever project.

#ifndef QLEVER_SRC_INDEX_FOLDEDINDEXREADER_H
#define QLEVER_SRC_INDEX_FOLDEDINDEXREADER_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "engine/idTable/IdTable.h"
#include "global/FoldedValueId.h"
#include "global/Id.h"
#include "index/IndexMetaData.h"
#include "index/PatternCreator.h"
#include "index/vocabulary/EncodedIriManager.h"
#include "util/CompactStringVector.h"
#include "util/Executor.h"
#include "util/Iterators.h"
#include "util/json.h"

// Reading an index with the folded layout of the `Id` (see
// `foldedValueId::ValueId`) in a build with any layout, with all `Id`s
// converted to the `Id` of this build. The readers of a build with
// `USE_SPLIT_LAYOUT` read the split layout, so the `SplitLayoutIdConverter`
// (see `index/SplitLayoutIdConverter.h`) reads the index that it converts
// with this reader. Only the size of an `Id` on disk differs: 8 bytes in the
// metadata and a single ZSTD frame of 8-byte `Id`s per column of a block.
namespace qlever::foldedIndexReader {

// Converts the `Id`s of an index with the folded layout in the current format,
// given as their raw 64 bits, to the `Id`s of this binary. With the folded
// layout, nothing changes. With `USE_SPLIT_LAYOUT`, an `Id` keeps its datatype
// and its value: integers and doubles are converted via their value, encoded
// IRIs are re-encoded for the 64 bits of the split layout, and all other `Id`s
// keep their payload. This preserves the order of any two `Id`s, so a
// permutation can be converted by rewriting its `Id`s one by one.
class FoldedIdConverter {
 public:
  // The `EncodedIriManager` for the folded layout, which encodes the digits of
  // an IRI in 52 bits instead of the 56 bits of the split layout.
  using FoldedEncodedIriManager =
      EncodedIriManagerImpl<foldedValueId::ValueId::numDataBits, 8,
                            AlwaysOnPrefixes>;

 private:
  FoldedEncodedIriManager foldedEncodedIriManager_;
  EncodedIriManager encodedIriManager_;
  // For each pattern of the encoded IRIs (by its tag), whether its digits can
  // simply be moved instead of decoding and encoding the IRI again, see
  // `convertEncodedIri`.
  std::vector<bool> canMoveDigits_;

 public:
  // The encoded IRIs of the index are described by the `encodedIriPrefixes`
  // (the entry `encoded-iri-prefixes` of its configuration).
  explicit FoldedIdConverter(const nlohmann::json& encodedIriPrefixes);

  // The converter for the index with the base name `basename` and the given
  // `configuration`. The split layout encodes IRIs with up to 14 instead of 13
  // digits. Such an IRI is a word of the vocabulary of the index, but a query
  // for it would look for the encoded `Id` (`toValueIdOrBounds` tries the
  // encoding first), so throw if the vocabulary has such an IRI.
  static FoldedIdConverter forIndex(const std::string& basename,
                                    const nlohmann::json& configuration);

  // Convert the `Id` with the given raw `bits`. Throw if it is not a valid `Id`
  // of the index that is converted, and also if it is of type
  // `LocalVocabIndex`, which must never be stored on disk (such an `Id` holds a
  // pointer into the memory of the process that created it).
  Id operator()(uint64_t bits) const;

 private:
  // Convert an encoded IRI with the given `payload` in the folded layout.
  Id convertEncodedIri(uint64_t payload) const;
};

// The metadata of the blocks of the permutation in the file `filename`, with
// converted `Id`s. The metadata of the relations is not read, a converted
// permutation is written from scratch.
IndexMetaData readMetaData(const std::string& filename,
                           const FoldedIdConverter& convert);

// All blocks of the permutation in the file `filename` with the given
// `metaData`, with converted `Id`s. The number of rows of each block is
// reported to `progress`. The blocks are read and decompressed in parallel on
// the `executor`, like in a lazy index scan.
ad_utility::InputRangeTypeErased<IdTableStatic<0>> readBlocks(
    const std::string& filename, const IndexMetaData& metaData,
    const FoldedIdConverter& convert, std::function<void(size_t)> progress,
    qlever::Executor executor);

// The patterns in the file `filename` (see
// `PatternCreator::writePatternsToFile`), with converted `Id`s.
CompactVectorOfStrings<Id> readPatterns(const std::string& filename,
                                        const FoldedIdConverter& convert,
                                        PatternStatistics& statistics);

}  // namespace qlever::foldedIndexReader

#endif  // QLEVER_SRC_INDEX_FOLDEDINDEXREADER_H
