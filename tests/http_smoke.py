#!/usr/bin/env python3
"""Teste de ponta a ponta; porta efêmera para evitar colisões locais."""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
import urllib.request

# Evita que o proxy de rede intercepte conexões de teste em loopback local
os.environ["NO_PROXY"] = "127.0.0.1,localhost"
os.environ["no_proxy"] = "127.0.0.1,localhost"
local_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
urlopen = local_opener.open


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def post_json(url, data, headers=None):
    req_headers = {"Content-Type": "application/json"}
    if headers:
        req_headers.update(headers)
    body = json.dumps(data).encode("utf-8")
    req = urllib.request.Request(url, data=body, headers=req_headers, method="POST")
    with urlopen(req) as resp:
        return json.loads(resp.read().decode("utf-8")), resp.headers


def main():
    port = free_port()
    with tempfile.TemporaryDirectory() as tmp:
        db = str(Path(tmp) / "run.sqlite")
        proc = subprocess.Popen([sys.argv[1], "--port", str(port), "--db", db, "--dev"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            base = f"http://127.0.0.1:{port}"
            for _ in range(100):
                if proc.poll() is not None:
                    raise AssertionError("Servidor encerrou antes da resposta")
                try:
                    with urlopen(base + "/api/health", timeout=0.2) as response:
                        health = json.load(response)
                    break
                except (OSError, TimeoutError):
                    time.sleep(0.05)
            else:
                raise AssertionError("Servidor não iniciou em 5 segundos")
            assert health["status"] == "ok"
            assert health["version"] == "0.2.0"

            with urlopen(base + "/api/v1/stats") as response:
                stats = json.load(response)
                assert stats["documents"] == 0
                assert stats["evidences"] == 0
                assert stats["users"] >= 2

            with urlopen(base + "/api/v1/capabilities") as response:
                caps = json.load(response)
                assert caps["document_ingestion"] is True
                assert caps["pdf_extraction"] is True
                assert caps["auth_smtp"] is True

            with urlopen(base + "/api/v1/auth/providers") as response:
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
            with urlopen(req_me) as response:
                me = json.load(response)
                assert me["authenticated"] is True
                assert me["user"]["email"] == "pesquisador@labeco.local"

            # Teste listagem de documentos e evidências
            with urlopen(f"{base}/api/v1/documents") as response:
                docs = json.load(response)
                assert "documents" in docs

            with urlopen(f"{base}/api/v1/evidences") as response:
                evs = json.load(response)
                assert "evidences" in evs

            with urlopen(base + "/") as response:
                html = response.read().decode("utf-8")
                assert "EVIDA" in html
                assert "MORFO" in html or "SisTer" in html

            assert Path(db).is_file()
            print("PROBANTE: HTTP, SQLite v0.2, Autenticação MORFO, Ingestão e UI OK")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    main()
