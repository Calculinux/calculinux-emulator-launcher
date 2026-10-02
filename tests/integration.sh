#!/bin/bash
# End-to-end test of the headless commands against a local HTTP server.
# Usage: tests/integration.sh <path to calculinux-emulator-launcher>
set -euo pipefail

LAUNCHER="$(readlink -f "${1:?Usage: $0 <launcher binary>}")"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"
SERVER_PID=""
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    rm -rf "$T"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

start_server() { # extra range_server.py arguments
    [ -n "$SERVER_PID" ] && { kill "$SERVER_PID"; wait "$SERVER_PID" 2>/dev/null || true; }
    rm -f "$T/port"
    python3 "$ROOT/tools/range_server.py" --dir "$T/repo" --port-file "$T/port" --log "$T/requests.log" "$@" 2>/dev/null &
    SERVER_PID=$!
    for _ in $(seq 50); do
        [ -f "$T/port" ] && break
        sleep 0.1
    done
    [ -f "$T/port" ] || fail "server did not start"
    BASE="http://127.0.0.1:$(cat "$T/port")"
}

publish() { # name id channel version size-in-KiB seed
    head -c "$(($5 * 1024))" /dev/zero | tr '\0' "$6" > "$T/repo/$1"
    (cd "$T/repo" && sha256sum "$1" > "$1.sha256")
}

write_index() {
    python3 - "$T/repo" "$BASE" <<'EOF'
import json, os, sys
repo, base = sys.argv[1:3]
images = []
for name, image_id, channel, version in [
        ("main.qcow2", "continuous-main", "continuous-main", "1.0.0-continuous+abc"),
        ("alpha8.qcow2", "v1.0.0-alpha8", "prerelease", "v1.0.0-alpha8")]:
    path = os.path.join(repo, name)
    images.append({
        "id": image_id, "channel": channel, "label": image_id, "version": version,
        "size": os.path.getsize(path),
        "sha256": open(path + ".sha256").read().split()[0],
        "url": f"{base}/{name}",
    })
json.dump({"schema": 1, "latest_release": None, "images": images}, open(os.path.join(repo, "index.json"), "w"))
EOF
}

L() { "$LAUNCHER" --state "$T/state" --index-url "$BASE/index.json" --default-id continuous-main "$@"; }

mkdir -p "$T/repo"
start_server
publish main.qcow2 continuous-main continuous-main 1.0 600 m
publish alpha8.qcow2 v1.0.0-alpha8 prerelease 1.0 300 a
write_index

echo "== list"
L list > "$T/out"
grep -q '^continuous-main  *Not downloaded' "$T/out" || fail "list: $(cat "$T/out")"
grep -q '^v1.0.0-alpha8  *Not downloaded' "$T/out" || fail "list: $(cat "$T/out")"

echo "== fetch --auto downloads the default image"
out=$(L fetch --auto 2>"$T/err")
[ "$out" = "id=continuous-main" ] || fail "fetch --auto printed '$out': $(cat "$T/err")"
cmp -s "$T/repo/main.qcow2" "$T/state/images/continuous-main/base.qcow2" || fail "image differs"
[ "$(cut -d' ' -f1 "$T/state/images/continuous-main/base.qcow2.sha256")" = "$(cut -d' ' -f1 "$T/repo/main.qcow2.sha256")" ] || fail "sha file"
grep -q '^last_id=continuous-main$' "$T/state/settings" || fail "last_id not saved"

echo "== fetch --auto again needs no network"
: > "$T/requests.log"
out=$(L fetch --auto 2>/dev/null)
[ "$out" = "id=continuous-main" ] || fail "second fetch printed '$out'"
[ ! -s "$T/requests.log" ] || fail "second fetch hit the server: $(cat "$T/requests.log")"

echo "== fetch ID"
out=$(L fetch v1.0.0-alpha8 2>/dev/null)
[ "$out" = "id=v1.0.0-alpha8" ] || fail "fetch ID printed '$out'"
L list | grep -q '^v1.0.0-alpha8  *Downloaded' || fail "alpha8 not listed as downloaded"

echo "== unknown id"
rc=0; L fetch nonsense >/dev/null 2>&1 || rc=$?
[ "$rc" = 3 ] || fail "unknown id exit code $rc"

echo "== update: new image on the server"
echo overlay > "$T/state/images/continuous-main/disk.qcow2"
publish main.qcow2 continuous-main continuous-main 1.1 700 n
write_index
L list | grep -q '^continuous-main  *Update available' || fail "update not detected"
out=$(L fetch continuous-main 2>/dev/null)   # without --update: keep the old one
[ "$out" = "id=continuous-main" ] || fail "fetch printed '$out'"
[ -f "$T/state/images/continuous-main/disk.qcow2" ] || fail "overlay lost without --update"
out=$(L fetch continuous-main --update 2>"$T/err")
[ "$out" = "id=continuous-main" ] || fail "fetch --update printed '$out'"
cmp -s "$T/repo/main.qcow2" "$T/state/images/continuous-main/base.qcow2" || fail "image not updated"
[ ! -e "$T/state/images/continuous-main/disk.qcow2" ] || fail "overlay kept across an update"
grep -q 'saved state was reset' "$T/err" || fail "no reset message"

echo "== interrupted download resumes"
publish alpha8.qcow2 v1.0.0-alpha8 prerelease 1.0 2000 b
start_server --rate 400000
write_index
L fetch v1.0.0-alpha8 --update >/dev/null 2>&1 &
pid=$!
part="$T/state/images/v1.0.0-alpha8/base.qcow2.part"
for _ in $(seq 100); do
    [ -f "$part" ] && [ "$(stat -c %s "$part")" -gt 200000 ] && break
    sleep 0.1
done
kill "$pid" 2>/dev/null || fail "the download finished before it could be interrupted"
wait "$pid" 2>/dev/null || true
have=$(stat -c %s "$part")
[ "$have" -gt 0 ] && [ "$have" -lt 2048000 ] || fail "partial file is $have bytes"
start_server
write_index
: > "$T/requests.log"
out=$(L fetch v1.0.0-alpha8 --update 2>/dev/null)
[ "$out" = "id=v1.0.0-alpha8" ] || fail "resumed fetch printed '$out'"
grep -q "GET /alpha8.qcow2 range=bytes=$have-" "$T/requests.log" || fail "did not resume: $(cat "$T/requests.log")"
cmp -s "$T/repo/alpha8.qcow2" "$T/state/images/v1.0.0-alpha8/base.qcow2" || fail "resumed image differs"

echo "== server without Range support: restart from zero"
publish alpha8.qcow2 v1.0.0-alpha8 prerelease 1.0 500 c
start_server --no-range
write_index
mkdir -p "$T/state/images/v1.0.0-alpha8"
head -c 1000 "$T/repo/alpha8.qcow2" > "$part"
cut -d' ' -f1 "$T/repo/alpha8.qcow2.sha256" > "$part.meta"
out=$(L fetch v1.0.0-alpha8 --update 2>/dev/null)
[ "$out" = "id=v1.0.0-alpha8" ] || fail "no-range fetch printed '$out'"
cmp -s "$T/repo/alpha8.qcow2" "$T/state/images/v1.0.0-alpha8/base.qcow2" || fail "no-range image differs"

echo "== corrupt image on the server"
start_server
write_index
echo "0000000000000000000000000000000000000000000000000000000000000000  main.qcow2" > "$T/repo/main.qcow2.sha256"
rm -rf "$T/state/images/continuous-main"
rc=0; L fetch continuous-main >/dev/null 2>&1 || rc=$?
[ "$rc" = 5 ] || fail "checksum mismatch exit code $rc"
[ ! -e "$T/state/images/continuous-main/base.qcow2" ] || fail "corrupt image installed"
(cd "$T/repo" && sha256sum main.qcow2 > main.qcow2.sha256)

echo "== offline: downloaded images still work"
kill "$SERVER_PID"; wait "$SERVER_PID" 2>/dev/null || true; SERVER_PID=""
L list 2>/dev/null > "$T/out"
grep -q '^v1.0.0-alpha8  *Downloaded' "$T/out" || fail "offline list: $(cat "$T/out")"
grep -q 'continuous-main' "$T/out" && fail "offline list shows an image that is not downloaded"
out=$(L fetch --auto 2>/dev/null)
[ "$out" = "id=v1.0.0-alpha8" ] || fail "offline fetch --auto printed '$out'"
rc=0; L fetch continuous-main >/dev/null 2>&1 || rc=$?
[ "$rc" = 3 ] || fail "offline fetch of a missing image: exit code $rc"

echo "== reset and remove"
echo overlay > "$T/state/images/v1.0.0-alpha8/disk.qcow2"
L reset v1.0.0-alpha8 2>/dev/null
[ ! -e "$T/state/images/v1.0.0-alpha8/disk.qcow2" ] || fail "reset kept the overlay"
[ -f "$T/state/images/v1.0.0-alpha8/base.qcow2" ] || fail "reset removed the image"
L remove v1.0.0-alpha8 2>/dev/null
[ ! -e "$T/state/images/v1.0.0-alpha8" ] || fail "remove left the image"
grep -q '^last_id=' "$T/state/settings" && fail "last_id still set after remove"
rc=0; L remove v1.0.0-alpha8 >/dev/null 2>&1 || rc=$?
[ "$rc" = 3 ] || fail "remove of a missing image: exit code $rc"

echo "== explicit URL and migration of an old state directory"
start_server
old="$T/old"
mkdir -p "$old/images"
cp "$T/repo/main.qcow2" "$old/images/calculinux.qcow2"
sed 's/main.qcow2/calculinux.qcow2/' "$T/repo/main.qcow2.sha256" > "$old/images/calculinux.qcow2.sha256"
echo overlay > "$old/disk.qcow2"
cut -d' ' -f1 "$T/repo/main.qcow2.sha256" > "$old/image-id"
write_index
"$LAUNCHER" --state "$old" --index-url "$BASE/index.json" migrate
[ -f "$old/images/legacy/base.qcow2" ] || fail "migrate did not move the image"
out=$("$LAUNCHER" --state "$old" --index-url "$BASE/index.json" fetch --auto 2>/dev/null)
[ "$out" = "id=legacy" ] || fail "migrated fetch --auto printed '$out'"
"$LAUNCHER" --state "$old" --index-url "$BASE/index.json" list > "$T/out"
grep -q '^continuous-main  *Downloaded' "$T/out" || fail "legacy image not adopted: $(cat "$T/out")"
[ "$(cat "$old/images/continuous-main/disk.qcow2")" = overlay ] || fail "overlay lost in migration"
out=$("$LAUNCHER" --state "$old" --index-url "$BASE/index.json" fetch --url "$BASE/alpha8.qcow2" 2>/dev/null)
[ "$out" = "id=custom" ] || fail "fetch --url printed '$out'"
cmp -s "$T/repo/alpha8.qcow2" "$old/images/custom/base.qcow2" || fail "custom image differs"
grep -q '^last_id=continuous-main$' "$old/settings" || fail "fetch --url changed last_id: $(cat "$old/settings")"

echo "integration tests passed"
