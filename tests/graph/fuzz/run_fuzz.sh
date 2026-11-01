#!/usr/bin/env bash
# Bir fuzz adımı: süre sınırlı, derlem kalıcı; tekrar çağrılınca kaldığı yerden sürer. Toplam girdi sayısı STATE
# dosyasında birikir. Crash, 10 sn üstü sorgu ve sızıntı çıkış kodu 1 verir, kanıt ARTIFACTS altına düşer.
#   run_fuzz.sh <query|frame> <derleme dizini> <çalışma dizini> [süre sn] [işçi]
set -euo pipefail

die() { echo "ERROR: $*" >&2; exit 2; }

target="${1:?target}"
build_dir="${2:?build dir}"
work_dir="${3:?work dir}"
seconds="${4:-300}"
workers="${5:-8}"
source_dir="$(cd "$(dirname "$0")" && pwd)"
[[ "$target" == query || "$target" == frame || "$target" == jsonl ]] || die "unknown target: $target"

corpus="$work_dir/$target/corpus"
artifacts="$work_dir/$target/artifacts"
logs="$work_dir/$target/logs"
state="$work_dir/$target/total_inputs"
mkdir -p "$corpus" "$artifacts" "$logs"

seed_corpus() {
    if [[ "$target" == frame ]]; then
        "$build_dir/tests/graph/fuzz/make_frame_seeds" "$corpus"
        return
    fi
    if [[ "$target" == jsonl ]]; then
        printf '{"k":"noteapp-graph","v":1}' > "$corpus/seed-header"
        printf '{"k":"node","id":"00000000000000000000000000000001","labels":["Note"],"props":{"name":{"s":"a\\u00e7"},"body":{"b":"QUJD"},"n":{"i":"3"},"p":{"p":true}}}' > "$corpus/seed-node"
        printf '{"k":"edge","id":"00","src":"01","type":"T","dst":"02","props":{}}' > "$corpus/seed-edge"
        return
    fi
    local index=0 line
    while IFS= read -r line; do
        printf 'R%s' "$line" > "$corpus/seed-r-$index"
        printf 'W%s' "$line" > "$corpus/seed-w-$index"
        index=$((index + 1))
    done < "$source_dir/seed_queries.txt"
}

if [[ -z "$(ls -A "$corpus")" ]]; then
    seed_corpus
fi

dict_args=()
max_len=4096
if [[ "$target" == query ]]; then
    dict_args=("-dict=$source_dir/cypher.dict")
    max_len=8192
fi

rm -f "$logs"/fuzz-*.log
status=0
(
    cd "$logs"
    "$build_dir/tests/graph/fuzz/fuzz_$target" "$corpus" "${dict_args[@]}" \
        "-artifact_prefix=$artifacts/" "-max_len=$max_len" -timeout=10 -rss_limit_mb=4096 -detect_leaks=1 \
        "-max_total_time=$seconds" "-jobs=$workers" "-workers=$workers" -print_final_stats=1 >/dev/null 2>&1
) || status=$?

# Her işçi logunda son "cov:" satırı "#<sayı>" ile biter; sayaçlar toplanır.
step=0
for log in "$logs"/fuzz-*.log; do
    runs="$(grep -aE 'stat::number_of_executed_units' "$log" | tail -1 | awk '{print $2}')"
    if [[ -z "$runs" ]]; then
        runs="$(grep -aoE '^#[0-9]+' "$log" | tail -1 | tr -d '#')"
    fi
    step=$((step + ${runs:-0}))
done
previous=0
if [[ -f "$state" ]]; then
    previous="$(cat "$state")"
fi
total=$((previous + step))
echo "$total" > "$state"

problems="$(find "$artifacts" -type f | wc -l)"
echo "target=$target step_inputs=$step total_inputs=$total artifacts=$problems corpus=$(ls "$corpus" | wc -l) status=$status"
if [[ "$problems" -gt 0 ]]; then
    grep -ahE 'ERROR: |SUMMARY|deadly signal|timeout after|LeakSanitizer' "$logs"/fuzz-*.log | sort | uniq -c | sort -rn | head -20
    exit 1
fi
exit "$status"
