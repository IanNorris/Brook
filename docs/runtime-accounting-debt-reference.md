# Runtime service floor and consumed steal

Brook's service floor has an independent reference: the steal-accounting
recurrence in [Linux v6.12 update_rq_clock_task](https://github.com/torvalds/linux/blob/v6.12/kernel/sched/core.c#L3376).
That version caps the steal deducted from one interval to elapsed wall time and
advances its consumed-steal timestamp only by the deducted amount. The remaining
published steal carries into later intervals. The comparison here omits Linux's
separate IRQ deduction, because Brook deliberately includes kernel/IRQ work.
This is not complete Linux CPU-time equivalence or a statement about another
Linux version.

Within a valid monotone sampling epoch, let W be wall, S published steal, A
consumed steal and F Brook's service floor. Initialize A0=S0 and F0=W0−S0.
The independent recurrence is:

```
elapsed_i = W_i − W_(i−1)
deducted_i = min(S_i − A_(i−1), elapsed_i)
A_i = A_(i−1) + deducted_i
credit_i = elapsed_i − deducted_i
```

Its effective clock W_i−A_i equals:

```
max(W_(i−1) − A_(i−1), W_i − S_i)
```

By induction, this is Brook's existing F_i=max(F_(i−1),W_i−S_i).
The credited increment is F_i−F_(i−1). No tolerance, floor reset or retry until
a desirable sample is needed. Outstanding debt D_i=S_i−A_i is nonnegative and
equals F_i−(W_i−S_i). Per-epoch aggregate conservation is:

```
credited_i = (W_i−W0) − (S_i−S0) + D_i
```

A positive correction count is therefore compatible with correct handling of
valid inputs. It does not identify the provenance of those inputs or prove
negligible task attribution error. A delayed correction associated with one
owner can suppress later credit to another owner or idle. Zero final debt
establishes aggregate catch-up for that epoch, not accurate per-task attribution.

## Independent tests

`test_runtime_debt` compares a consumed-steal implementation, which keeps no
service high-water mark, with the actual RuntimeAccounting ledger. It covers:

- Three retained positive native adjacent pairs: deficits 7,682, 280 and
  13,246 ns; repeated observations below the floor; partial/exact catch-up;
  owner/group changes and subsequent actual credit.
- Equal timestamps, a large newly published correction, and subtraction near
  the signed 64-bit upper edge. These comparisons stay in the reference's
  representable delta domain rather than claiming arbitrary overflow behavior.
- 30,000 deterministic events across four CPU ledgers with distinct runnable
  owners, shared groups, idle transitions and corrections crossing ownership.
  Every boundary checks returned task credit, task/group totals and aggregate
  active/idle conservation against the independent recurrence.
- Invalid and backwards boundaries that explicitly discard epochs and known
  outstanding debt. A new valid baseline does not count as recovered debt.

All 73 hosted suites pass; the final added arithmetic edge cases also pass the
focused test. No production code or kernel behavior changes for this test.
The fixture's initial group-read assertion was corrected to query the ledger's
actual group-leader API; that assertion failure was a test error, not a new
kernel defect.

The three native pairs verify a positive correction step, not a complete
native ownership/history replay. The source proof and modeled owner changes
cannot substitute for missing real host publication/ownership evidence.
The separate 8.59-million-event native history capture is negative.

## Prospective acceptance work

Historical cohorts remain failed under their original zero-clamp gate. The
reference provides a reason to design a better prospective contract, without
rewriting those results or merely increasing a tolerated nanosecond count.
Before adopting one, retain correction count, maximum/final outstanding debt,
known debt discarded at unknown epochs, and corrections crossing ownership.
Keep invalid/backwards inputs, stale ownership and saturation as hard failures.
Treat unresolved final debt and task attribution uncertainty explicitly, and
replay a positive retained native history through the independent model.

No accounting acceptance rule, scheduler default, native sample retry or
service charge changes in this work. See the [cumulative failed evidence](runtime-clamp-evidence.md)
and [bounded history limitations](runtime-accounting-history.md).

The [debt observation follow-up](runtime-debt-observation.md) adds these
prospective diagnostic prerequisites and checks them against the independent
model after every event. Its native integration capture is negative; positive
full-history evidence and any new acceptance contract remain outstanding.
