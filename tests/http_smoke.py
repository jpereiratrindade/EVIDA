# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 EVIDA Contributors
# This file is part of EVIDA - Scientific Evidence Intelligence System
# Licensed under the GNU General Public License v3.0 or later.

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

# Assegura que proxy não intercepte chamadas locais de teste
os.environ["NO_PROXY"] = "127.0.0.1,localhost"
os.environ["no_proxy"] = "127.0.0.1,localhost"
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
urllib.request.install_opener(opener)

def get_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

def post_json(url: str, data: dict, headers: dict = None) -> tuple[dict, dict]:
    req_headers = {"Content-Type": "application/json"}
    if headers:
        req_headers.update(headers)
    req = urllib.request.Request(url, data=json.dumps(data).encode("utf-8"), headers=req_headers, method="POST")
    with urllib.request.urlopen(req, timeout=5) as response:
        body = json.load(response)
        resp_headers = dict(response.headers)
        return body, resp_headers

def main():
    if len(sys.argv) < 2:
        print("Uso: http_smoke.py <caminho_para_evida-server>", file=sys.stderr)
        sys.exit(2)
    port = get_free_port()
    tmpdir = tempfile.mkdtemp(prefix="evida-smoke-")
    db = os.path.join(tmpdir, "smoke.sqlite")
    try:
        proc = subprocess.Popen([sys.argv[1], "--port", str(port), "--db", db, "--dev"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            base = f"http://127.0.0.1:{port}"
            for _ in range(100):
                if proc.poll() is not None:
                    raise AssertionError("Servidor encerrou antes da resposta")
                try:
                    with urllib.request.urlopen(base + "/api/health", timeout=0.2) as response:
                        health = json.load(response)
                    break
                except (OSError, TimeoutError):
                    time.sleep(0.05)
            else:
                raise AssertionError("Servidor não iniciou em 5 segundos")
            assert health["status"] == "ok"
            assert health["version"] in ("0.3.3",)

            with urllib.request.urlopen(base + "/api/v1/stats") as response:
                stats = json.load(response)
                assert stats["documents"] >= 0
                assert stats["users"] >= 2

            with urllib.request.urlopen(base + "/api/v1/collection/status") as response:
                collection = json.load(response)
                assert collection['runs'] == []
                assert collection['coverage_complete'] is False

            with urllib.request.urlopen(base + "/api/v1/capabilities") as response:
                caps = json.load(response)
                assert caps["document_ingestion"] is True
                assert caps["pdf_extraction"] is True
                assert caps["auth_smtp"] is True
                assert caps["scientific_corpus"] is True
                assert caps["audit_vault"] is True
                assert caps["dynamic_byte_verification"] is True
                assert caps["unified_provenance"] is True

            with urllib.request.urlopen(base + "/api/v1/auth/providers") as response:
                prov = json.load(response)
                assert prov["email_otp"] is True
                assert prov["dev_mode"] is True

            # Teste do fluxo completo de autenticação MORFO (request-code -> verify-code -> session)
            req_data, _ = post_json(f"{base}/api/v1/auth/request-code", {"email": "pesquisador@labeco.local"})
            assert req_data["status"] == "ok"
            dev_code = req_data.get("dev_code")
            assert dev_code and len(dev_code) == 6

            ver_data, ver_headers = post_json(f"{base}/api/v1/auth/verify-code", {
                "email": "pesquisador@labeco.local",
                "code": dev_code
            })
            assert ver_data["status"] == "ok"
            session_token = ver_data["session_token"]
            assert session_token

            # Teste /api/v1/auth/me com token de sessão
            req_me = urllib.request.Request(f"{base}/api/v1/auth/me", headers={"Authorization": f"Bearer {session_token}"})
            with urllib.request.urlopen(req_me) as response:
                me = json.load(response)
                assert me["authenticated"] is True
                assert me["user"]["email"] == "pesquisador@labeco.local"

            # Teste listagem de documentos e evidências
            with urllib.request.urlopen(f"{base}/api/v1/documents") as response:
                docs = json.load(response)
                assert "documents" in docs

            with urllib.request.urlopen(f"{base}/api/v1/evidences") as response:
                evs = json.load(response)
                assert "evidences" in evs

            # Teste endpoints científicos
            with urllib.request.urlopen(f"{base}/api/v1/events?limit=5") as response:
                events = json.load(response)
                assert "events" in events

            with urllib.request.urlopen(f"{base}/api/v1/observations?limit=5") as response:
                obs = json.load(response)
                assert "observations" in obs

            with urllib.request.urlopen(f"{base}/api/v1/municipalities?limit=5") as response:
                muns = json.load(response)
                assert "municipalities" in muns

            with urllib.request.urlopen(f"{base}/api/v1/vault/documents") as response:
                vault_docs = json.load(response)
                assert "vault_documents" in vault_docs

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

if __name__ == "__main__":
    main()
