#!/usr/bin/env bash
set -u

## locate executable
clang_format="${ARANGODB_CLANG_FORMAT:-""}"
clang_format_version="${ARANGODB_CLANG_FORMAT_VERSION:-"6.0.1"}"

# fallback if nothing is found
if [[ -z $clang_format ]]; then
    for candidate in "$HOME/.local/bin/clang-format" "clang-format-arangodb" "clang-format-6" "clang-format"; do
        echo "checking candidate $candidate"
        path="$(type -p ${candidate})"
        if [[ -n $path ]]; then
            clang_format="$path"
            echo "selecting this candidate"
            break;
        fi
    done
else
    echo "clang-format provided by environment"
fi

# fallback if nothing is found
if [[ -z $clang_format ]]; then
    echo "using fallback"
    clang_format="clang-format"
fi

## check version
echo "checking version of $clang_format"
version_string=$(${clang_format} --version)
re=".*version ${clang_format_version}.*"
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

# do final formatting
${clang_format} -i -verbose -style=file ${files[@]}
