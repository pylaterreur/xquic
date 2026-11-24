#!/usr/bin/env bash
# Fork-only probe helper (pylaterreur/xquic, branch try/macos-unittests),
# not meant for upstream: build the XQUIC unit tests on macOS and run them.
#
#   probe.sh env | deps | boringssl | summary
#   probe.sh variant <name> [cmake args...]
#       configure, build with the -Werror of CMakeLists.txt, and when that
#       fails configure and build again with -Wno-error (DISABLE_WARNINGS=1)
#       to list every diagnostic; then run tests/run_tests
#   probe.sh buildonly <name> [cmake args...]
#       configure and build (make -k), report what does not build

set -o pipefail

SRC="${GITHUB_WORKSPACE:-$(pwd)}"
LOGS="$SRC/logs"
SSL="$SRC/third_party/boringssl"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NCPU="$(sysctl -n hw.ncpu)"
RESULTS="$LOGS/results.txt"
mkdir -p "$LOGS"

# The flags of the "Build on Ubuntu" job of build.yml that make sense on
# macOS: no GCOV=on, whose coverage only feeds the Codacy upload, and no
# XQC_SUPPORT_SENDMMSG_BUILD, as macOS has no sendmmsg(2) ("buildonly
# sendmmsg" tries it anyway).
UBUNTU_FLAGS=(
    -DCMAKE_BUILD_TYPE=Debug
    -DXQC_ENABLE_TESTING=1
    -DXQC_PRINT_SECRET=1
    -DXQC_ENABLE_EVENT_LOG=1
    -DXQC_ENABLE_BBR2=1
    -DXQC_ENABLE_RENO=1
    -DXQC_ENABLE_UNLIMITED=1
    -DXQC_ENABLE_COPA=1
    -DXQC_COMPAT_DUPLICATE=1
    -DXQC_ENABLE_FEC=1
    -DXQC_ENABLE_XOR=1
    -DXQC_ENABLE_RSC=1
    -DXQC_ENABLE_PKM=1
)
SSL_FLAGS=(
    -DSSL_TYPE=boringssl
    "-DSSL_PATH=$SSL"
    "-DSSL_INC_PATH=$SSL/include"
    "-DSSL_LIB_PATH=$SSL/build/libssl.a;$SSL/build/libcrypto.a"
)
TARGETS="libxquic.dylib libxquic-static.a tests/run_tests tests/test_server
tests/test_client tests/test_h3_qpack demo/demo_server demo/demo_client
mini/mini_server mini/mini_client"


result() {
    echo "$*" | tee -a "$RESULTS"
}


cmd_env() {
    {
        echo "ImageOS=${ImageOS:-} ImageVersion=${ImageVersion:-}" \
            "RUNNER_ARCH=${RUNNER_ARCH:-}"
        sw_vers
        uname -a
        sysctl -n machdep.cpu.brand_string hw.ncpu hw.memsize
        xcode-select -p
        xcodebuild -version
        cc --version
        cmake --version | head -n 1
        echo "brew prefix: $(brew --prefix)"
        echo "openssl: $(command -v openssl): $(openssl version)"
        python3 --version
        echo "PATH=$PATH"
    } 2>&1 | tee "$LOGS/env.txt"
}


cmd_deps() {
    brew install cunit libevent > "$LOGS/brew.log" 2>&1
    local st=$?
    tail -n 30 "$LOGS/brew.log"
    brew list --versions cunit libevent 2>&1 | tee -a "$LOGS/env.txt"
    return "$st"
}


# as the build-macos job of build.yml, with parallel make
cmd_boringssl() {
    local st

    git clone https://github.com/google/boringssl.git "$SSL" || return 1
    git -C "$SSL" log -1 --format='boringssl %H %cI %s' \
        | tee -a "$LOGS/env.txt"
    mkdir -p "$SSL/build"
    (
        cd "$SSL/build" \
        && cmake -DBUILD_SHARED_LIBS=0 -DCMAKE_C_FLAGS="-fPIC" \
            -DCMAKE_CXX_FLAGS="-fPIC" .. \
        && make -j"$NCPU" ssl crypto
    ) > "$LOGS/boringssl.log" 2>&1
    st=$?
    tail -n 30 "$LOGS/boringssl.log"
    ls -la "$SSL/build/libssl.a" "$SSL/build/libcrypto.a"
    return "$st"
}


configure() {
    local name="$1"
    shift
    local dir="$SRC/build-$name"
    local log="$LOGS/configure-$name.log"
    local st

    rm -rf "$dir"
    mkdir -p "$dir"
    (
        cd "$dir" \
        && cmake -DPLATFORM=mac "${UBUNTU_FLAGS[@]}" "${SSL_FLAGS[@]}" \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@" ..
    ) > "$log" 2>&1
    st=$?
    if [ "$st" -ne 0 ] \
        || ! grep -q '^CUNIT_LIBRARY:FILEPATH=/' "$dir/CMakeCache.txt"
    then
        # what a plain configure misses; then point CMake at Homebrew
        result "configure-$name: status $st with the default search paths;" \
            "$(grep '^CUNIT_LIBRARY:' "$dir/CMakeCache.txt" 2>&1)"
        mv "$log" "$LOGS/configure-$name.default-paths.log"
        rm -rf "$dir"
        mkdir -p "$dir"
        (
            cd "$dir" \
            && cmake -DPLATFORM=mac "${UBUNTU_FLAGS[@]}" "${SSL_FLAGS[@]}" \
                -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
                -DCMAKE_PREFIX_PATH="$(brew --prefix)" "$@" ..
        ) > "$log" 2>&1
        st=$?
    fi
    cat "$log"
    grep -E '^(CUNIT_|LIBEVENT_|CMAKE_C_COMPILER:|CMAKE_BUILD_TYPE:)' \
        "$dir/CMakeCache.txt" > "$LOGS/cache-$name.txt" 2>&1
    cat "$LOGS/cache-$name.txt"
    result "configure-$name: status $st"
    return "$st"
}


diagnostics() {
    local name="$1"
    shift
    local out="$LOGS/diagnostics-$name.txt"
    local link="$LOGS/link-$name.txt"

    # each compiler diagnostic once, relative to the source tree
    cat "$@" 2>/dev/null \
        | grep -E '^[^ ]+:[0-9]+:[0-9]+: (warning|error|fatal error): ' \
        | sed "s|^$SRC/||" | sort -u > "$out"
    # linker, driver and make errors
    cat "$@" 2>/dev/null \
        | grep -E -A 12 '^(ld: |Undefined symbols|clang: error|make.*\*\*\*)' \
        | sed "s|$SRC/||g" > "$link"
    echo "== build-$name: $(wc -l < "$out" | tr -d ' ') compiler diagnostics =="
    cat "$out"
    echo "== by file =="
    cut -d: -f1 "$out" | sort | uniq -c
    echo "== by flag =="
    grep -oE '\[-W[^]]+\]' "$out" | sort | uniq -c
    echo "== linker / make errors =="
    cat "$link"
}


build() {
    local name="$1"
    local dir="$SRC/build-$name"
    local log="$LOGS/build-$name.log"
    local st t built="" missing=""

    (cd "$dir" && make -k -j"$NCPU") > "$log" 2>&1
    st=$?
    if [ "$st" -ne 0 ]; then
        # again one job at a time, which keeps the diagnostics of each file
        # together; only what failed is compiled again
        (cd "$dir" && make -k) > "$LOGS/build-$name.serial.log" 2>&1
        cat "$LOGS/build-$name.serial.log"
    else
        tail -n 5 "$log"
    fi
    diagnostics "$name" "$log" "$LOGS/build-$name.serial.log"
    for t in $TARGETS; do
        if [ -e "$dir/$t" ]; then
            built="$built $t"
        else
            missing="$missing $t"
        fi
    done
    result "build-$name: status $st;" \
        "$(wc -l < "$LOGS/diagnostics-$name.txt" | tr -d ' ') diagnostics;" \
        "built:$built; missing:${missing:- none}"
    return "$st"
}


lldb_bt() {
    local name="$1"
    local t="$2"

    python3 "$HERE/run_pty.py" --quiet --timeout 300 \
        --log "$LOGS/lldb-$name-$t.txt" -- \
        sudo -n lldb --batch -o run -k 'thread backtrace all' -k 'quit 1' \
        -- ./tests/run_tests "$t"
}


# each test on its own, after a crash or a hang of the whole suite
single() {
    local name="$1"
    local out="$LOGS/single-$name"
    local t st timeouts=0 crashes=0

    mkdir -p "$out"
    perl -0777 -ne \
        'while (/CU_add_test\(\s*pSuite\s*,\s*"([^"]+)"/g) { print "$1\n" }' \
        "$SRC/tests/unittest/main.c" > "$out/names.txt"
    echo "== running the $(wc -l < "$out/names.txt" | tr -d ' ') tests" \
        "one at a time =="
    while read -r t; do
        python3 "$HERE/run_pty.py" --quiet --timeout 120 \
            --log "$out/$t.log" -- ./tests/run_tests "$t" < /dev/null
        st=$?
        if grep -q '^unknown test' "$out/$t.log"; then
            echo "$t: not registered" >> "$out/status.txt"
            continue
        fi
        echo "$t: exit $st" >> "$out/status.txt"
        if [ "$st" -eq 124 ]; then
            timeouts=$((timeouts + 1))
        elif [ "$st" -gt 128 ]; then
            crashes=$((crashes + 1))
            if [ "$crashes" -le 3 ]; then
                lldb_bt "$name" "$t" < /dev/null
            fi
        fi
        if [ "$timeouts" -ge 3 ]; then
            echo "three tests timed out: stopping"
            break
        fi
    done < "$out/names.txt"
    grep -v -E ': (exit 0|not registered)$' "$out/status.txt" \
        | tee "$LOGS/single-$name.bad.txt"
    result "single-$name:" \
        "$(grep -c ': exit 0$' "$out/status.txt") passed," \
        "$(grep -c -v -E ': (exit 0|not registered)$' "$out/status.txt")" \
        "did not pass," \
        "$(grep -c ': not registered$' "$out/status.txt") not registered"
}


unit() {
    local name="$1"
    local dir="$SRC/build-$name"
    local log="$LOGS/unit-$name.log"
    local st start summary total ran passed failed

    if [ ! -x "$dir/tests/run_tests" ]; then
        result "unit-$name: run_tests was not built"
        return 1
    fi
    otool -L "$dir/tests/run_tests" 2>&1 | tee "$LOGS/otool-$name.txt"
    cd "$dir" || return 1
    if ! openssl req -newkey rsa:2048 -x509 -nodes -keyout server.key \
        -new -out server.crt -subj /CN=test.xquic.com \
        > "$LOGS/openssl-$name.log" 2>&1
    then
        cat "$LOGS/openssl-$name.log"
        result "unit-$name: openssl req failed"
        return 1
    fi

    start=$SECONDS
    python3 "$HERE/run_pty.py" --timeout 900 --log "$log" \
        --sample "$LOGS/sample-$name.txt" -- ./tests/run_tests
    st=$?
    summary="$(awk '
        $1 == "tests" && NF == 6 && $2 ~ /^[0-9]+$/ \
            && $3 ~ /^[0-9]+$/ && $4 ~ /^[0-9]+$/ && $5 ~ /^[0-9]+$/ {
            s = $2 " " $3 " " $4 " " $5
        }
        END { print s }
    ' "$log")"
    read -r total ran passed failed <<< "${summary:-0 0 0 0}"
    grep -E -A 8 '\.\.\.FAILED' "$log" > "$LOGS/unit-$name.failures.txt"
    echo "== failed tests of unit-$name =="
    cat "$LOGS/unit-$name.failures.txt"
    result "unit-$name: exit $st after $((SECONDS - start)) s;" \
        "tests total $total ran $ran passed $passed failed $failed"

    if [ "$st" -ge 124 ] || [ -z "$summary" ]; then
        single "$name"
    fi
    if [ "$st" -eq 0 ] && [ "$total" -gt 0 ] && [ "$ran" -eq "$total" ] \
        && [ "$passed" -eq "$total" ] && [ "$failed" -eq 0 ]
    then
        result "unit-$name: PASSED"
        return 0
    fi
    result "unit-$name: FAILED"
    return 1
}


cmd_variant() {
    local name="$1"
    shift
    local st=0

    configure "$name" "$@" || return 1
    if build "$name"; then
        unit "$name" || st=1
    else
        st=1
        configure "$name-nowerror" "$@" -DDISABLE_WARNINGS=1 || return 1
        build "$name-nowerror"
        unit "$name-nowerror"
    fi
    return "$st"
}


cmd_buildonly() {
    local name="$1"
    shift

    configure "$name" "$@" || return 1
    build "$name"
}


cmd_summary() {
    cp "$HOME"/Library/Logs/DiagnosticReports/run_tests* "$LOGS/" 2>/dev/null
    {
        echo "### $(sw_vers -productName) $(sw_vers -productVersion)" \
            "$(uname -m), image ${ImageOS:-} ${ImageVersion:-}"
        echo '```'
        grep -E '^(Xcode|Build version|Apple clang|cmake version|boringssl|cunit|libevent)' \
            "$LOGS/env.txt"
        cat "$RESULTS"
        echo '```'
    } >> "${GITHUB_STEP_SUMMARY:-/dev/stdout}"
    cat "$RESULTS"
}


sub="$1"
shift
case "$sub" in
    env) cmd_env ;;
    deps) cmd_deps ;;
    boringssl) cmd_boringssl ;;
    variant) cmd_variant "$@" ;;
    buildonly) cmd_buildonly "$@" ;;
    summary) cmd_summary ;;
    *) echo "unknown subcommand: $sub" >&2; exit 2 ;;
esac
