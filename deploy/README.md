# Implantação no labeco-srv

Compile na estação de desenvolvimento; o servidor recebe o pacote pronto.

```bash
ops/build-release.sh dist 0.3.3-coleta --worktree
scp dist/evida-0.3.3-coleta.tar.gz dist/evida-0.3.3-coleta.tar.gz.sha256 ops/install-release.sh jpereiratrindade@10.163.80.129:~/
ssh -t jpereiratrindade@10.163.80.129 "sudo ./install-release.sh ./evida-0.3.3-coleta.tar.gz"
```

Use identificador novo para cada pacote. `--worktree` inclui os fontes atuais,
inclusive novos arquivos nas pastas do projeto, sem exigir commit. Sem essa opção,
o empacotamento exige alterações rastreadas commitadas e usa `git archive HEAD`.
A compilação e os testes ocorrem no toolbox fedora-44-sister quando disponível.

O instalador preserva `/var/lib/evida/var/evida.sqlite`, cria backup com a API online
do SQLite, aplica migrações ao iniciar o servidor, reprocessa o acervo com a versão
nova do extrator, audita vínculos e instala a coleta diária. O banco do pacote só é
usado quando não existe banco de produção. O rollback automático do executável
não restaura o banco; backups ficam ao lado do banco para recuperação deliberada.

```bash
systemctl status evida.service evida-sync.timer
systemctl list-timers evida-sync.timer
journalctl -u evida-sync.service
```

O timer executa às 18h30 America/Sao_Paulo, com até 5 minutos adicionais. Esse é um
horário operacional de consulta, sem pressupor horário de publicação da Emater.
O serviço consulta novamente no dia seguinte se houver falhas ou publicação tardia.
O ano corrente é calculado na execução. Para coletar anos anteriores:

```bash
sudo python3 /usr/local/share/evida/current/tools/sync_emater_repository.py --year 2025 --db /var/lib/evida/var/evida.sqlite --storage /var/lib/evida/storage/documents
```

Para reprocessar sem download, inclusive documentos com zero resultados:

```bash
sudo python3 /usr/local/share/evida/current/tools/extract_corpus_scientific.py --db /var/lib/evida/var/evida.sqlite --year 2025 --reprocess
```

Registros substituídos e decisões humanas permanecem no banco. Contagens ativas
podem diminuir: o extrator conservador não reproduz todas as variáveis do legado.
O histórico de coleta começa nesta versão; não é fabricado para cargas antigas.
A interface oferece instruções de importação, sem iniciar tarefas administrativas.

Páginas legadas sem checksum recebem hash calculado do texto já armazenado, com
registro explícito em `page_checksum_history`. Isso não equivale a revalidar a
extração contra o PDF. Checksums existentes divergentes causam falha da auditoria.
