#!/bin/bash
# Evidence that the audio test passed on the files it depends on.
#
#   tests/audio_evidence.sh run    builds audio_render_tests in build-audio/,
#                                  plays it 20 times one after another, stops
#                                  at the first failure and writes
#                                  tests/audio_evidence.json
#   tests/audio_evidence.sh check  fails unless that file records a pass on
#                                  covered files identical to these
#
# files_hash is a SHA-256 over the SHA-256 and path of each covered file, so
# a changed, added, removed or renamed covered file changes it, and a rebase
# or a change to any other file does not.
set -euo pipefail

cd "$(dirname "$0")/.."

readonly evidence=tests/audio_evidence.json
readonly build_dir=build-audio
readonly run_count=20
readonly tolerance_keys='["tau_ms", "level_delta_db", "peak_delta_db", "drift_limit_ms", "take_lateness_limit_ms"]'

# Covered files under the current directory, sorted by path: the root
# sources, the build files, the audio test's files and the fake MIDI input.
covered_files() {
    {
        find . -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.hpp' -o -name CMakeLists.txt \)
        find cmake -type f
        find tests -type f \( -path 'tests/audio_*' -o -path 'tests/fake_midi_input.*' \)
    } | sed 's|^\./||' | grep -vxF "$evidence" | LC_ALL=C sort
}

# One line per covered file: its SHA-256 and its path.
file_hashes() {
    covered_files | xargs -d '\n' sha256sum --
}

files_hash() {
    file_hashes | sha256sum | cut -d ' ' -f 1
}

# Extracts the tree of commit $1 into a new temporary directory and prints
# the directory.
extract() {
    local directory
    directory=$(mktemp -d)
    git archive "$1" | tar -x -C "$directory"
    echo "$directory"
}

# Covered files that differ between the tree in directory $1 and the work
# tree, added and removed ones included.
changed_files() {
    { diff <(cd "$1" && file_hashes) <(file_hashes) || true; } |
        sed -n 's/^[<>] [0-9a-f]\{64\}  //p' | LC_ALL=C sort -u
}

# Busy and total time of all cores since boot, in clock ticks.
cpu_ticks() {
    local user nice system idle iowait irq softirq steal
    read -r _ user nice system idle iowait irq softirq steal _ < /proc/stat
    echo "$((user + nice + system + irq + softirq + steal))" \
        "$((user + nice + system + idle + iowait + irq + softirq + steal))"
}

# Average use of all cores between two cpu_ticks readings, in percent.
cpu_percent() {
    local busy_before total_before busy_after total_after
    read -r busy_before total_before <<< "$1"
    read -r busy_after total_after <<< "$2"
    LC_ALL=C awk -v busy=$((busy_after - busy_before)) -v total=$((total_after - total_before)) \
        'BEGIN { printf "%.1f\n", 100 * busy / total }'
}

# One run's values from its GoogleTest report $2, which is missing when the
# test crashed.
run_json() {
    local number=$1 report=$2 cpu=$3
    if [[ -f $report ]]; then
        jq --argjson run "$number" --argjson cpu "$cpu" '
            .testsuites[0].testsuite[0]
            | {
                run: $run,
                result: (if (.failures // []) == [] then "passed" else "failed" end),
                seconds: (.time | rtrimstr("s") | tonumber),
                cpu_percent: $cpu
              }
              + (del(.name, .file, .line, .status, .result, .timestamp, .time, .classname, .failures)
                 | map_values(tonumber))' "$report"
    else
        jq -n --argjson run "$number" --argjson cpu "$cpu" '{run: $run, result: "failed", cpu_percent: $cpu}'
    fi
}

run() {
    local committed changed
    committed=$(extract HEAD)
    changed=$(changed_files "$committed")
    rm -rf "$committed"
    if [[ -n $changed ]]; then
        printf 'These covered files differ from the last commit; commit or remove the changes first:\n%s\n' \
            "$changed" >&2
        exit 1
    fi
    local hash commit date
    hash=$(files_hash)
    commit=$(git rev-parse HEAD)
    date=$(date -u +%Y-%m-%dT%H:%M:%SZ)

    cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
    cmake --build "$build_dir" --target audio_render_tests --parallel

    local results="$PWD/$build_dir/audio_evidence"
    rm -rf "$results"
    mkdir -p "$results"
    local result=passed number before
    for ((number = 1; number <= run_count; number++)); do
        echo "Audio evidence: run $number of $run_count"
        before=$(cpu_ticks)
        "$build_dir/audio_render_tests" --gtest_output="json:$results/$number.json" || result=failed
        run_json "$number" "$results/$number.json" "$(cpu_percent "$before" "$(cpu_ticks)")" >> "$results/runs.jsonl"
        if [[ $result == failed ]]; then
            break
        fi
    done

    local compiler
    compiler=$("$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$build_dir/CMakeCache.txt")" --version | head -n 1)
    jq -s \
        --arg result "$result" \
        --arg files_hash "$hash" \
        --arg commit "$commit" \
        --arg date "$date" \
        --argjson runs_planned "$run_count" \
        --arg cpu "$(LC_ALL=C lscpu | sed -n 's/^Model name: *//p' | head -n 1)" \
        --argjson cores "$(nproc)" \
        --arg kernel "$(uname -r)" \
        --arg compiler "$compiler" \
        --argjson tolerance_keys "$tolerance_keys" '
        {
            result: $result,
            files_hash: $files_hash,
            commit: $commit,
            date: $date,
            runs_planned: $runs_planned,
            machine: {cpu: $cpu, cores: $cores, kernel: $kernel},
            compiler: $compiler,
            build_type: "Debug",
            tolerances: (.[0] | with_entries(select(.key | IN($tolerance_keys[])))),
            runs: map(with_entries(select(.key | IN($tolerance_keys[]) | not)))
        }' "$results/runs.jsonl" > "$evidence"

    echo
    if [[ $result == passed ]]; then
        echo "All $run_count runs passed. To commit the evidence:"
        echo "  git add $evidence && git commit -m 'test: record the audio evidence'"
    else
        echo "Run $number failed, and $evidence records the failure. The capture and the" \
            "per-window values of a failed comparison are in $build_dir/audio_render/." >&2
        exit 1
    fi
}

check() {
    if [[ ! -f $evidence ]]; then
        echo "$evidence is missing. Run tests/audio_evidence.sh run on a quiet machine and commit the file." >&2
        exit 1
    fi
    local result recorded commit current failed=false
    result=$(jq -r .result "$evidence")
    recorded=$(jq -r .files_hash "$evidence")
    commit=$(jq -r .commit "$evidence")
    current=$(files_hash)
    if [[ $result != passed ]]; then
        echo "$evidence records the result \"$result\", not \"passed\"." >&2
        failed=true
    fi
    if [[ $recorded != "$current" ]]; then
        echo "The covered files differ from those the evidence was recorded on." >&2
        if git cat-file -e "$commit^{commit}" 2> /dev/null; then
            local old
            old=$(extract "$commit")
            printf 'Covered files changed since %s:\n%s\n' "$commit" "$(changed_files "$old")" >&2
            rm -rf "$old"
        fi
        failed=true
    fi
    if [[ $failed == true ]]; then
        echo "Run tests/audio_evidence.sh run on a quiet machine and commit $evidence." >&2
        exit 1
    fi
    echo "$evidence records a pass on these covered files (files_hash $current)."
}

case ${1:-} in
run) run ;;
check) check ;;
*)
    echo "usage: $0 run|check" >&2
    exit 2
    ;;
esac
