#!/bin/bash
# UA Reforged - sign a release file for the launchers' one-click update (release time, on the release machine only).
#
#   tools/sign-update.sh --key-cmd '<command that prints the PEM private key>' <file> <version> <mac|windows|linux> <url>
#
# Writes a small manifest (name, version, platform, bytes, sha256) and an ECDSA P-256 / SHA-256 signature over it, and
# prints the "update" object for links.json "latest.<platform>":
#   {"url": "<url>", "manifest": "<base64>", "sig": "<base64 DER>"}
# Also writes <file>.update.json (the same object) next to the file, for the release notes / the site.
# The private key comes from --key-cmd through a pipe: it is never written to disk and never on a command line. The
# launchers accept the signature only with the public key in packaging/macos/downloads.txt (update_key); test-hook
# builds also take tests/launcher/update-key-test.txt.
set -euo pipefail
KEYCMD=""
[ "${1:-}" = --key-cmd ] && { KEYCMD="$2"; shift 2; }
[ $# = 4 ] && [ -n "$KEYCMD" ] || { sed -n '4,5p' "$0"; exit 2; }
FILE="$1" VER="$2" PLAT="$3" URL="$4"
[ -f "$FILE" ] || { echo "no such file: $FILE" >&2; exit 2; }
[[ "$VER" =~ ^[0-9]{1,6}(\.[0-9]{1,6}){1,3}$ ]] || { echo "bad version: $VER" >&2; exit 2; }
case "$PLAT" in mac) ext=zip ;; windows) ext=exe ;; linux) ext=AppImage ;; *) echo "platform must be mac, windows or linux" >&2; exit 2 ;; esac
NAME="$(basename "$FILE")"
[[ "$NAME" =~ ^[A-Za-z0-9._-]{1,100}$ ]] && [ "${NAME##*.}" = "$ext" ] || { echo "file name must be plain and end in .$ext" >&2; exit 2; }
[ "${URL##*/}" = "$NAME" ] || { echo "the url must end in /$NAME" >&2; exit 2; }
OSSL=/usr/bin/openssl; command -v "$OSSL" >/dev/null || OSSL=openssl
T="$(mktemp -d "${TMPDIR:-/tmp}/ua-sign.XXXXXX")"; trap 'rm -rf "$T"' EXIT
BYTES=$(wc -c < "$FILE" | tr -d ' ')
SHA=$( (shasum -a 256 "$FILE" 2>/dev/null || sha256sum "$FILE") | cut -d' ' -f1)
printf 'UA Reforged update 1\nname\t%s\nversion\t%s\nplatform\t%s\nbytes\t%s\nsha256\t%s\n' "$NAME" "$VER" "$PLAT" "$BYTES" "$SHA" > "$T/manifest"
# the key goes from the command straight into openssl (stdin)
bash -c "$KEYCMD" | "$OSSL" dgst -sha256 -sign /dev/stdin -out "$T/sig" "$T/manifest"
# check it with the public half, as the launchers do
bash -c "$KEYCMD" | "$OSSL" pkey -pubout -out "$T/pub.pem"
"$OSSL" dgst -sha256 -verify "$T/pub.pem" -signature "$T/sig" "$T/manifest" >/dev/null
PUB=$(grep -v -- '-----' "$T/pub.pem" | tr -d '\n')
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
which=""
grep -q "^update_key	$PUB\$" "$ROOT/packaging/macos/downloads.txt" && which="release key"
[ "$PUB" = "$(tr -d '\n' < "$ROOT/tests/launcher/update-key-test.txt")" ] && which="TEST key (only test-hook builds accept it)"
[ -n "$which" ] || { echo "the key is neither the release key nor the test key" >&2; exit 1; }
MAN=$("$OSSL" base64 -A -in "$T/manifest"); SIG=$("$OSSL" base64 -A -in "$T/sig")
JSON=$(printf '{"url": "%s", "manifest": "%s", "sig": "%s"}' "$URL" "$MAN" "$SIG")
printf '%s\n' "$JSON" > "$FILE.update.json"
echo "signed with the $which: $NAME $VER $PLAT $BYTES bytes sha256 $SHA" >&2
printf '%s\n' "$JSON"
