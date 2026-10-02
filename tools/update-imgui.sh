#!/bin/sh
# Refresh the vendored copies in third_party/ from upstream release tags.
# Usage: tools/update-imgui.sh [imgui-tag] [nlohmann-json-tag]
set -eu

IMGUI_TAG="${1:-v1.92.9b}"
JSON_TAG="${2:-v3.12.0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

imgui="$ROOT/third_party/imgui"
rm -rf "$imgui"
mkdir -p "$imgui/backends"
base="https://raw.githubusercontent.com/ocornut/imgui/$IMGUI_TAG"
for f in imgui.cpp imgui.h imgui_draw.cpp imgui_internal.h imgui_tables.cpp \
         imgui_widgets.cpp imconfig.h imstb_rectpack.h imstb_textedit.h \
         imstb_truetype.h LICENSE.txt \
         backends/imgui_impl_sdl2.cpp backends/imgui_impl_sdl2.h \
         backends/imgui_impl_sdlrenderer2.cpp backends/imgui_impl_sdlrenderer2.h; do
    curl -fsSL -o "$imgui/$f" "$base/$f"
done
echo "$IMGUI_TAG" > "$imgui/VERSION"

json="$ROOT/third_party/nlohmann"
rm -rf "$json"
mkdir -p "$json"
base="https://raw.githubusercontent.com/nlohmann/json/$JSON_TAG"
curl -fsSL -o "$json/json.hpp" "$base/single_include/nlohmann/json.hpp"
curl -fsSL -o "$json/LICENSE.MIT" "$base/LICENSE.MIT"
echo "$JSON_TAG" > "$json/VERSION"

echo "imgui $IMGUI_TAG, nlohmann/json $JSON_TAG"
