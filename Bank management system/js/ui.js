/* ─────────────────────────────────────────────────────────────
   ui.js — everything that touches the DOM
   Rendering only: it reads from Bank/Store and writes to the page.
   ───────────────────────────────────────────────────────────── */

const UI = {

  $(sel) { return document.querySelector(sel); },
  $$(sel) { return [...document.querySelectorAll(sel)]; },

  money(v) {
    return Number(v).toLocaleString('en-US', {
      minimumFractionDigits: 2,
      maximumFractionDigits: 2
    });
  },

  when(date) {
    return date.toLocaleString([], {
      month: 'short', day: '2-digit', hour: '2-digit', minute: '2-digit'
    });
  },

  /** Swap the visible <section class="view">. */
  show(id) {
    this.$$('.view').forEach(v => v.classList.toggle('active', v.id === id));
    window.scrollTo({ top: 0, behavior: 'smooth' });
  },

  toast(message, isError = false) {
    const el = document.createElement('div');
    el.className = 'toast' + (isError ? ' err' : '');
    el.textContent = message;
    this.$('#toast-host').appendChild(el);
    setTimeout(() => {
      el.style.opacity = '0';
      el.style.transition = 'opacity .3s';
      setTimeout(() => el.remove(), 300);
    }, 2600);
  },

  clear(...selectors) {
    selectors.forEach(s => { const el = this.$(s); if (el) el.value = ''; });
  },

  // ── dashboard ──────────────────────────────────────────────

  renderDashboard(account) {
    this.$('#dash-balance').textContent = this.money(account.balance);
    this.$('#dash-id').textContent = account.id;
    this.$('#dash-type').textContent = account.type;
    this.$('#dash-meta').textContent =
      `${account.name} · available ${this.money(Bank.available(account))}`;
    this.renderTxns(account);
  },

  renderTxns(account) {
    const body = this.$('#txn-table tbody');
    body.innerHTML = '';

    if (!account.txns.length) {
      body.innerHTML = '<tr class="empty"><td colspan="4">No transactions yet.</td></tr>';
      return;
    }

    for (const t of account.txns.slice(0, 25)) {
      const debit = t.kind === 'WITHDRAW' || t.kind === 'TRANSFER-OUT';
      const tr = document.createElement('tr');
      tr.innerHTML = `
        <td>${t.kind}${t.note ? ` <span class="muted">${t.note}</span>` : ''}</td>
        <td class="${debit ? 'debit' : 'credit'}">${debit ? '−' : '+'}${this.money(t.amount)}</td>
        <td>${this.money(t.balance)}</td>
        <td class="muted">${this.when(t.at)}</td>`;
      body.appendChild(tr);
    }
  },

  // ── admin ──────────────────────────────────────────────────

  renderAdmin(query = '') {
    const s = Bank.stats();
    this.$('#admin-stats').innerHTML = `
      <div class="stat"><span class="muted">Accounts</span><b>${s.accounts}</b></div>
      <div class="stat"><span class="muted">Total deposits</span><b>${this.money(s.deposits)}</b></div>
      <div class="stat"><span class="muted">Transactions</span><b>${s.txns}</b></div>
      <div class="stat"><span class="muted">Savings / Current</span><b>${s.savings} / ${s.accounts - s.savings}</b></div>`;

    const body = this.$('#admin-table tbody');
    const rows = Bank.directory(query);
    body.innerHTML = '';

    if (!rows.length) {
      body.innerHTML = '<tr class="empty"><td colspan="5">No matching accounts.</td></tr>';
      return;
    }

    for (const a of rows) {
      const tr = document.createElement('tr');
      tr.innerHTML = `
        <td>${a.id}</td>
        <td>${a.name}</td>
        <td class="muted">${a.type}</td>
        <td class="${a.balance < 0 ? 'debit' : ''}">${this.money(a.balance)}</td>
        <td class="muted">${a.txns.length}</td>`;
      body.appendChild(tr);
    }
  },

  // ── tabs ───────────────────────────────────────────────────

  bindTabs() {
    this.$$('.tab').forEach(tab => {
      tab.addEventListener('click', () => {
        this.$$('.tab').forEach(t => t.classList.toggle('active', t === tab));
        this.$$('.pane').forEach(p =>
          p.classList.toggle('active', p.id === `pane-${tab.dataset.tab}`));
      });
    });
  }
};
