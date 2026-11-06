# EVIDA — Guia de Implantação e Agendamento no `labeco-srv`

Este diretório contém os artefatos de serviço do systemd e instruções para colocar o EVIDA em produção contínua no servidor do laboratório (`labeco-srv`), co-localizado com serviços existentes (como MORFO).

---

## 1. Viabilidade no `labeco-srv`

O EVIDA é implementado em C++26 nativo (Crow/Asio) com SQLite em modo WAL:
- **Consumo de memória em repouso:** ~25 MB a 30 MB RSS.
- **CPU em repouso:** 0.0%.
- **Sincronização semanal de novos PDFs:** ~1 a 2 segundos de CPU a cada quinta-feira.
- **Isolamento de recursos:** Configurado no `evida.service` com `MemoryHigh=256M`, `MemoryMax=512M` e `CPUQuota=50%`.
- **Impacto no MORFO e outros serviços:** Nulo. Não interfere em portas nem aloca recursos de GPU ou memória excessiva.

---

## 2. Instalação como Serviço Systemd

Para instalar os serviços no `labeco-srv`:

```bash
# 1. Copiar as unidades para o diretório de serviços do systemd
sudo cp deploy/evida.service /etc/systemd/system/
sudo cp deploy/evida-sync.service /etc/systemd/system/
sudo cp deploy/evida-sync.timer /etc/systemd/system/

# 2. Recarregar a configuração do systemd
sudo systemctl daemon-reload

# 3. Habilitar e iniciar o daemon principal
sudo systemctl enable --now evida.service

# 4. Habilitar e iniciar o timer de sincronização semanal
sudo systemctl enable --now evida-sync.timer

# 5. Conferir status
sudo systemctl status evida.service
sudo systemctl list-timers evida-sync.timer
```

---

## 3. Alternativa via Crontab do Usuário

Caso prefira não usar systemd timers, adicione à crontab (`crontab -e`):

```cron
# EVIDA: Sincronização semanal com Emater/RS às quintas-feiras às 18:30
30 18 * * 4 cd /run/media/jpereiratrindade/labeco10T/dev/cpp/evida && /usr/bin/python3 tools/sync_emater_repository.py --cron >> var/sync.log 2>&1
```

---

## 4. Endpoints Disponíveis na Rede Local

Com o servidor rodando em `0.0.0.0:18142`:
- **Interface Web:** `http://<ip-do-labeco-srv>:18142/`
- **Painel de Auditoria Documental:** `http://<ip-do-labeco-srv>:18142/audit`
- **Healthcheck:** `GET http://<ip-do-labeco-srv>:18142/api/health`
- **Conferência de Custódia:** `GET http://<ip-do-labeco-srv>:18142/api/custody/verify`
- **Consulta de Provas:** `GET http://<ip-do-labeco-srv>:18142/api/provenance/evidence/<id>`
