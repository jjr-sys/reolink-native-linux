#!/usr/bin/env bash
# Decode/render benchmark: run an N-pane grid of looping test streams under hardware and
# software decode, and report the app's CPU time for each once every pane is playing.
#
# Usage: tools/bench.sh [panes] [seconds]
#   panes:   simultaneous streams, one of the grid presets 1/4/6/9/16/20 (default 9)
#   seconds: measuring window after all panes are up (default 20)
# Env:
#   BENCH_APP   binary to run (default ./build/reolink-client)
#   BENCH_CLIP  H.264 file to loop; required if the ffmpeg CLI isn't installed
#               (otherwise a 720p25 test clip is generated)
#
# Runs as a separate instance (RL_ALLOW_MULTI) with throwaway data/config/cache dirs, so it
# never touches the real device list or layout and can run alongside the real app.
set -euo pipefail

PANES="${1:-9}"
SECS="${2:-20}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${BENCH_APP:-$ROOT/build/reolink-client}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

[ -x "$APP" ] || { echo "no app at $APP (build first, or set BENCH_APP)" >&2; exit 1; }
case "$PANES" in 1|4|6|9|16|20) ;; *) echo "panes must be a grid preset: 1 4 6 9 16 20" >&2; exit 1;; esac

if [ -n "${BENCH_CLIP:-}" ]; then
    cp "$BENCH_CLIP" "$WORK/clip.mp4"
elif command -v ffmpeg >/dev/null; then
    echo "Generating test clip…"
    ffmpeg -v error -y -f lavfi -i "testsrc2=size=1280x720:rate=25" -t 10 \
        -c:v libx264 -pix_fmt yuv420p "$WORK/clip.mp4"
else
    echo "no ffmpeg CLI: set BENCH_CLIP to an H.264 .mp4" >&2; exit 1
fi

# Throwaway app dirs: device DB with N stream devices, and a layout showing N panes.
export XDG_DATA_HOME="$WORK/data" XDG_CONFIG_HOME="$WORK/config" XDG_CACHE_HOME="$WORK/cache"
mkdir -p "$XDG_DATA_HOME/reolink-linux/reolink-client" "$XDG_CONFIG_HOME/reolink-linux" "$XDG_CACHE_HOME"
python3 -I - "$XDG_DATA_HOME/reolink-linux/reolink-client/reolink.db" "$PANES" "$WORK/clip.mp4" <<'PY'
import sqlite3, sys
db, n, clip = sys.argv[1], int(sys.argv[2]), sys.argv[3]
c = sqlite3.connect(db)
c.executescript("""
CREATE TABLE schema_version(version INTEGER NOT NULL);
INSERT INTO schema_version VALUES(1);
CREATE TABLE hosts(id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL,
  name TEXT NOT NULL, addr TEXT NOT NULL, port INTEGER NOT NULL DEFAULT 443,
  https INTEGER NOT NULL DEFAULT 1, username TEXT NOT NULL DEFAULT '',
  model TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')));""")
for i in range(1, n + 1):
    c.execute("INSERT INTO hosts(kind,name,addr,port,https) VALUES('stream',?,?,0,0)",
              (f"cam{i}", clip))
c.commit()
PY
printf '[liveGrid]\npreset=%s\nmosaic=false\n' "$PANES" > "$XDG_CONFIG_HOME/reolink-linux/reolink-client.conf"
export RL_ALLOW_MULTI=1

run() {
    local label="$1" hw="$2" log="$WORK/log_$1.txt"
    echo "=== $label ($PANES panes, ${SECS}s) ==="
    RL_HWDECODE="$hw" QT_FORCE_STDERR_LOGGING=1 "$APP" >/dev/null 2>"$log" &
    local pid=$! up=0 t
    # Measure only once every pane is playing; startup is not what we're timing.
    for t in $(seq 1 30); do
        sleep 1
        up=$(grep -c 'first frame delivered' "$log" || true)
        [ "$up" -ge "$PANES" ] && break
    done
    if [ "$up" -lt "$PANES" ]; then
        echo "  only $up/$PANES panes started within 30s; not measuring"
        kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true
        return 1
    fi
    sleep 2   # let the decoders settle past the opening keyframes
    local start end
    start=$(awk '{print $14+$15}' "/proc/$pid/stat")
    sleep "$SECS"
    end=$(awk '{print $14+$15}' "/proc/$pid/stat" 2>/dev/null || echo "$start")
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    awk -v t="$((end - start))" -v hz="$(getconf CLK_TCK)" -v w="$SECS" -v n="$PANES" 'BEGIN{
        cpu=t/hz; printf "  CPU: %.1f cpu-seconds over %ds (%.0f%% of one core, %.1f%% per pane)\n",
        cpu, w, 100*cpu/w, 100*cpu/w/n}'
    echo "  panes up after ${t}s; decode path: $(grep -o '\[hw\]\|\[sw\]' "$log" | sort | uniq -c | tr -s ' ' | tr '\n' ' ')"
    local off; off=$(grep -c 'zero-copy off' "$log" || true)
    [ "$hw" = 1 ] && echo "  zero-copy fallbacks: $off$( [ "$off" -gt 0 ] && grep -o 'zero-copy off[^)]*)' "$log" | sort | uniq -c | sed 's/^ */; /' | tr '\n' ' ')"
    return 0
}

run hardware 1 || true
run software 0 || true
echo "(Hardware decode offloads to the GPU; expect materially lower CPU for H.264/H.265.)"
