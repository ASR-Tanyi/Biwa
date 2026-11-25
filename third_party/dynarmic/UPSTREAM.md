# dynarmic (A64 decoder table)

| | |
|---|---|
| Source | https://github.com/yuzu-mirror/dynarmic |
| Commit | `9d4582339990d4eae53f1dc7160686920fc2075c` |
| Licence | 0BSD (`LICENSE.txt`, copied from the same commit) |

## Files taken

| File here | Upstream path | SHA-256 |
|---|---|---|
| `a64.inc` | `src/dynarmic/frontend/A64/decoder/a64.inc` | `2263503833d36933f9700cc3aa5a095ee47b04a55ded2656beb11240daabdfe1` |

## Changes

- `a64.inc` is used as it is. `src/a64/CMakeLists.txt` reads its `INST(...)` lines at configure
  time and writes `biwa/a64/ops.inc`, numbering repeated names (`UnallocatedEncoding_2`, ...), so
  every entry gets its own `biwa::a64::Op`.
- `a64_extra.inc` holds 16 lines that `a64.inc` has commented out because dynarmic does not
  implement them, although they are ARMv8.0 instructions: the scalar and vector SQDMLAL, SQDMLSL,
  SQRSHL and UQRSHL, the scalar SQDMULL, SQRSHRN, SQRSHRUN and UQRSHRN, and SQDMLAL / SQDMLSL by
  element. They are copied unchanged, without the leading `//`, and appended to the table. The
  comparison with Capstone over all 2^32 encodings (`tests/a64`) found them.
