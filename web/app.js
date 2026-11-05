// EVIDA — Observatório de Evidências Científicas
// Padrão de Identidade Visual e Autenticação MORFO
'use strict';

const $ = id => document.getElementById(id);
const h = v => String(v ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const number = new Intl.NumberFormat('pt-BR');

let currentUser = null;
let documents = [];
let evidences = [];
let currentDoc = null;
let currentPageNumber = 1;

const viewNames = {
  visao: 'Visão geral',
  ingestao: 'Ingestão documental',
  evidencias: 'Acervo de evidências',
  integridade: 'Integridade & origem',
  sobre: 'Sobre o EVIDA',
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

  if (view === 'ingestao') loadDocuments();
  if (view === 'evidencias') loadEvidences();
  if (view === 'usuarios') loadUsers();
}

async function api(path, options = {}) {
  const r = await fetch(path, options);
  let data = await r.json().catch(() => ({error: 'Resposta inválida do servidor'}));
  if (!r.ok) {
    if (r.status === 401 && !path.startsWith('/api/v1/auth/')) {
      currentUser = null;
      updateUserUI();
      showAuthOverlay('Sua sessão expirou ou é necessária autenticação.');
    }
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
  if (currentUser) {
    chip.classList.remove('hidden');
    $('user-avatar').textContent = getInitials(currentUser.name, currentUser.email);
    $('user-email').textContent = currentUser.email;
    $('user-email').title = currentUser.name ? `${currentUser.name} (${currentUser.email})` : currentUser.email;
    const roleLabel = currentUser.role === 'admin' ? 'Administrador' : currentUser.role === 'researcher' ? 'Pesquisador' : 'Usuário';
    const roleEl = document.querySelector('.user-role');
    if (roleEl) roleEl.textContent = roleLabel;
    if (navUsuarios) navUsuarios.classList.toggle('hidden', currentUser.role !== 'admin');
  } else {
    chip.classList.add('hidden');
    if (navUsuarios) navUsuarios.classList.add('hidden');
  }
}

// ----------------- AUTENTICAÇÃO PADRÃO MORFO -----------------

function setAuthFeedback(msg, type = 'error') {
  const box = $('auth-feedback');
  if (!msg) { box.classList.add('hidden'); box.textContent = ''; return; }
  box.className = `auth-feedback ${type}`;
  box.textContent = msg;
  box.classList.remove('hidden');
}

function showAuthOverlay(infoMsg) {
  $('auth-overlay').classList.remove('hidden');
  document.body.style.overflow = 'hidden';
  $('auth-email-form').classList.remove('hidden');
  $('auth-code-form').classList.add('hidden');
  $('auth-dev-preview').classList.add('hidden');
  setAuthFeedback(infoMsg || '', infoMsg ? 'error' : '');
  setTimeout(() => $('auth-email-input').focus(), 50);
}

function hideAuthOverlay() {
  $('auth-overlay').classList.add('hidden');
  document.body.style.overflow = '';
}

let cooldownTimer = null;
function startCooldownTimer(seconds = 60) {
  if (cooldownTimer) clearInterval(cooldownTimer);
  let remaining = seconds;
  const resendBtn = $('btn-resend-code');
  if (!resendBtn) return;
  resendBtn.disabled = true;
  resendBtn.textContent = `Aguarde ${remaining}s para reenviar`;

  cooldownTimer = setInterval(() => {
    remaining -= 1;
    if (remaining <= 0) {
      clearInterval(cooldownTimer);
      cooldownTimer = null;
      resendBtn.disabled = false;
      resendBtn.textContent = '↻ Reenviar novo código';
    } else {
      resendBtn.textContent = `Aguarde ${remaining}s para reenviar`;
    }
  }, 1000);
}

async function requestAuthCode(e) {
  if (e && e.preventDefault) e.preventDefault();
  const emailInput = $('auth-email-input');
  const email = emailInput.value.trim().toLowerCase();
  if (!email || !email.includes('@') || !email.includes('.')) {
    setAuthFeedback('Por favor, informe um endereço de e-mail válido.', 'error');
    emailInput.focus();
    return;
  }
  const btn = $('btn-request-code');
  btn.disabled = true;
  setAuthFeedback('Enviando código de verificação por e-mail...', 'success');
  try {
    const res = await api('/api/v1/auth/request-code', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({email})
    });
    $('auth-target-email').textContent = email;
    $('auth-email-form').classList.add('hidden');
    $('auth-code-form').classList.remove('hidden');
    if (res.dev_code) {
      $('auth-dev-preview').classList.remove('hidden');
      $('auth-dev-code-val').textContent = res.dev_code;
      $('auth-code-input').value = res.dev_code;
    } else {
      $('auth-dev-preview').classList.add('hidden');
      $('auth-code-input').value = '';
    }
    setAuthFeedback(res.message || 'Código enviado! Verifique seu e-mail.', 'success');
    setTimeout(() => $('auth-code-input').focus(), 50);
    startCooldownTimer(res.retry_after_seconds || 60);
  } catch (err) {
    setAuthFeedback(err.message, 'error');
  } finally {
    btn.disabled = false;
  }
}

async function verifyAuthCode(e) {
  if (e && e.preventDefault) e.preventDefault();
  const email = $('auth-email-input').value.trim().toLowerCase();
  const code = $('auth-code-input').value.trim();
  if (code.length !== 6) {
    setAuthFeedback('O código deve conter 6 dígitos numéricos.', 'error');
    return;
  }
  const btn = $('btn-verify-code');
  btn.disabled = true;
  setAuthFeedback('Validando chave de acesso...', 'success');
  try {
    const res = await api('/api/v1/auth/verify-code', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({email, code})
    });
    currentUser = res.user;
    updateUserUI();
    hideAuthOverlay();
    toast(`Bem-vindo, ${currentUser.name || currentUser.email}!`);
    loadStats();
  } catch (err) {
    setAuthFeedback(err.message, 'error');
    $('auth-code-input').select();
  } finally {
    btn.disabled = false;
  }
}

async function logout() {
  try {
    await api('/api/v1/auth/logout', {method: 'POST'});
  } catch (_) {}
  currentUser = null;
  updateUserUI();
  showAuthOverlay('Você encerrou sua sessão.');
}

async function checkAuth() {
  try {
    const res = await api('/api/v1/auth/me');
    if (res.authenticated && res.user) {
      currentUser = res.user;
      updateUserUI();
      hideAuthOverlay();
    } else {
      currentUser = null;
      updateUserUI();
      showAuthOverlay();
    }
  } catch (_) {
    showAuthOverlay();
  }
}

// ----------------- EVIDA STATS & DADOS -----------------

async function loadStats() {
  try {
    const data = await api('/api/v1/stats');
    if ($('stat-documents')) $('stat-documents').textContent = number.format(data.documents);
    if ($('stat-evidences')) $('stat-evidences').textContent = number.format(data.evidences);
    if ($('stat-users')) $('stat-users').textContent = number.format(data.users);
  } catch (_) {}
}

// ----------------- INGESTÃO DOCUMENTAL (v0.2.0) -----------------

async function loadDocuments() {
  const tbody = $('docs-table-body');
  try {
    const data = await api('/api/v1/documents');
    documents = data.documents || [];
    if (!documents.length) {
      tbody.innerHTML = '<tr><td colspan="7" style="text-align:center;padding:24px;color:var(--muted)">Nenhum documento indexado ainda. Use o formulário ou carregue a amostra.</td></tr>';
      return;
    }
    tbody.innerHTML = documents.map(d => `
      <tr>
        <td><strong>#${d.id}</strong></td>
        <td><strong>${h(d.title)}</strong><br><small style="color:var(--muted)">${h(d.source_uri)}</small></td>
        <td>${h(d.publication_date || '—')}</td>
        <td><code class="doc-hash-badge" title="${h(d.sha256)}">${h(d.sha256.substring(0, 16))}…</code></td>
        <td><span class="evidence-page-tag">${d.total_pages} págs</span></td>
        <td><strong>${d.evidence_count}</strong> evidências</td>
        <td>
          <button type="button" class="btn-subtle" onclick="openDocInspector(${d.id})">🔍 Inspecionar</button>
        </td>
      </tr>
    `).join('');
  } catch (err) {
    tbody.innerHTML = `<tr><td colspan="7" style="text-align:center;color:#b91c1c;padding:20px;">Falha ao carregar documentos: ${h(err.message)}</td></tr>`;
  }
}

async function ingestSampleDocument() {
  const btn = $('hero-sample-ingest') || $('btn-quick-sample');
  if (btn) btn.disabled = true;
  toast('Iniciando extração do Informativo Conjuntural de referência…');
  try {
    const res = await api('/api/v1/documents/ingest-sample', {method: 'POST'});
    toast(`Sucesso! Documento #${res.document_id} indexado (${res.total_pages} páginas, ${res.extracted_evidences} evidências).`);
    loadStats();
    loadDocuments();
    nav('ingestao');
    openDocInspector(res.document_id);
  } catch (err) {
    toast(`Erro na extração: ${err.message}`);
  } finally {
    if (btn) btn.disabled = false;
  }
}

async function handleFormIngest(e) {
  e.preventDefault();
  const filePath = $('ingest-file-path').value.trim();
  const title = $('ingest-title').value.trim();
  const pubDate = $('ingest-date').value;
  const feedback = $('ingest-feedback');

  if (!filePath) return;
  const btn = $('btn-submit-ingest');
  btn.disabled = true;
  feedback.className = 'auth-feedback success';
  feedback.textContent = 'Calculando hash SHA-256 e extraindo texto das páginas...';
  feedback.classList.remove('hidden');

  try {
    const res = await api('/api/v1/documents/ingest', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({path: filePath, title, publication_date: pubDate})
    });
    feedback.textContent = `Documento #${res.document_id} incorporado com sucesso! Extraídas ${res.extracted_evidences} evidências.`;
    loadStats();
    loadDocuments();
    openDocInspector(res.document_id);
  } catch (err) {
    feedback.className = 'auth-feedback error';
    feedback.textContent = `Falha na ingestão: ${err.message}`;
  } finally {
    btn.disabled = false;
  }
}

// ----------------- INSPECTOR DE PÁGINAS & PROVENIÊNCIA -----------------

window.openDocInspector = async function(docId) {
  try {
    const data = await api(`/api/v1/documents/${docId}`);
    currentDoc = data.document;
    const pages = data.pages || [];

    const inspector = $('doc-inspector');
    inspector.classList.remove('hidden');
    $('inspector-title').textContent = currentDoc.title;
    $('inspector-subtitle').textContent = `SHA-256: ${currentDoc.sha256} · Total: ${currentDoc.total_pages} páginas`;

    const select = $('select-page');
    select.innerHTML = pages.map(p => `<option value="${p.page}">Página ${p.page}</option>`).join('');
    currentPageNumber = 1;
    select.value = '1';
    await loadPageContent(docId, 1);
    inspector.scrollIntoView({behavior: 'smooth'});
  } catch (err) {
    toast(`Erro ao inspecionar documento: ${err.message}`);
  }
};

async function loadPageContent(docId, pageNumber) {
  const viewer = $('page-text-viewer');
  const evList = $('page-evidences-list');
  $('page-indicator').textContent = `Página ${pageNumber} de ${currentDoc ? currentDoc.total_pages : '—'}`;
  viewer.textContent = 'Extraindo texto da página…';
  evList.innerHTML = '<p style="color:var(--muted)">Consultando evidências…</p>';

  try {
    const pData = await api(`/api/v1/documents/${docId}/pages/${pageNumber}`);
    viewer.textContent = pData.page.text || '(Página vazia ou sem texto selecionável)';

    // Evidências desta página
    const evData = await api(`/api/v1/evidences?doc_id=${docId}`);
    const pageEvs = (evData.evidences || []).filter(e => e.page === pageNumber);

    if (!pageEvs.length) {
      evList.innerHTML = '<p style="color:var(--muted);font-size:0.85rem;">Nenhuma evidência isolada nesta página específica.</p>';
    } else {
      evList.innerHTML = pageEvs.map(e => `
        <div class="evidence-card" style="padding:12px;">
          <div class="evidence-meta">
            <span class="pill ${e.status}">${e.status}</span>
            <small>${e.reviewer ? 'Revisor: ' + h(e.reviewer) : 'Extração automática'}</small>
          </div>
          <p class="evidence-excerpt" style="font-size:0.82rem;">${h(e.excerpt)}</p>
          <div class="evidence-actions">
            <button class="btn-subtle" onclick="changeEvidenceStatus(${e.id}, 'reviewed')">✓ Confirmar</button>
            <button class="btn-subtle" onclick="changeEvidenceStatus(${e.id}, 'disputed')">? Contestar</button>
          </div>
        </div>
      `).join('');
    }
  } catch (err) {
    viewer.textContent = `Falha ao carregar conteúdo: ${err.message}`;
  }
}

// ----------------- ACERVO DE EVIDÊNCIAS -----------------

async function loadEvidences() {
  const container = $('evidences-grid');
  const statusFilter = $('evidence-status-filter') ? $('evidence-status-filter').value : '';
  const search = $('evidence-search') ? $('evidence-search').value.trim().toLowerCase() : '';

  try {
    const query = statusFilter ? `?status=${encodeURIComponent(statusFilter)}` : '';
    const data = await api(`/api/v1/evidences${query}`);
    let list = data.evidences || [];

    if (search) {
      list = list.filter(e => e.excerpt.toLowerCase().includes(search) || e.doc_title.toLowerCase().includes(search));
    }

    if (!list.length) {
      container.innerHTML = '<p style="color:var(--muted);padding:24px;">Nenhuma evidência encontrada para os filtros selecionados.</p>';
      return;
    }

    container.innerHTML = list.map(e => `
      <article class="evidence-card">
        <div class="evidence-meta">
          <span class="evidence-page-tag">Pág. ${e.page}</span>
          <span class="pill ${e.status}">${e.status}</span>
          <code class="doc-hash-badge" title="${h(e.doc_sha256)}">${h(e.doc_sha256.substring(0, 8))}…</code>
        </div>
        <p class="evidence-excerpt">${h(e.excerpt)}</p>
        <small style="color:var(--muted);font-size:0.75rem;">Fonte: ${h(e.doc_title)} · ${e.reviewer || 'sistema'}</small>
        <div class="evidence-actions">
          <button type="button" class="btn-subtle" onclick="changeEvidenceStatus(${e.id}, 'reviewed')" title="Marcar como revisado">✓ Revisar</button>
          <button type="button" class="btn-subtle" onclick="changeEvidenceStatus(${e.id}, 'disputed')" title="Marcar como contestado / incerteza">? Contestar</button>
          <button type="button" class="btn-subtle danger" onclick="changeEvidenceStatus(${e.id}, 'rejected')" title="Rejeitar evidência">✕ Rejeitar</button>
        </div>
      </article>
    `).join('');
  } catch (err) {
    container.innerHTML = `<p style="color:#b91c1c;padding:20px;">Falha ao carregar evidências: ${h(err.message)}</p>`;
  }
}

window.changeEvidenceStatus = async function(evidenceId, newStatus) {
  try {
    await api(`/api/v1/evidences/${evidenceId}/status`, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({status: newStatus})
    });
    toast(`Evidência #${evidenceId} atualizada para ${newStatus}`);
    if (currentDoc) {
      loadPageContent(currentDoc.id, currentPageNumber);
    }
    loadEvidences();
  } catch (err) {
    toast(`Erro ao atualizar evidência: ${err.message}`);
  }
};

// ----------------- GESTÃO DE USUÁRIOS -----------------

async function loadUsers() {
  const tbody = $('users-table-body');
  if (!tbody) return;
  try {
    const data = await api('/api/v1/auth/users');
    const users = data.users || [];
    if (!users.length) {
      tbody.innerHTML = '<tr><td colspan="5" style="text-align:center;padding:20px;color:var(--muted)">Nenhum usuário cadastrado.</td></tr>';
      return;
    }
    tbody.innerHTML = users.map(u => `
      <tr>
        <td><strong>${h(u.name)}</strong></td>
        <td>${h(u.email)}</td>
        <td><span class="pill">${h(u.role)}</span></td>
        <td><span class="pill ${u.is_active ? 'reviewed' : 'rejected'}">${u.is_active ? 'Ativo' : 'Inativo'}</span></td>
        <td>${h(u.last_login_at || 'Nunca')}</td>
      </tr>
    `).join('');
  } catch (err) {
    tbody.innerHTML = `<tr><td colspan="5" style="text-align:center;color:#b91c1c;padding:20px;">Falha ao listar usuários: ${h(err.message)}</td></tr>`;
  }
}

// ----------------- INICIALIZAÇÃO -----------------

document.addEventListener('DOMContentLoaded', () => {
  // Navegação
  for (const b of document.querySelectorAll('[data-view]')) {
    b.addEventListener('click', () => nav(b.dataset.view));
  }
  const hash = location.hash.replace('#', '');
  if (viewNames[hash]) nav(hash);
  else nav('visao');

  // Menu móvel
  $('menu').addEventListener('click', () => {
    const open = $('sidebar').classList.toggle('open');
    $('mobile-scrim').classList.toggle('shown', open);
    $('menu').setAttribute('aria-expanded', open ? 'true' : 'false');
  });
  $('mobile-scrim').addEventListener('click', () => {
    $('sidebar').classList.remove('open');
    $('mobile-scrim').classList.remove('shown');
    $('menu').setAttribute('aria-expanded', 'false');
  });

  // Autenticação
  $('auth-email-form').addEventListener('submit', requestAuthCode);
  $('auth-code-form').addEventListener('submit', verifyAuthCode);
  $('btn-resend-code').addEventListener('click', requestAuthCode);
  $('btn-back-email').addEventListener('click', () => {
    $('auth-code-form').classList.add('hidden');
    $('auth-email-form').classList.remove('hidden');
    $('auth-email-input').focus();
  });
  $('btn-logout').addEventListener('click', logout);

  // Ingestão
  if ($('hero-go-ingest')) $('hero-go-ingest').addEventListener('click', () => nav('ingestao'));
  if ($('hero-sample-ingest')) $('hero-sample-ingest').addEventListener('click', ingestSampleDocument);
  if ($('btn-quick-sample')) $('btn-quick-sample').addEventListener('click', ingestSampleDocument);
  if ($('form-ingest')) $('form-ingest').addEventListener('submit', handleFormIngest);
  if ($('btn-refresh-docs')) $('btn-refresh-docs').addEventListener('click', loadDocuments);

  // Inspector
  $('btn-close-inspector').addEventListener('click', () => $('doc-inspector').classList.add('hidden'));
  $('select-page').addEventListener('change', e => {
    currentPageNumber = parseInt(e.target.value, 10);
    if (currentDoc) loadPageContent(currentDoc.id, currentPageNumber);
  });
  $('btn-prev-page').addEventListener('click', () => {
    if (currentPageNumber > 1) {
      currentPageNumber--;
      $('select-page').value = String(currentPageNumber);
      if (currentDoc) loadPageContent(currentDoc.id, currentPageNumber);
    }
  });
  $('btn-next-page').addEventListener('click', () => {
    if (currentDoc && currentPageNumber < currentDoc.total_pages) {
      currentPageNumber++;
      $('select-page').value = String(currentPageNumber);
      loadPageContent(currentDoc.id, currentPageNumber);
    }
  });

  // Evidências
  if ($('btn-refresh-evidences')) $('btn-refresh-evidences').addEventListener('click', loadEvidences);
  if ($('evidence-status-filter')) $('evidence-status-filter').addEventListener('change', loadEvidences);
  if ($('evidence-search')) $('evidence-search').addEventListener('input', loadEvidences);

  // Gestão de Usuários
  if ($('btn-refresh-users')) $('btn-refresh-users').addEventListener('click', loadUsers);

  // Verifica sessão e estatísticas iniciais
  checkAuth();
  loadStats();
});
