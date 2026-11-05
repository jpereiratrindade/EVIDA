# EVIDA — Sistema Científico de Evidências Documentais e Análise

**Sempre pronto. Sempre incompleto.** Versão 0.3.0.

Sistema científico independente para estudar documentos, preservar proveniência estrita,
examinar relações narrativas e construir análises temporais, territoriais e de redes.
Corpus ativo: Informativos Conjunturais Emater/RS-Ascar (2026).

## Licença

Distribuído sob os termos da **GNU General Public License v3.0 (GNU GPLv3)**.
Consulte o arquivo [`LICENSE`](file:///run/media/jpereiratrindade/labeco10T/dev/cpp/evida/LICENSE) para o texto integral da licença.

## Fronteira científica & Regras Epistêmicas

- **Regra Metodológica SisTer:** Uma afirmação em relatório oficial é uma *evidência documental institucional*, e **não** comprovação física automática de que o fenômeno aconteceu exatamente daquela maneira.
- **Cold Custody Archival:** Os arquivos PDF originais são mantidos em cofre frio local (`storage/documents/<sha256>.pdf`) com duplo hash criptográfico (binário e texto normalizado).
- **Prova Documental Sob Demanda:** A interface do sistema foca no conhecimento científico e entidades (Eventos, Observações, Territórios, Trajetórias). Qualquer evidência pode ser auditada sob demanda através de um drawer probatório que exibe o documento fonte, a assinatura SHA-256, a página física exata e o contexto textual integral da página.
- **Coocorrência vs. Causalidade:** Coocorrência textual ou proximidade física não comprovam relação causal independente sem validação empírica.

## Corpus Científico (2026)

| Dimensão Científica | Quantidade Catalogada | Proveniência |
| :--- | :--- | :--- |
| **Documentos sob Custódia** | 34 relatórios completos | SHA-256 verificado no cofre |
| **Eventos & Riscos** | 2.354 eventos relatados | Seca, estiagem, chuvas, pragas, doenças |
| **Observações & Métricas** | 5.230 medições de campo | Precipitação (mm), colheita (%), produtividade |
| **Territórios IBGE (RS)** | 497 municípios mapeados | Códigos de 7 dígitos e ocorrências nominais |
| **Episódios & Trajetórias** | 326 episódios contínuos | Agrupamentos em janelas temporais de 21 dias |
| **Cotações de Preços** | 1.387 séries históricas | Produtos agrícolas nas regiões da Emater |
| **Páginas Textuais Indexadas** | 1.273 páginas extraídas | Texto integral por página para conferência auditável |

## Compilação e Execução (Fedora / Linux)

```bash
# Dependências: gcc-c++ (C++26), cmake, ninja-build, boost-devel, sqlite-devel, openssl-devel, poppler-utils
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/evida-server --port 18142 --db var/evida.sqlite --dev
```

Acesso local disponível em: **http://127.0.0.1:18142**

## Principais Endpoints da API REST (v0.3.0)

- `GET /api/health` — integridade e versão do serviço;
- `GET /api/v1/capabilities` — mapa de capacidades científicas ativas;
- `GET /api/v1/stats` — estatísticas reais do corpus em tempo de execução;
- `GET /api/v1/events` — eventos filtráveis por classe, região e atividade;
- `GET /api/v1/observations` — medições de campo filtráveis por variável, cultura e região;
- `GET /api/v1/municipalities` — municípios IBGE do RS com contagem de ocorrências;
- `GET /api/v1/occurrences` — ocorrências por código IBGE municipal;
- `GET /api/v1/episodes` — episódios temporais de 21 dias;
- `GET /api/v1/vault/documents` — inventário criptográfico dos 34 PDFs sob custódia no cofre;
- `GET /api/v1/evidence/:id/audit` — selo probatório, hash SHA-256, página e contexto integral;
- `POST /api/v1/documents/upload` — ingestão contínua com cálculo instantâneo de SHA-256;
- `POST /api/v1/auth/request-code` / `POST /api/v1/auth/verify-code` — autenticação OTP padrão MORFO.
