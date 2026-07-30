/*==============================================================================
 *  VAULT — an in-memory Bank Management System in pure C
 *  ---------------------------------------------------------------------------
 *  Data structures on display:
 *    * Chained HASH TABLE  ......  O(1) account lookup by account number
 *    * Singly LINKED LIST  ......  per-account transaction ledger (newest first)
 *    * UNDO STACK (LIFO)   ......  every balance-mutating op is reversible
 *    * Dynamic array + qsort ....  leaderboards & reports
 *    * FNV-1a 64 hash       .....  salted PIN digests (never stores plaintext)
 *
 *  Storage: in-memory only. Everything vanishes on exit, by design.
 *
 *  Build:  gcc -std=c11 -Wall -Wextra -O2 -o vault vault_bank.c
 *  Run  :  ./vault
 *============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#  include <conio.h>
#  include <windows.h>
#else
#  include <termios.h>
#  include <unistd.h>
#endif

/*--------------------------------------------------------------------------*/
/*  Terminal styling                                                        */
/*--------------------------------------------------------------------------*/
#define RST   "\033[0m"
#define BOLD  "\033[1m"
#define DIM   "\033[2m"
#define RED   "\033[38;5;203m"
#define GRN   "\033[38;5;114m"
#define YEL   "\033[38;5;221m"
#define BLU   "\033[38;5;75m"
#define MAG   "\033[38;5;176m"
#define CYN   "\033[38;5;80m"
#define GRY   "\033[38;5;245m"
#define WHT   "\033[38;5;255m"

#define BOX_W 66            /* inner width of every panel */

/*--------------------------------------------------------------------------*/
/*  Tunables                                                                */
/*--------------------------------------------------------------------------*/
#define TABLE_SIZE   211    /* prime -> good modulo dispersion              */
#define UNDO_MAX     64
#define NAME_LEN     48
#define ADMIN_PIN    "9999"
#define SAVINGS_RATE 0.045
#define CURRENT_RATE 0.012
#define OVERDRAFT    500.0  /* CURRENT accounts may dip this far below zero */
#define PIN_SALT     0x9E3779B97F4A7C15ULL

/*--------------------------------------------------------------------------*/
/*  Types                                                                   */
/*--------------------------------------------------------------------------*/
typedef enum { SAVINGS = 0, CURRENT = 1 } AccType;

typedef struct Txn {
    long        id;
    char        kind[14];      /* DEPOSIT / WITHDRAW / TRANSFER+ / ... */
    double      amount;
    double      balance_after;
    int         counterparty;  /* -1 when not applicable */
    time_t      when;
    struct Txn *next;          /* newest first */
} Txn;

typedef struct Account {
    int              id;
    char             name[NAME_LEN];
    AccType          type;
    unsigned long long pin;    /* salted digest */
    double           balance;
    time_t           opened;
    long             txn_count;
    Txn             *ledger;
    struct Account  *next;     /* hash-chain link */
} Account;

/* One reversible balance change. An operation may touch many accounts. */
typedef struct { int id; double delta; int pop; } Delta;

typedef struct {
    char   label[48];
    Delta *deltas;
    int    n;
    time_t when;
} Op;

/*--------------------------------------------------------------------------*/
/*  Globals                                                                 */
/*--------------------------------------------------------------------------*/
static Account *table[TABLE_SIZE];
static int   account_count = 0;
static int   next_id       = 100001;
static long  next_txn      = 1;

static Op    undo_stack[UNDO_MAX];
static int   undo_top = 0;

/*--------------------------------------------------------------------------*/
/*  Small platform helpers                                                  */
/*--------------------------------------------------------------------------*/
static void nap(int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static int get_hidden_char(void)
{
#ifdef _WIN32
    return _getch();
#else
    struct termios old, tmp;
    int c;
    if (tcgetattr(STDIN_FILENO, &old) != 0) return getchar();
    tmp = old;
    tmp.c_lflag &= (unsigned)~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &tmp);
    c = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    return c;
#endif
}

/*--------------------------------------------------------------------------*/
/*  Panel drawing                                                           */
/*--------------------------------------------------------------------------*/
static void line(const char *l, const char *r)
{
    printf("%s%s", CYN, l);
    for (int i = 0; i < BOX_W; i++) printf("─");
    printf("%s%s\n", r, RST);
}
static void box_top(void) { line("╭", "╮"); }
static void box_mid(void) { line("├", "┤"); }
static void box_bot(void) { line("╰", "╯"); }

/* Visible width: count UTF-8 lead bytes, ignore continuation bytes. */
static int dwidth(const char *s)
{
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) w++;
    return w;
}

/* Print one padded row. `fmt` must be plain text (no escape codes). */
static void row(const char *color, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    int pad = BOX_W - 2 - dwidth(buf);
    if (pad < 0) pad = 0;
    printf("%s│%s %s%s%s%*s %s│%s\n", CYN, RST, color, buf, RST, pad, "", CYN, RST);
}

static void row_center(const char *color, const char *text)
{
    int pad = (BOX_W - 2 - dwidth(text)) / 2;
    char buf[512];
    if (pad < 0) pad = 0;
    snprintf(buf, sizeof buf, "%*s%s", pad, "", text);
    row(color, "%s", buf);
}

static void banner(const char *title, const char *subtitle)
{
    printf("\n");
    box_top();
    row_center(BOLD MAG, "V A U L T   B A N K");
    row_center(GRY, "in-memory core \xE2\x80\xA2 hash + list + undo stack");
    box_mid();
    row_center(BOLD WHT, title);
    if (subtitle && *subtitle) row_center(GRY, subtitle);
    box_mid();
}

static void toast(const char *color, const char *icon, const char *fmt, ...)
{
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("  %s%s %s%s\n", color, icon, buf, RST);
}

static void spinner(const char *label, int steps)
{
    const char *frames = "|/-\\";
    printf("  %s%s ", GRY, label);
    fflush(stdout);
    for (int i = 0; i < steps; i++) {
        printf("%c\b", frames[i % 4]);
        fflush(stdout);
        nap(45);
    }
    printf("%sok%s\n", GRN, RST);
}

static void progress(const char *label)
{
    const int width = 28;
    printf("  %s%-14s%s[", GRY, label, RST);
    for (int i = 0; i < width; i++) putchar(' ');
    printf("]");
    for (int i = 0; i < width + 1; i++) putchar('\b');
    fflush(stdout);
    for (int i = 0; i < width; i++) {
        printf("%s█%s", GRN, RST);
        fflush(stdout);
        nap(18);
    }
    printf("]\n");
}

static void pause_screen(void)
{
    printf("  %spress ENTER to continue%s", DIM, RST);
    fflush(stdout);
    int c;
    while ((c = getchar()) != '\n' && c != EOF) { }
    printf("\n");
}

/*--------------------------------------------------------------------------*/
/*  Input helpers                                                           */
/*--------------------------------------------------------------------------*/
static int read_line(const char *prompt, char *out, size_t n)
{
    printf("  %s❯%s %s", CYN, RST, prompt);
    fflush(stdout);
    if (!fgets(out, (int)n, stdin)) { out[0] = '\0'; return 0; }
    out[strcspn(out, "\r\n")] = '\0';
    return 1;
}

static int read_int(const char *prompt, int *out)
{
    char buf[64], *end;
    if (!read_line(prompt, buf, sizeof buf) || buf[0] == '\0') return 0;
    long v = strtol(buf, &end, 10);
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end != '\0') return 0;
    *out = (int)v;
    return 1;
}

static int read_money(const char *prompt, double *out)
{
    char buf[64], *end;
    if (!read_line(prompt, buf, sizeof buf) || buf[0] == '\0') return 0;
    double v = strtod(buf, &end);
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end != '\0' || v <= 0.0 || v > 1e9) return 0;
    *out = v;
    return 1;
}

/* Masked 4-digit PIN entry. Falls back to plain read when stdin is a pipe. */
static int read_pin(const char *prompt, char *out, size_t n)
{
    size_t i = 0;
#ifndef _WIN32
    if (!isatty(STDIN_FILENO)) return read_line(prompt, out, n);
#endif
    printf("  %s❯%s %s", CYN, RST, prompt);
    fflush(stdout);
    for (;;) {
        int c = get_hidden_char();
        if (c == '\n' || c == '\r' || c == EOF) break;
        if ((c == 127 || c == 8) && i > 0) { i--; printf("\b \b"); fflush(stdout); continue; }
        if (isdigit(c) && i < n - 1) { out[i++] = (char)c; printf("%s•%s", YEL, RST); fflush(stdout); }
    }
    out[i] = '\0';
    printf("\n");
    return (int)i;
}

/*--------------------------------------------------------------------------*/
/*  Hashing                                                                 */
/*--------------------------------------------------------------------------*/
static unsigned long long fnv1a(const char *s, unsigned long long seed)
{
    unsigned long long h = 1469598103934665603ULL ^ seed;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}
static unsigned long long pin_digest(int id, const char *pin)
{
    char mix[80];
    snprintf(mix, sizeof mix, "%d:%s", id, pin);
    return fnv1a(mix, PIN_SALT);
}
static int bucket_of(int id) { return (int)(((unsigned)id * 2654435761u) % TABLE_SIZE); }

/*--------------------------------------------------------------------------*/
/*  Hash table operations                                                   */
/*--------------------------------------------------------------------------*/
static Account *find_account(int id)
{
    for (Account *a = table[bucket_of(id)]; a; a = a->next)
        if (a->id == id) return a;
    return NULL;
}

static Account *insert_account(const char *name, AccType type,
                               const char *pin, double opening)
{
    Account *a = calloc(1, sizeof *a);
    if (!a) { toast(RED, "✖", "out of memory"); exit(EXIT_FAILURE); }
    a->id      = next_id++;
    snprintf(a->name, sizeof a->name, "%s", name);
    a->type    = type;
    a->pin     = pin_digest(a->id, pin);
    a->balance = opening;
    a->opened  = time(NULL);

    int b = bucket_of(a->id);
    a->next  = table[b];
    table[b] = a;
    account_count++;
    return a;
}

static void free_ledger(Txn *t)
{
    while (t) { Txn *nx = t->next; free(t); t = nx; }
}

static int remove_account(int id)
{
    int b = bucket_of(id);
    Account *cur = table[b], *prev = NULL;
    while (cur && cur->id != id) { prev = cur; cur = cur->next; }
    if (!cur) return 0;
    if (prev) prev->next = cur->next; else table[b] = cur->next;
    free_ledger(cur->ledger);
    free(cur);
    account_count--;
    return 1;
}

/*--------------------------------------------------------------------------*/
/*  Ledger (linked list)                                                    */
/*--------------------------------------------------------------------------*/
static void post(Account *a, const char *kind, double amount, int counterparty)
{
    Txn *t = calloc(1, sizeof *t);
    if (!t) return;
    t->id            = next_txn++;
    snprintf(t->kind, sizeof t->kind, "%s", kind);
    t->amount        = amount;
    t->balance_after = a->balance;      /* caller updates balance first */
    t->counterparty  = counterparty;
    t->when          = time(NULL);
    t->next          = a->ledger;       /* prepend: newest at head */
    a->ledger        = t;
    a->txn_count++;
}

static void pop_txn(Account *a)
{
    if (!a->ledger) return;
    Txn *t = a->ledger;
    a->ledger = t->next;
    free(t);
    a->txn_count--;
}

/*--------------------------------------------------------------------------*/
/*  Undo stack                                                              */
/*--------------------------------------------------------------------------*/
static void push_op(const char *label, Delta *d, int n)
{
    if (n <= 0) return;
    if (undo_top == UNDO_MAX) {                 /* drop the oldest entry */
        free(undo_stack[0].deltas);
        memmove(&undo_stack[0], &undo_stack[1], sizeof(Op) * (UNDO_MAX - 1));
        undo_top--;
    }
    Op *op = &undo_stack[undo_top++];
    snprintf(op->label, sizeof op->label, "%s", label);
    op->deltas = malloc(sizeof(Delta) * (size_t)n);
    if (!op->deltas) { undo_top--; return; }
    memcpy(op->deltas, d, sizeof(Delta) * (size_t)n);
    op->n    = n;
    op->when = time(NULL);
}

static int undo_last(char *what, size_t wn)
{
    if (undo_top == 0) return 0;
    Op *op = &undo_stack[--undo_top];
    for (int i = 0; i < op->n; i++) {
        Account *a = find_account(op->deltas[i].id);
        if (!a) continue;
        a->balance -= op->deltas[i].delta;
        for (int k = 0; k < op->deltas[i].pop; k++) pop_txn(a);
    }
    snprintf(what, wn, "%s", op->label);
    free(op->deltas);
    op->deltas = NULL;
    op->n = 0;
    return 1;
}

/*--------------------------------------------------------------------------*/
/*  Formatting                                                              */
/*--------------------------------------------------------------------------*/
static const char *type_name(AccType t) { return t == SAVINGS ? "SAVINGS" : "CURRENT"; }

static const char *money(double v)
{
    static char buf[4][40];
    static int  slot = 0;
    char raw[40];
    char *p = buf[slot = (slot + 1) & 3];
    snprintf(raw, sizeof raw, "%.2f", v < 0 ? -v : v);

    /* group the integer part in 3s */
    char *dot = strchr(raw, '.');
    int   ilen = (int)(dot - raw);
    int   o = 0;
    if (v < 0) p[o++] = '-';
    for (int i = 0; i < ilen; i++) {
        if (i && (ilen - i) % 3 == 0) p[o++] = ',';
        p[o++] = raw[i];
    }
    snprintf(p + o, 40 - (size_t)o, "%s", dot);
    return p;
}

static const char *stamp(time_t t)
{
    static char buf[32];
    struct tm *tm = localtime(&t);
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", tm);
    return buf;
}

/*--------------------------------------------------------------------------*/
/*  Receipts                                                                */
/*--------------------------------------------------------------------------*/
static void receipt(const char *title, Account *a, double amt, const char *note)
{
    printf("\n      %s┌────────────────────────────────────┐%s\n", GRN, RST);
    printf("      %s│%s  %s%-33s%s %s│%s\n", GRN, RST, BOLD, title, RST, GRN, RST);
    printf("      %s│%s  %-33s %s│%s\n", GRN, RST, stamp(time(NULL)), GRN, RST);
    printf("      %s│%s  %-33s %s│%s\n", GRN, RST, "- - - - - - - - - - - - - - - - -", GRN, RST);
    printf("      %s│%s  acct   %-26d %s│%s\n", GRN, RST, a->id, GRN, RST);
    printf("      %s│%s  amount %-26s %s│%s\n", GRN, RST, money(amt), GRN, RST);
    printf("      %s│%s  bal    %-26s %s│%s\n", GRN, RST, money(a->balance), GRN, RST);
    if (note && *note)
        printf("      %s│%s  %-33s %s│%s\n", GRN, RST, note, GRN, RST);
    printf("      %s│%s  %-33s %s│%s\n", GRN, RST, "reversible from admin console", GRN, RST);
    printf("      %s└────────────────────────────────────┘%s\n\n", GRN, RST);
}

/*--------------------------------------------------------------------------*/
/*  Account operations                                                      */
/*--------------------------------------------------------------------------*/
static void do_open_account(void)
{
    char name[NAME_LEN], pin1[16], pin2[16];
    int  choice = 0;
    double opening = 0;

    banner("OPEN AN ACCOUNT", "minimum opening balance 500.00");
    row(GRY, "SAVINGS  •  %.1f%% interest, no overdraft", SAVINGS_RATE * 100);
    row(GRY, "CURRENT  •  %.1f%% interest, overdraft up to %s",
        CURRENT_RATE * 100, money(OVERDRAFT));
    box_bot();
    printf("\n");

    if (!read_line("full name        : ", name, sizeof name) || strlen(name) < 2) {
        toast(RED, "✖", "name must be at least 2 characters."); return;
    }
    if (!read_int("type (1=SAV 2=CUR): ", &choice) || (choice != 1 && choice != 2)) {
        toast(RED, "✖", "pick 1 or 2."); return;
    }
    if (!read_money("opening deposit  : ", &opening) || opening < 500.0) {
        toast(RED, "✖", "opening deposit must be at least 500.00."); return;
    }
    if (read_pin("choose 4-digit PIN: ", pin1, sizeof pin1) != 4) {
        toast(RED, "✖", "PIN must be exactly 4 digits."); return;
    }
    read_pin("confirm PIN       : ", pin2, sizeof pin2);
    if (strcmp(pin1, pin2) != 0) { toast(RED, "✖", "PINs did not match."); return; }

    Account *a = insert_account(name, choice == 1 ? SAVINGS : CURRENT, pin1, opening);
    post(a, "OPENING", opening, -1);

    Delta d = { a->id, opening, 1 };
    char label[48];
    snprintf(label, sizeof label, "OPEN account %d", a->id);
    push_op(label, &d, 1);

    progress("provisioning");
    printf("\n");
    box_top();
    row_center(BOLD GRN, "ACCOUNT CREATED");
    box_mid();
    row(WHT,  "account number : %d", a->id);
    row(WHT,  "holder         : %s", a->name);
    row(WHT,  "type           : %s", type_name(a->type));
    row(GRN,  "balance        : %s", money(a->balance));
    row(GRY,  "bucket         : #%d of %d", bucket_of(a->id), TABLE_SIZE);
    box_bot();
    toast(YEL, "⚠", "write the account number down — nothing is saved to disk.");
    printf("\n");
}

static Account *authenticate(void)
{
    int id;
    char pin[16];
    if (!read_int("account number : ", &id)) { toast(RED, "✖", "invalid number."); return NULL; }
    read_pin("PIN            : ", pin, sizeof pin);

    spinner("verifying ", 8);
    Account *a = find_account(id);
    if (!a || a->pin != pin_digest(id, pin)) {
        toast(RED, "✖", "account number or PIN is wrong.");
        return NULL;
    }
    return a;
}

static void do_deposit(Account *a)
{
    double amt;
    if (!read_money("amount to deposit: ", &amt)) { toast(RED, "✖", "invalid amount."); return; }
    a->balance += amt;
    post(a, "DEPOSIT", amt, -1);

    Delta d = { a->id, amt, 1 };
    char label[48];
    snprintf(label, sizeof label, "DEPOSIT %s to %d", money(amt), a->id);
    push_op(label, &d, 1);

    progress("clearing");
    receipt("DEPOSIT RECEIPT", a, amt, NULL);
}

static void do_withdraw(Account *a)
{
    double amt;
    if (!read_money("amount to withdraw: ", &amt)) { toast(RED, "✖", "invalid amount."); return; }
    double floor_ = (a->type == CURRENT) ? -OVERDRAFT : 0.0;
    if (a->balance - amt < floor_) {
        toast(RED, "✖", "declined — available: %s", money(a->balance - floor_));
        return;
    }
    a->balance -= amt;
    post(a, "WITHDRAW", amt, -1);

    Delta d = { a->id, -amt, 1 };
    char label[48];
    snprintf(label, sizeof label, "WITHDRAW %s from %d", money(amt), a->id);
    push_op(label, &d, 1);

    progress("dispensing");
    receipt("WITHDRAWAL RECEIPT", a, amt,
            a->balance < 0 ? "overdraft in use" : NULL);
}

static void do_transfer(Account *a)
{
    int to;
    double amt;
    if (!read_int("payee account   : ", &to)) { toast(RED, "✖", "invalid account."); return; }
    if (to == a->id) { toast(RED, "✖", "cannot transfer to yourself."); return; }
    Account *b = find_account(to);
    if (!b) { toast(RED, "✖", "payee not found."); return; }
    if (!read_money("amount          : ", &amt)) { toast(RED, "✖", "invalid amount."); return; }

    double floor_ = (a->type == CURRENT) ? -OVERDRAFT : 0.0;
    if (a->balance - amt < floor_) {
        toast(RED, "✖", "declined — available: %s", money(a->balance - floor_));
        return;
    }

    printf("  %sconfirm: send %s to %d (%s)? [y/N] %s", YEL, money(amt), b->id, b->name, RST);
    fflush(stdout);
    char ans[16];
    if (!fgets(ans, sizeof ans, stdin) || (ans[0] != 'y' && ans[0] != 'Y')) {
        toast(GRY, "○", "cancelled."); return;
    }

    a->balance -= amt;  post(a, "TRANSFER-", amt, b->id);
    b->balance += amt;  post(b, "TRANSFER+", amt, a->id);

    Delta d[2] = { { a->id, -amt, 1 }, { b->id, amt, 1 } };
    char label[48];
    snprintf(label, sizeof label, "TRANSFER %d → %d", a->id, b->id);
    push_op(label, d, 2);

    progress("settling");
    char note[48];
    snprintf(note, sizeof note, "to %d (%.20s)", b->id, b->name);
    receipt("TRANSFER RECEIPT", a, amt, note);
}

static void do_statement(Account *a)
{
    int limit = 10;
    banner("MINI STATEMENT", a->name);
    row(GRY, "%-4s %-10s %12s %14s  %s", "#", "type", "amount", "balance", "when");
    box_mid();

    if (!a->ledger) {
        row_center(GRY, "no transactions yet");
    } else {
        int i = 0;
        for (Txn *t = a->ledger; t && i < limit; t = t->next, i++) {
            int debit = (strcmp(t->kind, "WITHDRAW") == 0 ||
                         strcmp(t->kind, "TRANSFER-") == 0);
            const char *c = debit ? RED : GRN;
            char amt[32];
            snprintf(amt, sizeof amt, "%c%s", debit ? '-' : '+', money(t->amount));
            row(c, "%-4ld %-10s %12s %14s  %s",
                t->id, t->kind, amt, money(t->balance_after), stamp(t->when));
        }
    }
    box_mid();
    row(WHT, "current balance : %s", money(a->balance));
    row(GRY, "lifetime entries: %ld  (showing newest %d)", a->txn_count, limit);
    box_bot();
    printf("\n");
}

static void do_change_pin(Account *a)
{
    char old[16], p1[16], p2[16];
    read_pin("current PIN : ", old, sizeof old);
    if (a->pin != pin_digest(a->id, old)) { toast(RED, "✖", "wrong PIN."); return; }
    if (read_pin("new PIN     : ", p1, sizeof p1) != 4) { toast(RED, "✖", "PIN must be 4 digits."); return; }
    read_pin("repeat      : ", p2, sizeof p2);
    if (strcmp(p1, p2) != 0) { toast(RED, "✖", "PINs did not match."); return; }
    a->pin = pin_digest(a->id, p1);
    toast(GRN, "✔", "PIN updated. Digest rotated with a fresh salt round.");
}

static int do_close(Account *a)
{
    char pin[16];
    toast(YEL, "⚠", "closing pays out %s and erases the ledger.", money(a->balance));
    read_pin("confirm with PIN: ", pin, sizeof pin);
    if (a->pin != pin_digest(a->id, pin)) { toast(RED, "✖", "wrong PIN. Cancelled."); return 0; }
    if (a->balance < 0) { toast(RED, "✖", "settle the overdraft first."); return 0; }
    int id = a->id;
    remove_account(id);
    toast(GRN, "✔", "account %d closed and unlinked from bucket #%d.", id, bucket_of(id));
    return 1;
}

/*--------------------------------------------------------------------------*/
/*  Customer session                                                        */
/*--------------------------------------------------------------------------*/
static void customer_session(Account *a)
{
    for (;;) {
        char sub[96];
        snprintf(sub, sizeof sub, "%s • %s • %s",
                 a->name, type_name(a->type), money(a->balance));
        banner("CUSTOMER CONSOLE", sub);
        row(WHT, " 1  Deposit");
        row(WHT, " 2  Withdraw");
        row(WHT, " 3  Transfer to another account");
        row(WHT, " 4  Mini statement");
        row(WHT, " 5  Balance enquiry");
        row(WHT, " 6  Change PIN");
        row(WHT, " 7  Close account");
        row(GRY, " 0  Sign out");
        box_bot();
        printf("\n");

        int c;
        if (!read_int("select : ", &c)) { toast(RED, "✖", "enter a number."); continue; }
        printf("\n");
        switch (c) {
            case 1: do_deposit(a);   break;
            case 2: do_withdraw(a);  break;
            case 3: do_transfer(a);  break;
            case 4: do_statement(a); break;
            case 5:
                box_top();
                row_center(BOLD WHT, "BALANCE");
                row_center(GRN, money(a->balance));
                row_center(GRY, a->type == CURRENT ? "overdraft available" : "no overdraft on savings");
                box_bot();
                printf("\n");
                break;
            case 6: do_change_pin(a); break;
            case 7: if (do_close(a)) { printf("\n"); return; } break;
            case 0: toast(GRY, "○", "signed out."); printf("\n"); return;
            default: toast(RED, "✖", "no such option.");
        }
        pause_screen();
    }
}

/*--------------------------------------------------------------------------*/
/*  Admin: reports over the hash table                                      */
/*--------------------------------------------------------------------------*/
static Account **snapshot(int *n)
{
    Account **arr = malloc(sizeof(Account *) * (size_t)(account_count ? account_count : 1));
    int k = 0;
    if (!arr) { *n = 0; return NULL; }
    for (int b = 0; b < TABLE_SIZE; b++)
        for (Account *a = table[b]; a; a = a->next) arr[k++] = a;
    *n = k;
    return arr;
}

static int cmp_balance(const void *x, const void *y)
{
    const Account *a = *(Account * const *)x, *b = *(Account * const *)y;
    if (a->balance < b->balance) return 1;
    if (a->balance > b->balance) return -1;
    return a->id - b->id;
}

static void admin_directory(void)
{
    int n;
    Account **arr = snapshot(&n);
    banner("ACCOUNT DIRECTORY", "sorted by balance, descending");
    if (n == 0) {
        row_center(GRY, "no accounts on the books");
    } else {
        qsort(arr, (size_t)n, sizeof *arr, cmp_balance);
        row(GRY, "%-8s %-22s %-8s %14s %6s", "acct", "holder", "type", "balance", "txns");
        box_mid();
        for (int i = 0; i < n; i++) {
            Account *a = arr[i];
            row(a->balance < 0 ? RED : WHT, "%-8d %-22.22s %-8s %14s %6ld",
                a->id, a->name, type_name(a->type), money(a->balance), a->txn_count);
        }
    }
    box_mid();
    double total = 0;
    for (int i = 0; i < n; i++) total += arr[i]->balance;
    row(BOLD GRN, "accounts: %-6d           total deposits: %s", n, money(total));
    box_bot();
    free(arr);
    printf("\n");
}

static void admin_hash_stats(void)
{
    int used = 0, longest = 0, collisions = 0;
    for (int b = 0; b < TABLE_SIZE; b++) {
        int len = 0;
        for (Account *a = table[b]; a; a = a->next) len++;
        if (len) used++;
        if (len > 1) collisions += len - 1;
        if (len > longest) longest = len;
    }
    banner("HASH TABLE DIAGNOSTICS", "Knuth multiplicative hash, separate chaining");
    row(WHT, "table size        : %d buckets (prime)", TABLE_SIZE);
    row(WHT, "accounts stored   : %d", account_count);
    row(WHT, "buckets occupied  : %d", used);
    row(WHT, "load factor       : %.3f", (double)account_count / TABLE_SIZE);
    row(WHT, "collisions        : %d", collisions);
    row(WHT, "longest chain     : %d  (avg lookup ≈ %.2f probes)",
        longest, used ? 1.0 + (double)collisions / (used * 2.0) : 0.0);
    box_mid();
    row(GRY, "occupancy map (first 60 buckets)");
    char mapbuf[80];
    int shown = TABLE_SIZE < 60 ? TABLE_SIZE : 60, o = 0;
    for (int b = 0; b < shown; b++) {
        int len = 0;
        for (Account *a = table[b]; a; a = a->next) len++;
        mapbuf[o++] = len == 0 ? '.' : (len == 1 ? 'o' : (len == 2 ? 'O' : '#'));
    }
    mapbuf[o] = '\0';
    row(CYN, "%s", mapbuf);
    row(GRY, ". empty   o one   O two   # three or more");
    box_bot();
    printf("\n");
}

static void admin_interest(void)
{
    int n;
    Account **arr = snapshot(&n);
    if (n == 0) { toast(GRY, "○", "no accounts to credit."); free(arr); return; }

    Delta *d = malloc(sizeof(Delta) * (size_t)n);
    if (!d) { free(arr); return; }
    int k = 0;
    double paid = 0;

    banner("INTEREST RUN", "one annual credit applied to every account");
    for (int i = 0; i < n; i++) {
        Account *a = arr[i];
        double rate = (a->type == SAVINGS) ? SAVINGS_RATE : CURRENT_RATE;
        double amt  = a->balance > 0 ? a->balance * rate : 0.0;
        if (amt <= 0.004) { row(GRY, "%-8d %-22.22s  skipped (no positive balance)", a->id, a->name); continue; }
        amt = (double)((long)(amt * 100 + 0.5)) / 100.0;
        a->balance += amt;
        post(a, "INTEREST", amt, -1);
        d[k].id = a->id; d[k].delta = amt; d[k].pop = 1; k++;
        paid += amt;
        row(GRN, "%-8d %-22.22s  +%-12s → %s", a->id, a->name, money(amt), money(a->balance));
    }
    box_mid();
    row(BOLD WHT, "credited %d account(s), total interest paid: %s", k, money(paid));
    row(GRY, "pushed to the undo stack as a single reversible operation");
    box_bot();

    push_op("INTEREST RUN (all accounts)", d, k);
    free(d);
    free(arr);
    printf("\n");
}

static void admin_search(void)
{
    char q[NAME_LEN];
    if (!read_line("name contains : ", q, sizeof q) || !q[0]) { toast(RED, "✖", "empty query."); return; }
    for (char *p = q; *p; p++) *p = (char)tolower((unsigned char)*p);

    banner("DIRECTORY SEARCH", q);
    int hits = 0;
    for (int b = 0; b < TABLE_SIZE; b++) {
        for (Account *a = table[b]; a; a = a->next) {
            char low[NAME_LEN];
            snprintf(low, sizeof low, "%s", a->name);
            for (char *p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
            if (strstr(low, q)) {
                row(WHT, "%-8d %-24.24s %-8s %14s",
                    a->id, a->name, type_name(a->type), money(a->balance));
                hits++;
            }
        }
    }
    if (!hits) row_center(GRY, "no matches");
    box_bot();
    printf("\n");
}

static void admin_undo(void)
{
    banner("UNDO STACK", "LIFO — balance and ledger roll back together");
    if (undo_top == 0) {
        row_center(GRY, "stack is empty");
        box_bot();
        printf("\n");
        return;
    }
    for (int i = undo_top - 1, shown = 0; i >= 0 && shown < 8; i--, shown++)
        row(shown == 0 ? YEL : GRY, "%s%-30.30s %s  x%d",
            shown == 0 ? "top > " : "      ",
            undo_stack[i].label, stamp(undo_stack[i].when), undo_stack[i].n);
    box_bot();
    printf("\n");

    char ans[16];
    printf("  %spop and reverse the top operation? [y/N] %s", YEL, RST);
    fflush(stdout);
    if (!fgets(ans, sizeof ans, stdin) || (ans[0] != 'y' && ans[0] != 'Y')) {
        toast(GRY, "○", "left untouched."); return;
    }
    char what[64];
    if (undo_last(what, sizeof what)) {
        spinner("rewinding ", 10);
        toast(GRN, "✔", "reversed: %s", what);
    }
}

static void admin_console(void)
{
    char pin[16];
    banner("ADMIN AUTHENTICATION", "restricted console");
    box_bot();
    printf("\n");
    read_pin("admin PIN : ", pin, sizeof pin);
    spinner("checking  ", 8);
    if (strcmp(pin, ADMIN_PIN) != 0) { toast(RED, "✖", "access denied."); printf("\n"); return; }
    toast(GRN, "✔", "welcome, administrator.");
    printf("\n");

    for (;;) {
        char sub[80];
        snprintf(sub, sizeof sub, "%d accounts • %d undoable ops", account_count, undo_top);
        banner("ADMIN CONSOLE", sub);
        row(WHT, " 1  Account directory & totals");
        row(WHT, " 2  Search holders by name");
        row(WHT, " 3  Hash table diagnostics");
        row(WHT, " 4  Run interest credit");
        row(WHT, " 5  Undo stack / reverse an operation");
        row(WHT, " 6  Inspect a single account ledger");
        row(GRY, " 0  Leave console");
        box_bot();
        printf("\n");

        int c;
        if (!read_int("select : ", &c)) { toast(RED, "✖", "enter a number."); continue; }
        printf("\n");
        switch (c) {
            case 1: admin_directory();  break;
            case 2: admin_search();     break;
            case 3: admin_hash_stats(); break;
            case 4: admin_interest();   break;
            case 5: admin_undo();       break;
            case 6: {
                int id;
                if (!read_int("account number : ", &id)) { toast(RED, "✖", "invalid."); break; }
                Account *a = find_account(id);
                if (!a) { toast(RED, "✖", "not found."); break; }
                do_statement(a);
                break;
            }
            case 0: printf("\n"); return;
            default: toast(RED, "✖", "no such option.");
        }
        pause_screen();
    }
}

/*--------------------------------------------------------------------------*/
/*  Demo seed + teardown                                                    */
/*--------------------------------------------------------------------------*/
static void seed_demo(void)
{
    struct { const char *name; AccType t; const char *pin; double bal; } s[] = {
        { "Arpan Das",       SAVINGS, "1234", 42500 },
        { "Mira Chatterjee", CURRENT, "1111",  8300 },
        { "Devlin Roy",      SAVINGS, "2222", 15750 },
    };
    for (unsigned i = 0; i < sizeof s / sizeof s[0]; i++) {
        Account *a = insert_account(s[i].name, s[i].t, s[i].pin, s[i].bal);
        post(a, "OPENING", s[i].bal, -1);
    }
}

static void teardown(void)
{
    for (int b = 0; b < TABLE_SIZE; b++) {
        Account *a = table[b];
        while (a) { Account *nx = a->next; free_ledger(a->ledger); free(a); a = nx; }
        table[b] = NULL;
    }
    for (int i = 0; i < undo_top; i++) free(undo_stack[i].deltas);
    undo_top = 0;
    account_count = 0;
}

/*--------------------------------------------------------------------------*/
/*  Main                                                                    */
/*--------------------------------------------------------------------------*/
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    seed_demo();

    printf("\n");
    box_top();
    row_center(BOLD MAG, "V A U L T   B A N K   —   c o l d   s t a r t");
    box_mid();
    row(GRY, "hash table  : %d buckets, separate chaining", TABLE_SIZE);
    row(GRY, "ledger      : per-account singly linked list, newest first");
    row(GRY, "undo        : LIFO stack, depth %d", UNDO_MAX);
    row(GRY, "persistence : none — memory only, wiped on exit");
    box_mid();
    row(YEL, "demo logins : 100001/1234  100002/1111  100003/2222");
    row(YEL, "admin PIN   : %s", ADMIN_PIN);
    box_bot();

    for (;;) {
        banner("MAIN MENU", "select an option");
        row(WHT, " 1  Open a new account");
        row(WHT, " 2  Customer login");
        row(WHT, " 3  Admin console");
        row(WHT, " 4  About this build");
        row(GRY, " 0  Shut down");
        box_bot();
        printf("\n");

        int c;
        if (!read_int("select : ", &c)) {
            if (feof(stdin)) break;
            toast(RED, "✖", "enter a number.");
            continue;
        }
        printf("\n");

        switch (c) {
            case 1: do_open_account(); pause_screen(); break;
            case 2: {
                banner("CUSTOMER LOGIN", "PIN input is masked");
                box_bot();
                printf("\n");
                Account *a = authenticate();
                printf("\n");
                if (a) customer_session(a); else pause_screen();
                break;
            }
            case 3: admin_console(); break;
            case 4:
                banner("ABOUT", "VAULT — single translation unit, C11, zero dependencies");
                row(WHT, "lookup    O(1) avg   hash table with chaining");
                row(WHT, "statement O(k)       walk k nodes of the ledger list");
                row(WHT, "undo      O(m)       pop one op, reverse m deltas");
                row(WHT, "reports   O(n log n) snapshot + qsort");
                row(WHT, "PIN       FNV-1a 64 digest, salted per account id");
                box_mid();
                row(GRY, "Nothing here touches the filesystem or the network.");
                box_bot();
                printf("\n");
                pause_screen();
                break;
            case 0:
                spinner("flushing memory ", 12);
                toast(GRY, "○", "%d account(s) discarded. Goodbye.", account_count);
                printf("\n");
                teardown();
                return 0;
            default: toast(RED, "✖", "no such option."); break;
        }
    }

    teardown();
    return 0;
}
