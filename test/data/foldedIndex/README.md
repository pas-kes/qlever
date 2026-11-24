# An index with the folded layout in the current format

This directory contains a (very small) QLever index in the current index format
with the folded layout of the `Id` (built without `USE_SPLIT_LAYOUT`). It is
used by `test/index/SplitLayoutIdConverterTest.cpp` to test the conversion of
an index to the split layout (see `src/index/SplitLayoutIdConverter.h`).

It is built from the input files of `../oldIndexFormat` (see the `README.md`
there for what they cover: all datatypes that occur in a permutation, the
patterns, a text index, and a materialized view). It was created by running
`generate.sh` with the `qlever-index` binary of a build without
`USE_SPLIT_LAYOUT`. The test copies exactly the files whose name starts with
`folded.` into a temporary directory of its own.
