#!/usr/bin/env sh
# Fills the winget manifest templates for one release.
#
#   tools/winget-manifests.sh <PackageIdentifier> <version> <zip> <download-url> <publisher> <repo-url>
#
# Output goes to build/winget/manifests/<x>/<Publisher>/<App>/<version>/, the same
# layout as microsoft/winget-pkgs, ready for `wingetcreate submit` or a PR.
set -eu

ID=$1 VERSION=$2 ZIP=$3 URL=$4 PUBLISHER=$5 REPO_URL=$6
SHA256=$(sha256sum "$ZIP" | cut -d' ' -f1 | tr 'a-f' 'A-F')
DATE=$(date -u +%Y-%m-%d)

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PUB=${ID%%.*}
APP=${ID#*.}
FIRST=$(printf '%s' "$PUB" | cut -c1 | tr 'A-Z' 'a-z')
OUT="$ROOT/build/winget/manifests/$FIRST/$PUB/$APP/$VERSION"
mkdir -p "$OUT"

fill() {
    sed -e "s|@ID@|$ID|g" -e "s|@VERSION@|$VERSION|g" -e "s|@URL@|$URL|g" \
        -e "s|@SHA256@|$SHA256|g" -e "s|@DATE@|$DATE|g" -e "s|@PUBLISHER@|$PUBLISHER|g" \
        -e "s|@REPO_URL@|$REPO_URL|g" "$1" > "$2"
}

fill "$ROOT/packaging/winget/version.yaml.in"         "$OUT/$ID.yaml"
fill "$ROOT/packaging/winget/installer.yaml.in"       "$OUT/$ID.installer.yaml"
fill "$ROOT/packaging/winget/locale.en-US.yaml.in"    "$OUT/$ID.locale.en-US.yaml"
echo "$OUT"
