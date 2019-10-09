#!/bin/bash

#!/usr/bin/env bash
set -u

## locate executable
clang_tidy="${ARANGODB_CLANG_TIDY:-""}"
clang_tidy_version="${ARANGODB_CLANG_TIDY_VERSION:-"9.0.0"}"

# fallback if nothing is found
if [[ -z $clang_tidy ]]; then
    for candidate in "$HOME/.local/bin/clang-tidy" "clang-tidy-arangodb" "clang-tidy-9" "clang-tidy"; do
        echo "checking candidate $candidate"
        path="$(type -p ${candidate})"
        if [[ -n $path ]]; then
            clang_tidy="$path"
            echo "selecting this candidate"
            break;
        fi
    done
else
    echo "clang-tidy provided by environment"
fi

# fallback if nothing is found
if [[ -z $clang_tidy ]]; then
    echo "using fallback"
    clang_tidy="clang-tidy"
fi

## check version
echo "checking version of $clang_tidy"
version_string=$(${clang_tidy} --version)
re=".*version ${clang_tidy_version}.*"
if ! [[ $version_string =~ $re ]]; then
    echo "your version: '$version_string' does not match version 6.0.1"
    exit 1
fi

## find relevant files
files="$(
    find arangod arangosh lib enterprise \
        -name Zip -prune -o \
        -type f "(" -name "*.cpp" -o -name "*.h" ")" \
        "!" "(" -name "tokens.*" -o -name "v8-json.*" -o -name "voc-errors.*" -o -name "grammar.*" -o -name "xxhash.*" -o -name "exitcodes.*" ")"
)"

${clang_tidy} -checks=-*,clang-analyzer-*,-clang-analyzer-cplusplus* ${files}
