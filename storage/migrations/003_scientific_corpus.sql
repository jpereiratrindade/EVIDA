-- EVIDA migration v0.3.0: Corpus científico completo Emater/RS (protocolo original)
-- Licença: GNU General Public License v3.0 (GPLv3)
BEGIN TRANSACTION;

CREATE TABLE IF NOT EXISTS "documentos" (
  "documento_id" TEXT PRIMARY KEY,
  "arquivo" TEXT,
  "numero_informativo" INTEGER,
  "data_publicacao" TEXT,
  "ano" INTEGER,
  "mes" INTEGER,
  "numero_paginas" INTEGER,
  "texto_extraivel" TEXT,
  "caracteres_extraidos" INTEGER,
  "sha256" TEXT,
  "sha256_texto_normalizado" TEXT,
  "problema_detectado" REAL
);
CREATE INDEX IF NOT EXISTS idx_doc_sha256 ON documentos(sha256);
CREATE INDEX IF NOT EXISTS idx_doc_num ON documentos(numero_informativo);

CREATE TABLE IF NOT EXISTS "observacoes" (
  "observacao_id" TEXT PRIMARY KEY,
  "documento_id" TEXT,
  "arquivo_origem" TEXT,
  "numero_informativo" INTEGER,
  "data_publicacao" TEXT,
  "periodo_referencia" TEXT,
  "dimensao" TEXT,
  "categoria" TEXT,
  "atividade" TEXT,
  "cultura_criacao" TEXT,
  "variavel" TEXT,
  "valor" TEXT,
  "unidade" TEXT,
  "qualificador" REAL,
  "regiao_emater" TEXT,
  "municipio" TEXT,
  "pagina" INTEGER,
  "secao" TEXT,
  "subsecao" TEXT,
  "evidencia_textual" TEXT,
  "confianca" TEXT,
  FOREIGN KEY (documento_id) REFERENCES documentos(documento_id)
);
CREATE INDEX IF NOT EXISTS idx_obs_doc ON observacoes(documento_id);
CREATE INDEX IF NOT EXISTS idx_obs_ativ ON observacoes(atividade, variavel);
CREATE INDEX IF NOT EXISTS idx_obs_reg ON observacoes(regiao_emater);

CREATE TABLE IF NOT EXISTS "eventos" (
  "evento_id" TEXT PRIMARY KEY,
  "documento_id" TEXT,
  "arquivo_origem" TEXT,
  "numero_informativo" INTEGER,
  "data_publicacao" TEXT,
  "periodo_referencia" TEXT,
  "regiao_emater" TEXT,
  "municipio" TEXT,
  "atividade" TEXT,
  "tipo_evento" TEXT,
  "classe_evento" TEXT,
  "descricao_evento" TEXT,
  "efeito_ecologico" TEXT,
  "efeito_produtivo" TEXT,
  "efeito_sanitario" TEXT,
  "efeito_economico" TEXT,
  "magnitude_explicita" REAL,
  "dimensao" TEXT,
  "status_continuidade" TEXT,
  "pagina" INTEGER,
  "secao" TEXT,
  "evidencia_textual" TEXT,
  "confianca" TEXT,
  FOREIGN KEY (documento_id) REFERENCES documentos(documento_id)
);
CREATE INDEX IF NOT EXISTS idx_evt_doc ON eventos(documento_id);
CREATE INDEX IF NOT EXISTS idx_evt_chave ON eventos(classe_evento, regiao_emater, atividade, data_publicacao);
CREATE INDEX IF NOT EXISTS idx_evt_classe ON eventos(classe_evento);

CREATE TABLE IF NOT EXISTS "respostas" (
  "resposta_id" TEXT PRIMARY KEY,
  "evento_id" TEXT,
  "documento_id" TEXT,
  "arquivo_origem" TEXT,
  "numero_informativo" INTEGER,
  "data_publicacao" TEXT,
  "regiao_emater" TEXT,
  "municipio" TEXT,
  "atividade" TEXT,
  "ator" TEXT,
  "resposta" TEXT,
  "categoria_resposta" TEXT,
  "objetivo_resposta" TEXT,
  "dimensao" TEXT,
  "pagina" INTEGER,
  "secao" TEXT,
  "evidencia_textual" TEXT,
  "confianca" TEXT,
  FOREIGN KEY (evento_id) REFERENCES eventos(evento_id),
  FOREIGN KEY (documento_id) REFERENCES documentos(documento_id)
);
CREATE INDEX IF NOT EXISTS idx_rsp_evt ON respostas(evento_id);
CREATE INDEX IF NOT EXISTS idx_rsp_doc ON respostas(documento_id);

CREATE TABLE IF NOT EXISTS "precos" (
  "documento_id" TEXT,
  "data_publicacao" TEXT,
  "periodo_referencia" TEXT,
  "produto" TEXT,
  "regiao" TEXT,
  "preco_medio" TEXT,
  "preco_minimo" TEXT,
  "preco_maximo" TEXT,
  "unidade" TEXT,
  "comparacao_semana_anterior" TEXT,
  "comparacao_mes_anterior" TEXT,
  "comparacao_ano_anterior" TEXT,
  "media_historica" TEXT,
  "media_historica_mes" TEXT,
  "tipo_registro" TEXT,
  "pagina" INTEGER,
  "arquivo_origem" TEXT,
  "evidencia_textual" TEXT,
  "confianca" TEXT,
  FOREIGN KEY (documento_id) REFERENCES documentos(documento_id)
);
CREATE INDEX IF NOT EXISTS idx_preco_serie ON precos(produto, regiao, data_publicacao);

CREATE TABLE IF NOT EXISTS "episodios" (
  "episodio_id" TEXT PRIMARY KEY,
  "territorio" TEXT,
  "atividade" TEXT,
  "data_inicio" TEXT,
  "data_fim" TEXT,
  "perturbacao" TEXT,
  "impactos_observados" TEXT,
  "respostas_observadas" TEXT,
  "evidencias_de_recuperacao" TEXT,
  "documentos_relacionados" TEXT,
  "numero_documentos" INTEGER,
  "nivel_confianca" TEXT,
  "nota_metodologica" TEXT
);

CREATE TABLE IF NOT EXISTS "acoes_institucionais" (
  "acao_institucional_id" TEXT PRIMARY KEY,
  "resposta_id" TEXT,
  "documento_id" TEXT,
  "data_publicacao" TEXT,
  "ator" TEXT,
  "acao" TEXT,
  "territorio" TEXT,
  "atividade" TEXT,
  "objetivo" TEXT,
  "pagina" INTEGER,
  "arquivo_origem" TEXT,
  "evidencia_textual" TEXT,
  "confianca" TEXT
);

CREATE TABLE IF NOT EXISTS "relacoes" (
  "relacao_id" TEXT PRIMARY KEY,
  "documento_id" TEXT,
  "evento_id" TEXT,
  "resposta_id" TEXT,
  "tipo_relacao" TEXT,
  "relacao_causal_explicita" TEXT,
  "pagina" INTEGER,
  "arquivo_origem" TEXT,
  "evidencia_textual" TEXT,
  "confianca" TEXT
);

CREATE TABLE IF NOT EXISTS "amostra_validacao" (
  "tabela" TEXT,
  "registro_id" TEXT,
  "documento_id" TEXT,
  "pagina" INTEGER,
  "resultado" TEXT,
  "nota" TEXT
);

CREATE TABLE IF NOT EXISTS "municipios_ibge_rs" (
  "codigo_ibge" TEXT PRIMARY KEY,
  "municipio" TEXT,
  "uf" TEXT,
  "municipio_normalizado" TEXT,
  "fonte_ibge" TEXT,
  "data_obtencao" TEXT
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_mun_ibge_codigo ON municipios_ibge_rs(codigo_ibge);

CREATE TABLE IF NOT EXISTS "ocorrencias_municipais" (
  "ocorrencia_id" TEXT PRIMARY KEY,
  "grupo_ocorrencia_id" TEXT,
  "data_publicacao" TEXT,
  "data_ocorrencia" TEXT,
  "data_inicio_ocorrencia" TEXT,
  "data_fim_ocorrencia" TEXT,
  "tipo_data_ocorrencia" TEXT,
  "codigo_ibge" TEXT,
  "municipio" TEXT,
  "municipio_texto_original" TEXT,
  "municipio_normalizado" TEXT,
  "uf" TEXT,
  "regiao_emater" TEXT,
  "precisao_espacial" TEXT,
  "classe_ocorrencia" TEXT,
  "subclasse_ocorrencia" TEXT,
  "atividade" TEXT,
  "componente" TEXT,
  "valor" TEXT,
  "unidade" TEXT,
  "intensidade_qualitativa" TEXT,
  "ator" TEXT,
  "descricao" TEXT,
  "documento" TEXT,
  "numero_informativo" INTEGER,
  "pagina" INTEGER,
  "secao" TEXT,
  "evidencia_textual" TEXT,
  "confianca_extracao" TEXT,
  FOREIGN KEY (codigo_ibge) REFERENCES municipios_ibge_rs(codigo_ibge)
);
CREATE INDEX IF NOT EXISTS idx_oc_mun_codigo ON ocorrencias_municipais(codigo_ibge);
CREATE INDEX IF NOT EXISTS idx_oc_mun_data_ocorrencia ON ocorrencias_municipais(data_ocorrencia);
CREATE INDEX IF NOT EXISTS idx_oc_mun_data_publicacao ON ocorrencias_municipais(data_publicacao);
CREATE INDEX IF NOT EXISTS idx_oc_mun_classe ON ocorrencias_municipais(classe_ocorrencia);
CREATE INDEX IF NOT EXISTS idx_oc_mun_atividade ON ocorrencias_municipais(atividade);

CREATE TABLE IF NOT EXISTS "ocorrencias_municipais_mensais" (
  "codigo_ibge" TEXT,
  "municipio" TEXT,
  "ano" INTEGER,
  "mes" INTEGER,
  "classe_ocorrencia" TEXT,
  "subclasse_ocorrencia" TEXT,
  "atividade" TEXT,
  "numero_registros_documentais" INTEGER,
  "numero_informativos" INTEGER,
  "primeira_data" TEXT,
  "ultima_data" TEXT
);
CREATE INDEX IF NOT EXISTS idx_oc_mensal_codigo ON ocorrencias_municipais_mensais(codigo_ibge);

CREATE TABLE IF NOT EXISTS "municipio_tempo_classe" (
  "codigo_ibge" TEXT,
  "municipio" TEXT,
  "ano" INTEGER,
  "mes" INTEGER,
  "classe_ocorrencia" TEXT,
  "n_ocorrencias_documentais" INTEGER
);

CREATE TABLE IF NOT EXISTS "municipio_tempo_classe_semanal" (
  "codigo_ibge" TEXT,
  "municipio" TEXT,
  "ano" INTEGER,
  "semana_iso" INTEGER,
  "classe_ocorrencia" TEXT,
  "n_ocorrencias_documentais" INTEGER
);

CREATE TABLE IF NOT EXISTS "municipios_nao_resolvidos" (
  "documento" TEXT,
  "numero_informativo" INTEGER,
  "data_publicacao" TEXT,
  "pagina" INTEGER,
  "regiao_emater" TEXT,
  "atividade" TEXT,
  "municipio_texto_original" TEXT,
  "municipio_normalizado" TEXT,
  "motivo" TEXT,
  "evidencia_textual" TEXT
);

CREATE TABLE IF NOT EXISTS "ocorrencias_nao_municipais" (
  "documento_id" TEXT,
  "documento" TEXT,
  "numero_informativo" INTEGER,
  "data_publicacao" TEXT,
  "pagina" INTEGER,
  "secao" TEXT,
  "atividade" TEXT,
  "regiao_emater" TEXT,
  "precisao_espacial" TEXT,
  "classe_ocorrencia" TEXT,
  "subclasse_ocorrencia" TEXT,
  "evidencia_textual" TEXT
);

CREATE TABLE IF NOT EXISTS "amostra_validacao_municipal" (
  "codigo_ibge" TEXT,
  "municipio" TEXT,
  "municipio_texto_original" TEXT,
  "data" TEXT,
  "classe" TEXT,
  "subclasse" TEXT,
  "atividade" TEXT,
  "documento" TEXT,
  "pagina" INTEGER,
  "evidencia_textual" TEXT,
  "fonte_relocalizada" INTEGER,
  "associacao_explicita_validada" INTEGER,
  "resultado_validacao" TEXT
);

DROP VIEW IF EXISTS v_evento_resposta;
CREATE VIEW v_evento_resposta AS
  SELECT e.*, r.resposta_id, r.ator, r.categoria_resposta, r.resposta
  FROM eventos e LEFT JOIN respostas r ON e.evento_id = r.evento_id;

PRAGMA user_version = 3;
COMMIT;
