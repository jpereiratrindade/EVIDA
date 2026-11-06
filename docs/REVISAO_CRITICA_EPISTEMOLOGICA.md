# Revisão crítica do EVIDA — Integridade documental, validade semântica e confiabilidade científica

Minha avaliação inicial é que estamos diante de um problema potencialmente mais grave do que uma implementação defeituosa: o EVIDA corre o risco de confundir a integridade de uma evidência documental com a validade científica de uma interpretação produzida a partir dela.

E essa distinção precisa ser estrutural, não apenas uma advertência metodológica na documentação.

A versão declarada é a v0.3.2 — Consistência Probatória.

O README informa atualmente:
- 34 Documentos
- 1.272 Páginas
- 2.354 Eventos
- 5.230 Observações
- 356 Ocorrências
- 326 Episódios

Essas são quantidades declaradas pelo projeto, não contagens que representem demonstração de que os registros correspondem corretamente aos fenômenos descritos nos documentos.

O problema central: uma cadeia de custódia íntegra pode preservar perfeitamente uma interpretação errada. O SHA-256 prova a identidade dos bytes; não prova a correção das inferências.

## 1. Falhas confirmadas no código

A inspeção de `tools/extract_corpus_scientific.py`, `src/core/corpus.cpp`, `src/server/main.cpp` e das migrações SQL revela problemas concretos:

| Gravidade | Achado confirmado | Consequência |
| :--- | :--- | :--- |
| **Crítica** | Eventos gerados pela presença de palavras-chave | Prevenção, histórico e ocorrência real podem ser confundidos |
| **Crítica** | Todos os quatro tipos de efeitos são preenchidos automaticamente com `sim` | O sistema atribui impactos não demonstrados |
| **Crítica** | APIs científicas não exigem validação humana | Dados heurísticos são entregues como registros analíticos |
| **Alta** | Município escolhido pela primeira correspondência textual | Erros de atribuição territorial |
| **Alta** | Data da publicação usada como período de referência | Erros temporais sistemáticos |
| **Alta** | Valores monetários recebem unidade fixa `R$/sc` | Indicadores econômicos potencialmente inválidos |
| **Alta** | Teste de consistência verifica quantidades mínimas, não correção semântica | Uma auditoria pode passar mesmo com interpretações erradas |

### O problema mais sério

No extrator (`tools/extract_corpus_scientific.py`), basta encontrar uma palavra associada a perturbação para gerar um evento com atributos causais automáticos:

```sql
tipo_evento = 'perturbacao'
efeito_ecologico = 'sim'
efeito_produtivo = 'sim'
efeito_sanitario = 'sim'
efeito_economico = 'sim'
```

Isso significa que uma simples menção textual a uma doença produz um registro que afirma efeitos ecológicos, produtivos, sanitários e econômicos, independentemente de terem sido relatados. Não é apenas um falso positivo: é a fabricação algorítmica de atributos causais.

Também se atribui confiança `ALTA` a determinadas medições detectadas por regex, sem demonstrar que resolveu corretamente território, cultura, período, unidade e contexto.

## 2. Problemas na própria arquitetura probatória

1. **A revisão humana existe, mas não controla a publicação científica:** O endpoint de revisão registra decisões em `review_decision`, com `record_type = 'evidence'`. Entretanto, as consultas de eventos, observações e ocorrências não filtram ou verificam essas decisões antes de devolver os registros analíticos.
2. **A auditoria pode aprovar vínculos com registros inexistentes:** A criação de vínculos sem a correspondente inserção do registro analítico não violava a auditoria porque `record_evidence_link` carecia de chave estrangeira rígida para entidades dinâmicas.
3. **A proveniência semântica e a proveniência da extração de PDF estão misturadas:** O extrator associa seus registros a identificadores da extração textual de página, sem identificar rigorosamente qual versão do algoritmo semântico produziu cada interpretação.
4. **Idempotência insuficiente:** O processamento era decidido pela ausência de observações no documento, e não pela combinação unívoca entre documento, versão do extrator e parâmetros.

## 3. A nova definição de evidência: Cinco Entidades Científicas

É imperativo separar explicitamente cinco entidades:

1. **Documento Original:** PDF preservado, identidade criptográfica e custódia em cofre frio.
2. **Enunciado Documental:** O que a fonte efetivamente afirma, onde e em qual contexto literal (`verbatim`).
3. **Interpretação Candidata:** Hipótese de estruturação produzida por um extrator versionado.
4. **Enunciado Estruturado Validado:** Entidades, relações, contexto e limites confrontados com a fonte por curadoria e validação epistêmica.
5. **Produto Analítico:** Séries, mapas, episódios e modelos estatísticos científicos.

A validação do quarto nível demonstra fidelidade documental, não necessariamente a ocorrência física independente do fenômeno. O EVIDA precisa ter condições de dizer: *a Emater relatou uma situação de estiagem naquele território e período*, sem converter isso automaticamente em uma medição instrumental de seca ou em um impacto econômico comprovado.

## 4. O novo algoritmo deve admitir que não sabe

Substituição da lógica simplista:
`palavra encontrada → evento criado`

Por uma cadeia epistêmica rigorosa:
`trecho localizado → contexto identificado → proposição candidata → verificações → validação → disponibilidade analítica`

| Texto documental | Tratamento correto |
| :--- | :--- |
| *"A matéria seca da pastagem aumentou"* | Indicador zootécnico, não evento de seca |
| *"Foram instalados sistemas antigranizo"* | Ação preventiva, não ocorrência de granizo |
| *"Não foram registradas geadas"* | Relato negativo, não evento ocorrido |
| *"As perdas na estiagem de 2025..."* | Referência histórica, com data própria |
| *"Rio Grande do Sul apresentou..."* | Referência estadual, não ao município Rio Grande |
| *"Investimento de R$ 4,17 bilhões"* | Valor orçamentário, não cotação agrícola |

Arquitetura híbrida mandatória: regras determinísticas para validações formais, processamento sensível ao layout para tabelas e modelos de linguagem para interpretação contextual sob esquemas tipados rígidos. Nenhum modelo automatizado tem autoridade para homologar sua própria extração.

## 5. Prioridades de Desenvolvimento

- **P0 — Contenção científica imediata:** Preservar integralmente o acervo documental; classificar registros legados como não validados; impedir seu uso em análises publicadas, mapas e agregações sem qualificação. Separar APIs de candidatos das APIs de dados validados.
- **P1 — Reconstrução da governança e proveniência:** Implementar estados científicos vinculantes, decisões de revisão por registro, versões reais de extratores, posicionamento verificável do trecho na página e reprocessamento idempotente. Toda revisão deve preservar o histórico original.
- **P2 — Novo extrator científico:** Separar resolução territorial, temporalidade, modalidade discursiva, relações entre entidades, medidas e unidades, leitura tabular e identificação de fenômenos. Extratores especializados com capacidade de abstenção.
- **P3 — Validação científica reproduzível:** Construir um conjunto de referência (*gold standard*) anotado por pesquisadores, medindo calibração e capacidade de abstenção em vez de volume bruto extraído.

## 6. Conclusão Fundamental

> **"A rastreabilidade de uma interpretação não constitui prova de sua validade. A capacidade de produzir um registro não autoriza sua promoção a conhecimento científico."**

Preservamos o patrimônio documental, colocamos limites claros à automação e reconstruímos a passagem entre documentos, evidências, interpretações e conhecimento científico.
