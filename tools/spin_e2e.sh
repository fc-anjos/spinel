#!/bin/sh
# spin end-to-end check: scaffold -> path dep -> git dep -> lock -> vendor ->
# offline -> test, all hermetic (file:// git remote, scratch HOME cache).
# Usage: tools/spin_e2e.sh <spin-binary>   (run from the repo root)
set -e

SPIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
WORK=$(mktemp -d /tmp/spin-e2e.XXXXXX)
export XDG_CACHE_HOME="$WORK/cache"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "spin-e2e FAIL: $1" >&2; exit 1; }
expect() { # expect <label> <want> <got>
  [ "$3" = "$2" ] || fail "$1: want [$2] got [$3]"
}

cd "$WORK"

# --- scaffold + run -----------------------------------------------------------
"$SPIN" new app >/dev/null
cd app
expect "scaffold run" "Hello from app" "$("$SPIN" run 2>&1 | tail -1)"

# --- `--` with no target ahead of it names every target, not `--` itself --------
# `spin build -- --profile` passes a compiler flag and names no target. The
# flag has to reach the compiler and every bin has to build; naming `--` as the
# target is the bug (it looked for bin/--.rb and built nothing).
"$SPIN" build -- --profile >/dev/null 2>&1 || fail "build -- <flag>: exited non-zero"
[ -f build/bin/app ] || fail "build -- <flag>: built no target"
[ -f build/bin/app.symbols.json ] || fail "build -- <flag>: flag never reached the compiler"
"$SPIN" clean >/dev/null
"$SPIN" build app -- --profile >/dev/null 2>&1 || fail "build <target> -- <flag>: exited non-zero"
[ -f build/bin/app.symbols.json ] || fail "build <target> -- <flag>: flag never reached the compiler"
"$SPIN" clean >/dev/null

# --- an emit-only flag after `--` is refused, and leaves the binary alone -----
# `spin build -- -c` used to write C SOURCE over build/bin/<target>, keep its
# executable bit and exit 0, so nothing said the binary was gone.
"$SPIN" build >/dev/null 2>&1 || fail "build before emit-only check: exited non-zero"
before=$(head -c 4 build/bin/app | od -An -c | tr -d ' \n')
"$SPIN" build -- -c >/dev/null 2>&1 && fail "build -- -c: exited zero"
[ -f build/bin/app ] || fail "build -- -c: removed the binary"
after=$(head -c 4 build/bin/app | od -An -c | tr -d ' \n')
expect "build -- -c leaves the binary" "$before" "$after"
"$SPIN" clean >/dev/null

# --- library package (path dependency) --------------------------------------------
cd "$WORK"
"$SPIN" new spinel-ansi >/dev/null
rm -rf spinel-ansi/bin
printf '[package]\nname = "ansi"\n' > spinel-ansi/spin.toml
cat > spinel-ansi/ansi.rb <<'EOF'
module Ansi
  def self.red(s) = "\e[31m" + s + "\e[0m"
end
EOF

# --- git-source package (file:// remote) -------------------------------------------
mkdir gitgem
cd gitgem
git init -q
printf '[package]\nname = "greet"\n' > spin.toml
printf 'module Greet\n  def self.hi(n) = "hi " + n\nend\n' > greet.rb
git add spin.toml greet.rb
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm init
cd "$WORK/app"

printf '[package]\nname = "app"\n\n[dependencies]\nansi = { path = "../spinel-ansi" }\ngreet = { git = "file://%s/gitgem" }\n' "$WORK" > spin.toml
printf 'require "ansi"\nrequire "greet"\nputs Ansi.red(Greet.hi("spin"))\n' > bin/app.rb

expect "fetch" "fetched 2 package(s)" "$("$SPIN" fetch 2>&1 | tail -1)"
WANT_OUT=$(printf '\033[31mhi spin\033[0m')
expect "run with deps" "$WANT_OUT" "$("$SPIN" run 2>&1 | tail -1)"

# --- lock ----------------------------------------------------------------------
"$SPIN" lock >/dev/null
[ -f spin.lock ] || fail "spin.lock not written"
grep -q '^\[lock\.greet\]$' spin.lock || fail "spin.lock lacks [lock.greet]"
grep -q '^ref = "[0-9a-f]\{40\}"$' spin.lock || fail "spin.lock lacks a full-SHA ref"

# --- add / remove edit the manifest --------------------------------------------
"$SPIN" add extra --path ../spinel-ansi >/dev/null
grep -q '^extra = ' spin.toml || fail "spin add didn't edit spin.toml"
"$SPIN" remove extra >/dev/null
grep -q '^extra = ' spin.toml && fail "spin remove didn't edit spin.toml"

# --- test: snapshot regen + CRuby parity fallback (both need dep -I) ------------
printf 'require "ansi"\nputs Ansi.red("t")\n' > test/color_test.rb
expect "test (CRuby parity)" "1/1 passed" "$("$SPIN" test 2>&1 | tail -1)"
"$SPIN" test --regen >/dev/null 2>&1
[ -s test/color_test.rb.expected ] || fail "test --regen wrote no snapshot"
expect "test (snapshot)" "1/1 passed" "$("$SPIN" test 2>&1 | tail -1)"

# a snapshot has to round-trip: `test --regen` then `test` must agree. regen
# took stdout alone while the comparison takes stdout+stderr, so any program
# writing to stderr regenerated a snapshot the next run could not match (#3405).
printf '$stderr.puts "err"\nputs "out"\n' > test/streams_test.rb
"$SPIN" test --regen streams_test.rb >/dev/null 2>&1
grep -q '^err$' test/streams_test.rb.expected || fail "test --regen dropped stderr"
expect "test (regen round-trip)" "1/1 passed" "$("$SPIN" test streams_test.rb 2>&1 | tail -1)"
rm -f test/streams_test.rb test/streams_test.rb.expected

# the compiler beside spin has to be an executable FILE. A checkout of the
# compiler repo cloned next to the project is a DIRECTORY of that name, and
# spin used to try to run it (#3407).
mkdir -p "$WORK/sib/spinel"
cp "$SPIN" "$WORK/sib/spin"
printf 'puts "sib"\n' > test/sib_test.rb        # fresh: must actually compile
# What it falls through TO is the compiler on PATH, so put it there rather than
# relying on the caller's: a developer shell usually has bin/ on PATH and CI
# does not, which is the difference between this passing everywhere and only
# passing locally.
expect "test (sibling spinel dir)" "1/1 passed" "$(PATH="$(dirname "$SPIN"):$PATH" "$WORK/sib/spin" test sib_test.rb 2>&1 | tail -1)"
rm -rf "$WORK/sib"; rm -f test/sib_test.rb test/sib_test.rb.expected

# a build that FAILS must not be reported ok by the run phase: a failed compile
# leaves the previous binary where it was, and File.exist? read that as "it
# built", so `spin test` printed the parse error and then ran the executable an
# older source had produced (#4085).
printf 'puts "first"\n' > test/stale_test.rb
printf 'first\n' > test/stale_test.rb.expected
expect "test (builds once)" "1/1 passed" "$("$SPIN" test stale_test.rb 2>&1 | tail -1)"
printf 'def broken\n  puts "first"\n' > test/stale_test.rb   # no closing end
out=$("$SPIN" test stale_test.rb 2>&1) && status=0 || status=$?
expect "test (broken build is not ok)" "0/1 passed" "$(printf '%s\n' "$out" | tail -1)"
case "$out" in *"ok   stale_test.rb"*) fail "broken build still reported ok" ;; esac
if [ "$status" = "0" ]; then fail "broken build exited 0"; fi
rm -f test/stale_test.rb test/stale_test.rb.expected

# a large test/ directory: enumerating ~60 entries allocates enough to GC
# mid-glob, which swept the unrooted result array (heap corruption before
# any child spawned, #2178)
for i in $(seq 1 60); do printf 'puts 1\n' > "test/gc$i.rb"; done
expect "test (many files)" "1/1 passed" "$("$SPIN" test color_test.rb 2>&1 | tail -1)"
rm -f test/gc*.rb

# --- carried native C (M2): package .c compiled to the shared cache, --link'ed ------
cd "$WORK"
mkdir -p spinel-fast
printf '[package]\nname = "fast"\n' > spinel-fast/spin.toml
cat > spinel-fast/fast.rb <<'EOF'
module Fast
  ffi_func :fast_quad, [:int], :int
end
EOF
cat > spinel-fast/fast_ext.c <<'EOF'
#include <stdint.h>
intptr_t fast_quad(intptr_t x) { return x * 4; }
EOF
cd "$WORK/app"
printf '[package]\nname = "app"\n\n[dependencies]\nansi = { path = "../spinel-ansi" }\ngreet = { git = "file://%s/gitgem" }\nfast = { path = "../spinel-fast" }\n' "$WORK" > spin.toml
printf 'require "ansi"\nrequire "greet"\nrequire "fast"\nputs Ansi.red(Greet.hi("spin"))\nputs Fast.fast_quad(10)\n' > bin/app.rb
OUT=$("$SPIN" run 2>&1)
echo "$OUT" | grep -q "^cc fast/fast_ext.c$" || fail "native compile line missing"
expect "carried-C result" "40" "$(echo "$OUT" | tail -1)"
# second build: object cached, no recompile
"$SPIN" clean >/dev/null
OUT=$("$SPIN" run 2>&1)
echo "$OUT" | grep -q "^cc " && fail "native object not reused from cache"
expect "carried-C cached result" "40" "$(echo "$OUT" | tail -1)"
# touching the .c invalidates the cached object and the app binary
sleep 1
touch ../spinel-fast/fast_ext.c
OUT=$("$SPIN" run 2>&1)
echo "$OUT" | grep -q "^cc fast/fast_ext.c$" || fail "touched .c not recompiled"

# CC is a command line, not a path: a compiler-cache wrapper makes it two words,
# which is what CI and most developer setups pass. The object cache keys its
# directory on the compiler, so the space went straight into a path and the
# unquoted -o argument split at it.
"$SPIN" clean >/dev/null
OUT=$(CC="env ${CC:-cc}" "$SPIN" run 2>&1)
expect "carried-C with a wrapped CC" "40" "$(echo "$OUT" | tail -1)"

# --- unresolved require is a hard error (spin sets SPINEL_REQUIRE_GATE) ---------
printf 'require "nosuchgem"\nputs 1\n' > bin/broken.rb
if "$SPIN" build broken >/dev/null 2>"$WORK/gate.err"; then
  fail "unresolved require compiled anyway"
fi
grep -q "nosuchgem" "$WORK/gate.err" || fail "gate error doesn't name the missing gem"
rm -f bin/broken.rb

# --- vendor -> offline, with and without the lock -------------------------------
"$SPIN" vendor >/dev/null
rm -rf "$XDG_CACHE_HOME"
"$SPIN" clean >/dev/null
OUT=$(SPIN_OFFLINE=1 "$SPIN" run 2>&1)
expect "offline (locked, vendored)" "$WANT_OUT
40" "$(echo "$OUT" | tail -2)"
rm -f spin.lock
"$SPIN" clean >/dev/null
OUT=$(SPIN_OFFLINE=1 "$SPIN" run 2>&1)
expect "offline (no lock)" "$WANT_OUT
40" "$(echo "$OUT" | tail -2)"

# --- list / tree -----------------------------------------------------------------
"$SPIN" list | grep -q "^ansi 0.0.0 (path " || fail "spin list lacks the path dep"
"$SPIN" list | grep -q "^greet 0.0.0 (git " || fail "spin list lacks the git dep"
"$SPIN" list --json | grep -q '^\[{"name":"' || fail "spin list --json shape"
"$SPIN" tree | grep -q "^  fast 0.0.0" || fail "spin tree lacks the nested dep line"
"$SPIN" tree --json | grep -q '"deps":\[' || fail "spin tree --json shape"

# --- index (M3): TOML index, MVS selection, search ------------------------------
cd "$WORK"
mkdir hello
cd hello
git init -q
printf '[package]\nname = "hello"\nversion = "1.0.0"\n' > spin.toml
printf 'module Hello\n  def self.greet = "hello v1"\nend\n' > hello.rb
git add spin.toml hello.rb
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm v1
SHA1=$(git rev-parse HEAD)
printf '[package]\nname = "hello"\nversion = "1.1.0"\n' > spin.toml
printf 'module Hello\n  def self.greet = "hello v11"\nend\n' > hello.rb
git add spin.toml hello.rb
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm v11
SHA2=$(git rev-parse HEAD)
cd "$WORK"
mkdir -p index/packages
cd index
git init -q
printf 'name = "hello"\nrepo = "file://%s/hello"\n\n[[release]]\nversion = "1.0.0"\nref = "%s"\n\n[[release]]\nversion = "1.1.0"\nref = "%s"\n' "$WORK" "$SHA1" "$SHA2" > packages/hello.toml
git add packages
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm seed
cd "$WORK"
export SPIN_INDEX="file://$WORK/index"
"$SPIN" new idxapp >/dev/null
cd idxapp
printf '[package]\nname = "idxapp"\n\n[dependencies]\nhello = ">= 1.0"\n' > spin.toml
printf 'require "hello"\nputs Hello.greet\n' > bin/idxapp.rb
expect "index MVS (lowest satisfying)" "hello v1" "$("$SPIN" run 2>&1 | tail -1)"
"$SPIN" lock >/dev/null
grep -q '^version = "1.0.0"$' spin.lock || fail "index lock lacks the selected version"
grep -q "^ref = \"$SHA1\"$" spin.lock || fail "index lock lacks the release SHA"
printf '[package]\nname = "idxapp"\n\n[dependencies]\nhello = "~> 1.1"\n' > spin.toml
OUT=$("$SPIN" run 2>&1)
echo "$OUT" | grep -q "reselecting 1.1.0" || fail "constraint change didn't reselect"
expect "index reselect" "hello v11" "$(echo "$OUT" | tail -1)"
"$SPIN" search hell | grep -q "^hello 1.1.0 " || fail "spin search misses the gem"
printf '[package]\nname = "idxapp"\n\n[dependencies]\n' > spin.toml
"$SPIN" add hello --version "~> 1.0" >/dev/null
grep -q '^hello = "~> 1.0"$' spin.toml || fail "spin add --version didn't write the constraint"
"$SPIN" lock >/dev/null
"$SPIN" vendor >/dev/null
rm -rf "$XDG_CACHE_HOME/spin/packages" "$XDG_CACHE_HOME/spin/index"
"$SPIN" clean >/dev/null
expect "index offline (locked, vendored)" "hello v1" "$(SPIN_OFFLINE=1 "$SPIN" run 2>&1 | tail -1)"
unset SPIN_INDEX

# --- publish (M4): validations + --direct into the index -------------------------
export SPIN_INDEX="file://$WORK/index"
git -C "$WORK/index" config receive.denyCurrentBranch updateInstead
cd "$WORK"
"$SPIN" new publib --lib >/dev/null
cd publib
printf '[package]\nname = "publib"\nversion = "0.1.0"\n' > spin.toml
printf 'module Publib\n  def self.hi = "hi"\nend\n' > publib.rb
git init -q
git add spin.toml publib.rb .gitignore
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm v1
git init -q --bare "$WORK/publib-remote.git"
git remote add origin "$WORK/publib-remote.git"
git push -q origin HEAD
git fetch -q origin
OUT=$("$SPIN" publish --direct --repo https://example.com/you/spinel-publib 2>&1) \
  && fail "publish without tests must be refused"
echo "$OUT" | grep -q "publish requires tests" || fail "missing-tests message"
printf 'require "publib"\nputs Publib.hi\n' > test/hi_test.rb
git add test/hi_test.rb
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm tests
git push -q origin HEAD
git fetch -q origin
expect "publish --direct" "published publib 0.1.0 (direct)" "$("$SPIN" publish --direct --repo https://example.com/you/spinel-publib 2>&1 | tail -1)"
grep -q '^ref = "[0-9a-f]\{40\}"$' "$WORK/index/packages/publib.toml" || fail "published entry lacks a full SHA"
OUT=$("$SPIN" publish --direct --repo https://example.com/you/spinel-publib 2>&1) \
  && fail "duplicate publish must be refused"
echo "$OUT" | grep -q "already in the index" || fail "duplicate message"
OUT=$("$SPIN" publish --direct --repo https://example.com/other/spinel-publib 2>&1) \
  && fail "same name, different repo must be refused"
echo "$OUT" | grep -q "name policy" || fail "name-policy message"
"$SPIN" search publib | grep -q "^publib 0.1.0 " || fail "published package missing from search"

# --- R8 probes: publish records a pass; resolution warns on recorded fails -------
grep -q '^\[\[probe\]\]$' "$WORK/index/packages/publib.toml" || fail "publish wrote no probe record"
grep -q '^result = "pass"$' "$WORK/index/packages/publib.toml" || fail "probe record isn't a pass"
REV=$(cd "$WORK" && "$(dirname "$SPIN")/spinel" --version | awk '{print $2}')
printf '\n[[probe]]\nversion = "0.1.0"\nspinel = "%s"\nresult = "fail"\ndetail = "e2e-injected"\ndate = "2026-01-01"\n' "$REV" >> "$WORK/index/packages/publib.toml"
git -C "$WORK/index" add packages/publib.toml
git -C "$WORK/index" -c user.email=spin@e2e -c user.name=spin-e2e commit -qm failprobe
cd "$WORK"
"$SPIN" new probeapp >/dev/null
cd probeapp
printf '[package]\nname = "probeapp"\n\n[dependencies]\npublib = "0.1.0"\n' > spin.toml
rm -rf "$XDG_CACHE_HOME/spin/index"
OUT=$("$SPIN" fetch 2>&1 || true)
echo "$OUT" | grep -q "recorded FAILING with this compiler build" || fail "exact-build fail probe didn't warn"
unset SPIN_INDEX

# --- install: build + copy bin/ onto a prefix, uninstall removes ----------------
cd "$WORK/app"
"$SPIN" install --prefix "$WORK/binhome" >/dev/null
OUT=$("$WORK/binhome/app" | tail -2)
expect "installed binary runs" "$WANT_OUT
40" "$OUT"
"$SPIN" install --uninstall --prefix "$WORK/binhome" >/dev/null
[ -e "$WORK/binhome/app" ] && fail "uninstall left the binary"

# --- [[build]]: declared native build steps (#1820) ------------------------------
# A package vendoring a project with its own build system declares the step;
# it runs at dependent-application build time only (never at fetch), needs
# explicit consent, caches by content key, applies declared patches to a
# scratch copy, and feature-gated entries stay off by default. The same
# package works as the build root (app) and as a path dependency (lib).
export XDG_CONFIG_HOME="$WORK/config"
cd "$WORK"
mkdir -p spinel-mathx/vendor/mx spinel-mathx/patches spinel-mathx/bin
cat > spinel-mathx/spin.toml <<'EOF'
[package]
name = "mathx"
version = "0.1.0"

[[build]]
workdir   = "vendor/mx"
patches   = ["patches/*.patch"]
command   = "${CC:-cc} -O2 -c mx.c -o mx.o && ar rcs libmx.a mx.o"
artifacts = ["libmx.a"]

[[build]]
features  = ["cuda"]
workdir   = "vendor/mx"
command   = "${CC:-cc} -O2 -DCUDA -c mx.c -o mxc.o && ar rcs libmx-cuda.a mxc.o"
artifacts = ["libmx-cuda.a"]

[native]
libs = ["${build.out}/libmx.a", "${build.out}/libmx-cuda.a"]
EOF
printf 'int mx_add(int a, int b) { return a + b; }\n' > spinel-mathx/vendor/mx/mx.c
cat > spinel-mathx/patches/01-bias.patch <<'EOF'
--- a/mx.c
+++ b/mx.c
@@ -1 +1 @@
-int mx_add(int a, int b) { return a + b; }
+int mx_add(int a, int b) { return a + b + 1; }
EOF
printf 'module Mathx\n  ffi_func :mx_add, [:int, :int], :int\nend\n' > spinel-mathx/mathx.rb
printf 'require "mathx"\nputs Mathx.mx_add(20, 21)\n' > spinel-mathx/bin/mxdemo.rb

# app-as-root: unconsented build is refused with instructions
cd spinel-mathx
OUT=$("$SPIN" build 2>&1) && fail "unconsented native build must be refused"
echo "$OUT" | grep -q "declares a native build step" || fail "refusal message"
echo "$OUT" | grep -q "allow-native-build" || fail "refusal hint"
# consented: the patch applied to the scratch copy biases the sum by one
OUT=$("$SPIN" run --allow-native-build 2>&1)
expect "native app-as-root run (patched)" "42" "$(echo "$OUT" | tail -1)"
echo "$OUT" | grep -q '^native mathx:' || fail "native build step didn't run"
# the vendored tree is a read-only input: the patch never touches it
grep -q 'a + b + 1' vendor/mx/mx.c && fail "patch leaked into the vendored tree"
# feature-gated entry stays off by default: only the default artifact exists
N_ART=$(find "$XDG_CACHE_HOME/spin/native" -name 'libmx*.a' | wc -l | tr -d ' ')
expect "feature-gated artifact count" "1" "$N_ART"
# second build reuses the content-keyed cache (no native line)
"$SPIN" clean >/dev/null
OUT=$("$SPIN" run --allow-native-build 2>&1)
echo "$OUT" | grep -q '^native mathx:' && fail "cached native build re-ran"
expect "native cached rerun" "42" "$(echo "$OUT" | tail -1)"
# a content change moves the key and rebuilds
printf 'int mx_add(int a, int b) { return a + b; }\nint mx_two(void) { return 2; }\n' > vendor/mx/mx.c
printf -- '--- a/mx.c\n+++ b/mx.c\n@@ -1,2 +1,2 @@\n-int mx_add(int a, int b) { return a + b; }\n+int mx_add(int a, int b) { return a + b + 1; }\n int mx_two(void) { return 2; }\n' > patches/01-bias.patch
OUT=$("$SPIN" run --allow-native-build 2>&1)
echo "$OUT" | grep -q '^native mathx:' || fail "content change didn't rebuild the native step"
expect "native rebuild after change" "42" "$(echo "$OUT" | tail -1)"

# lib case: a consumer app pulls mathx as a path dependency. A fresh cache:
# a cached artifact skips consent by design (the consented command already
# ran on this machine), so refusal is only observable on a cold cache.
export XDG_CACHE_HOME="$WORK/cache-nb"
cd "$WORK"
mkdir -p nbconsumer/bin
printf '[package]\nname = "nbconsumer"\n\n[dependencies]\nmathx = { path = "../spinel-mathx" }\n' > nbconsumer/spin.toml
printf 'require "mathx"\nputs Mathx.mx_add(100, 100)\n' > nbconsumer/bin/app.rb
cd nbconsumer
# fetch never runs a native build (R2: packages compute nothing at fetch)
"$SPIN" fetch >/dev/null 2>&1
# unconsented dependent build is refused; `spin trust` records durable consent
OUT=$("$SPIN" build 2>&1) && fail "unconsented dependent native build must be refused"
"$SPIN" trust mathx | grep -q '^trusted: mathx' || fail "spin trust"
OUT=$("$SPIN" run 2>&1)
expect "native lib-dependency run (patched)" "201" "$(echo "$OUT" | tail -1)"

# consumer-side features: `spin add --features` records the enablement in the
# manifest (cargo-style; the lock stays resolution-only), the gated [[build]]
# entry runs, and its artifact joins the link line.
cd "$WORK"
printf 'int mx_cuda(void) { return 999; }\n' > spinel-mathx/vendor/mx/cuda.c
cat >> spinel-mathx/spin.toml <<'EOF'

[[build]]
features  = ["cuda"]
workdir   = "vendor/mx"
command   = "${CC:-cc} -O2 -c cuda.c -o cuda.o && ar rcs libmx-cu2.a cuda.o"
artifacts = ["libmx-cu2.a"]

[native]
libs = ["${build.out}/libmx.a", "${build.out}/libmx-cuda.a", "${build.out}/libmx-cu2.a"]
EOF
printf 'module Mathx\n  ffi_func :mx_add, [:int, :int], :int\n  ffi_func :mx_cuda, [], :int\nend\n' > spinel-mathx/mathx.rb
mkdir -p featconsumer/bin
printf '[package]\nname = "featconsumer"\n' > featconsumer/spin.toml
printf 'require "mathx"\nputs Mathx.mx_cuda\n' > featconsumer/bin/app.rb
cd featconsumer
"$SPIN" add mathx --path ../spinel-mathx --features cuda >/dev/null
grep -q 'features = \["cuda"\]' spin.toml || fail "spin add --features didn't record the enablement"
OUT=$("$SPIN" run 2>&1)
expect "feature-enabled native run" "999" "$(echo "$OUT" | tail -1)"
find "$XDG_CACHE_HOME/spin/native" -name 'libmx-cuda.a' | grep -q . || fail "enabled feature entry didn't build"

# --- [[build]]: a symlinked workdir must not write through into the target ------
# cp -R of a symlink source copies the LINK: every later step (patch, the
# build command) then runs inside the live tree the link points at (#2180
# destroyed a real checkout's outputs this way). The scratch copy now
# dereferences the workdir operand; the pointed-to tree stays pristine.
cd "$WORK"
mkdir -p symreal spinel-symx/vendor spinel-symx/patches spinel-symx/bin
printf 'int sx_add(int a, int b) { return a + b; }\n' > symreal/sx.c
printf 'PRECIOUS\n' > symreal/prior-output.a
ln -s "$WORK/symreal" spinel-symx/vendor/sx
cat > spinel-symx/spin.toml <<'SYMEOF'
[package]
name = "symx"
version = "0.1.0"

[[build]]
workdir   = "vendor/sx"
patches   = ["patches/*.patch"]
command   = "${CC:-cc} -O2 -c sx.c -o sx.o && ar rcs libsx.a sx.o"
artifacts = ["libsx.a"]

[native]
libs = ["${build.out}/libsx.a"]
SYMEOF
printf -- '--- a/sx.c\n+++ b/sx.c\n@@ -1 +1 @@\n-int sx_add(int a, int b) { return a + b; }\n+int sx_add(int a, int b) { return a + b + 1; }\n' > spinel-symx/patches/01-bias.patch
printf 'module Symx\n  ffi_func :sx_add, [:int, :int], :int\nend\n' > spinel-symx/symx.rb
printf 'require "symx"\nputs Symx.sx_add(20, 21)\n' > spinel-symx/bin/sxdemo.rb
cd spinel-symx
OUT=$("$SPIN" run --allow-native-build 2>&1)
expect "symlinked-workdir native run (patched)" "42" "$(echo "$OUT" | tail -1)"
# the LIVE tree behind the link stays pristine: source unpatched, no build
# droppings (.o/.a/.rej), the prior output intact
grep -q 'a + b + 1' "$WORK/symreal/sx.c" && fail "patch wrote through the symlinked workdir"
[ "$(cat "$WORK/symreal/prior-output.a")" = "PRECIOUS" ] || fail "prior output clobbered through the symlink"
LEFT=$(ls "$WORK/symreal" | sort | tr '\n' ' ')
expect "symlinked tree contents" "prior-output.a sx.c " "$LEFT"

# --- [[build]] mechanics from the guinea-pig report (#1845) ---------------------
# One package exercising: a top-level (non-vendor/) workdir excluded from
# carried-C discovery (its source hard-errors under a bare cc sweep);
# relative-path artifacts (a header staged under inc/); ${build.out} in a
# later entry's command (cross-entry inputs); dot-dir exclusion from the
# content key (a .git in the workdir must not churn the cache); and an
# ffi_lib name satisfied by the --link artifact (no -l against system paths).
cd "$WORK"
mkdir -p spinel-crossx/csrc spinel-crossx/csrc2
cat > spinel-crossx/spin.toml <<'EOF'
[package]
name = "crossx"
version = "0.1.0"

[[build]]
workdir   = "csrc"
command   = "${CC:-cc} -DCXBUILD=1 -O2 -c cx.c -o cx.o && ar rcs libcx.a cx.o && mkdir -p inc && cp cx.h inc/cx.h"
artifacts = ["libcx.a", "inc/cx.h"]
exclude   = ["devout*"]

[[build]]
workdir   = "csrc2"
command   = "${CC:-cc} -DCXBUILD=1 -O2 -I ${build.out}/inc -c cx2.c -o cx2.o && ar rcs libcx2.a cx2.o"
artifacts = ["libcx2.a"]

[native]
libs = ["${build.out}/libcx.a", "${build.out}/libcx2.a"]
EOF
printf '#ifndef CXBUILD\n#error carried-C swept a [[build]] workdir\n#endif\nint cx_val(void) { return 7; }\nlong cx_fsum(const double *d, long n) { return d && n >= 2 ? (long)(d[0] + d[1]) : -1; }\n' > spinel-crossx/csrc/cx.c
printf '#define CX_BONUS 30\n' > spinel-crossx/csrc/cx.h
printf '#ifndef CXBUILD\n#error carried-C swept a [[build]] workdir\n#endif\n#include "cx.h"\nint cx2_val(void) { return CX_BONUS + 5; }\n' > spinel-crossx/csrc2/cx2.c
printf 'module Crossx\n  ffi_lib "cx"\n  ffi_func :cx_val, [], :int\n  ffi_func :cx2_val, [], :int\n  ffi_func :cx_fsum, [:float_array, :int], :int\nend\n' > spinel-crossx/crossx.rb
mkdir -p crossapp/bin
printf '[package]\nname = "crossapp"\n\n[dependencies]\ncrossx = { path = "../spinel-crossx" }\n' > crossapp/spin.toml
printf 'require "crossx"\ndef widen(a)\n  a\nend\nwiden(["x"])\nputs Crossx.cx_fsum(widen([60.5, 4.5]), 2)\nputs Crossx.cx_val + Crossx.cx2_val\n' > crossapp/bin/app.rb
cd crossapp
OUT=$(SPIN_ALLOW_NATIVE_BUILD=1 "$SPIN" run 2>&1)
expect "cross-entry native run" "42" "$(echo "$OUT" | tail -1)"
# a poly-collapsed float array marshals its element data (the :float_array
# twin of the #1855/#1867 int fix; 60.5+4.5=65, NULL would print -1)
echo "$OUT" | grep -q '^65$' || fail "poly-collapsed :float_array marshalled wrong data"
# a .git dropped into the workdir must not move the content key
mkdir -p ../spinel-crossx/csrc/.git
echo junk > ../spinel-crossx/csrc/.git/HEAD
"$SPIN" clean >/dev/null
OUT=$(SPIN_ALLOW_NATIVE_BUILD=1 "$SPIN" run 2>&1)
echo "$OUT" | grep -q '^native crossx:' && fail "VCS metadata churned the native cache key"
expect "dot-dir key stability" "42" "$(echo "$OUT" | tail -1)"
# ...but a SOURCE edit must move the key (the root-prune bug hashed every
# workdir as empty input, so this and the .git test above were
# indistinguishable): a comment appended to cx.c has to trigger a rebuild.
printf '/* key-moving edit */\n' >> ../spinel-crossx/csrc/cx.c
"$SPIN" clean >/dev/null
OUT=$(SPIN_ALLOW_NATIVE_BUILD=1 "$SPIN" run 2>&1)
echo "$OUT" | grep -q '^native crossx:' || fail "source edit did not move the native cache key"
expect "source-edit key movement" "42" "$(echo "$OUT" | tail -1)"
# an `exclude`d glob (a dev tree's build-output dir) must ride neither the
# key nor the scratch copy: dropping junk into it stays a cache hit
mkdir -p ../spinel-crossx/csrc/devout-cuda
echo junk > ../spinel-crossx/csrc/devout-cuda/stale.o
"$SPIN" clean >/dev/null
OUT=$(SPIN_ALLOW_NATIVE_BUILD=1 "$SPIN" run 2>&1)
echo "$OUT" | grep -q '^native crossx:' && fail "excluded glob churned the native cache key"
expect "exclude-glob key stability" "42" "$(echo "$OUT" | tail -1)"
# a [native] libs path no [[build]] entry produces must die loud (a stale
# path otherwise surfaces as undefined symbols at link time)
cp ../spinel-crossx/spin.toml ../spinel-crossx/spin.toml.bak
sed '/^libs/s|libcx2.a"\]|libcx2.a", "${build.out}/libnope.a"]|' ../spinel-crossx/spin.toml.bak > ../spinel-crossx/spin.toml
OUT=$(SPIN_ALLOW_NATIVE_BUILD=1 "$SPIN" run 2>&1) && fail "unproduced [native] libs path did not die"
echo "$OUT" | grep -q 'not produced by any' || fail "unproduced-libs diagnostic missing: $OUT"
mv ../spinel-crossx/spin.toml.bak ../spinel-crossx/spin.toml

# a threaded program takes the package's `_mt` variant: one [[build]] entry
# produces both, `[native] libs` names the plain one, and the compiler prefers
# `<stem>_mt.<ext>` beside every link input (#4032).
cd "$WORK"
mkdir -p spinel-tvar/vendor/tv spinel-tvar/bin
cat > spinel-tvar/spin.toml <<'EOF'
[package]
name = "tvar"
version = "0.1.0"

[[build]]
workdir   = "vendor/tv"
command   = "${CC:-cc} -O2 -c tv.c -o tv.o && ${CC:-cc} -O2 -DSP_THREADS -ftls-model=initial-exec -c tv.c -o tv_mt.o"
artifacts = ["tv.o", "tv_mt.o"]

[native]
libs = ["${build.out}/tv.o"]
EOF
cat > spinel-tvar/vendor/tv/tv.c <<'EOF'
#ifdef SP_THREADS
int tv_variant(void) { return 2; }
#else
int tv_variant(void) { return 1; }
#endif
EOF
printf 'module Tvar
  ffi_func :tv_variant, [], :int
end
' > spinel-tvar/tvar.rb
printf 'require "tvar"
puts Tvar.tv_variant
' > spinel-tvar/bin/plain.rb
printf 'require "tvar"
t = Thread.new { Tvar.tv_variant }
puts t.value
' > spinel-tvar/bin/threaded.rb
cd spinel-tvar
OUT=$("$SPIN" run plain --allow-native-build 2>&1)
expect "native variant: single-threaded takes the plain object" "1" "$(echo "$OUT" | tail -1)"
OUT=$("$SPIN" run threaded --allow-native-build 2>&1)
expect "native variant: threaded takes the _mt object" "2" "$(echo "$OUT" | tail -1)"
# --- `spin flags`: the handoff to a build spin does not drive (#4105) ---------
# spin resolves the dependencies and warms the native cache, then prints the
# flags; whoever is driving the build compiles with them. What it prints has to
# be what `spin build` compiles with, so the check is that a hand-driven
# compile produces the same program -- not that the text matches.
cd "$WORK/app"
rm -rf "$XDG_CACHE_HOME/spin/native"          # cold: `flags` must compile the carried C
FLAGS=$("$SPIN" flags 2>/dev/null)
# One line. A cold cache compiles here, and that progress used to print on
# stdout, which would splice `cc fast/fast_ext.c` into the flag string.
[ "$(printf '%s' "$FLAGS" | wc -l)" = "0" ] || fail "spin flags: progress leaked onto stdout"
case "$FLAGS" in
  --require-gate*) ;;
  *) fail "spin flags: does not carry the require gate spin build compiles under" ;;
esac
# Absolute throughout: the caller's working directory is its own tree.
case "$FLAGS" in
  *" -I ."*|*" -I ../"*|*" --link ."*|*" --link ../"*)
    fail "spin flags: relative path in [$FLAGS]" ;;
esac
case "$FLAGS" in
  *--link*) ;;
  *) fail "spin flags: carried-C object missing from [$FLAGS]" ;;
esac
SPINEL_BIN=$(dirname "$SPIN")/spinel
"$SPINEL_BIN" $FLAGS bin/app.rb -o "$WORK/handoff" >/dev/null 2>&1 ||
  fail "spin flags: hand-driven compile failed"
expect "spin flags: hand-driven build matches spin build" \
  "$("$SPIN" run 2>&1 | tail -1)" "$("$WORK/handoff" 2>&1 | tail -1)"

# --- `[package] exclude`: C the package says is not part of this build --------
# `.rb` enters by require, `.c` by presence, so an application whose repository
# also holds a C program of its own has no other way to keep it out. Without
# the field its main() collides with the generated one and the link fails.
cd "$WORK"
mkdir -p excl/bin
printf 'puts "excluded ok"\n' > excl/bin/excl.rb
cat > excl/standalone.c <<'EOF'
#include <stdio.h>
int main(void) { puts("a program of my own"); return 0; }
EOF
mkdir -p excl/cbits
printf 'int cbits_unused(void) { return 7; }\n' > excl/cbits/helper.c
cd excl
printf '[package]\nname = "excl"\n' > spin.toml
"$SPIN" build >/dev/null 2>&1 && fail "exclude: a main()-bearing .c linked in without complaint"
printf '[package]\nname = "excl"\nexclude = ["standalone.c", "cbits"]\n' > spin.toml
expect "exclude: named file and directory both pruned" "excluded ok" "$("$SPIN" run 2>&1 | tail -1)"

# --- the runtime headers when spin and the compiler are not co-located (#4115) -
# `spin install` puts spin in ~/.local/bin with no spinel beside it, and package
# C includes "spinel/runtime.h". spin gave up locating the headers the moment
# the compiler came from PATH, so -I <spinel>/lib vanished and every package
# carrying C failed on a missing runtime.h -- while the same package built fine
# from a tree where the two sat together.
cd "$WORK"
mkdir -p lonely/bin
cp "$SPIN" lonely/bin/spin                     # spin alone; no spinel beside it
cd "$WORK/app"
OUT=$(PATH="$(dirname "$SPIN"):$PATH" SPIN_NO_NATIVE_CACHE=1 "$WORK/lonely/bin/spin" flags 2>&1) || true
case "$OUT" in
  *"runtime.h"*|*"native compile failed"*)
    fail "headers not found when spin is not beside the compiler: [$OUT]" ;;
esac
case "$OUT" in
  *--link*) ;;
  *) fail "PATH-resolved compiler: carried C did not build [$OUT]" ;;
esac

# ... and through a SYMLINK, which is what `spin install` leaves behind: a link
# in ~/.local/bin pointing into the install tree, with no lib beside the link.
# PATH hands back the link itself, so the walk for the headers started from the
# wrong parent and found nothing (#4126).
#
# The package has to include "spinel/runtime.h" for this to test anything --
# the carried C above includes only <stdint.h>, so it compiles with or without
# the runtime include path, and a check written against it passes either way.
cd "$WORK"
mkdir -p spinel-rthdr linked/bin
printf '[package]\nname = "rthdr"\n' > spinel-rthdr/spin.toml
cat > spinel-rthdr/rthdr.rb <<'EOF'
module Rthdr
  native_lib "rthdr"
  native_func :len2, [:string], :int, "sp_rthdr_len2"
end
EOF
cat > spinel-rthdr/sp_rthdr.c <<'EOF'
#include "spinel/runtime.h"
sp_int sp_rthdr_len2(const char *s) { return (sp_int)sp_str_byte_len(s) * 2; }
EOF
ln -sf "$SPIN" linked/bin/spin
ln -sf "$(dirname "$SPIN")/spinel" linked/bin/spinel
[ -f "$(dirname "$SPIN")/spinel_rbs_extract" ] && ln -sf "$(dirname "$SPIN")/spinel_rbs_extract" linked/bin/spinel_rbs_extract
mkdir -p rtapp/bin
printf '[package]\nname = "rtapp"\n\n[dependencies]\nrthdr = { path = "../spinel-rthdr" }\n' > rtapp/spin.toml
printf 'require "rthdr"\nputs Rthdr.len2("abc")\n' > rtapp/bin/rtapp.rb
cd "$WORK/rtapp"
# SPIN_NO_NATIVE_CACHE, or a cached object is reused and nothing is compiled --
# exactly the masking #4115 complained about, and it made this check pass
# against the bug the first time it was written.
# `|| true`: spin exits non-zero when the compile fails, and under set -e the
# assignment itself would then end the script before the check below could
# say what went wrong -- a reproduction that dies silently.
OUT=$(PATH="$WORK/linked/bin:$PATH" SPIN_NO_NATIVE_CACHE=1 spin flags 2>&1) || true
case "$OUT" in
  *"runtime.h"*|*"native compile failed"*)
    fail "runtime headers not found through a symlinked install: [$OUT]" ;;
esac
expect "symlinked install builds and runs" "6" \
  "$(PATH="$WORK/linked/bin:$PATH" spin run 2>&1 | tail -1)"
cd "$WORK/app"

# --- the native cache is relocatable and skippable (#4115) --------------------
# A run behaves differently depending on whether an object is already cached,
# which is what you least want while working out why a build differs.
rm -rf "$WORK/ncache"
OUT=$(SPIN_NATIVE_CACHE="$WORK/ncache" "$SPIN" flags 2>/dev/null)
case "$OUT" in
  *"$WORK/ncache"*) ;;
  *) fail "SPIN_NATIVE_CACHE ignored: [$OUT]" ;;
esac
# Cached: the second run compiles nothing. Not cached: it compiles again.
CC1=$(SPIN_NATIVE_CACHE="$WORK/ncache" "$SPIN" flags 2>&1 >/dev/null | grep -c "^cc " || true)
expect "native cache reused on the second run" "0" "$CC1"
CC2=$(SPIN_NATIVE_CACHE="$WORK/ncache" SPIN_NO_NATIVE_CACHE=1 "$SPIN" flags 2>&1 >/dev/null | grep -c "^cc " || true)
[ "$CC2" -ge 1 ] || fail "SPIN_NO_NATIVE_CACHE did not force a rebuild"

echo "spin-e2e: ALL GREEN"
