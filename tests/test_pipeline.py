import hashlib
import io
import json
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import extract_corpus_scientific as extractor
import sync_emater_repository as sync
from pipeline_state import snapshot, connect


class PipelineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.db = Path(self.tmp.name)/'test.sqlite'
        self.con = sqlite3.connect(self.db)
        self.addCleanup(self.con.close)
        for migration in sorted((ROOT/'storage/migrations').glob('*.sql')):
            self.con.executescript(migration.read_text())
        sha='a'*64
        self.con.execute("INSERT INTO document_blob(sha256,byte_size,storage_path) VALUES(?,1,'x')",(sha,))
        self.con.execute("INSERT INTO document_acquisition(document_id,sha256,source_title) VALUES('DOC',?,'test')",(sha,))
        self.con.execute("INSERT INTO documentos(documento_id,sha256,data_publicacao,numero_informativo,arquivo) VALUES('DOC',?,'2026-11-05',1,'test.pdf')",(sha,))
        self.con.execute("INSERT INTO extraction_run(run_id,document_id,extractor_name,extractor_version,parameters_json) VALUES('pdf','DOC','test','1','{}')")
        self.con.commit()

    def page(self,text):
        self.con.execute("INSERT OR REPLACE INTO document_page_provenance(page_id,extraction_run_id,document_id,page_number,text_content,text_sha256) VALUES('p','pdf','DOC',5,?,?)",(text,hashlib.sha256(text.encode()).hexdigest()))
        self.con.commit()

    def test_table_alignment(self):
        text='Fases da cultura\nSafra atual Safra anterior\nPlantio 100% 90%\nGerminação/Des. Vegetativo 0% 5%\nFloração 0% 1%\nColhido 53% 34%'
        rows=extractor.parse_phenology_table(text)
        self.assertIn(('Plantio 100% 90%','semeadura','100'),rows)
        self.assertIn(('Germinação/Des. Vegetativo 0% 5%','lavoura_em_germinação','0'),rows)
        self.assertEqual(extractor.parse_phenology_table(' '.join(text.split())),[])

    def test_zero_results_are_completed(self):
        self.page('Texto documental sem informações quantitativas reconhecíveis.')
        extractor.run_extraction(self.db)
        extractor.run_extraction(self.db)
        self.assertEqual(self.con.execute('SELECT count(*) FROM scientific_processing').fetchone()[0],1)

    def test_reprocessing_preserves_history(self):
        self.page('Trigo: produtividade média de 2.833 kg/ha. O volume acumulado de 20 mm foi informado.')
        extractor.run_extraction(self.db)
        old=self.con.execute('SELECT observacao_id FROM observacoes').fetchall()
        self.assertTrue(old)
        self.assertIn(('2.833',),self.con.execute("SELECT valor FROM observacoes WHERE variavel='produtividade'").fetchall())
        extractor.run_extraction(self.db)
        self.assertEqual(self.con.execute('SELECT count(*) FROM observacoes').fetchone()[0],len(old))
        extractor.run_extraction(self.db,reprocess=True)
        self.assertEqual(self.con.execute("SELECT count(*) FROM record_status WHERE status='superseded'").fetchone()[0],len(old))
        self.assertEqual(self.con.execute('PRAGMA foreign_key_check').fetchall(),[])
        self.assertEqual(self.con.execute("SELECT count(*) FROM observacoes WHERE confianca='ALTA'").fetchone()[0],0)

    def test_missing_text_rolls_back(self):
        with self.assertRaises(RuntimeError):
            extractor.run_extraction(self.db)
        self.assertEqual(self.con.execute('SELECT count(*) FROM scientific_processing').fetchone()[0],0)

    def test_month_failure_is_recorded(self):
        report=[]
        with patch.object(sync.urllib.request,'urlopen',side_effect=OSError('offline')):
            self.assertEqual(sync.discover_emater_documents(2024,report),{})
        self.assertEqual(len(report),12)
        self.assertTrue(all(r[1]=='falha' for r in report))

    def test_acquisition_revision_and_repeat(self):
        self.page('Documento de controle sem dados quantitativos extraíveis.')
        item=dict(url='https://example.test/edition.pdf',date_iso='2026-11-05',issue_number=2,title='Teste',day='05',month='11',year='2026')
        vault=Path(self.tmp.name)/'documents'
        vault.mkdir()
        def ingest(payload, text):
            with patch.object(sync.urllib.request,'urlopen',return_value=io.BytesIO(payload)), patch.object(sync,'extract_pdf_pages_text',return_value=[text]):
                return sync.ingest_document(self.con,vault,item)
        self.assertTrue(ingest(b'%PDF-1.4 original','Produtividade média de 100 kg/ha. Informação documental original.'))
        extractor.run_extraction(self.db)
        ids=self.con.execute("SELECT observacao_id FROM observacoes WHERE documento_id='DOC-2026-11-05'").fetchall()
        self.assertTrue(ids)
        self.assertFalse(ingest(b'%PDF-1.4 original','Não deve ser reextraído.'))
        self.assertTrue(ingest(b'%PDF-1.4 revisado','Produtividade média de 200 kg/ha. Informação documental revisada.'))
        self.assertEqual(self.con.execute('SELECT count(*) FROM document_revision').fetchone()[0],1)
        self.assertEqual(len(list(vault.glob('*.pdf'))),2)
        self.assertEqual(self.con.execute('PRAGMA foreign_key_check').fetchall(),[])
        extractor.run_extraction(self.db)
        values=self.con.execute("SELECT valor FROM observacoes o WHERE documento_id='DOC-2026-11-05' AND NOT EXISTS(SELECT 1 FROM record_status s WHERE s.record_id=o.observacao_id AND s.status='superseded')").fetchall()
        self.assertEqual(values,[('200',)])

    def test_legacy_checksum_backfill_is_documented(self):
        self.page('Texto legado cujo checksum ainda não foi calculado.')
        self.con.execute("UPDATE document_page_provenance SET text_sha256=''")
        self.con.commit()
        con=connect(self.db)
        self.assertEqual(con.execute('SELECT previous_sha256,method FROM page_checksum_history').fetchone(),('', 'sha256_of_existing_stored_text'))
        self.assertEqual(len(con.execute('SELECT text_sha256 FROM document_page_provenance').fetchone()[0]),64)
        con.close()

    def test_snapshot_preserves_pages_and_hash(self):
        self.page('Texto original preservado no histórico documental.')
        snapshot(self.con,'DOC','a'*64)
        saved=json.loads(self.con.execute('SELECT snapshot_json FROM document_revision').fetchone()[0])
        self.assertEqual(saved['document_page_provenance'][0]['text_content'],'Texto original preservado no histórico documental.')


if __name__=='__main__':
    unittest.main()
