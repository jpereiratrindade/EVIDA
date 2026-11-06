#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 EVIDA Contributors
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
OUTPUT_DIR="${1:-$ROOT/dist}"
RELEASE_ID="${2:-$(git -C "$ROOT" rev-parse --short=12 HEAD)}"

[[ "$RELEASE_ID" =~ ^[0-9A-Za-z._-]+$ ]] || {
  echo "release-id inválido" >&2
  exit 2
}
[[ -z "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]] || {
  echo "Há alterações rastreadas sem commit; faça commit antes de empacotar." >&2
  exit 1
}

# Compilação e testes (executa dentro de toolbox se fedora-44-sister existir)
RUN_ENV=""
if command -v toolbox >/dev/null 2>&1 && toolbox list | grep -q "fedora-44-sister"; then
  RUN_ENV="toolbox run -c fedora-44-sister"
fi

BUILD_DIR="$ROOT/build"
$RUN_ENV cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
$RUN_ENV cmake --build "$BUILD_DIR" --parallel
$RUN_ENV ctest --test-dir "$BUILD_DIR" --output-on-failure

test -x "$BUILD_DIR/evida-server"

STAGING="$(mktemp -d)"
cleanup() { rm -rf -- "$STAGING"; }
trap cleanup EXIT

git -C "$ROOT" archive HEAD | tar -x -C "$STAGING"
install -d "$STAGING/build" "$STAGING/var" "$STAGING/storage/documents"
install -m 0755 "$BUILD_DIR/evida-server" "$STAGING/build/evida-server"

# Inclui base de dados do corpus e cofre de documentos na release
if [[ -f "$ROOT/var/evida.sqlite" ]]; then
  cp "$ROOT/var/evida.sqlite" "$STAGING/var/evida.sqlite"
fi
if [[ -d "$ROOT/storage/documents" ]]; then
  cp -a "$ROOT/storage/documents/"* "$STAGING/storage/documents/" 2>/dev/null || true
fi

mkdir -p "$OUTPUT_DIR"
ARCHIVE="$OUTPUT_DIR/evida-$RELEASE_ID.tar.gz"
tar -czf "$ARCHIVE" -C "$STAGING" .
SHA256="$(sha256sum "$ARCHIVE" | awk '{print $1}')"
printf '%s  %s\n' "$SHA256" "$(basename "$ARCHIVE")" >"$ARCHIVE.sha256"
printf 'Bundle EVIDA: %s\nSHA-256: %s\n' "$ARCHIVE" "$SHA256"
