#!/usr/bin/env bash
#
# Packages the client of a finished build into TV Demo Rewinder.app, nothing is compiled here.
# Usage: make-app.command [build folder] [destination folder]
# Defaults to the build folder of this repository and ~/Applications.

set -Eeuo pipefail

source_folder="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
repo="$( cd "$source_folder/../../.." && pwd )"
build="${1:-$repo/build}"
destination="${2:-$HOME/Applications}"

app="$destination/TV Demo Rewinder.app"
contents="$app/Contents"
game="$contents/Resources/game"
engine="$build/ET Legacy.app/Contents/MacOS/ET Legacy"
lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister

for file in "$engine" "$build/librenderer_opengl1_mac" "$build/legacy/cgame_mac" "$build/legacy/ui_mac"; do
	if [ ! -f "$file" ]; then
		echo "Missing $file, build the client first" >&2
		exit 1
	fi
done

warn_if_stale() {
	local binary=$1
	shift
	local newer
	newer=$(find "$@" -type f -name '*.[chm]' -newer "$binary")
	if [ -n "$newer" ]; then
		echo "WARNING: $(basename "$binary") is older than these sources, rebuild to include them:"
		echo "$newer" | sed 's/^/  /'
	fi
}

warn_if_stale "$engine" "$repo"/src/client "$repo"/src/qcommon "$repo"/src/sdl "$repo"/src/sys
warn_if_stale "$build/legacy/cgame_mac" "$repo"/src/cgame "$repo"/src/game
warn_if_stale "$build/legacy/ui_mac" "$repo"/src/ui

mkdir -p "$contents/MacOS" "$contents/Resources" "$game/legacy"

# rsync replaces files instead of writing into them, macOS kills a signed binary which got overwritten in place
rsync -a "$source_folder/Info.plist" "$contents/"
rsync -a "$source_folder/launcher" "$contents/MacOS/"
rsync -a "$engine" "$contents/MacOS/tvdemorewinder"
rsync -a "$build/librenderer_opengl1_mac" "$game/"
rsync -a --delete --include='legacy_*.pk3' --include='cgame_mac' --include='ui_mac' --exclude='*' "$build/legacy/" "$game/legacy/"

temp_dir=$(mktemp -d)
mkdir "$temp_dir/icon.iconset"
for size in 16 32 128 256 512; do
	sips -s format png -z $size $size "$source_folder/icon.svg" --out "$temp_dir/icon.iconset/icon_${size}x${size}.png" >/dev/null
	sips -s format png -z $((size * 2)) $((size * 2)) "$source_folder/icon.svg" --out "$temp_dir/icon.iconset/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$temp_dir/icon.iconset" -o "$contents/Resources/icon.icns"
rm -Rf "$temp_dir"

touch "$app"
"$lsregister" -f "$app"

echo "Created $app"
