# EVIDA — Evidências Documentais e Análise (nome provisório)

**Sempre pronto. Sempre incompleto.** Versão 0.1.0.

Sistema científico independente para estudar documentos, preservar proveniência,
examinar relações narrativas e construir análises temporais, territoriais e de redes.
Corpus-piloto: Informativos Conjunturais Emater/RS-Ascar (2026).

## Fronteira científica

- Uma menção documental **não** comprova uma ocorrência independente.
- Coocorrência e similaridade editorial **não** comprovam relação causal/territorial.
- Alegações, inferências e validações humanas são objetos distintos.
- Toda evidência exige referência identificável, página, trecho e estado epistêmico.
- O original permanece inalterado e fora do Git; nenhuma análise apaga sua fonte.

## Compilação (Fedora)

```bash
sudo dnf install -y gcc-c++ cmake ninja-build boost-devel sqlite-devel git python3
./scripts/dev.sh
```

A primeira execução compila, testa e inicia em http://127.0.0.1:18142.
O servidor escuta **somente em 127.0.0.1**; ainda não existe autenticação.
Não exponha na LAN ou na Internet.

Modo manual:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/evida-server --port 18142 --db var/evida.sqlite
```

## Endpoints v0.1.0

- `GET /api/health` — saúde da aplicação;
- `GET /api/v1/capabilities` — recursos realmente disponíveis;
- `GET /api/v1/stats` — contagens SQL reais (começam em zero);
- `GET /` — interface web estática sem dependências CDN.

## Escopo v0.1.0

Este incremento cria **infraestrutura funcional**, não importa nem processa PDFs.
A migração da análise, extração, grafos e dossiês deve reproduzir resultados do
legado por testes de equivalência antes de ser declarada concluída.

Veja `docs/ARCH-001-v0.1.0.tex` e `docs/ROADMAP.md`.
