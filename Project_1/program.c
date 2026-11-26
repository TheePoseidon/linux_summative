/*
 * program.c - Simple bank transaction processing system
 *
 * Build:  gcc -Wall -O0 -fno-inline -o program program.c
 *         strip program
 * Run:    ./program
 */

#include <stdio.h>
#include <stdlib.h>

#define NUM_TRANSACTIONS 6
#define OPENING_BALANCE  1000.00

/* Transaction type: deposit or withdrawal */
typedef enum {
    DEPOSIT,
    WITHDRAWAL
} TransactionType;

/* A single bank transaction */
typedef struct {
    int             id;      /* Transaction ID     */
    double          amount;  /* Transaction amount */
    TransactionType type;    /* Deposit/withdrawal */
} Transaction;

/* ---- Global variables ---- */
double account_balance = OPENING_BALANCE;  /* initialised   -> .data section */
int    rejected_count;                     /* uninitialised -> .bss section  */

/* ---- The three user-defined functions ---- */
Transaction *create_transactions(void);
void process_transactions(const Transaction *txns, int count,
                          double *total_deposits, double *total_withdrawals);
void print_summary(double total_deposits, double total_withdrawals);

int main(void)
{
    double total_deposits    = 0.0;
    double total_withdrawals = 0.0;

    Transaction *txns = create_transactions();
    if (txns == NULL) {
        fprintf(stderr, "Error: memory allocation failed\n");
        return EXIT_FAILURE;
    }

    process_transactions(txns, NUM_TRANSACTIONS,
                         &total_deposits, &total_withdrawals);
    print_summary(total_deposits, total_withdrawals);

    free(txns);      /* release heap memory */
    txns = NULL;

    return EXIT_SUCCESS;
}

/*
 * Allocates space for NUM_TRANSACTIONS transactions on the heap
 * and fills them with sample data. Returns NULL on failure.
 */
Transaction *create_transactions(void)
{
    const double amounts[NUM_TRANSACTIONS] = {
        500.00, 200.00, 1500.00, 750.00, 3000.00, 120.50
    };
    const TransactionType types[NUM_TRANSACTIONS] = {
        DEPOSIT, WITHDRAWAL, DEPOSIT, WITHDRAWAL, WITHDRAWAL, DEPOSIT
    };

    Transaction *txns = calloc(NUM_TRANSACTIONS, sizeof(Transaction));
    if (txns == NULL)
        return NULL;

    for (int i = 0; i < NUM_TRANSACTIONS; i++) {
        txns[i].id     = 1001 + i;
        txns[i].amount = amounts[i];
        txns[i].type   = types[i];
    }

    return txns;
}

/*
 * Applies each transaction to the global account balance and
 * accumulates the deposit and withdrawal totals. Withdrawals larger
 * than the current balance are rejected.
 */
void process_transactions(const Transaction *txns, int count,
                          double *total_deposits, double *total_withdrawals)
{
    printf("%-6s %-12s %12s %14s\n", "ID", "TYPE", "AMOUNT", "BALANCE");
    printf("------------------------------------------------\n");

    for (int i = 0; i < count; i++) {
        const Transaction *t = &txns[i];

        if (t->type == DEPOSIT) {
            account_balance   += t->amount;
            *total_deposits   += t->amount;
            printf("%-6d %-12s %12.2f %14.2f\n",
                   t->id, "DEPOSIT", t->amount, account_balance);
        } else if (t->type == WITHDRAWAL) {
            if (t->amount <= account_balance) {
                account_balance    -= t->amount;
                *total_withdrawals += t->amount;
                printf("%-6d %-12s %12.2f %14.2f\n",
                       t->id, "WITHDRAWAL", t->amount, account_balance);
            } else {
                rejected_count++;
                printf("%-6d %-12s %12.2f %14s\n",
                       t->id, "WITHDRAWAL", t->amount, "REJECTED");
            }
        }
    }
}

/* Prints the totals and the final balance. */
void print_summary(double total_deposits, double total_withdrawals)
{
    printf("------------------------------------------------\n");
    printf("Opening balance      : %10.2f\n", OPENING_BALANCE);
    printf("Total deposits       : %10.2f\n", total_deposits);
    printf("Total withdrawals    : %10.2f\n", total_withdrawals);
    printf("Rejected transactions: %10d\n",   rejected_count);
    printf("Final balance        : %10.2f\n", account_balance);
}