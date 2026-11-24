#!/usr/bin/env bash
# Create the index in this directory from the input files of `../oldIndexFormat`
# with the `qlever-index` binary of a build without `USE_SPLIT_LAYOUT` (see
# `README.md`).
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <path to a qlever-index binary with the folded layout>" >&2
  exit 1
fi
QLEVER_INDEX="$(realpath "$1")"
cd "$(dirname "$0")"
INPUT=../oldIndexFormat

"$QLEVER_INDEX" \
  --index-basename folded \
  --kg-input-file "$INPUT/input.ttl" \
  --file-format ttl \
  --settings-file "$INPUT/settings.json" \
  --text-words-input-file "$INPUT/words.tsv" \
  --text-docs-input-file "$INPUT/docs.tsv" \
  --encode-as-id "https://example.org/id/" \
  --materialized-views '{"testview": "SELECT ?s ?o WHERE { ?s <http://example.org/label> ?o }"}' \
  --no-resource-usage-log

cp "$INPUT/settings.json" folded.settings.json
