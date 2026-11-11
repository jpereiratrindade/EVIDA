# EVIDA — Sistema Científico de Evidências Documentais e Análise

**Sempre pronto. Sempre incompleto.** Versão 0.3.3 (*Consistência Probatória*).

Sistema científico independente para estudar documentos, preservar proveniência estrita,
examinar relações narrativas e construir análises temporais, territoriais e de redes.
Corpus: Informativos Conjunturais Emater/RS-Ascar, com cobertura por ano em construção.

## Licença

Distribuído sob os termos da **GNU General Public License v3.0 (GNU GPLv3)**.
Consulte o arquivo [`LICENSE`](file:///run/media/jpereiratrindade/labeco10T/dev/cpp/evida/LICENSE) para o texto integral da licença.

## Fronteira científica & Regras Epistêmicas

- **Regra Metodológica SisTer:** Uma afirmação em relatório oficial é uma *evidência documental institucional*, e **não** comprovação física automática de que o fenômeno aconteceu exatamente daquela maneira.
- **Custódia Documental & Verificação Dinâmica de Bytes:** Os arquivos PDF originais residem em cofre frio (`storage/documents/<sha256>.pdf`). O status do documento inicia em `pending` e somente transita para `verified` após recálculo físico do hash SHA-256 via OpenSSL sobre os bytes do arquivo em disco durante as auditorias.
- **Proveniência M:N e Revisão Humana:** Vínculos explícitos (`supports`, `revises`, `disputes`) conectam registros analíticos aos trechos literais (`evidence_span`). Revisões de pesquisadores são registradas na entidade `review_decision`, preservando o extrato original intacto.
- **Isolamento de Rede:** Servidor vinculado estritamente ao loopback local `127.0.0.1`, sem exposição em interfaces externas e com `dev_mode` desativado por padrão.

## Coleta, cobertura e revisão

O acervo contém informativos de múltiplos anos. Quantidades atuais são consultadas
em `/api/v1/years` e `/api/v1/stats`; não representam certificação de completude.
O histórico de consultas por mês e falhas está em `/api/v1/collection/status`.
Execução concluída significa que as consultas terminaram, não que todo documento
historicamente publicado está presente.

A coleta diária às 18h30 (America/Sao_Paulo, até 5 minutos de atraso aleatório)
consulta o ano corrente, verifica os PDFs conhecidos para detectar revisões e
registra falhas. Novas tentativas ocorrem nas execuções diárias seguintes.
Anos anteriores são solicitados explicitamente. PDFs e versões textuais anteriores
são preservados. Reprocessamentos mantêm os registros anteriores com status
`superseded`; decisões humanas não são apagadas.

Resultados da extração automática são candidatos heurísticos, inclusive as tabelas.
Unidades ambíguas não são inventadas; intervalos de chuva preservam ambos os limites.
O extrator é conservador e não cobre todos os tipos de tabela ou todas as variáveis.
A auditoria verifica bytes, referências e vínculos, sem certificar interpretação
semântica, independência das observações ou completude histórica.
Séries e ocorrências legadas não são reconstruídas pelo extrator quantitativo.

## Compilação e Execução (Fedora / Linux)

```bash
# Dependências: gcc-c++ (C++26), cmake, ninja-build, boost-devel, sqlite-devel, openssl-devel, poppler-utils
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/evida-server --port 18142 --db var/evida.sqlite
```

Acesso local disponível em: **http://127.0.0.1:18142**

## Principais Endpoints da API REST (v0.3.2)

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

## Premissas de desenvolvimento

**Simples, leve. Sempre pronto. Sempre incompleto!**

[Significado e símbolos das premissas](docs/sister-principles/README.md).
