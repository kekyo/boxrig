# Matching arbitrary UTF-8 text with regular expressions

[日本語](../ja/regex.md)

`boxrig/regex.h` provides an API for matching UTF-8 text without creating a terminal or GUI.
Link against `libboxrig.a` or the shared library `libboxrig.so`.
It uses the same syntax and matching rules as screen text recognizers.

## Try the CLI

The following command finds the leftmost-longest match in standard input.

```sh
make boxrig-regex
printf 'zaaa' | build/boxrig-regex 'a|aa' 0 1
```

The result `match 1 3` denotes a match from byte offset 1 up to, but not including, offset 3 in the input.
`work` reports the work consumed by matching. Setting the last argument to 1 resumes matching one work unit at a time.
The starting byte offset defaults to 0, and the work budget per call defaults to 4096.
The exit status is 0 for a match, 1 for no match, and 2 for an input or pattern error.

Use `--pattern-file FILE` for patterns containing NUL.
NUL bytes in the input are also matched. The CLI limits patterns to 64 KiB and input to 1 MiB.
Exceeding either limit is an error.

## Use the API

Create limits with `boxrig_regex_limits_default()`, then pass a pattern and its byte length to `boxrig_regex_compile()`.
The compiled result is an immutable `boxrig_regex_program`.
On failure, inspect the code and `pattern_offset` in `boxrig_text_regex_diagnostic` for the cause and location.
The diagnostic type is shared with screen recognizers.

Use `boxrig_regex_matcher_create()` to create a matcher.
Supply the input, its byte length, and a starting byte offset. The matcher validates and copies the input.
Validation and copying at creation are synchronous and take time proportional to input length.
For invalid UTF-8 or a starting position error, inspect the returned error offset.

Pass a positive work budget to `boxrig_regex_matcher_resume()` to advance matching.
Resume the same matcher while it reports `IN_PROGRESS`.
For `MATCH`, the range runs from `start` up to, but not including, `end`, in bytes from the beginning of the original input.
`NOT_FOUND` means matching has finished. A finished matcher cannot be resumed.

The matcher retains its input and compiled program, so you can change or free the caller's input after creation.
You can also release the caller's program reference with `boxrig_regex_program_destroy()`.
Use `boxrig_regex_program_retain()` when multiple owners retain the same program.
Release each reference you acquire.
The caller must serialize operations that share a program, including reference-count changes.
Release the matcher itself with `boxrig_regex_matcher_destroy()`.

## Syntax and matching rules

Supported syntax includes literals, `.`, character classes, groups, `|`, `?`, `*`, `+`, `{m}`, `{m,n}`, `^`, and `$`.
You can also escape metacharacters and use `\n`, `\r`, `\t`, and `\u{hex}`.
`\d`, `\s`, and `\w` are ASCII character classes.
Unicode categories such as `\p{L}` are supported; see “Unicode character categories” below.
Expressions that match the empty string, backreferences, lookahead, lookbehind, and consecutive quantifiers are unsupported.

Matching operates on Unicode scalars, with both ends of the whole match restricted to extended grapheme boundaries.
For example, `.` alone cannot match a character consisting of `a` followed by a combining accent.
If the starting position is inside a grapheme, searching begins at the next grapheme boundary.
A starting offset at a UTF-8 continuation byte is an error.

`^` and `$` refer to the start and end of the entire input. Changing the starting offset does not change what counts as the start.
`.` also matches newlines. Matching performs no normalization or case conversion.
When multiple matches begin at the same position, the longest one is returned.

Pattern length, state count, repetition count, and input length have separate limits. Zero never means unlimited.
For a fixed pattern, total matching work is proportional to input length, including when no match exists.
One work unit is a state visit, transition, or input-position check.
A character-class visit compares the ranges contained in that class.
For a class containing Unicode categories, each visit performs at most one constant-time category lookup.

## Unicode character categories

General categories classify characters as letters, numbers, symbols, and other kinds.
`\p{category}` matches one Unicode scalar in the named category.
`\P{category}` matches one Unicode scalar outside that category.
Classification uses the Unicode data in the utf8proc version bundled with boxrig.
See the [Unicode General_Category table](https://www.unicode.org/reports/tr44/#General_Category_Values) for category definitions
and the [utf8proc API documentation](https://juliastrings.github.io/utf8proc/doc/utf8proc_8h.html) for the classification API.

The following short names are supported. Names are case-sensitive, and braces are required.

| Name | Meaning | Subcategories |
| --- | --- | --- |
| `L` | Letters | `Lu`, `Ll`, `Lt`, `Lm`, `Lo` |
| `M` | Combining marks | `Mn`, `Mc`, `Me` |
| `N` | Numbers | `Nd`, `Nl`, `No` |
| `P` | Punctuation | `Pc`, `Pd`, `Ps`, `Pe`, `Pi`, `Pf`, `Po` |
| `S` | Symbols | `Sm`, `Sc`, `Sk`, `So` |
| `Z` | Space, line, and paragraph separators | `Zs`, `Zl`, `Zp` |
| `C` | Control, format, private-use, unassigned, and related categories | `Cc`, `Cf`, `Cs`, `Co`, `Cn` |
| `LC` | Uppercase, lowercase, and titlecase letters | Union of `Lu`, `Ll`, and `Lt` |

Subcategories can also be specified directly. For example, `\p{Lu}` matches uppercase letters and `\p{Nd}` matches decimal digits.
`Cs` is the surrogate category, but it never matches because valid UTF-8 input contains no surrogates.
Scripts such as `\p{Greek}`, properties outside General_Category such as `\p{White_Space}`, and long names such as `Letter` are unsupported.
Unsupported names or malformed braces produce `BOXRIG_TEXT_REGEX_INVALID_ESCAPE`, with the diagnostic offset pointing to the backslash that starts the escape.

Both `\p` and `\P` work on their own and inside `[]`.
The elements of a character class form a union. For example, `[\P{L}A]` matches a non-letter or `A`.
`[^\P{L}]` negates that set, so its category coverage is the same as `\p{L}`.
Using a category as a range endpoint, as in `[a-\p{L}]`, produces `BOXRIG_TEXT_REGEX_INVALID_RANGE`.
To include a literal hyphen, place it first or last in the class, or escape it.

The following expression captures a hostname containing Japanese text and the scheme before it.

```sh
printf '前 https://example.みんな/' | build/boxrig-regex --captures '(?<scheme>https?)://(?<host>[\p{L}0-9.-]+)' 0 1
```

The whole match runs from byte offset 4 up to, but not including, offset 29 in the input.
Capture offsets are relative to the beginning of the matched text: `scheme` runs from 0 to 5, and `host` from 8 to 25.
This expression extracts hostname candidates; it does not validate every requirement of a domain name.

`\d`, `\s`, and `\w` remain ASCII-only.
For example, fullwidth digits match `\p{Nd}` but not `\d`.
`\p{Zs}` includes the fullwidth space but excludes tabs and newlines.

Grapheme-boundary rules still apply.
In a character consisting of `e` and a combining accent, `e` belongs to `L` and the accent to `M`.
`\p{L}+` cannot end the whole match inside a grapheme, so it does not match that character.
Use `[\p{L}\p{M}]+` to include combining marks.

## Retrieve captures

`(expression)` creates a numbered capture, and `(?<name>expression)` creates a named capture.
Numbers start at 1 in opening-parenthesis order; group 0 represents the whole match.
`(?:expression)` groups an expression without assigning a capture number.
Names are ASCII strings following `[A-Za-z_][A-Za-z_0-9]*` and must be unique.

For the same whole-match range, captures prefer left-hand alternatives and greedy repetition.
For example, matching `(a|aa)(a?)` against `aa` gives `a` for group 1.
Matching `(a*)(a*)b` against `aaab` gives `aaa` for group 1.
Empty repetitions stop paths that revisit the same input position and state.
Thus, matching `(a?)*b` against `aaab` leaves the last `a` in group 1; matching it against `b` leaves an empty string.
A repeated group retains the value from its last participation.
If a later repetition does not visit that group, the earlier participating value remains.

After `MATCH`, call `boxrig_regex_matcher_result()` to obtain an immutable result.
Use `boxrig_regex_result_get_capture()` to retrieve a capture by number or `boxrig_regex_result_get_named_capture()` to retrieve one by name.
`participated` is false for a group that did not participate. Participation with an empty string sets it to true, with equal `start` and `end` offsets.

Capture positions are UTF-8 byte offsets relative to the matched text returned by `boxrig_regex_result_data()`.
They have a different origin from the matcher's offsets into the entire input.
Capture endpoints are Unicode scalar boundaries and may lie inside a grapheme.
For example, when `(a)\u{0301}` matches a character made of `a` and a combining accent, group 1 has the byte range `[0, 1)`.

Results remain readable after the matcher and program are released.
Call `boxrig_regex_result_destroy()` for each reference when you no longer need it.
In the CLI, put `--captures` first to display numbered capture ranges and nonparticipating groups.

```sh
printf 'zb' | build/boxrig-regex --captures '(?<missing>a)?(a*)b' 0 1
```

## Validate names before saving settings

Call `boxrig_regex_program_find_named_group()` on a compiled program to find whether a name exists and obtain its group number.
No input, matcher, or terminal is needed.
For example, to validate `${path}` in a template before saving settings, pass the name `path`.

```c
bool found;
size_t group;
boxrig_status status = boxrig_regex_program_find_named_group(
    program, "path", &found, &group);
```

For a defined name, `status` is `BOXRIG_STATUS_OK`, `found` is true, and `group` is a number of at least 1.
For an undefined name, the status is still `OK`, `found` is false, and `group` is 0. Use this result to reject undefined references in the application.
Names are case-sensitive. Empty strings and strings outside the name grammar are also treated as undefined.
If any argument is NULL, the function returns `INVALID_ARGUMENT` and resets non-NULL outputs to false and 0.

The name `path` in `(?<path>a)?b` is defined even if the group does not participate in a match.
Groups with a repetition count of zero, specified with `{0}`, can also be found.
Numeric references are valid from 0 through `boxrig_regex_program_group_count()`. Group 0 is the whole match and has no name.
The lookup allocates no memory, performs no recompilation, and changes neither the program nor its reference count.
Sharing the same program still requires serialization, as with the other APIs.

## Share a program with a screen recognizer

Register a program with `boxrig_terminal_register_compiled_text_recognizer()` to share it.
The recognizer retains a program reference, so you can release the caller's reference after registration.
Compilation limits are already fixed by the program; registration uses the scan, snapshot, and result limits.
The existing registration API that accepts a pattern also supports the same captures.

Retrieve a screen result with `boxrig_terminal_text_recognizer_result()`, passing the recognizer ID, `match_id`, and `result_set_revision`.
It returns the captures from recognition without matching again at retrieval time.
Changes to text or layout, an updated result revision, or recognizer removal cause retrieval to return `STALE`.
Results already obtained remain valid after updates or terminal destruction.

## Capture limits

`capture_capacity` limits the number of groups, and `capture_byte_capacity` limits capture storage during matching.
The defaults are 32 groups and 16 MiB; the maxima are 256 groups and 256 MiB.
Capture storage during matching does not grow with input length. Expressions that cannot fit within the limits fail compilation.
A work unit also includes copying group information within those limits.

A screen recognizer's `result_byte_capacity` limits the combined matched bytes and capture information per result set.
The default is 16 MiB. The current result set and the set under construction during the next scan can coexist, retaining up to two sets.
Exceeding the capacity reports an incomplete result without publishing partial results.
Result references acquired and retained by the application are outside this limit. The application must manage how many it retains and when it releases them.

[Documentation index](./index.md)
