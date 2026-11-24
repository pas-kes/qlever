add_subdirectory(vocabulary)
add_subdirectory(vocabularyMerger)
add_subdirectory(compressedRelationWriter)
add_subdirectory(chunkwisePermutationWriter)
addLinkAndDiscoverTest(PatternCreatorTest index)
addLinkAndDiscoverTest(ScanSpecificationTest index)
addLinkAndDiscoverTest(PermutationPairWriterTest index)
addLinkAndDiscoverTestNoLibs(KeyOrderTest)
addLinkAndDiscoverTestNoLibs(EncodedIriManagerTest vocabulary)
addLinkAndDiscoverTestNoLibs(EncodedIriPatternTest vocabulary)
addLinkAndDiscoverTestNoLibs(NibbleEncodingTest vocabulary)
addLinkAndDiscoverTest(GraphNameManagerTest index)
# The steps that both index converters share, in both layouts.
addLinkAndDiscoverTest(IndexConverterBaseTest index engine)
# The converter that `qlever-upgrade-index` uses depends on the layout of the
# `Id` (see `src/IndexUpgraderMain.cpp`), so only its test is built.
if (NOT USE_SPLIT_LAYOUT)
    addLinkAndDiscoverTest(IndexFormatConverterTest index engine)
else ()
    addLinkAndDiscoverTest(SplitLayoutIdConverterTest index engine)
endif ()
# Emscripten tests cannot access the host filesystem, so embed the test data.
if (EMSCRIPTEN AND NOT USE_SPLIT_LAYOUT)
    if (SINGLE_TEST_BINARY)
        set(target QLeverAllUnitTestsMain)
    else ()
        set(target IndexFormatConverterTest)
    endif ()
    target_link_options(${target} PRIVATE
            "--embed-file=${QLEVER_TEST_DATA_DIR}@${QLEVER_TEST_DATA_DIR}")
    # Relink when the test data changes.
    file(GLOB_RECURSE testDataFiles CONFIGURE_DEPENDS "${QLEVER_TEST_DATA_DIR}/*")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${testDataFiles})
endif ()
# When the `server` library is not built (Emscripten, see
# `src/engine/CMakeLists.txt`), the server-integration test is compiled out
# via `#ifndef __EMSCRIPTEN__` in the test file and all other tests still run.
if (TARGET server)
    addLinkAndDiscoverTest(IndexRebuilderTest index server)
else ()
    addLinkAndDiscoverTest(IndexRebuilderTest index)
endif ()
addLinkAndDiscoverTest(IndexSwapTest index)
addLinkAndDiscoverTest(InputFileSpecificationTest sparqlParser index)
addLinkAndDiscoverTest(VocabularyMergerImplTest index)
addLinkAndDiscoverTest(PartialVocabularyFileTest index)
addLinkAndDiscoverTest(PartialVocabularyBuilderTest index)
