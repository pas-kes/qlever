// Copyright 2026 The QLever Authors, in particular:
//
// 2026 Pascal Keßler <kesslerp@cs.uni-freiburg.de>, UFR
//
// UFR = University of Freiburg, Chair of Algorithms and Data Structures
//
// You may not use this file except in compliance with the Apache 2.0 License,
// which can be found in the `LICENSE` file at the root of the QLever project.

#ifndef QLEVER_SRC_INDEX_SPLITLAYOUTIDCONVERTER_H
#define QLEVER_SRC_INDEX_SPLITLAYOUTIDCONVERTER_H

#include <string>

#include "util/Executor.h"

// The conversion of an index with the folded layout of the `Id` (see
// `foldedValueId::ValueId`) to the split layout (`USE_SPLIT_LAYOUT`). Only the
// layout changes: the index has to be in a current format, and a change of the
// format is the job of `index/IndexFormatConverter.h`. `qlever-upgrade-index`
// uses it in a build with `USE_SPLIT_LAYOUT` for an index with the folded
// layout (see `src/IndexUpgraderMain.cpp`). An index with the layout of the
// binary is upgraded by `index/IndexFormatConverter.h`. The steps that both
// converters share are in `index/IndexConverterBase.h`.
//
// The index that is converted is read with `index/FoldedIndexReader.h`,
// because the readers of a split build read the split layout.
namespace qlever::splitLayoutIdConverter {

// Return a description of the conversion, the overview message of
// `qlever-upgrade-index`.
std::string conversionDescription();

// Convert the index with the base name `oldBasename` to the split layout and
// write it to the base name `newBasename`, like
// `indexFormatConverter::convertIndexToCurrentFormat`. The index has to have
// the folded layout and a current format, which the converted index keeps.
//
// Throw if the index does not exist, if it already has the split layout, if it
// is not in a current format, if it has persisted updates, if its vocabulary
// has an IRI that the split layout encodes (see `FoldedIdConverter::forIndex`),
// if a materialized view is incomplete or in another version, or if any of the
// files at `newBasename` already exist.
void convertIndexToSplitLayout(const std::string& oldBasename,
                               const std::string& newBasename,
                               qlever::Executor executor);

// Convert the index in place, like `indexFormatConverter::upgradeIndexInPlace`.
void upgradeIndexInPlace(const std::string& basename,
                         qlever::Executor executor);

}  // namespace qlever::splitLayoutIdConverter

#endif  // QLEVER_SRC_INDEX_SPLITLAYOUTIDCONVERTER_H
