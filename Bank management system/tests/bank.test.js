/* ─────────────────────────────────────────────────────────────
   bank.test.js — plain Node test runner, no dependencies.
   Run:  node tests/bank.test.js
   ───────────────────────────────────────────────────────────── */

const { Store } = require('../js/store.js');
global.Store = Store;                       // bank.js reads the global store
const { Bank, RULES } = require('../js/bank.js');

let passed = 0, failed = 0;

function check(name, condition) {
  if (condition) { passed++; console.log(`  ok    ${name}`); }
  else { failed++; console.log(`  FAIL  ${name}`); }
}

function reset() {
  Store.accounts.clear();
  Store.nextId = 100001;
  Store.nextTxn = 1;
}

// ── opening accounts ────────────────────────────────────────
reset();
console.log('\nopening accounts');
check('rejects a short name',
  !Bank.open({ name: 'A', type: 'SAVINGS', pin: '1234', deposit: 1000 }).ok);
check('rejects a 3-digit PIN',
  !Bank.open({ name: 'Test User', type: 'SAVINGS', pin: '123', deposit: 1000 }).ok);
check('rejects a deposit below the minimum',
  !Bank.open({ name: 'Test User', type: 'SAVINGS', pin: '1234', deposit: 100 }).ok);

const opened = Bank.open({ name: 'Test User', type: 'SAVINGS', pin: '1234', deposit: 1000 });
check('accepts a valid application', opened.ok);
check('assigns the first account number', opened.account.id === 100001);
check('never stores the PIN in plain text', opened.account.pin !== '1234');
check('logs an OPENING entry', opened.account.txns[0].kind === 'OPENING');

// ── authentication ──────────────────────────────────────────
console.log('\nauthentication');
check('signs in with the right PIN', Bank.login(100001, '1234').ok);
check('rejects the wrong PIN', !Bank.login(100001, '9999').ok);
check('rejects an unknown account', !Bank.login(999999, '1234').ok);

// ── deposits and withdrawals ────────────────────────────────
console.log('\ndeposits and withdrawals');
const acc = Bank.login(100001, '1234').account;
Bank.deposit(acc, 250.505);
check('rounds a deposit to 2 decimals', acc.balance === 1250.51);
check('rejects a negative deposit', !Bank.deposit(acc, -50).ok);
check('rejects a non-numeric deposit', !Bank.deposit(acc, 'abc').ok);

check('rejects an overdraft on savings', !Bank.withdraw(acc, 99999).ok);
Bank.withdraw(acc, 250.51);
check('withdrawal debits the balance', acc.balance === 1000);
check('ledger keeps the newest entry first', acc.txns[0].kind === 'WITHDRAW');

// ── overdraft on current accounts ───────────────────────────
console.log('\noverdraft');
const cur = Bank.open({ name: 'Current Holder', type: 'CURRENT', pin: '1111', deposit: 1000 }).account;
check('current account may use the overdraft',
  Bank.available(cur) === 1000 + RULES.OVERDRAFT);
check('overdraft withdrawal succeeds', Bank.withdraw(cur, 1400).ok);
check('balance goes negative', cur.balance === -400);
check('overdraft limit is enforced', !Bank.withdraw(cur, 200).ok);

// ── transfers ───────────────────────────────────────────────
console.log('\ntransfers');
Bank.deposit(cur, 1400);
const before = acc.balance + cur.balance;
check('rejects an unknown payee', !Bank.transfer(acc, 555555, 100).ok);
check('rejects a self transfer', !Bank.transfer(acc, acc.id, 100).ok);
check('transfer succeeds', Bank.transfer(acc, cur.id, 300).ok);
check('money is conserved', +(acc.balance + cur.balance).toFixed(2) === +before.toFixed(2));
check('sender sees TRANSFER-OUT', acc.txns[0].kind === 'TRANSFER-OUT');
check('payee sees TRANSFER-IN', cur.txns[0].kind === 'TRANSFER-IN');

// ── interest and reporting ──────────────────────────────────
console.log('\ninterest and reporting');
reset();
const s1 = Bank.open({ name: 'Saver', type: 'SAVINGS', pin: '1234', deposit: 1000 }).account;
const c1 = Bank.open({ name: 'Spender', type: 'CURRENT', pin: '1234', deposit: 1000 }).account;
const run = Bank.applyInterest();
check('credits every account in credit', run.credited === 2);
check('savings gets 4.5%', s1.balance === 1045);
check('current gets 1.2%', c1.balance === 1012);
check('total matches the sum credited', run.total === 57);

const stats = Bank.stats();
check('stats count accounts', stats.accounts === 2);
check('stats total deposits', stats.deposits === 2057);
check('directory sorts richest first', Bank.directory()[0].id === s1.id);
check('directory filters by name', Bank.directory('spend').length === 1);
check('directory filters by account number', Bank.directory('100001').length === 1);

// ── closing ─────────────────────────────────────────────────
console.log('\nclosing');
check('wrong PIN cannot close an account', !Bank.close(s1, '0000').ok);
check('correct PIN closes the account', Bank.close(s1, '1234').ok);
check('closed account is gone', Store.get(s1.id) === null);

console.log(`\n${passed} passed, ${failed} failed\n`);
process.exit(failed ? 1 : 0);
