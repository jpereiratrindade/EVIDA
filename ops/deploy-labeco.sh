#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 EVIDA Contributors
set -Eeuo pipefail

TARGET_HOST="${1:-jpereiratrindade@10.163.80.129}"
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"

echo "=== [1/4] Gerando pacote de release do EVIDA ==="
"$ROOT/ops/build-release.sh"

LATEST_BUNDLE="$(ls -t "$ROOT"/dist/evida-*.tar.gz | head -n 1)"
BUNDLE_NAME="$(basename "$LATEST_BUNDLE")"
SHA_FILE="${LATEST_BUNDLE}.sha256"

echo "=== [2/4] Enviando arquivos para $TARGET_HOST ==="
scp "$LATEST_BUNDLE" "$SHA_FILE" "$ROOT/ops/install-release.sh" "$TARGET_HOST:~/"

echo "=== [3/4] Instalando EVIDA no $TARGET_HOST ==="
ssh -t "$TARGET_HOST" "sudo ./install-release.sh ./$BUNDLE_NAME"

echo "=== [4/4] Integrando EVIDA ao Sister-Synth em $TARGET_HOST ==="
ssh -t "$TARGET_HOST" '
if [ -f /etc/sister-synth-poc/systems.json ]; then
  if ! grep -q "\"evida\"" /etc/sister-synth-poc/systems.json; then
    echo "Registrando evida em /etc/sister-synth-poc/systems.json..."
    sudo python3 -c '\''
import json
with open("/etc/sister-synth-poc/systems.json", "r") as f:
    data = json.load(f)
data["evida"] = {
    "version": "0.3.2",
    "endpoint": "http://10.163.80.129:18142/",
    "health_path": "/api/health",
    "expected_marker": "evida",
    "description": "EVIDA scientific documentary evidence and analysis interface"
}
with open("/etc/sister-synth-poc/systems.json", "w") as f:
    json.dump(data, f, indent=2)
'\''
    sudo systemctl restart sister-synth-observer.service sister-synth-portal.service 2>/dev/null || true
    echo "Sister-synth atualizado com EVIDA!"
  else
    echo "EVIDA já está registrado em /etc/sister-synth-poc/systems.json."
  fi
fi
'

echo "=========================================================="
echo " DEPLOY CONCLUÍDO COM SUCESSO!"
echo " EVIDA no labeco-srv: http://10.163.80.129:18142/"
echo " Teste de saúde: curl -s http://10.163.80.129:18142/api/health"
echo "=========================================================="
