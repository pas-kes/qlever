// Copyright 2026 The QLever Authors, in particular:
//
// 2026 Pascal Keßler <kesslerp@cs.uni-freiburg.de>, UFR
//
// UFR = University of Freiburg, Chair of Algorithms and Data Structures
//
// You may not use this file except in compliance with the Apache 2.0 License,
// which can be found in the `LICENSE` file at the root of the QLever project.

#include "index/FoldedIndexReader.h"

#include <absl/strings/str_cat.h>

#include "global/RuntimeParameters.h"
#include "index/ConstantsIndexBuilding.h"
#include "index/IndexConverterBase.h"
#include "index/vocabulary/Vocabulary.h"
#include "index/vocabulary/VocabularyType.h"
#include "util/BitUtils.h"
#include "util/File.h"
#include "util/compressionUsingZstd/ZstdWrapper.h"
#include "util/serializer/ByteBufferSerializer.h"
#include "util/serializer/FileSerializer.h"
#include "util/views/AsyncTransformView.h"

namespace qlever::foldedIndexReader {

namespace {
// A serializer that reads every `Id` as the 8 bytes of a folded `Id` and
// converts it (see the `serialize` function below), and everything else
// unchanged from the `UnderlyingSerializer`. So the metadata is read with the
// normal `serialize` functions of its classes.
//
// NOTE: With the folded layout, a vector of `Id`s (the graphs of a block) is
// read in one piece without a conversion. Only the first and last triple of a
// block are used (see `verifyConvertedPermutation`), so this does not matter.
template <typename UnderlyingSerializer>
struct FoldedIdReadSerializer {
  using SerializerType = ad_utility::serialization::ReadSerializerTag;
  UnderlyingSerializer& underlyingSerializer_;
  const FoldedIdConverter& convert_;

  void serializeBytes(char* bytePointer, size_t numBytes) {
    underlyingSerializer_.serializeBytes(bytePointer, numBytes);
  }
};

// More specialized than the `serialize` function of an `Id`, so it is chosen
// for every `Id` that is read with a `FoldedIdReadSerializer`.
template <typename UnderlyingSerializer>
void serialize(FoldedIdReadSerializer<UnderlyingSerializer>& serializer,
               Id& id) {
  uint64_t bits;
  serializer.serializeBytes(reinterpret_cast<char*>(&bits), sizeof(bits));
  id = serializer.convert_(bits);
}
}  // namespace

// _____________________________________________________________________________
FoldedIdConverter::FoldedIdConverter(const nlohmann::json& encodedIriPrefixes)
    : foldedEncodedIriManager_{encodedIriPrefixes
                                   .get<FoldedEncodedIriManager>()},
      encodedIriManager_{encodedIriPrefixes.get<EncodedIriManager>()} {
  // The digits of a plain prefix are left-aligned nibbles (see
  // `NibbleEncoding.h`), so they only move to the left. A pattern is a plain
  // prefix of the split layout only if the configuration lists prefixes, then
  // it also is one of the folded layout. If it lists patterns, a plain prefix
  // is stored as a pattern of the folded layout and loaded as it is.
  for (const auto& pattern : encodedIriManager_.patterns()) {
    canMoveDigits_.push_back(encodedIri::isPlainPrefixPattern(
        pattern, EncodedIriManager::NumBitsEncoding));
  }
}

namespace {
// Throw if the vocabulary of the index with the base name `basename` and the
// given `configuration` has an IRI that the `encodedIriManager` encodes (see
// `FoldedIdConverter::forIndex`). Only the words that start with the prefix of
// a pattern can be encoded, so only these ranges of the (sorted) vocabulary
// are checked.
[[maybe_unused]] void throwIfVocabularyHasEncodableIris(
    const std::string& basename, const nlohmann::json& configuration,
    const EncodedIriManager& encodedIriManager) {
  RdfsVocabulary vocabulary;
  vocabulary.resetToType(configuration.value(
      "vocabulary-type",
      ad_utility::VocabularyType{
          ad_utility::VocabularyType::Enum::OnDiskCompressed}));
  const auto& locale = configuration.at("locale");
  vocabulary.setLocale(locale.at("language").get<std::string>(),
                       locale.at("country").get<std::string>(),
                       locale.at("ignore-punctuation").get<bool>());
  vocabulary.readFromFile(absl::StrCat(basename, VOCAB_SUFFIX));
  for (const auto& pattern : encodedIriManager.patterns()) {
    auto [begin, end] = vocabulary.prefixRanges(pattern.prefix_).ranges()[0];
    for (uint64_t index = begin.get(); index < end.get(); ++index) {
      auto word = vocabulary[VocabIndex::make(index)];
      if (encodedIriManager.encode(word).has_value()) {
        throw std::runtime_error{absl::StrCat(
            "The vocabulary of the index \"", basename, "\" contains the IRI ",
            word,
            ", which the split layout encodes as an `Id`. Such an index cannot "
            "be converted, please rebuild it.")};
      }
    }
  }
}
}  // namespace

// _____________________________________________________________________________
FoldedIdConverter FoldedIdConverter::forIndex(
    const std::string& basename, const nlohmann::json& configuration) {
  // An index without this entry uses the default patterns, like in
  // `IndexImpl::readConfiguration`.
  auto encodedIriPrefixes = configuration.value(
      "encoded-iri-prefixes", nlohmann::json(EncodedIriManager{}));
  FoldedIdConverter converter{encodedIriPrefixes};
  // Only the split layout has more bits for the encoding.
  if constexpr (EncodedIriManager::NumBitsEncoding !=
                FoldedEncodedIriManager::NumBitsEncoding) {
    throwIfVocabularyHasEncodableIris(basename, configuration,
                                      converter.encodedIriManager_);
  }
  return converter;
}

// _____________________________________________________________________________
Id FoldedIdConverter::operator()(uint64_t bits) const {
  auto foldedId = foldedValueId::ValueId::fromBits(bits);
  auto datatypeBits = foldedId.getDatatypeBits();
  if (datatypeBits >= qlever::enumSize<Datatype>) {
    throw std::runtime_error{absl::StrCat(
        "Encountered an `Id` with the invalid datatype ", datatypeBits,
        ", the index that is converted is corrupted")};
  }
  auto datatype = static_cast<Datatype>(datatypeBits);
  auto payload = foldedId.getPayloadBits();
  // With the folded layout, every case returns the same `Id`.
  switch (datatype) {
    case Datatype::LocalVocabIndex:
      throw std::runtime_error{
          "Encountered an `Id` of type `LocalVocabIndex`, which must never be "
          "stored on disk (it holds a pointer into the memory of the process "
          "that created it), so the index that is converted is corrupted"};
    case Datatype::Int:
      return Id::makeFromInt(
          foldedValueId::ValueId::fromBits(datatype, payload).getInt());
    case Datatype::Double:
      return Id::makeFromDouble(
          foldedValueId::ValueId::fromBits(datatype, payload).getDouble());
    case Datatype::EncodedVal:
      return convertEncodedIri(payload);
    default:
      return Id::fromBits(datatype, payload);
  }
}

// _____________________________________________________________________________
Id FoldedIdConverter::convertEncodedIri(uint64_t payload) const {
  static constexpr size_t foldedBits = FoldedEncodedIriManager::NumBitsEncoding;
  static constexpr size_t bits = EncodedIriManager::NumBitsEncoding;
  if constexpr (foldedBits == bits) {
    return Id::makeFromEncodedVal(payload);
  } else {
    // The tag (the index of the pattern) is the same in both layouts. If
    // possible, the digits are moved (see the constructor), else the IRI is
    // decoded and encoded again.
    auto tag = payload >> foldedBits;
    if (canMoveDigits_.at(tag)) {
      auto digits = payload & ad_utility::bitMaskForLowerBits(foldedBits);
      return Id::makeFromEncodedVal((tag << bits) |
                                    (digits << (bits - foldedBits)));
    }
    auto iri =
        foldedEncodedIriManager_.toString(Id::makeFromEncodedVal(payload));
    auto id = encodedIriManager_.encode(iri);
    AD_CORRECTNESS_CHECK(id.has_value(), "The IRI ", iri,
                         " was encoded with the folded layout, so it also has "
                         "to be encodable with the split layout");
    return id.value();
  }
}

// _____________________________________________________________________________
IndexMetaData readMetaData(const std::string& filename,
                           const FoldedIdConverter& convert) {
  ad_utility::File file{filename, "r"};
  ad_utility::serialization::ByteBufferReadSerializer underlyingSerializer{
      IndexMetaData::readSerializedBlockMetaData(file)};
  FoldedIdReadSerializer<decltype(underlyingSerializer)> serializer{
      underlyingSerializer, convert};
  IndexMetaData metaData;
  serializer >> metaData;
  return metaData;
}

// _____________________________________________________________________________
ad_utility::InputRangeTypeErased<IdTableStatic<0>> readBlocks(
    const std::string& filename, const IndexMetaData& metaData,
    const FoldedIdConverter& convert, std::function<void(size_t)> progress,
    qlever::Executor executor) {
  auto file = std::make_shared<ad_utility::File>(filename, "r");
  auto blocks = metaData.blockDataShared();
  size_t numColumns = indexConverterBase::numColumns(metaData);
  auto readBlock = [file, blocks, numColumns, &convert,
                    progress = std::move(progress)](size_t blockIndex) {
    const auto& block = blocks->at(blockIndex);
    IdTableStatic<0> table{numColumns,
                           ad_utility::makeUnlimitedAllocator<Id>()};
    table.resize(block.numRows_);
    std::vector<uint64_t> bits(block.numRows_);
    for (size_t column = 0; column < numColumns; ++column) {
      const auto& [offset, compressedSize] =
          block.getOffsetAndCompressedSizeForColumn(column);
      std::vector<char> compressed(compressedSize);
      file->read(compressed.data(), compressedSize, offset);
      // A block for which no bytes were written is empty (see
      // `ZstdContext::decompressChunks`).
      if (compressed.empty()) {
        AD_CORRECTNESS_CHECK(bits.empty());
        continue;
      }
      size_t numBytes = ZstdWrapper::decompressToBuffer(
          compressed.data(), compressed.size(), bits.data(),
          bits.size() * sizeof(uint64_t));
      AD_CORRECTNESS_CHECK(numBytes == bits.size() * sizeof(uint64_t));
      ql::ranges::transform(bits, table.getColumn(column).begin(),
                            std::cref(convert));
    }
    progress(block.numRows_);
    return table;
  };
  using Blocks = ad_utility::AsyncTransformView<
      decltype(ad_utility::integerRange(size_t{})), decltype(readBlock)>;
  size_t numBlocksInFlight =
      getRuntimeParameter<
          &RuntimeParameters::lazyIndexScanNumBlocksInFlight_>() +
      1;
  return ad_utility::InputRangeTypeErased<IdTableStatic<0>>{
      std::make_unique<Blocks>(ad_utility::integerRange(blocks->size()),
                               std::move(readBlock), numBlocksInFlight,
                               std::move(executor))};
}

// _____________________________________________________________________________
CompactVectorOfStrings<Id> readPatterns(const std::string& filename,
                                        const FoldedIdConverter& convert,
                                        PatternStatistics& statistics) {
  // The `Id`s are read as their raw bits.
  CompactVectorOfStrings<uint64_t> foldedPatterns;
  ad_utility::serialization::FileReadSerializer reader{filename};
  reader >> statistics;
  reader >> foldedPatterns;
  auto convertPattern = [&convert](const auto& foldedPattern) {
    std::vector<Id> pattern;
    pattern.reserve(foldedPattern.size());
    ql::ranges::transform(foldedPattern, std::back_inserter(pattern),
                          std::cref(convert));
    return pattern;
  };
  CompactVectorOfStrings<Id> patterns;
  patterns.build(foldedPatterns | ql::views::transform(convertPattern));
  return patterns;
}

}  // namespace qlever::foldedIndexReader
