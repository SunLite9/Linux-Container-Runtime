#!/usr/bin/env bash
# Measures the metrics that don't require code changes: leaked
# mounts/cgroups/veth interfaces, cached vs. cold-pull latency, p50/p95
# startup time, image compatibility, and repeated-run pass rate.
# Must be run with sudo from the project root, after building.
set -uo pipefail

BIN=./build/container-runtime
RUNS_FOR_STATS=20
IMAGES=("alpine:latest" "python:3.11-slim" "busybox:latest" "debian:bookworm-slim")

pass=0
fail=0
LAST_ELAPSED=0
LAST_RC=0

check_leaks() {
    local leaked=0
    local cg veth ov mnt
    cg=$(ls /sys/fs/cgroup/container-runtime/ 2>/dev/null | grep -vc '\.')
    veth=$(ip -o link show type veth 2>/dev/null | wc -l)
    ov=$(ls overlay-data/ 2>/dev/null | wc -l)
    mnt=$(mount | grep -c 'overlay-data.*merged')
    if [ "$cg" -ne 0 ]; then echo "  LEAK: $cg cgroup dir(s) left"; leaked=1; fi
    if [ "$veth" -ne 0 ]; then echo "  LEAK: $veth veth interface(s) left"; leaked=1; fi
    if [ "$ov" -ne 0 ]; then echo "  LEAK: $ov overlay-data dir(s) left"; leaked=1; fi
    if [ "$mnt" -ne 0 ]; then echo "  LEAK: $mnt overlay mount(s) left"; leaked=1; fi
    return $leaked
}

# Runs one container, timing it. Sets LAST_ELAPSED and LAST_RC, and
# updates pass/fail — run directly (no command substitution around the
# call) so those updates aren't lost in a subshell.
run_timed() {
    local image="$1"
    local start end
    start=$(date +%s.%N)
    echo 'true' | sudo "$BIN" run "$image" /bin/sh > /tmp/harness-run.log 2>&1
    LAST_RC=$?
    end=$(date +%s.%N)
    LAST_ELAPSED=$(echo "$end - $start" | bc)
    if [ "$LAST_RC" -eq 0 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "  FAIL ($image): $(tail -3 /tmp/harness-run.log)"
    fi
}

echo "=== image compatibility ==="
for img in "${IMAGES[@]}"; do
    run_timed "$img"
    echo "$img: ${LAST_ELAPSED}s (rc=$LAST_RC)"
done

echo
echo "=== cold vs. cached pull latency (alpine:latest) ==="
sudo rm -rf image-cache/library_alpine
run_timed "alpine:latest"
echo "cold pull: ${LAST_ELAPSED}s"
run_timed "alpine:latest"
echo "cached:    ${LAST_ELAPSED}s"

echo
echo "=== repeated-run stats (alpine:latest, cached, x$RUNS_FOR_STATS) ==="
times=()
for i in $(seq 1 "$RUNS_FOR_STATS"); do
    run_timed "alpine:latest"
    times+=("$LAST_ELAPSED")
done
sorted=($(printf '%s\n' "${times[@]}" | sort -n))
n=${#sorted[@]}
p50_idx=$((n * 50 / 100))
p95_idx=$((n * 95 / 100))
echo "n=$n  p50=${sorted[$p50_idx]}s  p95=${sorted[$p95_idx]}s  min=${sorted[0]}s  max=${sorted[$((n-1))]}s"

echo
echo "=== leak check after all runs ==="
if check_leaks; then
    echo "  clean: no leaked mounts, cgroups, veth interfaces, or overlay dirs"
fi

echo
echo "=== summary ==="
total=$((pass + fail))
echo "pass=$pass fail=$fail total=$total  pass_rate=$(echo "scale=1; $pass/$total*100" | bc)%"
