#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 EVIDA Contributors
"""
EVIDA — Extrator Científico de Observações e Eventos (Emater 2026).
Extrai medições quantitativas de campo, dados fenológicos, produtivos e eventos
climáticos/fitossanitários a partir das páginas textuais registradas.
"""

import argparse
import uuid
from pipeline_state import connect

VERSION = "0.4.0"

import hashlib
import re
import sqlite3
import sys
from pathlib import Path

CROPS = [
    "Trigo", "Milho", "Soja", "Arroz", "Aveia", "Cevada", "Canola",
    "Feijão", "Citros", "Ovinocultura", "Bovinocultura de Corte",
    "Bovinocultura de Leite", "Piscicultura", "Hortaliças", "Geral"
]

PHENOLOGY_MAP = {
    "floração": "lavoura_em_floração",
    "germinação": "lavoura_em_germinação",
    "emergência": "lavoura_em_emergência",
    "desenvolvimento": "lavoura_em_desenvolvimento",
    "enchimento": "lavoura_em_enchimento_de_graos",
    "maturação": "lavoura_em_maturação",
    "colheita": "lavoura_colhida",
    "colhido": "lavoura_colhida",
    "colhidos": "lavoura_colhida",
    "semeado": "semeadura",
    "semeada": "semeadura",
    "plantio": "semeadura",
    "plantado": "semeadura",
}

def parse_phenology_table(table_text):
    """Extrai rigorosamente linhas de tabelas de fases de safras da Emater,
    garantindo que o percentual da safra atual seja vinculado à fase da MESMA linha."""
    results = []
    lines = table_text.strip().split("\n")
    for line in lines:
        line_clean = line.strip()
        # Procura percentuais na linha
        pcts = re.findall(r'(\d+[\.,]?\d*)\s*%', line_clean)
        if not pcts:
            continue
        if any(float(v.replace(',', '.')) > 100 for v in pcts):
            continue
        # Verifica se alguma fase fenológica está presente nesta linha específica
        for key, mapped in PHENOLOGY_MAP.items():
            if re.match(r'^(?:Em\s+)?' + re.escape(key) + r'\b', line_clean, re.IGNORECASE):
                # O primeiro percentual da linha é o valor da data da safra atual
                val = pcts[0]
                results.append((line_clean, mapped, val))
                break
    return results

EVENT_CLASSES = {
    "seca": "seca", "estiagem": "seca", "déficit hídrico": "seca",
    "chuva excessiva": "chuva_excessiva", "chuvas excessivas": "chuva_excessiva",
    "enchente": "enchente", "alagamento": "alagamento", "inundação": "enchente",
    "granizo": "granizo", "geada": "geada", "vento forte": "vento_forte",
    "vendaval": "vendaval", "ciclone": "vendaval",
    "cigarrinha": "praga", "lagarta": "praga", "pulgão": "praga", "percevejo": "praga",
    "bicudo": "praga", "broca": "praga", "mosca": "praga",
    "ferrugem": "doenca", "mancha": "doenca", "mofo": "doenca", "antracnose": "doenca",
    "oídio": "doenca", "bacteriose": "doenca", "virose": "doenca"
}

def load_municipalities(con: sqlite3.Connection):
    cur = con.cursor()
    muns = {}
    for code, name in cur.execute("SELECT codigo_ibge, municipio FROM municipios_ibge_rs;"):
        muns[name.lower()] = (code, name, re.compile(r"\b" + re.escape(name) + r"\b", re.IGNORECASE))
    return muns

def run_extraction(db_path: Path, reprocess=False, year=None):
    import fcntl
    with open(str(db_path) + '.extract.lock', 'a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('Outra extração está em execução')
        con = connect(db_path)
        try:
            return _run_extraction(con, reprocess, year)
        finally:
            con.close()


def _run_extraction(con, reprocess, year):
    cur = con.cursor()

    municipalities = load_municipalities(con)

    missing_docs = cur.execute("""
        SELECT documento_id, data_publicacao, numero_informativo, arquivo
        FROM documentos d WHERE (? IS NULL OR substr(data_publicacao,1,4)=?)
        AND (? OR NOT EXISTS (SELECT 1 FROM scientific_processing p
          WHERE p.document_id=d.documento_id AND p.source_sha256=d.sha256
          AND p.extractor_version=?)) ORDER BY data_publicacao
    """, (year, str(year) if year else None, reprocess, VERSION)).fetchall()
    if not missing_docs:
        con.close()
        return 0

    obs_seq = cur.execute("SELECT COALESCE(max(CAST(substr(observacao_id,5) AS INTEGER)),0) FROM observacoes WHERE observacao_id GLOB 'OBS-[0-9]*'").fetchone()[0]
    evt_seq = cur.execute("SELECT COALESCE(max(CAST(substr(evento_id,5) AS INTEGER)),0) FROM eventos WHERE evento_id GLOB 'EVT-[0-9]*'").fetchone()[0]

    cur.execute("BEGIN TRANSACTION;")

    total_obs = 0
    total_evts = 0

    for doc_id, data_pub, num_inf, arq in missing_docs:
        run_id = "science-" + uuid.uuid4().hex
        before_obs, before_evts = total_obs, total_evts
        sha = cur.execute("SELECT sha256 FROM documentos WHERE documento_id=?", (doc_id,)).fetchone()[0]
        cur.execute("INSERT INTO extraction_run(run_id,document_id,extractor_name,extractor_version,parameters_json) VALUES(?,?, 'evida-scientific',?, '{}')", (run_id,doc_id,VERSION))
        for table, kind, key in [('observacoes','observacao','observacao_id'),('eventos','evento','evento_id')]:
            cur.execute(f"INSERT OR IGNORE INTO record_status SELECT ?, {key}, 'superseded', ? FROM {table} WHERE documento_id=?", (kind,run_id,doc_id))
        print(f"\n[extractor] Processando {doc_id} (nº {num_inf}) - {data_pub}...")
        pages = cur.execute("""
            SELECT page_number, text_content
            FROM document_page_provenance
            WHERE document_id = ?
            ORDER BY page_number;
        """, (doc_id,)).fetchall()

        if not pages or not any(text.strip() for _, text in pages):
            raise RuntimeError(f"Documento sem texto extraível: {doc_id}")
        for page_num, text in pages:
            lines = text.split("\n")
            page_head = text[:350].lower()

            # Identifica a seção da página pelos cabeçalhos locais
            if any(k in page_head for k in ["pastagens", "criações", "forrageiras", "bovinocultura", "ovinocultura", "bovinos", "pecuária"]):
                page_section = "Criações & Pastagens"
                default_crop = "Pastagens e Forragens"
            elif any(k in page_head for k in ["hortigranjeiros", "olericultura", "fruticultura", "hortaliças", "citros"]):
                page_section = "Hortigranjeiros & Frutas"
                default_crop = "Hortigranjeiros"
            elif "meteorológicas" in page_head or page_num in (3, 4):
                page_section = "Condições Meteorológicas"
                default_crop = "Geral"
            elif any(k in page_head for k in ["culturas de inverno", "culturas de verão", "grãos"]):
                page_section = "Grãos"
                default_crop = "Geral"
            else:
                page_section = "Geral"
                default_crop = "Geral"

            section = page_section

            # Divide parágrafos
            paragraphs = [p.strip() for p in text.split("\n\n") if len(p.strip()) > 30]

            for para in paragraphs:
                para_clean = " ".join(para.split())

                # Determina a atividade específica NO PRÓPRIO PARÁGRAFO
                para_crop = None
                if re.search(r'\b(pastagens|pastagem|azevém|braquiária|braquiárias|forrageira|forrageiras|feno|silagem)\b', para_clean, re.IGNORECASE):
                    para_crop = "Pastagens e Forragens"
                elif re.search(r'\b(leite|gado leiteiro|bovinocultura de leite)\b', para_clean, re.IGNORECASE):
                    para_crop = "Bovinocultura de Leite"
                elif re.search(r'\b(corte|bovinos de corte|bovinocultura de corte)\b', para_clean, re.IGNORECASE):
                    para_crop = "Bovinocultura de Corte"
                elif re.search(r'\b(ovinos|ovinocultura)\b', para_clean, re.IGNORECASE):
                    para_crop = "Ovinocultura"
                else:
                    for crop in CROPS:
                        if re.search(r'\b' + re.escape(crop) + r'\b', para_clean, re.IGNORECASE):
                            para_crop = crop
                            break

                # Se não achou no parágrafo, verifica o cabeçalho DESTA PÁGINA ESPECÍFICA (sem vazar entre páginas)
                if not para_crop:
                    for crop in CROPS:
                        if re.search(r'\b' + re.escape(crop) + r'\b', text[:250], re.IGNORECASE):
                            para_crop = crop
                            break

                if not para_crop:
                    para_crop = default_crop

                # Identifica se algum município é citado no parágrafo
                para_mun = ""
                para_cod_ibge = ""
                for mun_name, (code, orig_name, pattern) in municipalities.items():
                    if pattern.search(para_clean):
                        para_mun = orig_name
                        para_cod_ibge = code
                        break

                # 1. Busca observações de precipitação (X mm)
                mm_matches = re.finditer(r'(?:variaram entre\s*(\d+[\.,]?\d*)\s*e\s*(\d+[\.,]?\d*)|acumulado(?:s)?(?:\s*de|\s*semanal)?\s*(?:de\s*)?(\d+[\.,]?\d*)|(?:registro|registrado|marca(?:ram)?|volume(?:s)?(?:\s*de)?)\s*(\d+[\.,]?\d*))\s*mm', para_clean, re.IGNORECASE)
                for m in mm_matches:
                    val = (m.group(1) + '–' + m.group(2)) if m.group(1) and m.group(2) else (m.group(3) or m.group(4))
                    if not val: continue
                    obs_seq += 1
                    obs_id = f"OBS-{obs_seq:06d}"
                    span_id = f"span-{obs_id}"
                    link_id = f"link-{obs_id}"
                    text_sha = hashlib.sha256(para_clean.encode("utf-8")).hexdigest()

                    cur.execute("""
                        INSERT INTO observacoes (
                            observacao_id, documento_id, arquivo_origem, numero_informativo,
                            data_publicacao, periodo_referencia, dimensao, categoria, atividade,
                            cultura_criacao, variavel, valor, unidade, regiao_emater, municipio,
                            pagina, secao, evidencia_textual, confianca
                        ) VALUES (?, ?, ?, ?, ?, ?, 'P', 'quantitativa_explicita', ?, ?, 'precipitação', ?, 'mm', '', ?, ?, ?, ?, 'CANDIDATO_HEURISTICO');
                    """, (obs_id, doc_id, arq, num_inf, data_pub, data_pub, para_crop, para_crop, val, para_mun, page_num, section, para_clean))

                    cur.execute("""
                        INSERT OR REPLACE INTO evidence_span (
                            evidence_id, extraction_run_id, document_id, page_number,
                            verbatim_text, text_sha256, section_name, confidence
                        ) VALUES (?, ?, ?, ?, ?, ?, ?, 'CANDIDATO_HEURISTICO');
                    """, (span_id, run_id, doc_id, page_num, para_clean, text_sha, section))

                    cur.execute("""
                        INSERT OR REPLACE INTO record_evidence_link (
                            link_id, record_type, record_id, evidence_id, link_type, notes
                        ) VALUES (?, 'observacao', ?, ?, 'supports', 'Observação quantitativa de precipitação extraída');
                    """, (link_id, obs_id, span_id))

                    total_obs += 1

                # 2. Busca observações de fenologia / percentuais (X %)
                is_table = ("fases da cultura" in para.lower() or "safra atual" in para.lower() or "safra anterior" in para.lower()
                            or len(re.findall(r'\d+[,.]?\d*\s*%', para)) >= 2 and bool(parse_phenology_table(para)))
                if is_table:
                    table_rows = parse_phenology_table(para)
                    for line_raw, var_name, val in table_rows:
                        obs_seq += 1
                        obs_id = f"OBS-{obs_seq:06d}"
                        span_id = f"span-{obs_id}"
                        link_id = f"link-{obs_id}"
                        text_sha = hashlib.sha256(line_raw.encode("utf-8")).hexdigest()

                        cur.execute("""
                            INSERT INTO observacoes (
                                observacao_id, documento_id, arquivo_origem, numero_informativo,
                                data_publicacao, periodo_referencia, dimensao, categoria, atividade,
                                cultura_criacao, variavel, valor, unidade, regiao_emater, municipio,
                                pagina, secao, evidencia_textual, confianca
                            ) VALUES (?, ?, ?, ?, ?, ?, 'P', 'extracao_tabular_heuristica', ?, ?, ?, ?, '%', '', ?, ?, ?, ?, 'CANDIDATO_HEURISTICO');
                        """, (obs_id, doc_id, arq, num_inf, data_pub, data_pub, para_crop, para_crop, var_name, val, para_mun, page_num, section, line_raw))

                        cur.execute("""
                            INSERT OR REPLACE INTO evidence_span (
                                evidence_id, extraction_run_id, document_id, page_number,
                                verbatim_text, text_sha256, section_name, confidence
                            ) VALUES (?, ?, ?, ?, ?, ?, ?, 'CANDIDATO_HEURISTICO');
                        """, (span_id, run_id, doc_id, page_num, line_raw, text_sha, section))

                        cur.execute("""
                            INSERT OR REPLACE INTO record_evidence_link (
                                link_id, record_type, record_id, evidence_id, link_type, notes
                            ) VALUES (?, 'observacao', ?, ?, 'supports', 'Estádio fenológico extraído de tabela oficial da Emater');
                        """, (link_id, obs_id, span_id))

                        total_obs += 1
                    continue

                pct_matches = re.finditer(r'(\d+[\.,]?\d*)\s*%\s*(?:das\s*lavouras|da\s*área|das\s*áreas)?\s*(?:estão|foram|em|de)?\s*(floração|germinação|emergência|desenvolvimento|enchimento(?:\s*de\s*grãos)?|maturação|colhid[ao]s?|semead[ao]s?|plantad[ao]s?)', para_clean, re.IGNORECASE)
                for m in pct_matches:
                    val = m.group(1)
                    if float(val.replace(',', '.')) > 100:
                        continue
                    phase_raw = m.group(2).lower()
                    var_name = "proporção"
                    for key, mapped in PHENOLOGY_MAP.items():
                        if key in phase_raw:
                            var_name = mapped
                            break

                    obs_seq += 1
                    obs_id = f"OBS-{obs_seq:06d}"
                    span_id = f"span-{obs_id}"
                    link_id = f"link-{obs_id}"
                    text_sha = hashlib.sha256(para_clean.encode("utf-8")).hexdigest()

                    cur.execute("""
                        INSERT INTO observacoes (
                            observacao_id, documento_id, arquivo_origem, numero_informativo,
                            data_publicacao, periodo_referencia, dimensao, categoria, atividade,
                            cultura_criacao, variavel, valor, unidade, regiao_emater, municipio,
                            pagina, secao, evidencia_textual, confianca
                        ) VALUES (?, ?, ?, ?, ?, ?, 'P', 'extracao_narrativa_heuristica', ?, ?, ?, ?, '%', '', ?, ?, ?, ?, 'CANDIDATO_HEURISTICO');
                    """, (obs_id, doc_id, arq, num_inf, data_pub, data_pub, para_crop, para_crop, var_name, val, para_mun, page_num, section, para_clean))

                    cur.execute("""
                        INSERT OR REPLACE INTO evidence_span (
                            evidence_id, extraction_run_id, document_id, page_number,
                            verbatim_text, text_sha256, section_name, confidence
                        ) VALUES (?, ?, ?, ?, ?, ?, ?, 'CANDIDATO_HEURISTICO');
                    """, (span_id, run_id, doc_id, page_num, para_clean, text_sha, section))

                    cur.execute("""
                        INSERT OR REPLACE INTO record_evidence_link (
                            link_id, record_type, record_id, evidence_id, link_type, notes
                        ) VALUES (?, 'observacao', ?, ?, 'supports', 'Estádio fenológico / percentual de safra em texto narrativo');
                    """, (link_id, obs_id, span_id))

                    total_obs += 1

                # 3. Busca produtividade (X kg/ha ou sc/ha)
                prod_matches = re.finditer(r'(?:produtividade(?:\s*média)?(?:\s*de|\s*estimada\s*em)?|rendimento(?:\s*médio)?)\s*(?:de\s*)?(\d+(?:\.\d{3})*(?:,\d+)?|\d+\.\d+)\s*(kg/ha|sc/ha|t/ha|sacas/ha)', para_clean, re.IGNORECASE)
                for m in prod_matches:
                    val = m.group(1)
                    unid = m.group(2)
                    obs_seq += 1
                    obs_id = f"OBS-{obs_seq:06d}"
                    span_id = f"span-{obs_id}"
                    link_id = f"link-{obs_id}"
                    text_sha = hashlib.sha256(para_clean.encode("utf-8")).hexdigest()

                    cur.execute("""
                        INSERT INTO observacoes (
                            observacao_id, documento_id, arquivo_origem, numero_informativo,
                            data_publicacao, periodo_referencia, dimensao, categoria, atividade,
                            cultura_criacao, variavel, valor, unidade, regiao_emater, municipio,
                            pagina, secao, evidencia_textual, confianca
                        ) VALUES (?, ?, ?, ?, ?, ?, 'P', 'quantitativa_explicita', ?, ?, 'produtividade', ?, ?, '', ?, ?, ?, ?, 'CANDIDATO_HEURISTICO');
                    """, (obs_id, doc_id, arq, num_inf, data_pub, data_pub, para_crop, para_crop, val, unid, para_mun, page_num, section, para_clean))

                    cur.execute("""
                        INSERT OR REPLACE INTO evidence_span (
                            evidence_id, extraction_run_id, document_id, page_number,
                            verbatim_text, text_sha256, section_name, confidence
                        ) VALUES (?, ?, ?, ?, ?, ?, ?, 'CANDIDATO_HEURISTICO');
                    """, (span_id, run_id, doc_id, page_num, para_clean, text_sha, section))

                    cur.execute("""
                        INSERT OR REPLACE INTO record_evidence_link (
                            link_id, record_type, record_id, evidence_id, link_type, notes
                        ) VALUES (?, 'observacao', ?, ?, 'supports', 'Produtividade de cultura');
                    """, (link_id, obs_id, span_id))

                    total_obs += 1

                # 4. Busca preços (R$ X,XX)
                price_matches = re.finditer(r'(?:preço(?:\s*médio)?(?:\s*pago\s*ao\s*produtor)?|cotado\s*em|valor\s*de)\s*(?:R\$\s*)?(\d+[\.,]\d{2})', para_clean, re.IGNORECASE)
                for m in price_matches:
                    val = m.group(1)
                    obs_seq += 1
                    obs_id = f"OBS-{obs_seq:06d}"
                    span_id = f"span-{obs_id}"
                    link_id = f"link-{obs_id}"
                    text_sha = hashlib.sha256(para_clean.encode("utf-8")).hexdigest()

                    cur.execute("""
                        INSERT INTO observacoes (
                            observacao_id, documento_id, arquivo_origem, numero_informativo,
                            data_publicacao, periodo_referencia, dimensao, categoria, atividade,
                            cultura_criacao, variavel, valor, unidade, regiao_emater, municipio,
                            pagina, secao, evidencia_textual, confianca
                        ) VALUES (?, ?, ?, ?, ?, ?, 'S', 'quantitativa_explicita', ?, ?, 'preço', ?, 'R$ (base não identificada)', '', ?, ?, ?, ?, 'CANDIDATO_HEURISTICO');
                    """, (obs_id, doc_id, arq, num_inf, data_pub, data_pub, para_crop, para_crop, val, para_mun, page_num, section, para_clean))

                    cur.execute("""
                        INSERT OR REPLACE INTO evidence_span (
                            evidence_id, extraction_run_id, document_id, page_number,
                            verbatim_text, text_sha256, section_name, confidence
                        ) VALUES (?, ?, ?, ?, ?, ?, ?, 'CANDIDATO_HEURISTICO');
                    """, (span_id, run_id, doc_id, page_num, para_clean, text_sha, section))

                    cur.execute("""
                        INSERT OR REPLACE INTO record_evidence_link (
                            link_id, record_type, record_id, evidence_id, link_type, notes
                        ) VALUES (?, 'observacao', ?, ?, 'supports', 'Preço pago ao produtor');
                    """, (link_id, obs_id, span_id))

                    total_obs += 1

                # 5. Busca Eventos Perturbações (Seca, Chuva Excessiva, Praga, Doença, Granizo, Geada)
                for pattern, ev_class in EVENT_CLASSES.items():
                    if re.search(r'\b' + re.escape(pattern) + r'\b', para_clean, re.IGNORECASE):
                        evt_seq += 1
                        evt_id = f"EVT-{evt_seq:06d}"
                        span_id = f"span-{evt_id}"
                        link_id = f"link-{evt_id}"
                        text_sha = hashlib.sha256(para_clean.encode("utf-8")).hexdigest()

                        cur.execute("""
                            INSERT INTO eventos (
                                evento_id, documento_id, arquivo_origem, numero_informativo,
                                data_publicacao, periodo_referencia, regiao_emater, municipio,
                                atividade, tipo_evento, classe_evento, descricao_evento,
                                efeito_ecologico, efeito_produtivo, efeito_sanitario, efeito_economico,
                                dimensao, pagina, secao, evidencia_textual, confianca
                            ) VALUES (?, ?, ?, ?, ?, ?, '', ?, ?, 'perturbacao', ?, ?, 'não determinado', 'não determinado', 'não determinado', 'não determinado', 'E', ?, ?, ?, 'CANDIDATO_HEURISTICO');
                        """, (evt_id, doc_id, arq, num_inf, data_pub, data_pub, para_mun, para_crop, ev_class, para_clean, page_num, section, para_clean))

                        cur.execute("""
                            INSERT OR REPLACE INTO evidence_span (
                                evidence_id, extraction_run_id, document_id, page_number,
                                verbatim_text, text_sha256, section_name, confidence
                            ) VALUES (?, ?, ?, ?, ?, ?, ?, 'CANDIDATO_HEURISTICO');
                        """, (span_id, run_id, doc_id, page_num, para_clean, text_sha, section))

                        cur.execute("""
                            INSERT OR REPLACE INTO record_evidence_link (
                                link_id, record_type, record_id, evidence_id, link_type, notes
                            ) VALUES (?, 'evento', ?, ?, 'supports', 'Evento documental extraído do Informativo Emater/RS');
                        """, (link_id, evt_id, span_id))

                        total_evts += 1
                        break

        cur.execute("INSERT INTO scientific_processing(run_id,document_id,source_sha256,extractor_version,observation_count,event_count) VALUES(?,?,?,?,?,?)", (run_id,doc_id,sha,VERSION,total_obs-before_obs,total_evts-before_evts))
        cur.execute("UPDATE extraction_run SET page_count=?,record_count=? WHERE run_id=?", (len(pages),total_obs-before_obs+total_evts-before_evts,run_id))
    con.commit()
    print(f"\n[extractor] Extração finalizada com sucesso!")
    print(f"  -> Novas Observações inseridas: {total_obs}")
    print(f"  -> Novos Eventos inseridos: {total_evts}")
    return 0

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('--db', type=Path, default=Path('var/evida.sqlite'))
    parser.add_argument('--year', type=int)
    parser.add_argument('--reprocess', action='store_true')
    args = parser.parse_args()
    sys.exit(run_extraction(args.db, args.reprocess, args.year))
