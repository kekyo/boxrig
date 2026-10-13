# Search transformation prototype evaluation

[日本語](../ja/search-capability-prototype.md)

This record documents evaluation and decisions for a prototype adding case conversion and Unicode normalization to text search.
It states the decision first, then explains transformations, mapping to original positions, resource budgets, and regular expressions.
See the [text guide](./text.md) for currently available search and recognition APIs.

## Purpose and decision

The evaluation investigated adding Unicode case conversion, normalization, and regular expressions to search with bounded work and memory.
Current exact search matches UTF-8 bytes and has no case-folding or normalization flags.

The prototype verified transformed matches and mapping back to original text ranges.
However, it could not suspend and resume a single transformation under an integer work budget while also bounding allocation inside the external library, so the addition to exact search was not adopted.

## Prototype versus public features

The prototype is in [search_transform_prototype.c](../../tests/search_transform_prototype.c) and is not linked into the product library.
It uses pinned utf8proc 2.11.3 and Unicode 17.0.0 and tests transformations of all 1,112,064 Unicode scalar values.

Regular-expression recognition added later is a separate public feature.
It uses an NFA with bounded language and work, rather than adding flags to exact search.
See [Retrieving, selecting, and searching text](./text.md) for current usage.

## Case conversion and normalization

| Evaluated transformation | Meaning | Exclusions |
| --- | --- | --- |
| Default case fold | Unicode default caseless matching | Locale-specific conversion and normalization |
| NFC | Canonical decomposition and composition | Compatibility mapping |
| Canonical caseless | Default case fold and canonical composition | Locale-specific conversion |

NFKC, accent stripping, and `UTF8PROC_IGNORE` were outside scope.
For example, NFC does not equate `①` and `1`.
Default case folding maps `İ` to `i` plus a combining dot, which does not match ASCII `i` alone.
Semantics and ownership were checked against the [pinned header](https://github.com/JuliaStrings/utf8proc/blob/v2.11.3/utf8proc.h) and [official C API](https://juliastrings.github.io/utf8proc/doc/utf8proc_8h.html).

## Mapping to original text positions

Two arrays retain original grapheme boundaries and byte boundaries after transforming up to those positions.
An original range is returned only when both match endpoints occur in the mapping.

| Original text | Query and transformation | Result |
| --- | --- | --- |
| `Straße` | `STRASSE`, case fold | Match; original bytes 0–7 |
| `ß` | `ss`, case fold | Match; original bytes 0–2 |
| `ß` | `s`, case fold | No match because it ends inside a transformation |
| `Cafe` + U+0301 | `Café`, NFC | Match; original bytes 0–6 |
| `E` + U+0301 | `é`, canonical caseless | Match; original bytes 0–3 |

These ranges exclude the end position.
Product adoption would require verifying that concatenating per-grapheme transformations equals transforming the whole document, using the pinned Unicode version's NormalizationTest and all boundaries.
Representative examples alone do not establish a public contract.

## Work and memory requirements

Original and transformed UTF-8 are each limited to 65,536 bytes, a grapheme passed to one transformation to 4,096 bytes, and boundaries to 65,537 entries.
The retained-data bound is `65,537 + 2 × 65,537 × sizeof(size_t)`: 1,114,129 bytes on 64-bit and 589,833 bytes on 32-bit.
Failure returns no partial object.

Transforming every scalar individually produced maximum expansions of 2→6 bytes for U+0390 under case fold and 4→12 bytes for U+1D160 under NFC and canonical caseless.
These are observations for the pinned data version, not fixed bounds for future versions.

`utf8proc_map()` allocates the required space internally and accepts neither a caller allocator nor a hard limit checked before allocation.
It also has no resumable API that returns intermediate transformation state.
These [public API conditions](https://github.com/JuliaStrings/utf8proc/blob/v2.11.3/utf8proc.h) prevent the same work and allocation contract as current search.

Reconsideration would first require resumable integer-budget cursors for scalar transformation, canonical ordering, and composition, caller-owned fixed-capacity storage, and original-boundary mappings.
Adding only transformed strings would not be sufficient.

## Regular expressions

POSIX [regexec](https://pubs.opengroup.org/onlinepubs/9799919799/functions/regexec.html) was rejected because its standard API lacks length-delimited subjects, suspension/resumption, and stack/heap/work limits, and does not provide a common Win32 implementation.

[PCRE2](https://www.pcre.org/current/doc/html/pcre2api.html) provides length-delimited subjects, match/depth/heap limits, and callout interruption.
However, it does not provide a cursor that continues from interrupted state; another call restarts the search.
Limits also have different meanings under JIT.
Passing a bounded copy of the complete text does not meet the search requirement to avoid copying all history.

The current recognition implementation uses a boxrig-specific Thompson NFA.
It exposes limits for states, work, logical lines, results, and snapshots, plus pull events.
It did not add interactive regex search with backward/wrapping behavior, captures, backreferences, lookaround, normalization, or case folding.

## Revalidation and sources

`make test` includes prototype functionality, allocation, and all-scalar checks.
See the [Makefile](../../Makefile) and [prototype tests](../../tests/test_search_transform_prototype.c) for individual connections.
There is no need to link the prototype into production to try it.

For external regex constraints, see [PCRE2 limits](https://www.pcre.org/current/doc/html/pcre2limits.html) and [build and resource limits](https://www.pcre.org/current/doc/html/pcre2build.html); for the adopted recognizer's work accounting, see [Text workload measurements](./text-workload-measurements.md).

[Documentation index](./index.md)
