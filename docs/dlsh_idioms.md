# dlsh idioms & cheatsheet (for Claude)

Working reference for the `dl_*` / `dg_*` commands that show up in ESS loaders,
extract scripts, and stim files. Optimized for "I want to do X — what's the
idiom?" rather than alphabetical reference. The per-function wiki under
`docsys/dokuwiki/4_function_reference/{dl,dg,dlg,df,dlp,dm}_functions/` has
prose + examples for each command; consult it for parameter details. C source
is at `/Users/sheinb/src/dlsh/src/{tcl_dl.c, dfana.c}`.

## Core mental model

- **Datatypes**: `DF_LONG` (int), `DF_FLOAT`, `DF_CHAR`, `DF_SHORT`, `DF_STRING`,
  `DF_LIST` (list of sub-lists). Arithmetic promotes long→float. `dl_int`,
  `dl_float`, `dl_char`, `dl_short` convert at the leaf level (recurse through
  `DF_LIST`).
- **Broadcasting**: every elementwise op (`dl_add`, `dl_mult`, `dl_eq`,
  `dl_floor`, `dl_int`, …) recurses through nested lists and operates at the
  leaves. Scalar arg paired with any-depth list broadcasts; two lists must
  have matching shape or one must be length 1.
- **Variadic arithmetic**: `dl_add a b c d ...` chains pairwise. Same for
  `dl_mult`, `dl_sub`, `dl_div`. Useful for `dl_add [dl_mult $m0 $v] [dl_mult $m1 $q] ...`.
- **References vs values**:
  - `dl_local name [expr]` — auto-freed when the enclosing proc/method returns.
  - `dl_set $g:col [expr]` — copies into the data group; persists with the dg.
  - Bare named dlists (`set foo [dl_create ...]`) leak unless you `dl_delete`.
  - Helper procs returning `dl_local` refs are dangerous — the name dangles
    once the helper returns. Either populate a dg column inside the helper
    (`dl_set $g:col ...`), or have the caller manage lifetime.

## Construction

| Need                              | Idiom                                       |
|-----------------------------------|---------------------------------------------|
| n zeros / ones                    | `dl_zeros $n` / `dl_ones $n` (longs)        |
| float zeros                       | `dl_zeros $n.` (decimal forces float)       |
| 0..n-1                            | `dl_fromto 0 $n`                            |
| Arithmetic series                 | `dl_series $start $stop ?$step?`            |
| n copies of value v               | `dl_repeat $v $n`                           |
| Repeat each elt of list by counts | `dl_repeat $list $countlist`                |
| Tile whole list n times           | `dl_replicate $list $n` (flat tiling)       |
| Wrap list as 1-elt list-of-lists  | `[dl_llist $list]`                          |
| Numeric list literal              | `[dl_ilist 1 2 3]` / `[dl_flist 1. 2. 3.]`  |
| String list                       | `[dl_slist "a" "b" "c"]`                    |
| Empty list of type T              | `[dl_create T]`                             |

### Shape-spec idiom for random generators

`dl_urand`, `dl_irand`, `dl_zrand`, `dl_zeros`, `dl_ones`, `dl_randfill` accept
a **nested int-list shape spec** and return matching nested floats/ints:

```tcl
dl_urand 6                                  ;# flat: [.3 .7 .1 ...] length 6
dl_urand [dl_repeat 3 $n]                   ;# nested: n sublists of length 3
dl_urand [dl_repeat $nlayers $n_obs]        ;# nested: n_obs sublists of length nlayers
```

Two-deep nesting (e.g. n_obs × nlayers × 3) needs a list-of-int-lists shape:

```tcl
dl_local inner [dl_repeat 3 $nlayers]                       ;# [3 3 3 ...]
dl_local shape [dl_repeat [dl_llist $inner] $n_obs]         ;# n_obs copies wrapped
dl_urand $shape                                              ;# n_obs × nlayers × 3
```

## Random generators

| Op                              | What                                              |
|---------------------------------|---------------------------------------------------|
| `dl_urand $n`                   | floats in [0, 1)                                  |
| `dl_irand $n $max`              | ints in [0, max)                                  |
| `dl_zrand $n`                   | standard normal (gaussian)                        |
| `dl_randchoose $m $n`           | choose n distinct ints from [0, m)                |
| `dl_pickone $list`              | one random element                                |
| `dl_shuffle $list`              | random permutation                                |
| `dl_randfill $n`                | random permutation of [0..n)                      |

All of these accept a shape spec instead of `$n` for nested output.

## Boolean masks

`dl_eq`, `dl_gt`, `dl_gte`, `dl_lt`, `dl_lte`, `dl_noteq` return int (0/1)
lists that broadcast with float arithmetic — multiply-and-sum is the
idiomatic "selector" pattern:

```tcl
# vectorized switch on hi ∈ {0..5}
dl_local m0 [dl_eq $hi 0] ; dl_local m1 [dl_eq $hi 1] ; ...
dl_local r [dl_add [dl_mult $m0 $v] [dl_mult $m1 $q] ...]
```

Logical ops: `dl_and`, `dl_or`, `dl_not`. Range test: `dl_between $list $lo $hi`.

## Element access

| Op                                | Returns                                     |
|-----------------------------------|---------------------------------------------|
| `dl_get $list $i`                 | scalar value at index i                     |
| `dl_get $list $i $j ...`          | nested indexing                             |
| `$g:col` or `$list:i`             | sublist access (colon syntax)               |
| `dl_first $list` / `dl_last $list`| first / last sublist or scalar              |
| `dl_index $list $idxlist`         | gather by index list                        |
| `dl_indices $list`                | [0..length-1]                               |
| `dl_put $list $i $val`            | in-place set element i (mutating)           |
| `dl_length $list`                 | top-level length                            |
| `dl_lengths $list`                | per-sublist lengths                         |
| `dl_depth $list`                  | nesting depth                               |
| `dl_datatype $list`               | "long" / "float" / "list" / "string" / ...  |

## Structural / shape transforms

These are the ones I look up most often:

| Op                                  | What it does                                                |
|-------------------------------------|-------------------------------------------------------------|
| `dl_transpose $listoflists`         | swap top two dims (matrix transpose)                        |
| `dl_transposeAt $list $level`       | recurse `level` deep, then transpose at that level          |
| `dl_pack $list`                     | wrap each leaf scalar into its own 1-elt sublist            |
| `dl_deepPack $list`                 | dl_pack at the deepest list level                           |
| `dl_unpack $list`                   | inverse of dl_pack at top level                             |
| `dl_deepUnpack $list`               | inverse at deepest                                          |
| `dl_collapse $list`                 | flatten one level of nesting                                |
| `dl_reshape $list $nrows $ncols`    | 1D → 2D with exact total-elements match                     |
| `dl_restructure $flat $template`    | reshape `$flat` to match `$template`'s nesting              |
| `dl_zip $a $b ...`                  | (NEW) zip N same-shape lists into N-tuples at the leaves    |
| `dl_llist $a $b ...`                | wrap args as elements of a new top-level list               |
| `dl_concat $a $b ...`               | concatenate (same type required)                            |
| `dl_combine $a $b ...`              | combine as sublists                                         |
| `dl_interleave $a $b ...`           | interleave element-by-element                               |

### `dl_restructure` contract

`dl_restructure $src $tmpl` requires `dl_length $src == dynListTotalElements
$tmpl` — i.e. the top-level count of source equals the total leaf-count of
the template. Each leaf-level sublist of `$tmpl` consumes its own length-worth
of elements from `$src` in order. Build templates with:

```tcl
dl_repeat [dl_llist [dl_ones $inner_len]] $outer_n
```

### When to use which "make this nested" trick

- **Have flat data, want nested output**: `dl_restructure $flat $template`.
- **Want nested generation from the start**: pass a nested shape spec to
  `dl_urand` / `dl_zeros` / etc.
- **Have N same-shape lists, want N-tuples at the leaves**: `dl_zip $a $b ...`
  (or, on older dlsh, `[dl_transposeAt [dl_transpose [dl_llist $a $b ...]] 1]`).

## Selection / filtering

| Op                                  | What                                                |
|-------------------------------------|-----------------------------------------------------|
| `dl_select $list $mask`             | keep where mask = 1                                 |
| `dl_choose $list $idxlist`          | gather by indices (numeric)                         |
| `dl_replace $list $old $new`        | replace values                                      |
| `dl_replaceByIndex $list $i $v`     | replace at given indices                            |
| `dl_find $list $val`                | indices where value equals val                      |
| `dl_findall $list $val`             | as above, deeper                                    |
| `dl_oneof $list $vals`              | mask of "is in"                                     |
| `dl_unique $list`                   | unique values                                       |

## Reduction (scalar from list)

`dl_sum`, `dl_max`, `dl_min`, `dl_mean`, `dl_median`, `dl_std`, `dl_var`,
`dl_prod`. For "per-sublist reduction" use the plural form: `dl_sums`,
`dl_means`, `dl_maxs`, `dl_mins`, `dl_stds`, `dl_vars`, `dl_prods`.

`dl_meanlist` averages multiple same-length lists elementwise.

## Type conversion

`dl_int $list`, `dl_float $list`, `dl_char $list`, `dl_short $list`. All
recurse into nested structure. Useful for forcing a downstream arithmetic
type or shrinking memory.

## Aggregation across nested structure

`dl_bsums`, `dl_bmeans`, `dl_bstds`, `dl_breverse`, `dl_bsort` operate
"by sublist" — once per sublist at the top level. The `b` prefix is the
mnemonic.

## Data groups (`dg_*`)

```tcl
if {[dg_exists stimdg]} { dg_delete stimdg }
set g [dg_create stimdg]
dg_rename $g stimdg                          ;# usually the same name
dl_set $g:col [expr-yielding-a-dl]            ;# add column
dg_listnames $g                               ;# list columns
dg_select $g $mask                            ;# row-select to new dg
dg_copyselected $src $mask $dest              ;# row-select into existing
dg_choose $g $idxlist                         ;# row-gather by indices
dg_append $g1 $g2                             ;# row-concat (matching cols)
dg_remove $g col                              ;# drop column
dg_tojson $g                                  ;# serialize
dg_fromcsv path                               ;# parse CSV
```

`dl_set` on a `$g:col` ref is the canonical way to "attach" data to a dg.
Per-column dls created by `dl_set $g:col` are owned by the dg and freed when
the dg is freed — no manual cleanup.

## Pitfalls

### `source` inside `::ess` namespace
ESS sources `*_loaders.tcl` / `*_variants.tcl` from inside `::ess::find_variants`
(see `lib/ess-2.0.tm`). Any *top-level* `proc foo {} {...}` in such a file
becomes `::ess::foo`, not `::foo`. Two fixes:

```tcl
# A: define inside the protocol's namespace (best practice — matches planko/steer)
namespace eval my_system::my_proto {
    proc helper {} { ... }       ;# becomes ::ess::my_system::my_proto::helper
    proc loaders_init { s } {
        $s add_loader setup_trials { ... } {
            ... ::ess::my_system::my_proto::helper ...   ;# full path from method body
        }
    }
}

# B: pin to global ns explicitly (use sparingly — pollutes ::)
proc ::helper {} { ... }
```

### `add_loader` body is a `TclOO` method body
The body runs as `$s setup_trials ...`, in `oo::Obj` method context with the
system's namespace. Procs from the global namespace are still callable via
`::full::path::name`. Bare names like `[helper]` resolve against the object's
namespace path, *not* the file's namespace.

### `dl_local` lifetime
Auto-freed at the enclosing proc/method end. Don't return `dl_local` refs
from helpers — by the time the caller reads the name, the dl is gone. Either
write into a dg column inside the helper, or use non-local dlists with
explicit `dl_delete` in the caller.

## Vectorized recipes

### HSV → RGB at any nesting depth
```tcl
# h ∈ [0,6), s ∈ [0,1], v ∈ [0,1] — any matching shape
dl_local hi_f [dl_floor $h]
dl_local hi   [dl_int $hi_f]
dl_local f    [dl_sub $h $hi_f]
dl_local p    [dl_mult $v [dl_sub 1.0 $s]]
dl_local q    [dl_mult $v [dl_sub 1.0 [dl_mult $s $f]]]
dl_local t    [dl_mult $v [dl_sub 1.0 [dl_mult $s [dl_sub 1.0 $f]]]]

foreach i {0 1 2 3 4 5} { dl_local m$i [dl_eq $hi $i] }

dl_local r  [dl_add [dl_mult $m0 $v] [dl_mult $m1 $q] [dl_mult $m2 $p] \
                    [dl_mult $m3 $p] [dl_mult $m4 $t] [dl_mult $m5 $v]]
dl_local gn [dl_add [dl_mult $m0 $t] [dl_mult $m1 $v] [dl_mult $m2 $v] \
                    [dl_mult $m3 $q] [dl_mult $m4 $p] [dl_mult $m5 $p]]
dl_local b  [dl_add [dl_mult $m0 $p] [dl_mult $m1 $p] [dl_mult $m2 $t] \
                    [dl_mult $m3 $v] [dl_mult $m4 $v] [dl_mult $m5 $q]]
```

### Random with deterministic per-row seeds (per-trial sequences)
```tcl
dl_set $g:seed [dl_irand $n_obs 1000000]      ;# seed column
# ... later, in stim/extract: expr srand([dl_get stimdg:seed $id]); rand()
```

### Per-trial × per-layer params via shape spec
```tcl
# n_obs trials, nlayers parameters each, uniform in [a, b)
dl_set $g:param [dl_add $a [dl_mult [expr {$b - $a}] \
                                    [dl_urand [dl_repeat $nlayers $n_obs]]]]
```

### Common "trial mask" idiom
```tcl
dl_local left  [dl_eq $g:side 0]
dl_local right [dl_eq $g:side 1]
dl_set $g:correct_x [dl_add [dl_mult $left  $left_x] \
                            [dl_mult $right $right_x]]
```

## dlsh ↔ NumPy Rosetta (rough)

| NumPy                              | dlsh                                              |
|------------------------------------|---------------------------------------------------|
| `np.zeros(n)` / `np.ones(n)`       | `dl_zeros $n` / `dl_ones $n`                      |
| `np.arange(n)`                     | `dl_fromto 0 $n`                                  |
| `np.linspace(a, b, n)`             | `dl_series $a $b $step`                           |
| `np.random.rand(n)`                | `dl_urand $n`                                     |
| `np.random.rand(m, n)`             | `dl_urand [dl_repeat $n $m]`                      |
| `np.random.randint(0, k, n)`       | `dl_irand $n $k`                                  |
| `np.random.randn(n)`               | `dl_zrand $n`                                     |
| `np.random.permutation(n)`         | `dl_randfill $n` or `dl_shuffle [dl_fromto 0 $n]` |
| `a + b` (broadcast)                | `dl_add $a $b`                                    |
| `a * b * c`                        | `dl_mult $a $b $c`                                |
| `a == k`                           | `dl_eq $a $k`                                     |
| `a > k`                            | `dl_gt $a $k`                                     |
| `np.where(mask, x, y)` (numeric)   | `dl_add [dl_mult $mask $x] [dl_mult [dl_not $mask] $y]` |
| `np.choose(idx, [v0, v1, ...])`    | sum of `dl_mult [dl_eq $idx $k] $vk` per k        |
| `a[i]`                             | `dl_get $a $i`                                    |
| `a[i:j]`                           | `dl_choose $a [dl_fromto $i $j]`                  |
| `a[mask]`                          | `dl_select $a $mask`                              |
| `a.reshape(m, n)`                  | `dl_reshape $a $m $n`                             |
| `a.T`                              | `dl_transpose $a`                                 |
| `np.stack([r, g, b], axis=-1)`     | `dl_zip $r $g $b`                                 |
| `np.sum(a)`                        | `dl_sum $a`                                       |
| `np.sum(a, axis=1)`                | `dl_sums $a`                                      |
| `a.astype(int)`                    | `dl_int $a`                                       |
| `len(a)`                           | `dl_length $a`                                    |
| `a.ndim`                           | `dl_depth $a`                                     |
| `np.unique(a)`                     | `dl_unique $a`                                    |

## Debugging tips

- `dl_tcllist $list` — dump to plain Tcl list for `puts` inspection.
- `dl_dump $list` — pretty-print to stdout.
- `dl_datatype $list` and `dl_depth $list` and `dl_length $list` — quick
  shape sanity check.
- For a dg: `dg_dump $g` or `dg_dumplistnames $g`.

When in doubt about an op's exact semantics, the per-function dokuwiki page
under `docsys/dokuwiki/4_function_reference/dl_functions/<name>.txt` usually
has a working example.
