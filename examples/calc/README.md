# calc: a second frontend for libmtlc

`calc` is a tiny C-like language with its own compiler in a single file,
[`calc.c`](calc.c). It exists to demonstrate one thing: **libmtlc is a
frontend-agnostic backend.** `calc` is not Mettle and shares no code with the
Mettle frontend. It is a self-contained lexer + recursive-descent parser that
lowers straight into libmtlc's IR through the public API and drives the backend
all the way to a native executable.

It includes **only** the public headers:

```c
#include <mtlc/build.h>     // the IR builder
#include <mtlc/mtlc.h>      // context, module, version
#include <mtlc/pipeline.h>  // optimize, emit object, link executable
```

and links **only** `bin/mtlc.lib` (or `bin/libmtlc.a`). No internal backend
headers, no Mettle frontend.

## The language

64-bit integers only. Functions, parameters, `var` locals, assignment,
`if`/`else`, `while`, `return`, calls (including recursion), and the usual
arithmetic / relational / logical operators. `main` is the entry point and its
return value becomes the process exit code.

```
// programs/factorial.calc
fn fact(n) {
  if (n < 2) { return 1; }
  return n * fact(n - 1);
}

fn main() {
  return fact(5);   // process exits 120
}
```

## Build and run

Build the compiler against the installed library and headers:

```bash
# Windows (after .\build.bat has produced bin\mtlc.lib)
gcc -Iinclude examples/calc/calc.c bin/mtlc.lib -o calc.exe -ldbghelp

# Linux (after `make libmtlc`)
cc -Iinclude examples/calc/calc.c bin/libmtlc.a -o calc
```

Then compile a `.calc` program to a native binary and run it:

```bash
./calc programs/factorial.calc factorial.exe
./factorial.exe ; echo $?      # -> 120
```

## What actually happens

`calc.c` walks its parse and calls the builder as it goes
(`mtlc_builder_function`, `mtlc_local`, `mtlc_binary`, `mtlc_call`,
`mtlc_branch_if_zero`, `mtlc_return`, and so on), then hands the finished module
to the backend:

```c
mtlc_optimize(ctx, module);              // classical optimizer (fold, inline, ...)
mtlc_build_executable(ctx, module, out); // native x86-64 codegen + internal PE link
```

Everything after `mtlc_builder_finish` (optimization, register allocation,
instruction selection/encoding, and on Windows linking a PE executable with no
external toolchain) is libmtlc doing exactly what it does for the Mettle
frontend, driven here by a completely different language.
