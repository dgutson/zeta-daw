#!/bin/bash
# Evidence that the audio test passed on the files it depends on.
#
#   tests/audio_evidence.sh run    builds audio_render_tests in build-audio/,
#                                  plays it 20 times one after another while
#                                  holding the desktop's screen-saver
#                                  inhibit, stops at the first failure and
#                                  writes tests/audio_evidence.json. It
#                                  prints a line per run, also appended to
#                                  build-audio/audio_evidence/progress.log,
#                                  and keeps the build's and each run's
#                                  output beside that file
#   tests/audio_evidence.sh check  fails unless that file records a pass on
#                                  covered files identical to these
#
# The covered files are the repository files that the compiler read to build
# audio_render_tests, as its dependency files list them, plus the build
# files, the audio test's files and the fake MIDI input. run stores each
# one's path and SHA-256, and check hashes exactly those files. The build can
# start reading another file only through a change to a covered file, so the
# stored list stays complete for as long as the evidence is valid, and a
# rebase or a change to any other file leaves the evidence valid. files_hash
# is a SHA-256 over the "SHA-256  path" line of each covered file, sorted by
# path.
set -euo pipefail

cd "$(dirname "$0")/.."

readonly evidence=tests/audio_evidence.json
readonly build_dir=build-audio
readonly results="$PWD/$build_dir/audio_evidence"
readonly run_count=20
readonly tolerance_keys='["tau_ms", "level_delta_db", "peak_delta_db", "drift_limit_ms", "take_lateness_limit_ms"]'

# The repository files that the compiler read to build audio_render_tests,
# from the dependency file of each of its objects in compile_commands.json.
audio_build_inputs() {
    local outputs output depfile
    outputs=$(jq -r '.[].output // empty | select(startswith("CMakeFiles/audio_render_tests.dir/"))' \
        "$build_dir/compile_commands.json")
    if [[ -z $outputs ]]; then
        echo "$build_dir/compile_commands.json lists no object of audio_render_tests." >&2
        return 1
    fi
    while IFS= read -r output; do
        depfile="$build_dir/$output.d"
        if [[ ! -f $depfile ]]; then
            echo "$depfile is missing." >&2
            return 1
        fi
        # Make syntax: the object, a colon, then the files read, separated by
        # spaces. A backslash ends each continued line and escapes a space or
        # # in a path; $$ stands for $.
        sed -e '1s/^[^:]*://' -e 's/\\$//' -e 's/\\ /\x1f/g' -e 's/\\#/#/g' -e 's/\$\$/$/g' "$depfile" |
            tr -s ' \t' '\n' | tr '\037' ' '
    done <<< "$outputs" | sed '/^$/d' | xargs -d '\n' realpath -m -s --relative-to=. -- |
        grep -v -e '^\.\./' -e "^$build_dir/" | LC_ALL=C sort -u
}

# Covered files, sorted by path: the audio test's build inputs, the build
# files, the audio test's files and the fake MIDI input.
covered_files() {
    local inputs
    inputs=$(audio_build_inputs) || return 1
    {
        echo "$inputs"
        echo CMakeLists.txt
        find cmake -type f
        find tests -type f \( -path 'tests/audio_*' -o -path 'tests/fake_midi_input.*' \)
    } | grep -vxF "$evidence" | LC_ALL=C sort -u
}

# files_hash of the "SHA-256  path" lines $1.
files_hash() {
    sha256sum <<< "$1" | cut -d ' ' -f 1
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

# Prints a progress line and appends it to progress.log, which tail -f can
# follow when the run was started elsewhere.
progress() {
    echo "Audio evidence: $*" | tee -a "$results/progress.log"
}

# The progress line of run $1, which took $2 seconds, from its values $4;
# the runs so far took $3 seconds, which dates the end of the rest.
report_run() {
    local number=$1 seconds=$2 total=$3 values=$4 result cpu discarded line
    read -r result cpu discarded < <(
        jq -r '[.result, .cpu_percent, (.discarded_attempts // 0)] | @tsv' <<< "$values"
    )
    line="run $number of $run_count $result in $seconds s, CPU $cpu%"
    if ((discarded > 0)); then
        line+=", discarded attempts: $discarded"
    fi
    if [[ $result == passed ]] && ((number < run_count)); then
        line+="; done at about $(date -d "+$(((run_count - number) * total / number)) seconds" +%H:%M)"
    fi
    progress "$line"
}

# The failure messages in run output $1, or its last lines when it has none,
# as after a crash.
print_failure() {
    local first
    first=$(grep -n -m 1 ': Failure$' "$1" | cut -d : -f 1 || true)
    if [[ -n $first ]]; then
        tail -n "+$first" "$1"
    else
        tail -n 40 "$1"
    fi
}

run() {
    rm -rf "$results"
    mkdir -p "$results"
    progress "building audio_render_tests in $build_dir/, output in $build_dir/audio_evidence/build.log"
    # The Makefile generator keeps each object's dependency file, which
    # audio_build_inputs reads; Ninja deletes them.
    if ! {
        cmake -S . -B "$build_dir" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON &&
            cmake --build "$build_dir" --target audio_render_tests hold_screen_unlocked --parallel
    } > "$results/build.log" 2>&1; then
        tail -n 40 "$results/build.log" >&2
        progress "the build failed; the end of its output is above" >&2
        exit 1
    fi

    local list changed
    local -a files
    list=$(covered_files)
    mapfile -t files <<< "$list"
    changed=$(git --literal-pathspecs status --porcelain --untracked-files=all -- "${files[@]}")
    if [[ -n $changed ]]; then
        printf 'These covered files differ from the last commit; commit or remove the changes first:\n%s\n' \
            "$changed" >&2
        exit 1
    fi
    local hashes hash commit date
    hashes=$(xargs -d '\n' sha256sum -- <<< "$list")
    hash=$(files_hash "$hashes")
    commit=$(git rev-parse HEAD)
    date=$(date -u +%Y-%m-%dT%H:%M:%SZ)

    # The helper holds the screen-saver inhibit, as zd does, until its
    # standard input closes when this script ends, and prints one line.
    local screen_lock_line
    coproc screen_lock { exec "$build_dir/hold_screen_unlocked" 2>&1; }
    if ! read -r screen_lock_line <&"${screen_lock[0]}"; then
        screen_lock_line="hold_screen_unlocked printed nothing; the screen may lock"
    fi
    progress "$screen_lock_line"

    progress "playing it $run_count times, about a minute each, from $(date +%H:%M);" \
        "each run's output goes to $build_dir/audio_evidence/"
    local result=passed number before started seconds total=0 values
    for ((number = 1; number <= run_count; number++)); do
        before=$(cpu_ticks)
        started=$SECONDS
        "$build_dir/audio_render_tests" --gtest_output="json:$results/$number.json" \
            > "$results/$number.log" 2>&1 || result=failed
        seconds=$((SECONDS - started))
        total=$((total + seconds))
        values=$(run_json "$number" "$results/$number.json" "$(cpu_percent "$before" "$(cpu_ticks)")")
        echo "$values" >> "$results/runs.jsonl"
        report_run "$number" "$seconds" "$total" "$values"
        if [[ $result == failed ]]; then
            print_failure "$results/$number.log" >&2
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
        --argjson files "$(jq -Rn '[inputs | {key: .[66:], value: .[:64]}] | from_entries' <<< "$hashes")" \
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
            runs: map(with_entries(select(.key | IN($tolerance_keys[]) | not))),
            files: $files
        }' "$results/runs.jsonl" > "$evidence"

    if [[ $result == passed ]]; then
        progress "all $run_count runs passed. To commit the evidence:"
        echo "  git add $evidence && git commit -m 'test: record the audio evidence'"
    else
        progress "run $number failed, and $evidence records the failure. Its output is in" \
            "$build_dir/audio_evidence/$number.log; the capture and the per-window values" \
            "of a failed comparison are in $build_dir/audio_render/." >&2
        exit 1
    fi
}

check() {
    if [[ ! -f $evidence ]]; then
        echo "$evidence is missing. Run tests/audio_evidence.sh run on a quiet machine and commit the file." >&2
        exit 1
    fi
    if [[ $(jq -r '.files | type' "$evidence") != object ]]; then
        echo "$evidence lists no covered files. Run tests/audio_evidence.sh run on a quiet machine and commit the file." >&2
        exit 1
    fi
    local result recorded hashes current changed failed=false
    result=$(jq -r .result "$evidence")
    recorded=$(jq -r .files_hash "$evidence")
    hashes=$(jq -r '.files | to_entries | sort_by(.key)[] | "\(.value)  \(.key)"' "$evidence")
    current=$(files_hash "$hashes")
    if [[ $result != passed ]]; then
        echo "$evidence records the result \"$result\", not \"passed\"." >&2
        failed=true
    fi
    if [[ $current != "$recorded" ]]; then
        echo "The file list in $evidence does not match its files_hash." >&2
        failed=true
    fi
    if ! changed=$(sha256sum --check --quiet <<< "$hashes" 2>&1); then
        printf 'These covered files differ from those the evidence was recorded on:\n%s\n' "$changed" >&2
        failed=true
    fi
    if [[ $failed == true ]]; then
        echo "Run tests/audio_evidence.sh run on a quiet machine and commit $evidence." >&2
        exit 1
    fi
    echo "$evidence records a pass on its $(jq '.files | length' "$evidence") covered files, all unchanged (files_hash $current)."
}

case ${1:-} in
run) run ;;
check) check ;;
*)
    echo "usage: $0 run|check" >&2
    exit 2
    ;;
esac
