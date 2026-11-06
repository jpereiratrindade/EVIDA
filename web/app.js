// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 EVIDA Contributors
// EVIDA — Sistema Científico de Evidências Documentais e Análise
// Padrão de Identidade Visual e Ingestão MORFO / SisTer
// Licenciado sob GNU General Public License v3.0 (GPLv3).
'use strict';

const $ = id => document.getElementById(id);
const h = v => String(v ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const number = new Intl.NumberFormat('pt-BR');

let currentUser = {email: 'pesquisador@labeco.local', name: 'Pesquisador SisTer', role: 'researcher'};
let selectedFiles = [];
let currentMun = null;

const viewNames = {
  visao: 'Visão geral',
  eventos: 'Eventos & Riscos',
  observacoes: 'Observações & Métricas',
  territorios: 'Territórios IBGE (RS)',
  episodios: 'Episódios & Séries',
  custodia: 'Cofre & Custódia',
  sobre: 'Sobre & Método',
  usuarios: 'Gestão de Usuários & Acesso'
};

function toast(message) {
  const el = $('toast');
  if (!el) return;
  el.textContent = message;
  el.classList.add('visible');
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => el.classList.remove('visible'), 4000);
}

function nav(view) {
  for (const name of Object.keys(viewNames)) {
    const el = $('view-' + name);
    if (el) el.classList.toggle('hidden', view !== name);
  }
  for (const b of document.querySelectorAll('[data-view]')) {
    const active = b.dataset.view === view;
    b.classList.toggle('active', active);
    if (active) b.setAttribute('aria-current', 'page');
    else b.removeAttribute('aria-current');
  }
  $('page-name').textContent = viewNames[view] || view;
  $('sidebar').classList.remove('open');
  $('mobile-scrim').classList.remove('shown');
  $('menu').setAttribute('aria-expanded', 'false');
  location.hash = view;
  window.scrollTo(0, 0);

  if (view === 'visao') loadStats();
  if (view === 'eventos') loadEvents();
  if (view === 'observacoes') loadObservations();
  if (view === 'territorios') loadMunicipalities();
  if (view === 'episodios') loadEpisodes();
  if (view === 'custodia') loadVaultDocuments();
  if (view === 'usuarios') loadUsers();
}

async function api(path, options = {}) {
  const r = await fetch(path, options);
  let data = await r.json().catch(() => ({error: 'Resposta inválida do servidor'}));
  if (!r.ok) {
    const err = new Error(data.message || data.error || `Erro HTTP ${r.status}`);
    err.status = r.status;
    err.data = data;
    throw err;
  }
  return data;
}

function getInitials(name, email) {
  if (name && name !== email) {
    const parts = name.trim().split(/\s+/);
    if (parts.length >= 2) return (parts[0][0] + parts[parts.length - 1][0]).toUpperCase();
    if (parts.length === 1 && parts[0].length >= 2) return parts[0].slice(0, 2).toUpperCase();
  }
  const clean = (email || '').split('@')[0].replace(/[^a-zA-Z]/g, '');
  return clean.slice(0, 2).toUpperCase() || 'PS';
}

function updateUserUI() {
  const chip = $('user-chip');
  const navUsuarios = $('nav-btn-usuarios');
  const opEmail = $('operator-email');
  if (currentUser) {
    chip.classList.remove('hidden');
    $('user-avatar').textContent = getInitials(currentUser.name, currentUser.email);
    $('user-email').textContent = currentUser.email;
    $('user-email').title = currentUser.name ? `${currentUser.name} (${currentUser.email})` : currentUser.email;
    const roleLabel = currentUser.role === 'admin' ? 'Administrador' : currentUser.role === 'researcher' ? 'Pesquisador' : 'Usuário';
    const roleEl = document.querySelector('.user-role');
    if (roleEl) roleEl.textContent = roleLabel;
    if (opEmail) opEmail.textContent = currentUser.email;
    if (navUsuarios) navUsuarios.classList.toggle('hidden', currentUser.role !== 'admin');
  } else {
    chip.classList.add('hidden');
    if (opEmail) opEmail.textContent = 'Acesso local';
    if (navUsuarios) navUsuarios.classList.add('hidden');
  }
}

// ----------------- AUTENTICAÇÃO PADRÃO MORFO -----------------
async function checkAuth() {
  try {
    const data = await api('/api/v1/auth/me');
    if (data.authenticated) {
      currentUser = data.user;
      updateUserUI();
      hideAuthOverlay();
    } else {
      currentUser = null;
      updateUserUI();
    }
  } catch (err) {
    console.warn('Sessão não verificada:', err);
  }
}

function showAuthOverlay() {
  $('auth-overlay').classList.remove('hidden');
  $('auth-email-input').focus();
}

function hideAuthOverlay() {
  $('auth-overlay').classList.add('hidden');
}

// ----------------- SELETOR MULTI-ANO E SÍNTESE TEMÁTICA -----------------
let currentYear = '2026';
let activeObsTheme = null;
let activeEventsClass = null;

async function loadYearsList() {
  const sel = $('global-year-select');
  if (!sel) return;
  try {
    const res = await api('/api/v1/years');
    const active = res.active_years || [];
    let html = '';
    for (const y of active) {
      const isSel = String(y.ano) === String(currentYear) ? 'selected' : '';
      html += `<option value="${y.ano}" ${isSel}>${y.ano} (${y.documentos} informativos)</option>`;
    }
    html += `<option value="todos" ${currentYear === '' ? 'selected' : ''}>Todos os Anos</option>`;
    html += `<option value="import_emater">+ Ingerir 2024 / 2025 da Emater...</option>`;
    sel.innerHTML = html;
  } catch (err) {
    console.warn('Falha ao listar anos:', err);
  }
}

function onYearChange() {
  const sel = $('global-year-select');
  if (!sel) return;
  const val = sel.value;
  if (val === 'import_emater') {
    promptImportYear();
    sel.value = currentYear || 'todos';
    return;
  }
  currentYear = val === 'todos' ? '' : val;
  const stateEl = $('service-state');
  if (stateEl) {
    stateEl.textContent = currentYear ? `Corpus ${currentYear} Ativo` : 'Corpus Multi-ano Ativo';
  }
  loadStats();
  const activeView = location.hash.replace('#', '') || 'visao';
  if (activeView === 'eventos') loadEvents();
  if (activeView === 'observacoes') loadObservations();
  if (activeView === 'custodia') loadVaultDocuments();
}

function promptImportYear() {
  const yr = prompt('Digite o ano que deseja sincronizar da Emater/RS (ex: 2024 ou 2025):', '2024');
  if (!yr) return;
  const yearInt = parseInt(yr.trim(), 10);
  if (isNaN(yearInt) || yearInt < 2015 || yearInt > 2026) {
    alert('Por favor informe um ano válido entre 2015 e 2026.');
    return;
  }
  toast(`Iniciando checagem de acervo da Emater para ${yearInt}…`);
  alert(`Para ingestão em lote do acervo histórico de ${yearInt} mantendo a custódia epistêmica estrita:\n\nExecute no terminal:\npython3 tools/sync_emater_repository.py --year ${yearInt} --extract\n\nIsso baixará as edições oficiais de ${yearInt}, calculará os hashes de custódia e indexará todas as evidências com vínculo criptográfico.`);
}

function filterEventsClass(cls) {
  activeEventsClass = cls;
  const select = $('events-class-filter');
  if (select) select.value = cls;
  
  document.querySelectorAll('#events-synthesis-cards .synthesis-card').forEach(card => {
    card.classList.toggle('active', card.getAttribute('onclick')?.includes(cls));
  });

  const bar = $('events-active-filter-bar');
  const text = $('events-active-filter-text');
  if (bar && text) {
    bar.classList.remove('hidden');
    const labelMap = {
      estiagem: '☀️ Seca & Estiagem',
      excesso_hidrico: '🌊 Chuvas & Enchentes',
      praga: '🐛 Pragas Fitossanitárias',
      doenca: '🍂 Fitopatologias',
      geada: '❄️ Geada & Granizo'
    };
    text.innerHTML = `Exibindo recorte temático: <strong>${labelMap[cls] || cls}</strong>`;
  }
  loadEvents();
}

function clearEventsFilter() {
  activeEventsClass = null;
  const select = $('events-class-filter');
  if (select) select.value = '';
  document.querySelectorAll('#events-synthesis-cards .synthesis-card').forEach(card => card.classList.remove('active'));
  const bar = $('events-active-filter-bar');
  if (bar) bar.classList.add('hidden');
  loadEvents();
}

function filterObsTheme(theme) {
  activeObsTheme = theme;
  const varFilter = $('obs-variable-filter');
  const actFilter = $('obs-activity-filter');
  
  document.querySelectorAll('#obs-synthesis-cards .synthesis-card').forEach(card => {
    card.classList.toggle('active', card.getAttribute('onclick')?.includes(theme));
  });

  const bar = $('obs-active-filter-bar');
  const text = $('obs-active-filter-text');
  if (bar && text) {
    bar.classList.remove('hidden');
    const labelMap = {
      clima: '🌧️ Clima & Chuvas (Precipitação em mm)',
      culturas: '🌾 Culturas & Safras (Trigo, Soja, Milho)',
      produtividade: '⚖️ Produtividade de Campo (kg/ha)',
      precos: '💰 Cotações Médias & Mercado Agropecuário'
    };
    text.innerHTML = `Exibindo recorte temático: <strong>${labelMap[theme] || theme}</strong>`;
  }

  if (theme === 'clima') {
    if (varFilter) varFilter.value = 'precipitação';
    if (actFilter) actFilter.value = '';
  } else if (theme === 'produtividade') {
    if (varFilter) varFilter.value = 'produtividade';
    if (actFilter) actFilter.value = '';
  } else if (theme === 'precos') {
    if (varFilter) varFilter.value = 'preço';
    if (actFilter) actFilter.value = '';
  } else if (theme === 'culturas') {
    if (varFilter) varFilter.value = '';
    if (actFilter) actFilter.value = 'trigo';
  }
  loadObservations();
}

function clearObsFilter() {
  activeObsTheme = null;
  const varFilter = $('obs-variable-filter');
  const actFilter = $('obs-activity-filter');
  if (varFilter) varFilter.value = '';
  if (actFilter) actFilter.value = '';
  document.querySelectorAll('#obs-synthesis-cards .synthesis-card').forEach(card => card.classList.remove('active'));
  const bar = $('obs-active-filter-bar');
  if (bar) bar.classList.add('hidden');
  loadObservations();
}

// ----------------- CARREGAMENTO DE DADOS -----------------

async function loadStats() {
  try {
    let url = '/api/v1/stats';
    if (currentYear) url += `?ano=${encodeURIComponent(currentYear)}`;
    const s = await api(url);
    if ($('stat-events')) $('stat-events').textContent = number.format(s.events || 0);
    if ($('stat-observations')) $('stat-observations').textContent = number.format(s.observations || 0);
    if ($('stat-municipalities')) $('stat-municipalities').textContent = number.format(s.municipalities || 0);
    if ($('stat-documents')) $('stat-documents').textContent = number.format(s.documents || 0);
  } catch (err) {
    console.error('Falha ao obter stats:', err);
  }
}

// ----------------- EVENTOS & RISCOS -----------------
let allEvents = [];

async function loadEvents() {
  const tbody = $('events-table-body');
  tbody.innerHTML = '<tr><td colspan="7" style="text-align:center;padding:24px;color:var(--muted)">Carregando eventos científicos…</td></tr>';
  
  const classe = $('events-class-filter').value;
  const regiao = $('events-region-filter').value;
  let url = `/api/v1/events?limit=150`;
  if (classe) url += `&classe=${encodeURIComponent(classe)}`;
  if (regiao) url += `&regiao=${encodeURIComponent(regiao)}`;
  if (currentYear) url += `&ano=${encodeURIComponent(currentYear)}`;

  try {
    const res = await api(url);
    allEvents = res.events || [];
    renderEvents();
  } catch (err) {
    tbody.innerHTML = `<tr><td colspan="7" style="text-align:center;padding:20px;color:#c2410c">Erro ao carregar eventos: ${h(err.message)}</td></tr>`;
  }
}

function renderEvents() {
  const tbody = $('events-table-body');
  const search = ($('events-search').value || '').toLowerCase().trim();
  
  const filtered = allEvents.filter(e => {
    if (!search) return true;
    return (e.descricao_evento || '').toLowerCase().includes(search) ||
           (e.atividade || '').toLowerCase().includes(search) ||
           (e.municipio || '').toLowerCase().includes(search) ||
           (e.regiao_emater || '').toLowerCase().includes(search) ||
           (e.classe_evento || '').toLowerCase().includes(search);
  });

  if (filtered.length === 0) {
    tbody.innerHTML = '<tr><td colspan="7" style="text-align:center;padding:24px;color:var(--muted)">Nenhum evento localizado com os filtros atuais.</td></tr>';
    return;
  }

  tbody.innerHTML = filtered.map(e => {
    const badgeClass = (e.classe_evento || 'padrao').toLowerCase().replace(/\s+/g, '_');
    const local = [e.municipio, e.regiao_emater].filter(Boolean).join(' / ') || 'Rio Grande do Sul';
    return `
      <tr>
        <td class="nowrap">
          <strong style="font-size:0.78rem;color:var(--navy);">${h(e.evento_id)}</strong><br>
          <small style="color:var(--muted)">${h(e.data_publicacao)}</small>
        </td>
        <td class="nowrap"><span class="badge-class ${badgeClass}">${h(e.classe_evento || 'Geral')}</span></td>
        <td class="nowrap"><strong>${h(local)}</strong></td>
        <td class="nowrap">${h(e.atividade || 'Agrícola Geral')}</td>
        <td class="col-evidence">
          <div style="display:-webkit-box;-webkit-line-clamp:3;-webkit-box-orient:vertical;overflow:hidden;" title="${h(e.evidencia_textual)}">
            ${h(e.evidencia_textual || e.descricao_evento)}
          </div>
        </td>
        <td class="nowrap" style="text-align:center;"><span class="page-badge">p. ${h(e.pagina)}</span></td>
        <td class="col-action">
          <button type="button" class="btn-subtle" onclick="openAuditProof('${h(e.evento_id)}')">
            📜 Prova Documental
          </button>
        </td>
      </tr>
    `;
  }).join('');
}

// ----------------- OBSERVAÇÕES & MÉTRICAS -----------------
let allObs = [];

async function loadObservations() {
  const tbody = $('obs-table-body');
  tbody.innerHTML = '<tr><td colspan="8" style="text-align:center;padding:24px;color:var(--muted)">Carregando observações de campo…</td></tr>';

  const variavel = $('obs-variable-filter').value;
  const atividade = $('obs-activity-filter').value;
  let url = `/api/v1/observations?limit=150`;
  if (variavel) url += `&variavel=${encodeURIComponent(variavel)}`;
  if (atividade) url += `&atividade=${encodeURIComponent(atividade)}`;
  if (currentYear) url += `&ano=${encodeURIComponent(currentYear)}`;

  try {
    const res = await api(url);
    allObs = res.observations || [];
    renderObservations();
  } catch (err) {
    tbody.innerHTML = `<tr><td colspan="8" style="text-align:center;padding:20px;color:#c2410c">Erro ao carregar observações: ${h(err.message)}</td></tr>`;
  }
}

function renderObservations() {
  const tbody = $('obs-table-body');
  const search = ($('obs-search').value || '').toLowerCase().trim();

  const filtered = allObs.filter(o => {
    if (!search) return true;
    return (o.variavel || '').toLowerCase().includes(search) ||
           (o.atividade || '').toLowerCase().includes(search) ||
           (o.valor || '').toLowerCase().includes(search) ||
           (o.regiao_emater || '').toLowerCase().includes(search) ||
           (o.evidencia_textual || '').toLowerCase().includes(search);
  });

  if (filtered.length === 0) {
    tbody.innerHTML = '<tr><td colspan="8" style="text-align:center;padding:24px;color:var(--muted)">Nenhuma observação localizada com os filtros atuais.</td></tr>';
    return;
  }

  tbody.innerHTML = filtered.map(o => {
    const metricStr = [o.valor, o.unidade].filter(Boolean).join(' ') || '—';
    const regiaoStr = [o.municipio, o.regiao_emater].filter(Boolean).join(' / ') || 'RS';
    return `
      <tr>
        <td class="nowrap">
          <strong style="font-size:0.78rem;color:var(--navy);">${h(o.observacao_id)}</strong><br>
          <small style="color:var(--muted)">${h(o.data_publicacao)}</small>
        </td>
        <td class="nowrap"><strong>${h(o.atividade || 'Geral')}</strong></td>
        <td class="nowrap">${h((o.variavel || '—').replace(/_/g, ' '))}</td>
        <td class="col-metric">${h(metricStr)}</td>
        <td class="nowrap">${h(regiaoStr)}</td>
        <td class="col-evidence">
          <div style="display:-webkit-box;-webkit-line-clamp:3;-webkit-box-orient:vertical;overflow:hidden;" title="${h(o.evidencia_textual)}">
            ${h(o.evidencia_textual)}
          </div>
        </td>
        <td class="nowrap" style="text-align:center;"><span class="page-badge">p. ${h(o.pagina)}</span></td>
        <td class="col-action">
          <button type="button" class="btn-subtle" onclick="openAuditProof('${h(o.observacao_id)}')">
            📜 Prova Documental
          </button>
        </td>
      </tr>
    `;
  }).join('');
}

// ----------------- TERRITÓRIOS IBGE (RS) -----------------
let allMuns = [];

async function loadMunicipalities() {
  const container = $('mun-list');
  container.innerHTML = '<p style="color:var(--muted);padding:14px;">Carregando municípios do RS…</p>';
  try {
    const res = await api('/api/v1/municipalities?limit=500');
    allMuns = res.municipalities || [];
    renderMunicipalities();
    if (allMuns.length > 0 && !currentMun) {
      selectMunicipality(allMuns[0].codigo_ibge, allMuns[0].municipio);
    }
  } catch (err) {
    container.innerHTML = `<p style="color:#c2410c;padding:14px;">Erro ao carregar municípios: ${h(err.message)}</p>`;
  }
}

function renderMunicipalities() {
  const container = $('mun-list');
  const search = ($('mun-search').value || '').toLowerCase().trim();

  const filtered = allMuns.filter(m => {
    if (!search) return true;
    return m.municipio.toLowerCase().includes(search) || m.codigo_ibge.includes(search);
  });

  if (filtered.length === 0) {
    container.innerHTML = '<p style="color:var(--muted);padding:14px;">Nenhum município encontrado.</p>';
    return;
  }

  container.innerHTML = filtered.map(m => {
    const activeClass = (currentMun && currentMun.codigo === m.codigo_ibge) ? 'active' : '';
    const occBadge = m.occurrences_count > 0 
      ? `<span class="mun-count-badge">${m.occurrences_count} ocorrências</span>`
      : `<span style="font-size:0.7rem;color:var(--muted)">0 ocorrências</span>`;
    return `
      <div class="mun-item ${activeClass}" onclick="selectMunicipality('${h(m.codigo_ibge)}', '${h(m.municipio)}')">
        <div>
          <strong>${h(m.municipio)}</strong>
          <small>IBGE: ${h(m.codigo_ibge)} · ${h(m.uf)}</small>
        </div>
        ${occBadge}
      </div>
    `;
  }).join('');
}

async function selectMunicipality(codigo, nome) {
  currentMun = {codigo, nome};
  $('mun-detail-title').textContent = `${nome} (${codigo})`;
  $('mun-detail-kicker').textContent = 'MUNICÍPIO IBGE ' + codigo;
  const listEl = $('mun-occurrences-list');
  listEl.innerHTML = '<p style="color:var(--muted);padding:24px;text-align:center;">Carregando ocorrências documentais…</p>';

  // Atualiza classe ativa na lista
  renderMunicipalities();

  try {
    const res = await api(`/api/v1/occurrences?codigo_ibge=${codigo}&limit=50`);
    const ocs = res.occurrences || [];
    if (ocs.length === 0) {
      listEl.innerHTML = `
        <div style="padding:28px 16px;text-align:center;background:#f8fafc;border-radius:8px;border:1px dashed var(--line);">
          <strong style="color:var(--navy);display:block;margin-bottom:6px;">Nenhuma ocorrência documentada explícita em 2026</strong>
          <p style="color:var(--muted);font-size:0.84rem;margin:0;">
            O município de ${h(nome)} pertence à base dos 497 municípios do RS, mas não teve menção nominal isolada nos informativos de 2026 analisados até agora.
          </p>
        </div>
      `;
      return;
    }

    listEl.innerHTML = `
      <div style="display:grid;gap:12px;">
        ${ocs.map(o => {
          const badgeClass = (o.classe_ocorrencia || 'padrao').toLowerCase();
          return `
            <div style="border:1px solid var(--line);border-radius:8px;padding:14px;background:#fff;">
              <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:8px;">
                <div style="display:flex;gap:8px;align-items:center;">
                  <span class="badge-class ${badgeClass}">${h(o.classe_ocorrencia)}</span>
                  ${o.subclasse_ocorrencia ? `<small style="color:var(--muted);font-weight:700;">${h(o.subclasse_ocorrencia)}</small>` : ''}
                </div>
                <small style="color:var(--muted);font-weight:600;">${h(o.data_publicacao)} · p. ${h(o.pagina)}</small>
              </div>
              <p style="margin:6px 0 10px;font-size:0.88rem;line-height:1.5;color:var(--ink);border-left:3px solid var(--sky);padding-left:10px;">
                ${h(o.evidencia_textual)}
              </p>
              <div style="display:flex;justify-content:space-between;align-items:center;">
                <small style="color:var(--muted)">Documento: <strong>${h(o.documento)}</strong></small>
                <button type="button" class="btn-subtle" onclick="openAuditProof('${h(o.ocorrencia_id)}')">
                  📜 Prova Documental
                </button>
              </div>
            </div>
          `;
        }).join('')}
      </div>
    `;
  } catch (err) {
    listEl.innerHTML = `<p style="color:#c2410c;padding:20px;">Erro ao carregar ocorrências: ${h(err.message)}</p>`;
  }
}

// ----------------- EPISÓDIOS & TRAJETÓRIAS -----------------
async function loadEpisodes() {
  const tbody = $('episodes-table-body');
  tbody.innerHTML = '<tr><td colspan="8" style="text-align:center;padding:24px;color:var(--muted)">Carregando episódios temporais…</td></tr>';

  try {
    const res = await api('/api/v1/episodes?limit=100');
    const eps = res.episodes || [];
    if (eps.length === 0) {
      tbody.innerHTML = '<tr><td colspan="8" style="text-align:center;padding:24px;color:var(--muted)">Nenhum episódio cadastrado.</td></tr>';
      return;
    }

    tbody.innerHTML = eps.map(e => `
      <tr>
        <td><strong style="color:var(--navy);font-size:0.8rem;">${h(e.episodio_id)}</strong></td>
        <td><strong>${h(e.territorio || 'Geral')}</strong></td>
        <td>${h(e.atividade || 'Geral')}</td>
        <td style="white-space:nowrap;font-size:0.78rem;">${h(e.data_inicio)} → ${h(e.data_fim)}</td>
        <td><span class="badge-class ${e.perturbacao ? e.perturbacao.toLowerCase() : 'padrao'}">${h(e.perturbacao || 'Perturbação')}</span></td>
        <td style="max-width:380px;font-size:0.82rem;line-height:1.4;">
          ${e.impactos_observados ? `<div><strong>Impacto:</strong> ${h(e.impactos_observados)}</div>` : ''}
          ${e.respostas_observadas ? `<div style="color:#15803d;margin-top:2px;"><strong>Resposta:</strong> ${h(e.respostas_observadas)}</div>` : ''}
        </td>
        <td style="text-align:center;"><strong>${e.numero_documentos}</strong></td>
        <td><span class="quality">${h(e.nivel_confianca || 'MÉDIA')}</span></td>
      </tr>
    `).join('');
  } catch (err) {
    tbody.innerHTML = `<tr><td colspan="8" style="text-align:center;padding:20px;color:#c2410c">Erro ao carregar episódios: ${h(err.message)}</td></tr>`;
  }
}

// ----------------- COFRE & CUSTÓDIA CRIPTOGRÁFICA -----------------
async function loadVaultDocuments() {
  const tbody = $('vault-table-body');
  tbody.innerHTML = '<tr><td colspan="7" style="text-align:center;padding:24px;color:var(--muted)">Carregando documentos do cofre…</td></tr>';

  try {
    const res = await api('/api/v1/vault/documents');
    const docs = res.vault_documents || [];
    if (docs.length === 0) {
      tbody.innerHTML = '<tr><td colspan="7" style="text-align:center;padding:24px;color:var(--muted)">Nenhum documento sob custódia no cofre.</td></tr>';
      return;
    }

    tbody.innerHTML = docs.map(d => {
      const statusHtml = d.in_vault
        ? '<span class="custody-tag">✓ Preservado no cofre</span>'
        : '<span style="background:#fee2e2;color:#991b1b;padding:3px 8px;border-radius:12px;font-size:0.72rem;font-weight:800;">Ausente no cofre</span>';
      return `
        <tr>
          <td class="nowrap"><strong style="color:var(--navy);font-size:0.78rem;">${h(d.documento_id)}</strong></td>
          <td>
            <strong>Informativo Conjuntural nº ${h(d.numero_informativo)}</strong><br>
            <small style="color:var(--muted)">${h(d.arquivo)}</small>
          </td>
          <td class="nowrap">${h(d.data_publicacao)}</td>
          <td class="nowrap"><strong>${d.numero_paginas}</strong> págs</td>
          <td class="nowrap" style="font-size:0.78rem;color:var(--muted);">${number.format(d.caracteres_extraidos)}</td>
          <td>
            <code class="hash-code" style="max-width:280px;display:inline-block;" title="${h(d.sha256)}">${h(d.sha256)}</code>
          </td>
          <td class="nowrap">${statusHtml}</td>
        </tr>
      `;
    }).join('');
  } catch (err) {
    tbody.innerHTML = `<tr><td colspan="7" style="text-align:center;padding:20px;color:#c2410c">Erro ao carregar documentos do cofre: ${h(err.message)}</td></tr>`;
  }
}

// ----------------- AUDIT DRAWER (PROVA DOCUMENTAL SOB DEMANDA) -----------------
let currentAuditEntityId = null;

async function openAuditProof(entityId) {
  currentAuditEntityId = entityId;
  const overlay = $('audit-drawer-overlay');
  overlay.classList.remove('hidden');

  $('audit-drawer-title').textContent = `Prova Documental: ${entityId}`;
  $('audit-entity-badge').textContent = entityId.split('-')[0] || 'REGISTRO';
  $('audit-doc-name').textContent = 'Carregando…';
  $('audit-doc-date').textContent = 'Carregando…';
  $('audit-page-loc').textContent = 'Página —';
  $('audit-sha256').textContent = '—';
  $('audit-excerpt-text').textContent = 'Carregando evidência literal citada na fonte…';
  $('audit-page-fulltext').textContent = 'Carregando contexto integral da página…';
  $('audit-blob-size').textContent = '—';
  $('audit-extraction-run').textContent = '—';
  $('audit-multiple-links-box').classList.add('hidden');
  $('audit-links-list').innerHTML = '';

  try {
    const res = await api(`/api/v1/evidence/${encodeURIComponent(entityId)}/audit`);
    const p = res.proof;
    if (!p) throw new Error('Dados de auditoria não localizados');

    $('audit-doc-name').textContent = `${p.arquivo || p.documento_id} (Informativo nº ${p.numero_informativo || '—'})`;
    $('audit-doc-date').textContent = `Publicado em ${p.data_publicacao || '—'}`;
    $('audit-page-loc').textContent = `Página ${p.pagina || '—'}`;
    $('audit-page-num-tag').textContent = `Pág. ${p.pagina || '—'}`;
    $('audit-sha256').textContent = p.sha256 || '—';
    $('audit-sha256').title = p.sha256;
    $('audit-excerpt-text').textContent = p.evidencia_textual || '—';

    // Status da conferência dinâmica de bytes
    const b = p.blob_integrity || {};
    const custodyEl = $('audit-custody-status');
    if (b.status === 'verified_intact') {
      custodyEl.className = 'custody-tag';
      custodyEl.textContent = '✓ Bytes Verificados Íntegros no Disco';
    } else if (b.status === 'corrupted_tampered') {
      custodyEl.className = 'badge-class seca';
      custodyEl.textContent = '✕ ADULTERADO / CORROMPIDO (Hash difere)';
    } else {
      custodyEl.className = 'badge-class seca';
      custodyEl.textContent = '✕ Arquivo Ausente no Disco';
    }

    if (b.byte_size > 0) {
      $('audit-blob-size').textContent = `${(b.byte_size / 1024 / 1024).toFixed(2)} MB (${number.format(b.byte_size)} bytes)`;
    } else {
      $('audit-blob-size').textContent = 'Não aferido';
    }

    if (p.extractor_name) {
      $('audit-extraction-run').textContent = `${p.extractor_name} v${p.extractor_version} (${p.extraction_run_id || 'run-base'})`;
    } else {
      $('audit-extraction-run').textContent = 'evida-pdf-extractor v0.3.1 (poppler-utils)';
    }

    // Vínculos múltiplos de evidências (M:N)
    if (p.links && p.links.length > 0) {
      $('audit-multiple-links-box').classList.remove('hidden');
      $('audit-links-list').innerHTML = p.links.map(l => {
        const linkColor = l.link_type === 'supports' ? '#0369a1' : l.link_type === 'revises' ? '#15803d' : '#b45309';
        const typeLabel = l.link_type === 'supports' ? 'Sustenta (confirmação)' : l.link_type === 'revises' ? 'Revisa (retificação)' : 'Contesta';
        return `
          <div style="background:#fff;border:1px solid #bae6fd;border-radius:6px;padding:8px 12px;font-size:0.78rem;">
            <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:3px;">
              <strong style="color:${linkColor};font-size:0.75rem;">Vínculo: ${typeLabel}</strong>
              <small style="color:var(--muted)">Pág. ${l.page_number} (${l.document_id})</small>
            </div>
            <p style="margin:2px 0 0;color:var(--ink);font-style:italic;">"${h(l.verbatim_text)}"</p>
            ${l.notes ? `<small style="display:block;color:var(--muted);margin-top:3px;">Nota: ${h(l.notes)}</small>` : ''}
          </div>
        `;
      }).join('');
    }

    // Registro de parecer científico humano
    $('btn-submit-review').onclick = async () => {
      const decision = $('review-decision-select').value;
      const notes = $('review-notes-input').value.trim();
      try {
        await api(`/api/v1/evidence/${encodeURIComponent(entityId)}/review`, {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({status: decision, justification: notes})
        });
        $('review-status-badge').textContent = decision === 'reviewed' ? 'Revisado' : decision === 'disputed' ? 'Contestado' : 'Rejeitado';
        $('review-status-badge').className = `badge-class ${decision === 'reviewed' ? 'produtiva' : 'seca'}`;
        toast('Decisão de revisão científica registrada com sucesso!');
      } catch (e) {
        toast('Erro ao registrar revisão: ' + e.message);
      }
    };

    // Contexto com destaque do trecho
    if (p.page_text) {
      renderPageWithHighlight(p.page_text, p.evidencia_textual);
    } else {
      $('audit-page-fulltext').textContent = 'Texto integral da página não extraído ou indisponível.';
    }

    $('btn-copy-sha').onclick = () => {
      navigator.clipboard.writeText(p.sha256);
      toast('Hash SHA-256 copiado para a área de transferência!');
    };

    $('btn-copy-excerpt').onclick = () => {
      const cite = `"${p.evidencia_textual}" (Informativo Conjuntural Emater/RS nº ${p.numero_informativo}, ${p.data_publicacao}, p. ${p.pagina})`;
      navigator.clipboard.writeText(cite);
      toast('Citação com proveniência copiada!');
    };

  } catch (err) {
    $('audit-doc-name').textContent = 'Erro ao carregar';
    $('audit-excerpt-text').textContent = `Não foi possível auditar a prova documental: ${err.message}`;
    $('audit-page-fulltext').textContent = '—';
  }
}

function renderPageWithHighlight(pageText, excerpt) {
  const container = $('audit-page-fulltext');
  if (!excerpt || !pageText.includes(excerpt)) {
    container.textContent = pageText;
    return;
  }
  const parts = pageText.split(excerpt);
  container.innerHTML = parts.map((part, i) => {
    if (i === 0) return h(part);
    return `<mark class="audit-highlight">${h(excerpt)}</mark>` + h(part);
  }).join('');
}

function closeAuditDrawer() {
  $('audit-drawer-overlay').classList.add('hidden');
}

// ----------------- GESTÃO DE USUÁRIOS -----------------
async function loadUsers() {
  const tbody = $('users-table-body');
  tbody.innerHTML = '<tr><td colspan="5" style="text-align:center;padding:20px;color:var(--muted)">Carregando usuários…</td></tr>';
  try {
    const res = await api('/api/v1/auth/users');
    const users = res.users || [];
    tbody.innerHTML = users.map(u => `
      <tr>
        <td><strong>${h(u.name)}</strong></td>
        <td><code>${h(u.email)}</code></td>
        <td><span class="badge-class ${u.role === 'admin' ? 'onda_calor' : 'climatica'}">${h(u.role)}</span></td>
        <td><span class="custody-tag">${u.is_active ? 'Ativo' : 'Inativo'}</span></td>
        <td><small style="color:var(--muted)">${h(u.last_login_at)}</small></td>
      </tr>
    `).join('');
  } catch (err) {
    tbody.innerHTML = `<tr><td colspan="5" style="text-align:center;padding:20px;color:#c2410c">Erro ao carregar usuários: ${h(err.message)}</td></tr>`;
  }
}

// ----------------- ENVIO E UPLOAD DE ARQUIVOS (INGESTÃO NO COFRE) -----------------
function initUpload() {
  const fileInput = $('file-input');
  const dropzone = $('dropzone');
  const fileList = $('file-list');
  const fileCount = $('file-count');
  const sendBtn = $('btn-send-files');

  if (!fileInput || !dropzone) return;

  function handleFiles(files) {
    selectedFiles = Array.from(files).filter(f => f.name.toLowerCase().endsWith('.pdf') || f.type === 'application/pdf');
    fileCount.textContent = selectedFiles.length > 0 
      ? `${selectedFiles.length} arquivo(s) PDF selecionado(s)` 
      : 'Nenhum arquivo selecionado';
    fileList.innerHTML = selectedFiles.map(f => `<li><strong>${h(f.name)}</strong> (${(f.size / 1024 / 1024).toFixed(2)} MB)</li>`).join('');
    sendBtn.disabled = selectedFiles.length === 0;
  }

  fileInput.addEventListener('change', e => handleFiles(e.target.files));
  dropzone.addEventListener('dragover', e => { e.preventDefault(); dropzone.classList.add('drag'); });
  dropzone.addEventListener('dragleave', () => dropzone.classList.remove('drag'));
  dropzone.addEventListener('drop', e => {
    e.preventDefault();
    dropzone.classList.remove('drag');
    handleFiles(e.dataTransfer.files);
  });

  $('upload-doc-form').addEventListener('submit', async e => {
    e.preventDefault();
    if (selectedFiles.length === 0) return;
    const results = $('upload-results');
    results.innerHTML = '<p>Transmitindo arquivo e calculando SHA-256 no cofre…</p>';
    sendBtn.disabled = true;

    for (const file of selectedFiles) {
      try {
        const bytes = await file.arrayBuffer();
        const url = `/api/v1/documents/upload?filename=${encodeURIComponent(file.name)}`;
        const res = await fetch(url, {
          method: 'POST',
          headers: {'Content-Type': 'application/pdf'},
          body: bytes
        });
        const data = await res.json();
        if (res.ok) {
          results.innerHTML += `<p>✓ <strong>${h(file.name)}</strong> arquivado com sucesso no cofre (SHA-256: <code>${h(data.sha256.slice(0, 16))}…</code>, ${data.total_pages} págs).</p>`;
          toast(`Informativo ${file.name} arquivado no cofre!`);
        } else {
          results.innerHTML += `<p class="fail">✕ Falha em ${h(file.name)}: ${h(data.error || 'Erro desconhecido')}</p>`;
        }
      } catch (err) {
        results.innerHTML += `<p class="fail">✕ Erro de conexão em ${h(file.name)}: ${h(err.message)}</p>`;
      }
    }
    selectedFiles = [];
    fileCount.textContent = 'Nenhum arquivo selecionado';
    fileList.innerHTML = '';
    loadVaultDocuments();
    loadStats();
  });
}

// ----------------- INICIALIZAÇÃO DO SISTEMA -----------------
document.addEventListener('DOMContentLoaded', () => {
  // Configuração da Navegação
  for (const btn of document.querySelectorAll('[data-view]')) {
    btn.addEventListener('click', () => nav(btn.dataset.view));
  }

  // Hero shortcuts
  $('hero-go-events').onclick = () => nav('eventos');
  $('hero-go-territories').onclick = () => nav('territorios');
  $('hero-go-vault').onclick = () => nav('custodia');

  // Stat card clicks
  $('card-stat-events').onclick = () => nav('eventos');
  $('card-stat-obs').onclick = () => nav('observacoes');
  $('card-stat-muns').onclick = () => nav('territorios');
  $('card-stat-vault').onclick = () => nav('custodia');

  // Filtros de Eventos
  $('events-class-filter').onchange = () => loadEvents();
  $('events-region-filter').onchange = () => loadEvents();
  $('events-search').oninput = () => renderEvents();
  $('btn-refresh-events').onclick = () => loadEvents();

  // Filtros de Observações
  $('obs-variable-filter').onchange = () => loadObservations();
  $('obs-activity-filter').onchange = () => loadObservations();
  $('obs-search').oninput = () => renderObservations();
  $('btn-refresh-obs').onclick = () => loadObservations();

  // Filtros de Municípios
  $('mun-search').oninput = () => renderMunicipalities();
  $('btn-refresh-muns').onclick = () => loadMunicipalities();

  // Episódios & Cofre
  $('btn-refresh-episodes').onclick = () => loadEpisodes();
  $('btn-refresh-vault').onclick = () => loadVaultDocuments();
  if ($('btn-refresh-users')) $('btn-refresh-users').onclick = () => loadUsers();

  // Drawer de Auditoria
  $('btn-close-audit').onclick = closeAuditDrawer;
  $('audit-drawer-overlay').addEventListener('click', e => {
    if (e.target === $('audit-drawer-overlay')) closeAuditDrawer();
  });
  window.addEventListener('keydown', e => {
    if (e.key === 'Escape') closeAuditDrawer();
  });

  // Mobile drawer
  $('menu').onclick = () => {
    $('sidebar').classList.toggle('open');
    $('mobile-scrim').classList.toggle('shown');
  };
  $('mobile-scrim').onclick = () => {
    $('sidebar').classList.remove('open');
    $('mobile-scrim').classList.remove('shown');
  };

  // Autenticação MORFO
  $('btn-logout').onclick = async () => {
    await fetch('/api/v1/auth/logout', {method: 'POST'}).catch(() => {});
    currentUser = null;
    updateUserUI();
    toast('Sessão encerrada com sucesso.');
  };

  $('btn-dismiss-auth').onclick = hideAuthOverlay;

  // Inicia upload
  initUpload();

  // Roteamento via Hash
  const hash = location.hash.replace('#', '');
  if (viewNames[hash]) nav(hash);
  else nav('visao');

  // Valida autenticação inicial
  checkAuth();

  // Carrega anos disponíveis no corpus
  loadYearsList();
});

// Exporta para chamadas inline
window.openAuditProof = openAuditProof;
window.selectMunicipality = selectMunicipality;
window.onYearChange = onYearChange;
window.filterEventsClass = filterEventsClass;
window.clearEventsFilter = clearEventsFilter;
window.filterObsTheme = filterObsTheme;
window.clearObsFilter = clearObsFilter;
