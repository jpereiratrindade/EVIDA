#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 EVIDA Contributors
set -Eeuo pipefail

[[ "$(id -u)" -eq 0 ]] || { echo "Execute como root." >&2; exit 1; }
[[ $# -eq 3 ]] || { echo "Uso: install-release.sh <release.tar.gz> <release-id> <sha256>" >&2; exit 2; }
ARCHIVE="$(readlink -f "$1")"
RELEASE_ID="$2"
EXPECTED_SHA256="$3"

[[ "$RELEASE_ID" =~ ^[0-9A-Za-z._-]+$ ]] || { echo "release-id inválido" >&2; exit 2; }
[[ "$EXPECTED_SHA256" =~ ^[0-9a-f]{64}$ ]] || { echo "SHA-256 inválido" >&2; exit 2; }
[[ -f "$ARCHIVE" ]] || { echo "Pacote ausente: $ARCHIVE" >&2; exit 1; }

printf '%s  %s\n' "$EXPECTED_SHA256" "$ARCHIVE" | sha256sum --check --status ||
  { echo "Integridade do pacote inválida." >&2; exit 1; }

BASE=/usr/local/share/evida
TARGET="$BASE/releases/$RELEASE_ID"
PREVIOUS="$(readlink -f "$BASE/current" 2>/dev/null || true)"
STAGING="$BASE/releases/.staging-$RELEASE_ID-$$"

cleanup() { rm -rf -- "$STAGING"; }
trap cleanup EXIT

install -d -m 0755 "$BASE/releases" /var/lib/evida/storage/documents /var/lib/evida/var
[[ ! -e "$TARGET" ]] || { echo "Release já existe: $TARGET" >&2; exit 1; }
install -d -m 0755 "$STAGING"
tar -xzf "$ARCHIVE" -C "$STAGING"

test -x "$STAGING/build/evida-server"
test -f "$STAGING/web/index.html"

mv "$STAGING" "$TARGET"
chown -R root:root "$TARGET"
find "$TARGET" -type d -exec chmod 0755 {} +
find "$TARGET" -type f -exec chmod a-w {} +
chmod 0755 "$TARGET/build/evida-server"

ln -sfn "$TARGET" "$BASE/current.new"
mv -Tf "$BASE/current.new" "$BASE/current"

# Atualiza e recarrega serviços systemd
if [[ -f "$TARGET/deploy/evida.service" ]]; then
  install -m 0644 "$TARGET/deploy/evida.service" /etc/systemd/system/
fi
if [[ -f "$TARGET/deploy/evida-sync.service" ]]; then
  install -m 0644 "$TARGET/deploy/evida-sync.service" /etc/systemd/system/
fi
if [[ -f "$TARGET/deploy/evida-sync.timer" ]]; then
  install -m 0644 "$TARGET/deploy/evida-sync.timer" /etc/systemd/system/
fi

systemctl daemon-reload
systemctl enable evida.service evida-sync.timer
if ! systemctl restart evida.service; then
  echo "Falha ao reiniciar evida.service; revertendo..." >&2
  if [[ -n "$PREVIOUS" && -d "$PREVIOUS" ]]; then
    ln -sfn "$PREVIOUS" "$BASE/current.new"
    mv -Tf "$BASE/current.new" "$BASE/current"
    systemctl restart evida.service || true
  fi
  exit 1
fi

# Health check
HEALTHY=0
for _ in {1..30}; do
  if /usr/bin/curl --noproxy "*" --fail --silent http://127.0.0.1:18142/api/health | grep -q "evida"; then
    HEALTHY=1
    break
  fi
  sleep 0.5
done

if [[ "$HEALTHY" -ne 1 ]]; then
  echo "Verificação de integridade falhou; executando rollback..." >&2
  if [[ -n "$PREVIOUS" && -d "$PREVIOUS" ]]; then
    ln -sfn "$PREVIOUS" "$BASE/current.new"
    mv -Tf "$BASE/current.new" "$BASE/current"
    systemctl restart evida.service || true
  fi
  exit 1
fi

echo "EVIDA release $RELEASE_ID instalado com sucesso e em operação."
