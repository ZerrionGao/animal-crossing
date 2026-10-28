#!/usr/bin/env bash
# Build the shader translation service as a self-contained Apple Silicon binary.
# Requires the .NET 10 SDK and network access for NuGet and NativeAOT on first build.
# Usage: publish_native.sh <output-binary> [dotnet-command]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ENGINE=$(cd "$HERE/../.." && pwd)
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: $0 <output-binary> [dotnet-command]" >&2
  exit 2
fi
OUTPUT_DIR=$(dirname "$1")
OUTPUT_NAME=$(basename "$1")
DOTNET=${2:-dotnet}
OPENSSL_LIB_DIR=${OPENSSL_LIB_DIR:-}
BROTLI_LIB_DIR=${BROTLI_LIB_DIR:-}
if [ -z "$OPENSSL_LIB_DIR" ] && command -v brew >/dev/null 2>&1; then
  OPENSSL_PREFIX=$(brew --prefix openssl@3 2>/dev/null || true)
  if [ -n "$OPENSSL_PREFIX" ] && [ -f "$OPENSSL_PREFIX/lib/libssl.dylib" ]; then
    OPENSSL_LIB_DIR="$OPENSSL_PREFIX/lib"
  fi
fi
if [ -z "$BROTLI_LIB_DIR" ] && command -v brew >/dev/null 2>&1; then
  BROTLI_PREFIX=$(brew --prefix brotli 2>/dev/null || true)
  if [ -n "$BROTLI_PREFIX" ] && [ -f "$BROTLI_PREFIX/lib/libbrotlienc.dylib" ]; then
    BROTLI_LIB_DIR="$BROTLI_PREFIX/lib"
  fi
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM HUP
mkdir -p "$WORK/ryujinx-src" "$WORK/xlate"
cp -R "$ENGINE/vendor/ryujinx/." "$WORK/ryujinx-src/"
find "$WORK/ryujinx-src/src" -maxdepth 2 \( -name obj -o -name bin \) -type d -exec rm -rf {} +
for p in "$HERE"/patches/*.patch; do patch -s -d "$WORK/ryujinx-src" -p1 --forward < "$p"; done
cp "$HERE"/*.cs "$HERE"/totk_xlate.csproj "$WORK/xlate/"
PUBLISH_ARGS=(-c Release -r osx-arm64 -p:PublishAot=true -p:InvariantGlobalization=true \
  "-p:RyuSrc=$WORK/ryujinx-src" -o "$WORK/out")
if [ -n "$OPENSSL_LIB_DIR" ]; then
  PUBLISH_ARGS+=("-p:OpenSslLibDir=$OPENSSL_LIB_DIR")
fi
if [ -n "$BROTLI_LIB_DIR" ]; then
  PUBLISH_ARGS+=("-p:BrotliLibDir=$BROTLI_LIB_DIR")
fi
"$DOTNET" publish "$WORK/xlate/totk_xlate.csproj" "${PUBLISH_ARGS[@]}"
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR=$(cd "$OUTPUT_DIR" && pwd)
OUTPUT="$OUTPUT_DIR/$OUTPUT_NAME"
cp "$WORK/out/totk_xlate" "$OUTPUT"
chmod 755 "$OUTPUT"
echo "built $OUTPUT"
