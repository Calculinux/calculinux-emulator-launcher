#!/bin/bash
# Build a small fake feed (images, .sha256 files and index.json) to try the
# launcher against:
#
#   tools/make-test-repo.sh /tmp/repo http://127.0.0.1:8000
#   tools/range_server.py --dir /tmp/repo --port 8000 --rate 2000000 &
#   build/calculinux-emulator-launcher --state /tmp/state \
#       --index-url http://127.0.0.1:8000/index.json gui
#
# Sizes are in MiB; the images are not bootable.
set -euo pipefail

REPO="${1:?Usage: $0 <dir> <base-url>}"
BASE="${2:?Usage: $0 <dir> <base-url>}"
mkdir -p "$REPO"

image() { # file size-in-MiB fill-character
    head -c "$(($2 * 1024 * 1024))" /dev/zero | tr '\0' "$3" > "$REPO/$1"
    (cd "$REPO" && sha256sum "$1" > "$1.sha256")
}
image main.qcow2 40 m
image develop.qcow2 42 d
image v1.0.0.qcow2 38 r
image v1.0.0-alpha8.qcow2 36 a

python3 - "$REPO" "${BASE%/}" <<'EOF'
import json, os, sys
repo, base = sys.argv[1:3]
images = []
for name, image_id, channel, label, version in [
        ("main.qcow2", "continuous-main", "continuous-main", "Latest (main)", "1.0.0-continuous+4e76999"),
        ("develop.qcow2", "continuous-develop", "continuous-develop", "Latest (develop)", "1.0.0-develop+ab12cd3"),
        ("v1.0.0.qcow2", "v1.0.0", "release", "v1.0.0", "v1.0.0"),
        ("v1.0.0-alpha8.qcow2", "v1.0.0-alpha8", "prerelease", "v1.0.0-alpha8", "v1.0.0-alpha8")]:
    path = os.path.join(repo, name)
    images.append({
        "id": image_id, "channel": channel, "label": label, "version": version,
        "size": os.path.getsize(path),
        "sha256": open(path + ".sha256").read().split()[0],
        "url": f"{base}/{name}",
    })
with open(os.path.join(repo, "index.json"), "w") as out:
    json.dump({"schema": 1, "machine": "calculinux-qemuarm", "latest_release": "v1.0.0", "images": images}, out, indent=2)
EOF
echo "index: ${BASE%/}/index.json"
