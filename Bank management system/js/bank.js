/* ─────────────────────────────────────────────────────────────
   bank.js — business rules
   Every method returns { ok: true, ... } or { ok: false, error }.
   No DOM access lives in this file, so it can be unit tested in
   plain Node (see tests/bank.test.js).
   ───────────────────────────────────────────────────────────── */

const RULES = {
  MIN_OPENING: 500,
  OVERDRAFT: 500,          // CURRENT accounts only
  MAX_TXN: 1000000,
  RATE: { SAVINGS: 0.045, CURRENT: 0.012 }
};

const Bank = {

  /** Tiny non-cryptographic digest so PINs are never held in plain text. */
  digest(id, pin) {
    let h = 0x811c9dc5;
    for (const ch of `${id}:${pin}`) {
      h ^= ch.charCodeAt(0);
      h = Math.imul(h, 0x01000193) >>> 0;
    }
    return h;
  },

  validPin(pin) {
    return /^\d{4}$/.test(String(pin || ''));
  },

  amount(raw) {
    const v = Number(raw);
    if (!Number.isFinite(v) || v <= 0) return null;
    if (v > RULES.MAX_TXN) return null;
    return Math.round(v * 100) / 100;
  },

  /** How far this account may go down. */
  floor(account) {
    return account.type === 'CURRENT' ? -RULES.OVERDRAFT : 0;
  },

  available(account) {
    return +(account.balance - this.floor(account)).toFixed(2);
  },

  // ── account lifecycle ──────────────────────────────────────

  open({ name, type, pin, deposit }) {
    name = String(name || '').trim();
    if (name.length < 2) return { ok: false, error: 'Enter a name of at least 2 characters.' };
    if (!['SAVINGS', 'CURRENT'].includes(type)) return { ok: false, error: 'Pick an account type.' };
    if (!this.validPin(pin)) return { ok: false, error: 'PIN must be exactly 4 digits.' };

    const amt = this.amount(deposit);
    if (amt === null || amt < RULES.MIN_OPENING)
      return { ok: false, error: `Opening deposit must be at least ${RULES.MIN_OPENING}.` };

    const account = Store.add(name, type, 0, amt);
    account.pin = this.digest(account.id, pin);
    Store.record(account, 'OPENING', amt);
    return { ok: true, account };
  },

  login(id, pin) {
    const account = Store.get(id);
    if (!account || account.pin !== this.digest(account.id, pin))
      return { ok: false, error: 'Account number or PIN is incorrect.' };
    return { ok: true, account };
  },

  close(account, pin) {
    if (account.pin !== this.digest(account.id, pin))
      return { ok: false, error: 'Wrong PIN.' };
    if (account.balance < 0)
      return { ok: false, error: 'Settle the overdraft before closing.' };
    Store.remove(account.id);
    return { ok: true, paidOut: account.balance };
  },

  // ── money movement ─────────────────────────────────────────

  deposit(account, raw) {
    const amt = this.amount(raw);
    if (amt === null) return { ok: false, error: 'Enter a valid amount.' };
    account.balance = +(account.balance + amt).toFixed(2);
    Store.record(account, 'DEPOSIT', amt);
    return { ok: true, amount: amt };
  },

  withdraw(account, raw) {
    const amt = this.amount(raw);
    if (amt === null) return { ok: false, error: 'Enter a valid amount.' };
    if (amt > this.available(account))
      return { ok: false, error: `Declined — available balance is ${this.available(account).toFixed(2)}.` };
    account.balance = +(account.balance - amt).toFixed(2);
    Store.record(account, 'WITHDRAW', amt);
    return { ok: true, amount: amt };
  },

  transfer(from, toId, raw) {
    const to = Store.get(toId);
    if (!to) return { ok: false, error: 'Payee account not found.' };
    if (to.id === from.id) return { ok: false, error: 'You cannot transfer to yourself.' };

    const amt = this.amount(raw);
    if (amt === null) return { ok: false, error: 'Enter a valid amount.' };
    if (amt > this.available(from))
      return { ok: false, error: `Declined — available balance is ${this.available(from).toFixed(2)}.` };

    from.balance = +(from.balance - amt).toFixed(2);
    Store.record(from, 'TRANSFER-OUT', amt, `to ${to.id}`);
    to.balance = +(to.balance + amt).toFixed(2);
    Store.record(to, 'TRANSFER-IN', amt, `from ${from.id}`);

    return { ok: true, amount: amt, to };
  },

  // ── admin ──────────────────────────────────────────────────

  /** Credit one year of interest to every account in credit. */
  applyInterest() {
    let credited = 0, total = 0;
    for (const account of Store.all()) {
      if (account.balance <= 0) continue;
      const amt = Math.round(account.balance * RULES.RATE[account.type] * 100) / 100;
      if (amt <= 0) continue;
      account.balance = +(account.balance + amt).toFixed(2);
      Store.record(account, 'INTEREST', amt);
      credited++;
      total = +(total + amt).toFixed(2);
    }
    return { ok: true, credited, total };
  },

  stats() {
    const accounts = Store.all();
    const deposits = accounts.reduce((sum, a) => sum + a.balance, 0);
    const txns = accounts.reduce((sum, a) => sum + a.txns.length, 0);
    return {
      accounts: accounts.length,
      deposits: +deposits.toFixed(2),
      txns,
      savings: accounts.filter(a => a.type === 'SAVINGS').length
    };
  },

  /** Accounts sorted richest first, optionally filtered by name or id. */
  directory(query = '') {
    const q = query.trim().toLowerCase();
    return Store.all()
      .filter(a => !q || a.name.toLowerCase().includes(q) || String(a.id).includes(q))
      .sort((a, b) => b.balance - a.balance);
  }
};

if (typeof module !== 'undefined') module.exports = { Bank, RULES };
