# spin — projects and packages

`spin` is Spinel's project tool: it scaffolds a project, resolves
dependencies (spin packages), and drives the compiler so you never write a
Makefile or a `spinel -I ...` line by hand. If you know cargo or mix, you
know the shape. The design record lives in
[internals/spin.md](internals/spin.md); this page is how to use it.

`spin` ships beside the compiler: building the repo (`make`) produces
`bin/spin`, and `make install` installs it next to `spinel`.

## Starting an application

```sh
spin new myapp        # scaffold: spin.toml, bin/myapp.rb, test/, .gitignore
cd myapp
spin run              # compile bin/myapp.rb and run it
```

```
myapp/
  spin.toml            # the manifest (name, [dependencies])
  myapp.rb            # library code: require "myapp" resolves here
  myapp/              # subfeatures: require "myapp/util" -> myapp/util.rb
  bin/myapp.rb        # each bin/*.rb is an executable (a compile root)
  test/               # each test/*.rb is a test program
  build/              # disposable output (spin clean)
```

An application **is** a package: there is no separate project kind. Executables
live in `bin/` (one per file, `spin run <name>` when there are several).
Grow the app by putting shared code in `myapp.rb` / `myapp/*.rb` and
requiring it from `bin/`; more `bin/*.rb` files become more executables.
`spin init` writes a `spin.toml` into an existing directory instead of
scaffolding. When the CLI is ready for daily use, `spin install` builds it
and copies the executables to `~/.local/bin` (`$XDG_BIN_HOME` / `--prefix`
override; `--uninstall` removes them).

Everything in the package participates by extension, not by manifest lists:
`.rb` is source, `.rbs` is an optional type sidecar, `.c`/`.h` is carried
native code (below). `build/`, `vendor/`, `test/`, and `bin/` are the only
special directory names.

## Starting a library

```sh
spin new mylib --lib  # spin.toml with [package] name/version, mylib.rb, test/
cd mylib
```

A library is the same package shape minus `bin/`: there is nothing to `spin
build` or `spin run` — a library is *exercised through its tests*:

```sh
cat > mylib.rb <<'RUBY'
module Mylib
  def self.shout(s) = s.upcase + "!"
end
RUBY
cat > test/shout_test.rb <<'RUBY'
require "mylib"
puts Mylib.shout("hi")   # HI!
RUBY
spin test                # runs it (against CRuby when no snapshot yet)
spin test --regen        # freeze the output as the .expected snapshot
```

While developing an application against your library, wire it up as a
live path dependency — edits take effect on the next build, nothing is
pinned:

```sh
cd ../myapp
spin add mylib --path ../mylib
```

To share it, push the directory as a git repo (conventionally named
`spinel-mylib`; the gem *name* stays `mylib` because it is the `require`
string). Consumers then use it directly:

```sh
spin add mylib --git https://github.com/you/spinel-mylib
```

or, once it has releases, through [the index](#the-index). Publishing a
release is one command:

```sh
spin publish
```

It validates the release (committed and pushed, `[package] name`/`version`
present, the tree at the release commit carrying that same version, the
name not owned by another repo in the index), **runs `spin test` as a hard
gate**, then submits `packages/mylib.toml` with a `[[release]]` entry pinning
the full commit SHA: as a pull request to
[spin-index](https://github.com/matz/spin-index) when the `gh` CLI is
available, by printed instructions otherwise, or pushed directly with
`--direct` if you have index write access. From then on
`spin add mylib --version "~> 0.1"` works, and bumping `version` +
`spin publish` again is how you ship an update. Libraries do not commit a
`spin.lock`; version selection belongs to the consuming application.

## Dependencies

Declare dependencies in `spin.toml`; `spin` computes the compiler's `-I`
list from them. Three source forms:

```toml
[dependencies]
ansi  = { path = "../spinel-ansi" }              # local checkout
greet = { git = "https://github.com/x/spinel-greet" }  # git URL (+ ref = "...")
hello = "~> 1.1"                                 # index constraint (see below)
```

```sh
spin add ansi --path ../spinel-ansi   # edits spin.toml and relocks
spin add hello --version "~> 1.1"     # index form
spin remove ansi
spin list                             # resolved set: name, version, source
spin tree                             # nested view (--json on both)
```

Dependencies are transitive: each fetched package's own `[dependencies]` is
resolved too. Inside a project every `require` must resolve — an
unsatisfiable `require` is a compile error naming the missing package, and
stdlib features need their `require` just like CRuby (`spin` compiles with
the require gate on; see [require.md](require.md)).

### The index

A bare `name = "constraint"` dependency is looked up in the index — a git
repository (no server) mapping names to repos and releases:
<https://github.com/matz/spin-index>. Constraints are `"~> 1.2"`
(pessimistic), `">= 1.2.3"`, an exact version, or `"*"`.

Selection is MVS: `spin` picks the **lowest** release satisfying the
constraint, so a build without a lockfile is still deterministic;
`spin.lock` then pins the exact commit. `spin search [term]` lists index
entries. Set `SPIN_INDEX` to use another index (a `file://` URL works).

Index entries also carry **probe records** — which compiler build a release
passed or failed its tests under (`spin publish` records a pass for your
build automatically; `spinel --version` prints the build revision). When
you depend on a release with a recorded failure, resolution warns before
fetching — strongly when the failure was recorded against your exact
compiler build — but never blocks: your own build is the final answer.

### spin.lock

`spin lock` (and `spin add`/`remove`) writes `spin.lock`: one `[lock.<name>]`
entry per dependency with the resolved version and, for git/index sources,
the full commit SHA. Commit it for applications. Resolution *verifies*
against the lock rather than reselecting; if you change a constraint so the
pinned version no longer satisfies it, the build warns and reselects, and
the next `spin lock` rewrites the pin.

### Offline and vendoring

Fetched packages live in a shared cache (`$XDG_CACHE_HOME/spin/packages/`),
keyed by the commit SHA. `spin vendor` copies the resolved tree into
`vendor/packages/` for hermetic builds; with `SPIN_OFFLINE=1`, resolution uses
only the cache and `vendor/` — nothing touches the network.

## Tests

Each `test/*.rb` is one test program, compiled with the package's sources and
dependencies spliced in. Pass/fail is snapshot-based:

```sh
spin test                 # run all tests
spin test smoke_test.rb   # one test
spin test --regen         # refresh .expected snapshots from CRuby
```

A committed `test/<name>.rb.expected` is diffed against the run's stdout.
With no snapshot, the same file runs under `ruby` and the outputs are
diffed directly — the test doubles as a CRuby-parity check. A non-zero
exit or a diff fails, so plain assert-and-raise style works.

## Native C in a package

Drop `.c`/`.h` files anywhere in the package tree and bind them with the
[FFI declarations](FFI.md):

```ruby
# fast.rb
module Fast
  ffi_func :fast_quad, [:int], :int
end
```

```c
/* fast_ext.c */
#include <stdint.h>
intptr_t fast_quad(intptr_t x) { return x * 4; }
```

`spin` compiles each `.c` once into a shared cache keyed by
(package, version, toolchain) — set `CC` to choose the compiler — and links the
objects into every dependent build. External libraries use the existing
`ffi_lib` declaration and need no manifest entry.

### Excluding C from the build

`.rb` enters the build by being required; `.c` enters by being there. That is
what lets a package carry native code without listing it, and it is the wrong
default when a repository holds a C program of its own — a `main()` beside the
Ruby will collide with the generated one at link time. `exclude` names what is
not part of this build:

```toml
[package]
name = "myapp"
exclude = ["standalone_c_app.c", "c_lib*.c", "cbits"]
```

Globs are relative to the package root; naming a directory prunes all of it.
`exclude` covers native discovery only — `.rb` needs no entry, since nothing
compiles it unless something requires it, and an excluded `.h` is still on the
include path for the C that is compiled. An application scaffolded by
`spin new` has no `[package]` table; add one to use the field.

## Building outside spin

`spin build` owns the tree it sits in. When the build is driven from somewhere
else — a Makefile that also builds a C program, a repository whose layout is
not spin's to arrange — `spin flags` hands over instead of taking over. It
resolves the dependencies, compiles any carried C into the cache, and prints
the compiler flags that implies:

```console
$ spin flags
--require-gate -I /path/pkgs/curses -I /path/backend --link ~/.cache/spin/native/curses-0.1.0-cc/sp_curses.o
```

Every path is absolute, so the caller's working directory can be anywhere:

```make
SPINFLAGS := $(shell cd spin/backend && spin flags)

ruby_app.exe: ruby_app.rb $(RUBY_SRCS)
	spinel $(SPINFLAGS) -I . $< -o $@
```

What it prints is what `spin build` compiles with, minus the entry file and
`-o`; the two come from one place, so they cannot drift.

Nothing here is required to consume a package by hand. `require "curses"`
resolves against any `-I` root as `<root>/curses.rb` or
`<root>/curses/curses.rb`, so `spinel -I spin/packages` finds a package sitting
at `spin/packages/curses/`, and `--link` takes its compiled object. `spin
flags` is the part that works out which roots and which objects.

## Rebuilds

`spin build`/`run`/`test` skip recompilation when nothing changed (input
mtimes across the project, its dependencies, and the compiler binary).
There is no file-granular incremental mode — whole-program type
specialization spans every source — but compiles are fast and package C
objects are reused from the cache. `spin clean` removes `build/`.

## Command summary

| command | what it does |
|---|---|
| `spin new <name> [--lib]` / `spin init` | scaffold / adopt a directory |
| `spin build [target..]` | compile `bin/` executables into `build/bin/` |
| `spin run [target] [-- args]` | build, then run one executable |
| `spin test [file..] [--regen]` | run `test/*.rb` against snapshots |
| `spin add` / `remove` | edit `[dependencies]` and relock |
| `spin lock` / `fetch` / `vendor` | pin / warm the cache / copy into `vendor/` |
| `spin flags` | print the compiler flags this project implies, for a build driven from outside spin |
| `spin list` / `tree` / `search` (`--json`) | inspect the resolved set / the index |
| `spin publish [--direct]` | validate + test, then submit this release to the index |
| `spin install [name..]` | build and copy `bin/` executables to `~/.local/bin` (`--prefix`, `--uninstall`) |
| `spin clean` | remove `build/` |

Environment: `SPIN_INDEX` (index URL), `SPIN_OFFLINE=1` (cache/vendor
only), `CC` (toolchain for package C), `SPIN_NATIVE_CACHE` (where compiled
package objects go, default `$XDG_CACHE_HOME/spin/native`),
`SPIN_NO_NATIVE_CACHE=1` (recompile package C every time),
`SPINEL_HDR_DIR` (where the runtime headers are, when spin cannot work it out
from the compiler's own path).

### When the cache is in the way

Package `.c` files compile into a shared cache keyed by (package, version,
toolchain), so the same package is not rebuilt for every consumer. That is
worth having across projects and unhelpful while debugging one: a run behaves
differently depending on whether an object happens to be there already, which
is exactly what you do not want when you are trying to find out why a build
differs. `SPIN_NO_NATIVE_CACHE=1` makes every run start from the same state,
and `SPIN_NATIVE_CACHE=<dir>` puts the objects somewhere you can delete.

Note what it does and does not save. The objects are the cheap half: hand-
written C compiles in milliseconds, while whole-program type inference over
the Ruby is where the time goes. The cache exists so a package is not
recompiled once per consuming project, not to make a single build fast.
