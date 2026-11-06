#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 EVIDA Contributors
"""
EVIDA — Extrator Científico de Observações e Eventos (Emater 2026).
Extrai medições quantitativas de campo, dados fenológicos, produtivos e eventos
climáticos/fitossanitários a partir das páginas textuais registradas.
"""

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
        muns[name.lower()] = (code, name)
    return muns

def run_extraction(db_path: Path):
    con = sqlite3.connect(str(db_path))
    cur = con.cursor()

    municipalities = load_municipalities(con)

    # Identifica documentos sem observações
    missing_docs = cur.execute("""
        SELECT documento_id, data_publicacao, numero_informativo, arquivo
        FROM documentos
        WHERE documento_id NOT IN (SELECT DISTINCT documento_id FROM observacoes)
        ORDER BY data_publicacao;
    """).fetchall()

    if not missing_docs:
        print("[extractor] Todos os documentos já possuem observações extraídas.")
        return 0

    print(f"[extractor] Detectados {len(missing_docs)} documentos para extração de observações e eventos.")

    # Obter os últimos contadores
    last_obs = cur.execute("SELECT max(observacao_id) FROM observacoes;").fetchone()[0] or "OBS-000000"
    obs_seq = int(re.search(r'\d+', last_obs).group(0))

    last_evt = cur.execute("SELECT max(evento_id) FROM eventos;").fetchone()[0] or "EVT-000000"
    evt_seq = int(re.search(r'\d+', last_evt).group(0))

    cur.execute("BEGIN TRANSACTION;")

    total_obs = 0
    total_evts = 0

    for doc_id, data_pub, num_inf, arq in missing_docs:
        print(f"\n[extractor] Processando {doc_id} (nº {num_inf}) - {data_pub}...")
        pages = cur.execute("""
            SELECT page_number, text_content
            FROM document_page_provenance
            WHERE document_id = ?
            ORDER BY page_number;
        """, (doc_id,)).fetchall()

        current_crop = "Geral"

        for page_num, text in pages:
            lines = text.split("\n")
            
            # Atualiza cultura pelo cabeçalho
            for crop in CROPS:
                if re.search(r'\b' + re.escape(crop) + r'\b', text[:300], re.IGNORECASE):
                    current_crop = crop
                    break

            # Seção
            section = "Condições Meteorológicas" if page_num in (3, 4) else ("Grãos" if page_num < 15 else "Geral")

            # Divide parágrafos
            paragraphs = [p.strip() for p in text.split("\n\n") if len(p.strip()) > 30]

            for para in paragraphs:
                para_clean = " ".join(para.split())

                # Identifica se algum município é citado no parágrafo
                para_mun = ""
                para_cod_ibge = ""
                for mun_name, (code, orig_name) in municipalities.items():
                    if re.search(r'\b' + re.escape(mun_name) + r'\b', para_clean, re.IGNORECASE):
                        para_mun = orig_name
                        para_cod_ibge = code
                        break

                # 1. Busca observações de precipitação (X mm)
                mm_matches = re.finditer(r'(?:variaram entre\s*(\d+[\.,]?\d*)\s*e\s*(\d+[\.,]?\d*)|acumulado(?:s)?(?:\s*de|\s*semanal)?\s*(?:de\s*)?(\d+[\.,]?\d*)|(?:registro|registrado|marca(?:ram)?|volume(?:s)?(?:\s*de)?)\s*(\d+[\.,]?\d*))\s*mm', para_clean, re.IGNORECASE)
                for m in mm_matches:
                    val = m.group(2) or m.group(3) or m.group(4) or m.group(1)
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
                        ) VALUES (?, ?, ?, ?, ?, ?, 'P', 'quantitativa_explicita', ?, ?, 'precipitação', ?, 'mm', '', ?, ?, ?, ?, 'ALTA');
                    """, (obs_id, doc_id, arq, num_inf, data_pub, data_pub, current_crop, current_crop, val, para_mun, page_num, section, para_clean))

                    cur.execute("""
                        INSERT OR REPLACE INTO evidence_span (
                            evidence_id, extraction_run_id, document_id, page_number,
                            verbatim_text, text_sha256, section_name, confidence
                        ) VALUES (?, ?, ?, ?, ?, ?, ?, 'ALTA');
                    """, (span_id, f"run-{doc_id}", doc_id, page_num, para_clean, text_sha, section))

                    cur.execute("""
                        INSERT OR REPLACE INTO record_evidence_link (
                            link_id, record_type, record_id, evidence_id, link_type, notes
                        ) VALUES (?, 'observacao', ?, ?, 'supports', 'Observação quantitativa de precipitação extraída');
                    """, (link_id, obs_id, span_id))

                    total_obs += 1

                # 2. Busca observações de fenologia / percentuais (X %)
                pct_matches = re.finditer(r'(\d+[\.,]?\d*)\s*%\s*(?:das\s*lavouras|da\s*área|das\s*áreas)?\s*(?:estão|foram|em|de)?\s*(floração|germinação|emergência|desenvolvimento|enchimento(?:\s*de\s*grãos)?|maturação|colhid[ao]s?|semead[ao]s?|plantad[ao]s?)', para_clean, re.IGNORECASE)
                for m in pct_matches:
                    val = m.group(1)
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
                        ) VALUES (?, ?, ?, ?, ?, ?, 'P', 'quantitativa_explicita', ?, ?, ?, ?, '%', '', ?, ?, ?, ?, 'ALTA');
                    """, (obs_id, doc_id, arq, num_inf, data_pub, data_pub, current_crop, current_crop, var_name, val, para_mun, page_num, section, para_clean))

                    cur.execute("""
                        INSERT OR REPLACE INTO evidence_span (
                            evidence_id, extraction_run_id, document_id, page_number,
                            verbatim_text, text_sha256, section_name, confidence
                        ) VALUES (?, ?, ?, ?, ?, ?, ?, 'ALTA');
                    """, (span_id, f"run-{doc_id}", doc_id, page_num, para_clean, text_sha, section))

                    cur.execute("""
                        INSERT OR REPLACE INTO record_evidence_link (
                            link_id, record_type, record_id, evidence_id, link_type, notes
                        ) VALUES (?, 'observacao', ?, ?, 'supports', 'Estádio fenológico / percentual de safra');
                    """, (link_id, obs_id, span_id))

                    total_obs += 1

                # 3. Busca produtividade (X kg/ha ou sc/ha)
                prod_matches = re.finditer(r'(?:produtividade(?:\s*média)?(?:\s*de|\s*estimada\s*em)?|rendimento(?:\s*médio)?)\s*(?:de\s*)?(\d+[\.,]?\d*)\s*(kg/ha|sc/ha|t/ha|toneladas|sacas/ha)', para_clean, re.IGNORECASE)
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
                        ) VALUES (?, ?, ?, ?, ?, ?, 'P', 'quantitativa_explicita', ?, ?, 'produtividade', ?, ?, '', ?, ?, ?, ?, 'ALTA');
                    """, (obs_id, doc_id, arq, num_inf, data_pub, data_pub, current_crop, current_crop, val, unid, para_mun, page_num, section, para_clean))

                    cur.execute("""
                        INSERT OR REPLACE INTO evidence_span (
                            evidence_id, extraction_run_id, document_id, page_number,
                            verbatim_text, text_sha256, section_name, confidence
                        ) VALUES (?, ?, ?, ?, ?, ?, ?, 'ALTA');
                    """, (span_id, f"run-{doc_id}", doc_id, page_num, para_clean, text_sha, section))

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
                        ) VALUES (?, ?, ?, ?, ?, ?, 'S', 'quantitativa_explicita', ?, ?, 'preço', ?, 'R$/sc', '', ?, ?, ?, ?, 'ALTA');
                    """, (obs_id, doc_id, arq, num_inf, data_pub, data_pub, current_crop, current_crop, val, para_mun, page_num, section, para_clean))

                    cur.execute("""
                        INSERT OR REPLACE INTO evidence_span (
                            evidence_id, extraction_run_id, document_id, page_number,
                            verbatim_text, text_sha256, section_name, confidence
                        ) VALUES (?, ?, ?, ?, ?, ?, ?, 'ALTA');
                    """, (span_id, f"run-{doc_id}", doc_id, page_num, para_clean, text_sha, section))

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
                            ) VALUES (?, ?, ?, ?, ?, ?, '', ?, ?, 'perturbacao', ?, ?, 'sim', 'sim', 'sim', 'sim', 'E', ?, ?, ?, 'MÉDIA');
                        """, (evt_id, doc_id, arq, num_inf, data_pub, data_pub, para_mun, current_crop, ev_class, para_clean, page_num, section, para_clean))

                        cur.execute("""
                            INSERT OR REPLACE INTO evidence_span (
                                evidence_id, extraction_run_id, document_id, page_number,
                                verbatim_text, text_sha256, section_name, confidence
                            ) VALUES (?, ?, ?, ?, ?, ?, ?, 'ALTA');
                        """, (span_id, f"run-{doc_id}", doc_id, page_num, para_clean, text_sha, section))

                        cur.execute("""
                            INSERT OR REPLACE INTO record_evidence_link (
                                link_id, record_type, record_id, evidence_id, link_type, notes
                            ) VALUES (?, 'evento', ?, ?, 'supports', 'Evento documental extraído do Informativo Emater/RS');
                        """, (link_id, evt_id, span_id))

                        total_evts += 1
                        break

    con.commit()
    print(f"\n[extractor] Extração finalizada com sucesso!")
    print(f"  -> Novas Observações inseridas: {total_obs}")
    print(f"  -> Novos Eventos inseridos: {total_evts}")
    return 0

if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    db = root / "var/evida.sqlite"
    sys.exit(run_extraction(db))
