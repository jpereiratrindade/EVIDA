#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 EVIDA Contributors
set -Eeuo pipefail

[[ "$(id -u)" -eq 0 ]] || { echo "Execute como root (ex: sudo $0)." >&2; exit 1; }

# Auto-descoberta se parâmetros não forem fornecidos
ARCHIVE_ARG="${1:-}"
if [[ -z "$ARCHIVE_ARG" ]]; then
  ARCHIVE_ARG="$(ls -t ./evida-*.tar.gz 2>/dev/null | head -n 1 || true)"
fi
[[ -n "$ARCHIVE_ARG" && -f "$ARCHIVE_ARG" ]] || {
  echo "Uso: $0 [release.tar.gz] [release-id] [sha256]" >&2
  echo "Erro: nenhum arquivo evida-*.tar.gz encontrado no diretório atual." >&2
  exit 1
}

ARCHIVE="$(readlink -f "$ARCHIVE_ARG")"
FILENAME="$(basename "$ARCHIVE")"

# Auto-descoberta de release-id
RELEASE_ID="${2:-}"
if [[ -z "$RELEASE_ID" ]]; then
  if [[ "$FILENAME" =~ evida-([0-9A-Za-z._-]+)\.tar\.gz ]]; then
    RELEASE_ID="${BASH_REMATCH[1]}"
  else
    RELEASE_ID="current"
  fi
fi

# Auto-descoberta ou validação de SHA-256
EXPECTED_SHA256="${3:-}"
if [[ -z "$EXPECTED_SHA256" && -f "${ARCHIVE}.sha256" ]]; then
  EXPECTED_SHA256="$(awk '{print $1}' "${ARCHIVE}.sha256")"
fi

if [[ -n "$EXPECTED_SHA256" ]]; then
  printf '%s  %s\n' "$EXPECTED_SHA256" "$ARCHIVE" | sha256sum --check --status || {
    echo "Integridade do pacote inválida (falha no SHA-256)." >&2
    exit 1
  }
else
  echo "[aviso] Arquivo .sha256 ausente; calculando SHA-256 do pacote..."
  sha256sum "$ARCHIVE"
fi

BASE=/usr/local/share/evida
TARGET="$BASE/releases/$RELEASE_ID"
PREVIOUS="$(readlink -f "$BASE/current" 2>/dev/null || true)"
STAGING="$BASE/releases/.staging-$RELEASE_ID-$$"

cleanup() { rm -rf -- "$STAGING"; }
trap cleanup EXIT

install -d -m 0755 "$BASE/releases" /var/lib/evida/storage/documents /var/lib/evida/var
rm -rf "$TARGET"
install -d -m 0755 "$STAGING"
tar -xzf "$ARCHIVE" -C "$STAGING"

test -x "$STAGING/build/evida-server"
test -f "$STAGING/web/index.html"

# Atualiza ou instala a base de dados do corpus e cofre de documentos
if [[ -f "$STAGING/var/evida.sqlite" ]]; then
  if [[ -f /var/lib/evida/var/evida.sqlite ]]; then
    cp /var/lib/evida/var/evida.sqlite "/var/lib/evida/var/evida.sqlite.bak-$$" 2>/dev/null || true
  fi
  cp "$STAGING/var/evida.sqlite" /var/lib/evida/var/evida.sqlite
fi
if [[ -d "$STAGING/storage/documents" ]]; then
  cp -rn "$STAGING/storage/documents/"* /var/lib/evida/storage/documents/ 2>/dev/null || true
fi

mv "$STAGING" "$TARGET"
chown -R root:root "$TARGET"
find "$TARGET" -type d -exec chmod 0755 {} +
find "$TARGET" -type f -exec chmod a-w {} +
chmod 0755 "$TARGET/build/evida-server"

ln -sfn "$TARGET" "$BASE/current.new"
mv -Tf "$BASE/current.new" "$BASE/current"

# Atualiza unidade systemd para apontar para /usr/local/share/evida/current
cat << 'EOF' > /etc/systemd/system/evida.service
[Unit]
Description=EVIDA — Evidências Científicas e Custódia Documental
After=network.target
Documentation=https://github.com/jpereiratrindade/EVIDA

[Service]
Type=simple
User=root
WorkingDirectory=/var/lib/evida
ExecStart=/usr/local/share/evida/current/build/evida-server --host 0.0.0.0 --port 18142 --db /var/lib/evida/var/evida.sqlite --web /usr/local/share/evida/current/web
Restart=always
RestartSec=5s
KillMode=process

MemoryHigh=256M
MemoryMax=512M
CPUQuota=50%

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now evida.service
systemctl restart evida.service

# Liberação de porta no firewall para rede local
if command -v firewall-cmd >/dev/null 2>&1; then
  firewall-cmd --permanent --add-port=18142/tcp 2>/dev/null || true
  firewall-cmd --permanent --zone=FedoraServer --add-port=18142/tcp 2>/dev/null || true
  firewall-cmd --reload 2>/dev/null || true
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

echo "=========================================================="
echo " EVIDA ($RELEASE_ID) instalado com sucesso e em operação!"
echo " Endpoint local: http://127.0.0.1:18142/"
echo "=========================================================="
