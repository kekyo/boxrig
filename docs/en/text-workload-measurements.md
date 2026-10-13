# Text workload budgets and retained data

[日本語](../ja/text-workload-measurements.md)

This guide explains how to inspect work and memory usage for search, occurrence monitoring, regular-expression recognition, copying, and pasting.
The measurement driver constructs inputs with long histories or many matches and reports visited cells and bytes, allocations, and peak event-queue sizes.
See the [text guide](./text.md) for API usage and short examples.

## Features measured

The driver covers resumable exact search, initial monitor scans and change notifications, regular-expression recognition, and text copying and pasting.
It counts processed cells and bytes, public API work units, and allocations to check work and retention limits.
Elapsed time is not used to pass or fail the regular tests.

## Running the driver

Run from the repository root. See the [build guide](./building.md) for the required environment.

```sh
make measure-text-workloads
```

Each workload runs in a separate process and writes a line starting with `MEASURE text workload=...` to standard output.
Only the measurement build defines `BOXRIG_MEASURE_TEXT_WORKLOADS`; ordinary libraries contain no counters or measurement branches.
The default compiler settings are `-O2 -g`, without ASan/UBSan.
See [measure_text_workloads.c](../../tests/measure_text_workloads.c) for input construction and measurement code, and the [Makefile](../../Makefile) for build and execution targets.

After building, you can run the case that updates text while retaining many existing matches separately.

```sh
build/text-measurement/measure_text_workloads recognizer-retained-matches
```

The full test suite runs the driver's `self-test`, which checks counters and results with small inputs.
When saving results, also record the target commit, inputs, build settings, and execution environment.

## Inputs and metrics

| Workload | Input and purpose |
| --- | --- |
| `maximum-scrollback` | Unsuccessful search with one column and 1,048,576 history lines |
| `long-soft-wrap` | Unsuccessful search in one logical line containing 262,145 `a` characters |
| `many-matches` | Enumerate 65,536 `hit` lines during the initial monitor scan |
| `continuous-output` | Append 256 `hit` lines before scanning 1 MiB of initial data |
| `large-copy-paste` | Send a 1 MiB snapshot in sixteen 64 KiB chunks |
| `recognizer-long-line` | Recognize 1,048,576 `a` characters as one `a+` match |
| `recognizer-many-patterns` | Register 32 recognizers over 4,096 `a` characters |
| `recognizer-continuous-output` | Alternate the same logical line between `hit` and `miss` 4,096 times |
| `recognizer-off-viewport` | Recognize `target` at the beginning of a 262,143-line history |
| `recognizer-retained-matches` | Update another line while retaining matches on 1,024 lines, then check match identities |

The last case is run separately; `make measure-text-workloads` runs the first nine cases.
Those nine cases set unrelated capacities, such as images and notifications, to zero.
Event capacity includes room for the expected events and termination, keeping overflow checks separate from retention measurements.
Their four recognizer cases use a dirty-line capacity of one and also check that the entire unmatched history is not retained.

| Output field | Meaning |
| --- | --- |
| `fragments`, `projected_bytes` | Number of physical line fragments in history and the screen; UTF-8 byte count of the logical text being examined |
| `visited_cells`, `visited_bytes` | Data actually visited during hashing, projection, search, or monitoring |
| `work_units`, `dispatches` | Work consumed from the public budget; calls with a budget of 4,096 |
| `recognizers`, `events`, `matches` | Registered recognizers, consumed events, and matches |
| `input_bytes`, `paste_chunks` | Input bytes processed; chunks sent during pasting |
| `allocations`, `allocation_peak_bytes` | Allocation calls during the measurement interval; peak simultaneously live bytes from allocations started in that interval |
| `peak_rss_kib` | Maximum RSS over the process lifetime, including input construction |
| `event_queue_high_water`, `staging_queue_high_water` | Maximum simultaneously retained events in the monitor's public and staging queues |
| `recognizer_event_queue_high_water` | Maximum retained recognizer events |
| `recognizer_prepare_steps`, `recognizer_unbudgeted_prepare_steps` | Recognition steps for line-end, UTF-8, boundary, and preparation work; the number not charged to the budget |
| `recognizer_cursor_creations` | New allocations of NFA state storage |
| `recognizer_match_identity_comparisons` | Comparisons with retained match identities |

Each case measures a different set of fields; fields that are not assigned remain zero.
For example, `recognizer-retained-matches` checks identity preservation and comparison counts but does not measure RSS.

## Interpreting work and retained data

Exact search performs budgeted linear scans without an additional index.
Even a long logical line can be processed across multiple calls; `work_units` and `dispatches` report total work and resume counts.
Monitoring scans an immutable initial snapshot and holds subsequent events within a fixed capacity during that scan.
Content hashes are recalculated for changed lines.

Recognizers charge line-end scanning and UTF-8 preparation to the budget and reuse NFA state storage.
`recognizer_unbudgeted_prepare_steps` helps detect preparation work omitted from that accounting.
`recognizer-retained-matches` also checks that match identities on unchanged lines are preserved.

Copying creates an owned snapshot of the required range; pasting sends chunks through an existing transaction.
A zero `work_units` value in this case does not mean no copying or transmission occurred.
Distinguish resumable-search work units from copy/paste byte and chunk counts.

Linker wrappers count allocations.
When measurement starts after initial data has been constructed, `allocation_peak_bytes` excludes allocations for that data.
RSS includes the initial data, so the two metrics cover different scopes.

## Limits and related APIs

These measurements check work, retained data, and result consistency for the specified inputs and capacity settings.
RSS also depends on the OS and allocator.
Do not interpret these results as general interaction times or speed comparisons with other terminals.

See the [text guide](./text.md) for budgets, capacities, and retrieving results, and [Internal design](./development/internals.md) for current data management.
The [search transformation prototype](./search-capability-prototype.md) explains the differences between the Unicode transformation prototype and public features.

[Documentation index](./index.md)
