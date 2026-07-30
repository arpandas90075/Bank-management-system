/* ─────────────────────────────────────────────────────────────
   store.js — the in-memory database
   A Map keyed by account number. Nothing is persisted; a page
   refresh gives you a fresh bank.
   ───────────────────────────────────────────────────────────── */

const Store = {
  accounts: new Map(),
  nextId: 100001,
  nextTxn: 1,

  /** Create and index a new account record. */
  add(name, type, pin, balance) {
    const account = {
      id: this.nextId++,
      name,
      type,                 // 'SAVINGS' | 'CURRENT'
      pin,                  // stored as a digest, see bank.js
      balance,
      opened: new Date(),
      txns: []              // newest first
    };
    this.accounts.set(account.id, account);
    return account;
  },

  get(id) {
    return this.accounts.get(Number(id)) || null;
  },

  all() {
    return [...this.accounts.values()];
  },

  remove(id) {
    return this.accounts.delete(Number(id));
  },

  /** Push a ledger entry onto the front of an account's history. */
  record(account, kind, amount, note = '') {
    account.txns.unshift({
      id: this.nextTxn++,
      kind,                        // DEPOSIT | WITHDRAW | TRANSFER-IN | ...
      amount,
      balance: account.balance,    // balance after the entry
      note,
      at: new Date()
    });
  },

  count() {
    return this.accounts.size;
  }
};

if (typeof module !== 'undefined') module.exports = { Store };
