/* ─────────────────────────────────────────────────────────────
   app.js — wiring
   Holds the session, listens for clicks, calls Bank, asks UI to
   redraw. Load order matters: store → bank → ui → app.
   ───────────────────────────────────────────────────────────── */

const App = {

  session: null,   // the signed-in account, or null

  init() {
    this.seed();
    UI.bindTabs();
    this.bindAuth();
    this.bindBanking();
    this.bindAdmin();
    UI.toast('Demo bank ready — try 100001 / 1234');
  },

  /** A few accounts so the app is not empty on first load. */
  seed() {
    const demo = [
      ['Arpan Das',       'SAVINGS', '1234', 42500],
      ['Mira Chatterjee', 'CURRENT', '1111',  8300],
      ['Devlin Roy',      'SAVINGS', '2222', 15750]
    ];
    for (const [name, type, pin, deposit] of demo)
      Bank.open({ name, type, pin, deposit });
  },

  refresh() {
    if (this.session) UI.renderDashboard(this.session);
  },

  // ── auth ───────────────────────────────────────────────────

  bindAuth() {
    UI.$('#btn-login').addEventListener('click', () => {
      const res = Bank.login(UI.$('#login-id').value.trim(), UI.$('#login-pin').value.trim());
      if (!res.ok) return UI.toast(res.error, true);

      this.session = res.account;
      UI.clear('#login-id', '#login-pin');
      UI.$('#nav-logout').classList.remove('hidden');
      UI.show('view-dash');
      this.refresh();
      UI.toast(`Welcome back, ${res.account.name.split(' ')[0]}.`);
    });

    UI.$('#btn-open').addEventListener('click', () => {
      const res = Bank.open({
        name: UI.$('#open-name').value,
        type: UI.$('#open-type').value,
        pin: UI.$('#open-pin').value.trim(),
        deposit: UI.$('#open-amount').value
      });
      if (!res.ok) return UI.toast(res.error, true);

      UI.clear('#open-name', '#open-amount', '#open-pin');
      UI.toast(`Account ${res.account.id} created — sign in with your PIN.`);
      UI.$('#login-id').value = res.account.id;
      UI.$('#login-id').focus();
    });

    UI.$('#nav-logout').addEventListener('click', () => this.logout());

    // Enter key submits the sign-in form
    UI.$('#login-pin').addEventListener('keydown', e => {
      if (e.key === 'Enter') UI.$('#btn-login').click();
    });
  },

  logout() {
    this.session = null;
    UI.$('#nav-logout').classList.add('hidden');
    UI.show('view-auth');
    UI.toast('Signed out.');
  },

  // ── banking actions ────────────────────────────────────────

  bindBanking() {
    UI.$('#btn-deposit').addEventListener('click', () => {
      const res = Bank.deposit(this.session, UI.$('#dep-amount').value);
      if (!res.ok) return UI.toast(res.error, true);
      UI.clear('#dep-amount');
      this.refresh();
      UI.toast(`Deposited ${UI.money(res.amount)}.`);
    });

    UI.$('#btn-withdraw').addEventListener('click', () => {
      const res = Bank.withdraw(this.session, UI.$('#wd-amount').value);
      if (!res.ok) return UI.toast(res.error, true);
      UI.clear('#wd-amount');
      this.refresh();
      UI.toast(`Withdrew ${UI.money(res.amount)}.`);
    });

    UI.$('#btn-transfer').addEventListener('click', () => {
      const res = Bank.transfer(this.session, UI.$('#tr-to').value.trim(), UI.$('#tr-amount').value);
      if (!res.ok) return UI.toast(res.error, true);
      UI.clear('#tr-to', '#tr-amount');
      this.refresh();
      UI.toast(`Sent ${UI.money(res.amount)} to ${res.to.name}.`);
    });
  },

  // ── admin console ──────────────────────────────────────────

  bindAdmin() {
    UI.$('#nav-admin').addEventListener('click', () => {
      UI.renderAdmin(UI.$('#admin-search').value);
      UI.show('view-admin');
    });

    UI.$('#btn-back').addEventListener('click', () => {
      UI.show(this.session ? 'view-dash' : 'view-auth');
      this.refresh();
    });

    UI.$('#admin-search').addEventListener('input', e => UI.renderAdmin(e.target.value));

    UI.$('#btn-interest').addEventListener('click', () => {
      const res = Bank.applyInterest();
      UI.renderAdmin(UI.$('#admin-search').value);
      UI.toast(`Interest credited to ${res.credited} account(s): ${UI.money(res.total)}.`);
    });
  }
};

document.addEventListener('DOMContentLoaded', () => App.init());
