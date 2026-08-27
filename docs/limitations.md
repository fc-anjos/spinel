# Spinel limitations: what an AOT compiler can and cannot do

Spinel is a whole-program **ahead-of-time** compiler: it reads the entire
program, infers a static type for every value, emits C, compiles it, and runs
the binary. There is no Ruby interpreter, parser, or type-inference engine in
the running program — it is just C. That model is what buys the speedup, and it
is also the source of every limitation below.

This document is the honest catalogue. It is organized by *kind* of limit:

- **Fundamental** — incompatible with whole-program AOT; will not change without
  abandoning the model (e.g. bundling an interpreter).
- **Partial / relaxable** — genuinely limited today, but additively fixable.
- **By design** — a deliberate, documented choice; the intentional CRuby
  deviations are catalogued under [By design](#by-design-deliberate-choices)
  below.
- **Now supported** — things that are *not* limits (corrects older write-ups
  that described an earlier version of the compiler).

---

## Fundamental limits (inherent to AOT)

These need a runtime parser, a runtime metaobject protocol, an allocation
registry, or stack reification — none of which exist in a flat compiled binary.

| Feature | Behaviour | Why it's fundamental |
|---|---|---|
| `eval` / `instance_eval("str")` / `class_eval("str")` | unsupported | needs a runtime parser + type system. (Block forms — `instance_eval { }` — DO work; the block is compiled.) |
| `method_missing` | not dispatched (defining it warns at compile time) | every call site is a direct C call; an undefined-method call can't fall back to a per-receiver hook. The method is still callable explicitly. |
| `define_method` with a runtime-computed name/body | only literal names work | a runtime-built method has no compiled body |
| `ObjectSpace` (`each_object`, `count_objects`) | unsupported | no class-keyed allocation registry; the GC tracks bytes, not a live-object index |
| `TracePoint` / `set_trace_func` | unsupported | require an interpreter loop to hook |
| `binding` as an object | unsupported | reifying the local scope needs a runtime name->slot table; locals are C stack slots. `binding.local_variable_get(:x)` with a **literal** name *is* supported -- it resolves to the known slot at compile time |
| Refinements (`refine` / `using`) | no-op / unresolved | scope-keyed dispatch is incompatible with direct C calls |
| `callcc` / `Continuation` | unsupported | multi-shot full-stack capture has no flat-C analogue |
| `Class.new(parent) { ... }` (runtime class) | unsupported | the class graph is baked at compile time |
| Singleton methods (`def obj.m`, `class << obj; def m; end; end`, `obj.define_singleton_method(:m) { }`, `obj.extend(Mod)`) on a receiver whose creation site is **not** visible | unsupported | these DO work when the receiver is a constant or a local whose only write is `<UserClass>.new(...)`: the object gets a synthesized anonymous subclass carrying the methods, which is the AOT form of CRuby's hidden singleton class. What is left out is a receiver spinel cannot trace to one `.new` (a factory return, a loop, a conditional), and one whose class has no subclassable layout: `Object.new` / `BasicObject`, a builtin (String, Array), a Struct or Data, an exception. Those report the method as undefined |
| `Object#singleton_class` as an OBJECT (and `Class#attached_object`) | unsupported | the singleton class above is synthesized, not reified: there is no runtime class object to hand back. `class << obj` as a *definition* form works -- see the row above |
| Runtime structural mutation of a class through an explicit receiver (`Klass.include(M)`, `Klass.attr_accessor(...)`, `Klass.define_method(...)` outside the class body) | unsupported | the class graph, ancestor chain, and method/ivar layout are baked at compile time; the same declarations *inside* a `class` body work |
| General reflection (`methods`, `instance_variables`) and `instance_variable_get`/`set` with a **non-literal** name | unsupported | ivars are C struct offsets with no name→offset table; DCE strips method names. A **literal** `instance_variable_get(:@x)` / `instance_variable_set(:@x, v)` *is* supported — it resolves to the known struct offset, like `send(:literal)` below. |
| User-defined `#hash` / `#eql?` for hash *keys* | not dispatched (identity probe) | the hash machinery can't call back into a user method per key |
| A method that **uses its block** (`yield` or `block.call`) **and recurses into itself** (`def rec(n, &b); ...; rec(n-1, &b); yield n; end`) | compile error (loud, was a hang / undefined-symbol) | a block-using method is inlined at each call site (there is no standalone function that takes the block), so a self-call inlines its own body unboundedly — the runtime base case is invisible at compile time. Recursion *through a yielded block* (`with_state { with_state { } }`, finite source nesting) does work |
| `Monitor#class` | reports `Thread::Mutex` | a Monitor IS a mutex here, with reentrancy switched on per object, and the class name for a `TY_MUTEX` value is decided at compile time from the type rather than read off the object. `#synchronize` (including reentrant use), `#try_enter` and mutual exclusion across threads all behave as CRuby's do; only the name differs. `Monitor#new_cond` / the `MonitorMixin` module are not modelled. |
| `require` of stdlib `.rb` that leans on metaprogramming / C extensions (e.g. `json/pure`, the `require "time"` parsing extensions like `Time.parse` / `Time.strptime`) | unsupported | such stdlib code runs off the AOT path. A `require` is resolved at parse time by splicing a bundled `lib/X.rb`; the libraries that ship this way — `set`, `forwardable`, `optparse`, `erb`, `csv`, `pathname`, `stringio`, `strscan` — do work. The built-in `Time` class (`Time.now` / `at` / `local` / `utc`, plus `strftime` / `zone`) works *without* any `require`; only the `require "time"` string-parsing additions are missing. |

**`net/http` / `uri`.** An HTTP/1.1 client with `Connection: close`, one
request per connection -- a second request inside one `Net::HTTP.start` block
reconnects transparently, as CRuby does, rather than failing, but the
connection is not reused: no keep-alive, no pipelining, no HTTP/2, no proxy,
no cookie jar and no automatic redirect following (a 3xx comes back as the
response it is, with its Location). Chunked transfer decoding is there;
content-encoding is not. `URI` parses http, https and a bare form; there is no
URI::FTP or the scheme registry behind it. An https request needs the
`openssl` package below.

**TLS / `openssl`.** The `openssl` package binds the system libssl and
provides `OpenSSL::SSL` only: `SSLContext`, `SSLSocket`, `SSLError` and the
`VERIFY_*` constants, which is what an outbound HTTPS client reaches.
`OpenSSL::Digest::SHA256` / `SHA1` and `OpenSSL::HMAC.hexdigest` are there,
over the runtime's own crypto rather than libssl -- class-method forms only,
and no MD5, which CRuby carries and the runtime does not. `Cipher`, `PKey`,
most of `X509`, and the incremental digest object API are not there, and a
program that names them fails to compile rather than at run time. Spinel
implements no TLS and bundles no trust anchors: the chain is validated against
the operating system's store, so a CA it stops trusting stops being trusted
here on an OS update. The package exists only where libssl's headers were
present at build time.

The require-gated stdlib Spinel *does* provide (`StringIO`, `IO#winsize`,
`Time#iso8601`, ...) requires its `require`, matching CRuby; an unsatisfiable
`require` is a compile error. This is opt-in today via `--require-gate` (or
`SPINEL_REQUIRE_GATE=1`); `spin build` always compiles with it on.
See [require.md](require.md) for which stdlib needs which `require`.
| Mixed / non-UTF-8 encodings | UTF-8 / ASCII-8BIT only | one internal representation; transcoding tables are out of scope |
| Embedded `NUL` in general binary strings | `char *` boundary assumption | most string ops are NUL-terminated at the C boundary |

`send`/`public_send`/`__send__` with a **non-literal** name (`send(meth)`) is
partially supported: an explicit-receiver send lowers to a static dispatch over
the method names that appear as symbol/string **literals** anywhere in the
program — `recv.send(name) → name == :a ? recv.a : name == :b ? recv.b : … :
raise NoMethodError` — with the receiver's type and the argument count selecting
which arms resolve (the result is `poly`). A name that is not one of those
literals, or not a method on the receiver, raises `NoMethodError` at runtime. A
name drawn from outside the program's closed set of literals still can't be
dispatched. A **literal** name is fully resolved — see below.

---

## Partial / relaxable limits

Limited today, but additively fixable; listed roughly easiest-first.

| Feature | Today | Path to relax |
|---|---|---|
| `Exception#backtrace` / `Kernel#caller` | return `[]` (class + message work) | populate frames from a compile-time call-site→source side-table (the `--line-map` map already exists) |
| `Thread` real parallelism | implemented as a true M:N runtime (no GVL): N OS workers (`min(online cores, SPINEL_WORKERS)`) run green threads in parallel over a stop-the-world GC, with real `Mutex`/`Queue`/`SizedQueue`/`ConditionVariable`. A monitor thread timeslices CPU-bound threads (~10ms quantum) so a thread looping without yielding cannot starve its siblings (it signals the worker with `SIGURG`, overridable via `SPINEL_PREEMPT_SIGNAL`). The single-threaded archive is unchanged (a non-`Thread` program is byte-identical) | the N workers run per-worker run queues with work stealing, and `Kernel#sleep` and blocking I/O are scheduler-aware (a sleeping / I/O-blocked thread frees its OS worker). preemption is taken at safepoint polls (loop back-edges), so a thread spending a long time inside a single runtime call with no poll yields only when that call returns; concurrent allocation is thread-safe (heap-lock-protected allocators, atomic heap byte counters, per-worker object pools) but every allocation still crosses one global heap lock; remaining work: fully async (signal-interrupted) preemption of such regions, and per-worker allocation buffers (TLAB) to make allocation-heavy parallel code scale. See [docs/thread.md](thread.md) |
| `Marshal` of user objects with container-typed ivars | primitives + Array + Hash + Bignum + Complex + Rational + plain user objects work, including cyclic and shared references (`Marshal.dump`/`load`, CRuby 4.8 wire format, byte-compatible for the supported subset); an object whose ivar is a *statically typed* Array/Hash (not a poly ivar) is not yet dumpable | a user object dumps/loads through a compile-time-generated per-class dispatcher. Supported ivar types: scalars (Integer/Float/String/true/false/Symbol/Bignum), `poly` (mixed) ivars, and nested user objects. A typed-container ivar would mismatch the loader's always-poly containers, so such a class raises `TypeError` on dump; value-type and Exception-subclass objects are also out of scope. Complex's components are float-only, so they round-trip as Floats |
| Mixin/inheritance lifecycle hooks (`included` / `inherited` / `extended`) | defined but not fired | emit a startup call with the literal class arg (the include/inherit graph is known at compile time) |
| External `Enumerator` — `.each` with no block is only an Enumerator on `Array` / `Range`, not on an arbitrary user method | mostly supported | `Array#each` / `Range#each` with no block return a working external Enumerator (`#next` / `#peek` / `#rewind` / `#size`, `loop` stops on `StopIteration`). `Enumerator.new { \|y\| ... }` is a fiber-backed generator (`y << v`, `y.yield(v)`, and the bare `y.yield v` without parentheses, plus `#next` / `#peek` / `#rewind` / `#take` / `#first`, infinite generators work). `Enumerator::Lazy` over an int range (incl. endless) or int array fuses map/select/reject/filter/take_while chains terminated by `first(n)` / `to_a` / `force`. Chained block→`.to_a` forms (`each_slice(n).to_a`, `filter_map`, `map{}.to_a`) also work. |
| `Enumerable#each_entry` on a user class whose `#each` yields MULTIPLE values | yields them spread, as `#each` does, rather than packed into an array | on every builtin enumerable (Array/Hash/Range/Enumerator/Dir) `#each` yields one value per element, so `each_entry` is compiled as `each` and matches CRuby exactly. The difference only shows for a user `#each` that does `yield a, b`, where CRuby's `each_entry` hands the block `[a, b]`. Packing needs the yield arity of the user's `#each`, which is a static property of its body |
| `StringIO#each_line` / `#each` / `#each_char` / `#each_byte` with NO block | `LocalJumpError`, where CRuby answers an `Enumerator` | the block forms are exact. Answering an Enumerator instead would make the method return either that or `self`, a union with no C slot. `io.readlines.each`, `io.read.each_char`, and `io.read.bytes` say the same thing and do have one |
| `Array#hash` (and arrays as hash keys) | unsupported | a builtin is additive, but array *keys* need the fundamental key-dispatch above |
| Sockets | TCP / UDP / UNIX-domain, as IO handles — see below | additive: each missing class and method is its own runtime binding |
| A promoted value stored into an int-typed Array (`--int-overflow=promote`) | truncated back to int64 by the store | the array's element type has to widen with the value; blanket-widening every int array costs promote mode more than it buys, so this wants a data-flow rule. Seeding the array with one value past 2^63 (or holding the state in a scalar) keeps the promotion today |

### Sockets

`require "socket"` is mandatory (see [require.md](require.md)); without it the
constants are undefined, as in CRuby.

**Supported classes.** `TCPServer`, `TCPSocket`, `UDPSocket`, `UNIXServer`,
`UNIXSocket`, and the `BasicSocket` / `IPSocket` / `Socket` classes they inherit
from. They sit in CRuby's chain (`TCPServer < TCPSocket < IPSocket <
BasicSocket < IO`), so `#is_a?`, `#class`, `.superclass` and `.ancestors`
answer as CRuby does. (IO's own mixins, `File::Constants` and `Enumerable`, are
still missing, so `ancestors` diverges past `IO`.)

**Constructors.** `TCPServer.new(port)` / `TCPServer.new(host, port)`,
`TCPSocket.new(host, port)`, `UDPSocket.new`, `UNIXServer.new(path)`,
`UNIXSocket.new(path)`.

**Methods.** `#accept`, `#addr`, `#peeraddr`, `#local_address`,
`#remote_address`, `#bind`, `#connect`, `#send`, `#recv`, `#recvfrom`,
`#listen`, `#shutdown`, `#setsockopt`, `#getsockopt`, and
the whole IO surface a handle carries (`#gets`, `#read`, `#readpartial`,
`#write`, `#puts`, `#print`, `#flush`, `#close`, `#closed?`, `#eof?`,
`#fileno`, `#each_line`, the `IO#wait_*` readiness family, `IO.select`).
`#addr` / `#peeraddr` return CRuby's numeric 4-element form. `#accept` parks
cooperatively on the green-thread scheduler, so a thread-per-connection server
does not stall its siblings, and socket writes bypass stdio (`#sync` is true,
as in CRuby).

`Socket::` constants (`SOL_SOCKET`, `SO_REUSEADDR`, `AF_INET`, `SOCK_DGRAM`,
`SHUT_RDWR`, `TCP_NODELAY`, ...) resolve at run time from the system headers,
so they carry the right platform-specific values.

**Class methods.** `Socket.gethostname`, `Socket.getaddrinfo`, `Socket.pair` /
`.socketpair`, `Socket.new(domain, type, protocol)`.

**Addrinfo.** `Addrinfo.tcp` / `.udp` / `.ip` / `.unix`, and `#ip_address`,
`#ip_port`, `#afamily`, `#pfamily`, `#socktype`, `#protocol`, `#unix_path`,
`#ipv4?`, `#ipv6?`, `#unix?`, `#ip?`, `#inspect`. `#local_address` and
`#remote_address` answer one.

**Socket::Option.** What `#getsockopt` returns: `#int`, `#bool`, `#level`,
`#optname`, `#family`, `#inspect`.

**Non-blocking.** `#accept_nonblock`, `#connect_nonblock`, `#recv_nonblock`,
`#read_nonblock`, `#write_nonblock`. Would-block raises the CRuby exception --
`IO::EAGAINWaitReadable` and friends, which answer to `IO::WaitReadable`, to
`Errno::EAGAIN` and to `SystemCallError` alike -- or, with `exception: false`,
returns the `:wait_readable` / `:wait_writable` marker. O_NONBLOCK is set for
the duration of one call and put back, so a blocking `#gets` on the same handle
still works.

**Divergences and gaps.**

- Only the **integer-valued** socket options are reachable, so
  `Socket::Option` carries an int rather than a byte string: `#data` and
  `#unpack` are missing, and `SO_LINGER` cannot be read back.
- `Addrinfo` covers the address itself, not the resolver surface:
  `Addrinfo.getaddrinfo`, `#getnameinfo`, `#to_sockaddr`, `#canonname`,
  `#bind`, `#connect`, `#listen` are missing. `Socket.getaddrinfo` returns
  CRuby's array-of-arrays form, which is the usual way in.
- `#recvfrom_nonblock`, `#sendmsg` and `#recvmsg` are missing; the rest of
  the non-blocking family (`#accept_nonblock`, `#connect_nonblock`,
  `#recv_nonblock`, `#read_nonblock`, `#write_nonblock`, and their
  `exception: false` forms) is supported.
- `SOCKSSocket` does not exist (CRuby only defines it when built with the
  SOCKS library, so a program cannot rely on it either).
- Missing instance methods: `#getsockname`, `#getpeername`,
  `#do_not_reverse_lookup`.
- Missing class methods: `.open`, `.gethostbyname`, `IPSocket.getaddress`.
- `TCPServer.new` takes no backlog argument (the listen backlog is fixed);
  `TCPSocket.new` has no four-argument local-address form.
- A class Spinel recognizes but has not implemented reports the missing
  **method** (`undefined method 'new' for class ...`), not a missing constant.

---

## By design (deliberate choices)

- **Integer overflow** — pick one mode at compile time: `raise` (default,
  `RangeError` on overflow), `wrap`, or `--int-overflow=promote` (auto-bignum).
  Not both in one binary, because the representation is chosen statically. See
  [int-overflow.md](int-overflow.md).
- **Float `round(ndigits)`** — the value is always correct; the *return class*
  follows CRuby (Integer for `round` with 0 digits, Float otherwise).
- **`Proc#ruby2_keywords`** — not supported (rejected at compile time). It is a
  migration shim for the Ruby 2.x-to-3.0 keyword-argument transition, flagging a
  proc so a trailing `Hash` forwarded through `*args` is treated as keywords.
  Spinel targets modern Ruby keyword semantics directly, so the shim has nothing
  to toggle; there is no 2.x behavior to opt back into.
  `Hash.ruby2_keywords_hash` / `Hash.ruby2_keywords_hash?` are the same shim
  from the hash side (marking / reading the flag) and are rejected the same
  way.
- **A bundled library carries only what it uses.** A `require` loads that
  library and nothing else. CRuby's own stdlib files often pull in others as an
  implementation detail -- `require "csv"` loads `stringio` there, so a program
  that requires csv can name `StringIO` without requiring it -- and Spinel's
  version of the same library has no reason to make the same internal choice.
  A program must `require` what it actually uses; the transitive requires of
  CRuby's implementation are not part of a library's interface. (Spinel's
  bundled libraries are listed in [require.md](require.md).)
- **`slice_before` / `slice_after` with a `Proc` pattern** — rejected at
  compile time (a stored-proc `===` call per element); use the block form.
  Range, Class, Regexp, and value patterns are supported.
- **`Comparable` with a non-conforming `#<=>`** — `<=>` is a protocol method
  returning `Integer` or `nil` (a `Float` is accepted, compared by sign). A
  `<=>` whose result type is statically something else (`String`, `Array`,
  `Hash`, `Symbol`, boolean) is a definite protocol violation: any Comparable
  operator (`<`, `<=`, `>`, `>=`, `==`, `between?`, `clamp`) on such a receiver
  is rejected at compile time rather than raising at run time as CRuby does.
  A `<=>` whose result is only `poly`/unknown statically keeps the CRuby
  runtime behavior (an incomparable pair raises `ArgumentError`).
- **`remove_method` / `undef_method` / `remove_class_variable`** — rejected at
  compile time. Methods are resolved statically and compiled to direct C calls,
  and class variables to static storage, so there is no runtime table for these
  to mutate; a construct would remove nothing. The call is reported rather than
  silently ignored. (A class that defines its own method by one of these names
  keeps it.)
- **Frozen literals** — explicit `.freeze` then mutation raises `FrozenError`,
  matching CRuby. String literals ARE frozen by default here
  (`frozen_string_literal: true` semantics, with no opt-out) — see
  "String literals are frozen by default" below for what that changes and
  where mutable strings come from.
- **Comparable is keyed on `<=>` presence** — the Comparable operator methods
  (`<`, `<=`, `>`, `>=`, `between?`, `clamp`) work on any class that defines
  `<=>`; CRuby additionally requires `include Comparable` (a `NoMethodError`
  otherwise). Spinel does not model the mixin, so it is permissive where CRuby
  raises. `sort`/`min`/`max`/`minmax` need only `<=>` in both. Related edges:
  the comparison-failed message names an operand's class where CRuby inspects
  special constants (`NilClass` vs `nil`); `sort_by` keeps incomparable keys
  in their original order where CRuby raises; `include?`/`index` on arrays of
  user objects compare by identity unless the class defines its own `==`.
  Sorts run a deterministic stable merge (identical on every platform, unlike
  libc `qsort`); it matches CRuby's comparison schedule for small arrays, but
  for larger ones (roughly 8 elements and up, where CRuby switches to its
  quicksort) the order of tied elements and which incomparable pair the
  ArgumentError names can differ from CRuby — deterministically so.
- **Thread data races are observable** — Spinel runs threads with real
  parallelism and no GVL, so two threads mutating the same `Array`/`Hash`/object
  without a `Mutex` is undefined at the Ruby level, exactly as in JRuby and
  TruffleRuby. CRuby's GVL makes individual operations appear atomic; Spinel does
  not, and adds no implicit per-object locking — correctness across threads is the
  program's responsibility via `Mutex`/`Queue`/`ConditionVariable`. Relatedly,
  thread *interleaving* (and so the ordering of `Thread.pass`, `Thread.list`
  membership, and the exact moment a `Thread#raise`/`#kill` is delivered) is
  nondeterministic, where the single-worker model was deterministic.
  `Thread#raise`/`#kill` targeting the **main** thread is a no-op (main runs on
  the scheduler's root fiber, which has no inject delivery points); CRuby
  delivers the exception to main.

### Intentional incompatibilities with CRuby

Spinel aims to be a subset of Ruby: programs it accepts should behave the same
as on CRuby. In a few cases CRuby's behavior depends on a feature Spinel does
not implement, and silently returning a wrong value would be worse than a
visible error. Those deliberate divergences are listed here.

#### A `super` chain whose callers pass differently-typed blocks

A method that yields is inlined at every call site, so it is specialized to
the block it is given there. A `super` reaching such a parent carries the
child's own caller block down, and the chain is inlined the same way -- a
three-link chain, several yields under the super, a class-method `super` and
`super(args)` all work.

What does not is a chain of three or more links where two callers pass blocks
whose *values* have different types (one returning an Integer, another a
String) and both routes meet at the same yielding ancestor: the middle link
carries a single type, so one of the two sites gets the wrong one and the
generated C is rejected. Two links are fine -- the parent is specialized per
call site there. Give the ancestor its own parameter, or make the block values
agree, if a chain that deep needs both.

#### Comparisons and predicates on a `nil` read out of an Integer container

A missing key on an Integer-valued Hash, or an out-of-range index on an
Integer array, answers `nil`. Spinel represents that `nil` as a sentinel
value inside the int slot, so the value is `nil` for `nil?`, `inspect`,
`class` and `||`, and every arithmetic operator on it raises the way CRuby's
`nil` does: `+`, `-`, `*`, `/`, `%` and `abs` raise `NoMethodError` (or the
coercion `TypeError` when the `nil` is the right operand).

Comparisons (`<`, `>`, `<=>`) and the numeric predicates (`zero?`,
`positive?`, ...) still answer from the sentinel rather than raising, because
checking them would put a branch on every integer comparison in the program,
including the loop conditions in the hottest code Spinel generates. A
comparison against a missing key therefore reports a result instead of
raising. Guard the read (`h[k]&.positive?`, `h.fetch(k, 0)`, or a `nil?`
test) when the key may be absent.

#### `Integer#**` with a negative exponent

CRuby evaluates a negative integer exponent to a `Rational`. Spinel matches
it whenever the sign is knowable: a literal negative exponent types the
result `Rational` statically (`2 ** -1 # => (1/2)`, `0 ** -1` raises
`ZeroDivisionError` as in CRuby), and the poly-dispatched path (a
poly-typed base or exponent, e.g. promote-mode parameters) picks `Integer`
or `Rational` from the sign at run time. The residual divergence is a
statically int-typed runtime exponent (`x ** y` with plain int locals):
typing it a sometimes-`Rational` would force the result poly and cascade
through every int-arithmetic consumer, so a negative value there still
raises `RangeError` rather than silently truncating. `Integer#pow(negative,
mod)` raises `RangeError` with CRuby's message.

#### `Integer#**` / `Rational#**` with a `Rational` exponent

CRuby returns an exact `Rational` when the exponent is integer-valued
(`3 ** 2r # => (9/1)`, `Rational(3,4) ** 2r # => (9/16)`) and a `Float`
otherwise. Spinel returns a `Float` in every case (`3 ** 2r # => 9.0`): the
exactness depends on the exponent's denominator at run time, so honoring it
would force the result to a boxed union and cascade through consumers. The
exponent's magnitude is still correct; only the class (Float vs Rational)
differs. `Integer ** Complex` and a `Complex` exponent generally evaluate to
the correct `Complex`, and `Integer#fdiv` / `#div` with a `Rational` argument
are exact.

#### A `Range` object needs `Integer`/`Float`/`String` bounds

A `Range` is an unboxed value with `sp_int` bounds, so a Range **object** over
user objects cannot be built (`rng = (Ver.new(1)..Ver.new(9))` is a compile
error naming the class). Comparing against such a range does not need one:
`Comparable#clamp` folds the bounds straight into the comparison, so the inline
and one-sided forms work.

```ruby
x.clamp(lo..hi)     # works -- no Range is built
x.clamp(lo, hi)     # works
x.clamp(..hi)       # works (one-sided)
x.clamp(lo..)       # works
rng = (lo..hi)      # compile error: a Range of Ver objects cannot be built
rng = (0..2**70)    # compile error: a Bignum bound does not fit sp_int
```

#### A call that cannot exist is refused at compile time, not raised at run time

Spinel resolves what it can resolve at compile time -- that is the point of the
AOT model -- and an undefined method is no exception. Where the receiver's type
is known and neither the class nor CRuby's own surface for it carries the name,
the call cannot succeed under any input, so it is reported when the program is
built rather than left to raise:

```ruby
[1].nope        # spinel: t.rb:1: undefined method 'nope' for an instance of Array (NoMethodError)
:s.nope         # ... for an instance of Symbol
Plain.new.nope  # ... for an instance of Plain
```

The diagnostic is CRuby's own wording, so the message reads the same as the
exception would; only the moment differs. The consequence is that a program
which *only reaches such a call behind a `rescue NoMethodError`* cannot be
built:

```ruby
r = (begin; [1].nope; rescue NoMethodError => e; e.receiver; end)   # compile error
```

A receiver whose type is not statically known -- a `nil`, a boxed value read
out of a container, a poly union -- keeps the runtime raise, since nothing
could be proved about it at compile time:

```ruby
x = nil
r = (begin; x.nope; rescue NoMethodError; "runtime"; end)   # => "runtime"
```

A name CRuby *does* define on that class, which Spinel has not implemented, is
a different thing entirely: that is a gap in Spinel, and it reports itself as
an `unsupported call` naming the node, not as a `NoMethodError`.

#### A `Float::INFINITY` bound reports the other bound as a `Float`

An integer `Range` is a value with `sp_int` bounds, which have no
representation for an infinity: the value can only record "unbounded". Where
that loses information CRuby keeps, Spinel resolves it as follows.

A range whose **begin** is infinite (`-Float::INFINITY..5`) takes the `Float`
representation, so `#begin` answers `-Infinity` as CRuby does. Its finite end
then reports as a `Float`:

```ruby
(-Float::INFINITY..5).begin    # => -Infinity   (as CRuby)
(-Float::INFINITY..5).cover?(0) # => true       (as CRuby)
(-Float::INFINITY..5).end      # => 5.0         (CRuby: 5)
(-Float::INFINITY..5).to_s     # => "-Infinity..5.0"  (CRuby: "-Infinity..5")
```

A range whose **end** is infinite (`1..Float::INFINITY`) keeps the integer
representation -- it is the canonical lazy source, and its integer enumeration
is what a fused `.lazy` pipeline walks. `#end`, `#size` and `#to_s` read the
bound off the literal, so they answer as CRuby does; a range of that shape held
in a variable and asked for `#end` answers `nil` (the value records only that
it is unbounded):

```ruby
(1..Float::INFINITY).end     # => Infinity      (as CRuby)
(1..Float::INFINITY).size    # => Infinity      (as CRuby)
(1..Float::INFINITY).to_s    # => "1..Infinity" (as CRuby)
r = (1..Float::INFINITY); r.end   # => nil      (CRuby: Infinity)
```

A finite mixed range (`1..5.0`) keeps the integer representation, where its
`#to_a`, `#sum` and `#cover?` are all right and its iteration is the integer
one CRuby performs; only `#end` reports `5` where CRuby reports `5.0`. A
one-sided float range (`(..5.0)`, `(1.0..)`) likewise keeps it, so `#to_s`
renders the bound as an integer (`"..5"`).

A `String`-bounded range (`("a".."e")`) is its own value type, so it keeps its
class, `#to_s` and `#inspect` whether it is used inline or held in a variable.
Its endpoint and membership methods (`begin`/`end`/`min`/`max`/`cover?`/`===`)
answer directly; every traversal (`each`, `map`, `to_a`, ...) materializes the
element array through `String#succ`, so an unbounded string range cannot be
iterated. `#size` is `nil`, as in CRuby, since a string range has no integer
element count.

#### `Time` sub-second precision is nanoseconds

A `Time` value stores its sub-second as an integer nanosecond count
(`int32 tv_nsec`), like `struct timespec`. A `Rational` sub-second argument
that does not fall on a nanosecond boundary is rounded to the nearest
nanosecond at construction:

```ruby
t = Time.utc(2020, 1, 1, 0, 0, 0, Rational(1, 3))  # 1/3 microsecond
t.subsec    # => (333/1000000000)   (CRuby: (1/3000000))
p t         # => 2020-01-01 00:00:00.000000333 UTC
            #    (CRuby: 2020-01-01 00:00:00 1/3000000 UTC)
```

Everything representable in whole nanoseconds -- every Integer `usec`, and
any `Rational` whose value lands on a nanosecond -- is exact, and `to_s`,
`usec`, `nsec` and `strftime("%N")` agree with CRuby. Only sub-nanosecond
exactness (and, as its consequence, the `Rational`-form `#inspect` display
CRuby uses for non-decimal sub-seconds) is lost.

#### Unboxed value types: identity IS the value

`Complex`, `Rational`, and `Range` values are unboxed C structs with no
internal pointers, so there is no per-object address to observe: `equal?`
(and `object_id` comparisons) are component equality. `x.equal?(x)` is `true`
as in CRuby, but two separately-constructed equal values also compare
`equal?` (CRuby: `false`). This extends the treatment CRuby itself applies to
its immediate values -- `1.equal?(1)`, `:s.equal?(:s)`, and (on 64-bit)
`1.0.equal?(1.0)` are all `true` there because the value is the identity.
The same applies to `freeze` on these values: they are value-frozen already
(`frozen?` is `true`), and `freeze` is an identity no-op.

**`/i` folds one codepoint to one.** Case-insensitive matching uses Unicode
simple case folding, so `/ä/i` matches "Ä" and `/k/i` matches "K" (U+212A).
A source whose fold is several codepoints has no single counterpart to fold
to and is matched literally: `"ß" =~ /ss/i` is `nil` where CRuby answers `0`.
Building the regexp engine with `-DRE_NO_UNICODE_CASE` (`make
RE_CASE_FLAGS=-DRE_NO_UNICODE_CASE`) leaves the ~3KB fold table out and folds
ASCII alone; a non-ASCII literal then matches literally under `/i` too.

**A pattern may have at most 31 capture groups.** The match registers `$~`
and `$1`..`$9` are built from hold that many, and so does the frame that saves
them across a call to a method that matches, so a wider pattern is refused
with `too many capture groups (maximum 31)` rather than compiled and then
truncated to what fits. CRuby has no ceiling here. 31 is where the registers
sit rather than where a program is likely to need to stop: of the 8,135 regexp
literals in CRuby 4.0.4's stdlib and bundled gems, the widest has 8 capture
groups. `(?:...)` costs nothing against it, and a named group counts as one.
The refusal is at compile time for a literal and at run time for a pattern
built there.

**A regexp literal is compiled when the program is.** A pattern the engine
cannot read is refused at compile time rather than raising `RegexpError` when
the built program reaches the literal, which is where CRuby reports it too
(as a `SyntaxError` from the parse). A pattern built at run time -- an
interpolated literal, `Regexp.new` on anything but a constant -- is still a
runtime question and still raises `RegexpError`.

**A search that backtracks is bounded by the state it holds.** A pattern with
a backreference, a lookaround or an atomic group runs on the backtracking
engine, whose choice points and undo records are capped together by
`MRB_REGEXP_STACK_LIMIT` (32768 entries). A greedy repetition leaves one
choice point per iteration, so what a search holds grows with the length of
the subject: `"a" * n + "b" + "a" * n =~ /\A(a+)b\1\z/` is answered for n up
to roughly 30000 and gives up above it, where CRuby keeps going. Giving up
answers `nil`, the same as no match. The step ceiling
(`MRB_REGEXP_STEP_LIMIT`) bounds the catastrophic shapes the same way, so
`("a" * 40 + "!") =~ /(a+)+$/` returns `nil` in milliseconds rather than
running for years.

**A POSIX bracket and a word boundary read Unicode above ASCII.**
`[[:alpha:]]` and its ten siblings hold what CRuby's brackets hold in every
script, and `\b` / `\B` sit beside a character of any script, both read off
the type table in `lib/regexp/re_ctype.h`. `\d`, `\w` and `\s` are ASCII in
Ruby's syntax and stay so, exactly as in CRuby, so `/\w/` and `/\b/` answer
different questions about the same character on purpose. Building with
`-DRE_NO_UNICODE_CTYPE` (`make RE_CASE_FLAGS=-DRE_NO_UNICODE_CTYPE`) leaves
the ~14KB table out and a bracket then holds its ASCII set alone, which the
boundary reads too. One case above ASCII still differs from CRuby: a bracket
under `/i` reaches a character through the 1:1 foldings only, so
`"ß" =~ /[[:upper:]]/i` is nil here and 0 in CRuby, for the same reason
`"ß" =~ /ss/i` does not match (see the fold note above).

**A regexp construct the engine does not carry is refused, not read as its
letters.** `\K` (drop what was matched before it), `\R` (any linebreak), `\X`
(a grapheme cluster) and `\p{...}` / `\P{...}` (a character property) each mean
something in CRuby that this engine does not do. Left as unknown escapes each
was simply its own letter, so `/\R/` matched an `R` rather than a newline and
`/\p{Alpha}/` answered a request for a letter with the text of the request.
They raise `RegexpError` at compile time instead. Inside a character class
CRuby reads `\K`, `\R` and `\X` as the letter too, and so does the class
parser here, so `[\R]` still matches an `R`. `\p{...}` / `\P{...}` differ:
CRuby reads a property escape inside a class as a property test there too
(`[\p{Alpha}]` matches letters), so the class parser refuses it the same way
the bare form is refused, rather than reading it as the letters `p{Alpha}`.
`\G` and `\g<name>` ARE carried and behave as CRuby does.

The same applies inside a character class, where a `[` never stands for
itself: `[[a][b]]` (a nested class), `[[.a.]]` (a collating element) and
`[[=a=]]` (an equivalence class) each raise `RegexpError` rather than compile
a different pattern than the one written -- taken as plain members, `[[a][b]]`
was `[` or `a`, then `b`, then `]`. `[[:alpha:]]` is read, and `[\[]` holds
the bracket itself as it does in CRuby.

**An `--rbs` seed is enforced where a value crosses into it.** A parameter
seeded `Hash[Symbol, untyped]` handed a hash whose keys the caller widened to
any type converts at the call, and a key the declared type cannot hold raises
`TypeError` there. CRuby ignores the signature, so a program whose keys really
are Symbols agrees with it and one whose keys are not diverges: the seed is a
claim about the program, and this is where the claim is checked.

**Regexp literals share one compiled object.** Each pattern is compiled once
at startup and every textually-equal literal names that one object, so
`/ab/.equal?(/ab/)` is `true` (CRuby allocates per literal: `false`). Same
shared-immutable-storage treatment as above; `==`/`eql?`/matching are
unaffected.

**String literals are frozen by default (`frozen_string_literal: true`
semantics).** Spinel's baseline is the direction Ruby itself is headed
(plain CRuby already warns "literal string will be frozen in the future"):
a literal is frozen (`"lit".frozen?` is `true`), mutating one raises
FrozenError, and mutable strings come from `+"lit"`, `String.new`,
interpolation, or `dup` -- exactly as under CRuby's
`--enable=frozen-string-literal`. There is no opt-out: a
`# frozen_string_literal: false` magic comment warns at compile time and
is ignored, and `--disable=frozen-string-literal` is rejected. (The
whole-program shared-mutable-string machinery relies on the frozen-literal
guarantee; a chilled mode would be a second, subtly different mutation
semantics.)

A note on identity: what `frozen_string_literal` specifies is frozenness,
not object identity, so the identity of frozen literals is
implementation-dependent. CRuby happens to intern equal-content literals
(`"abc".equal?("abc")` is `true` there); Spinel compiles each literal
OCCURRENCE to its own static object, so the same comparison answers
`false`, while re-evaluating one occurrence (a literal in a loop) yields
the same object where plain CRuby would allocate per evaluation. Programs
should not depend on either arrangement -- `equal?`/`object_id` on frozen
literals is exactly the implementation-defined corner. Value semantics
(`==`, hashing, matching) are unaffected.

**Aliased in-place mutation is observed.** A mutable string (from
`String.new`, `+"lit"`, interpolation, or `dup`) that is both aliased and mutated in
place shares one mutable buffer, matching CRuby's mutable String objects:
the mutation is visible through every reference and `equal?` across the
alias set is `true`. This covers the full in-place mutator surface --
`<<`, `concat`, `prepend`, `replace`, `insert`, `clear`, `slice!`, index
assignment (`s[i] = x`), `setbyte`, `bytesplice`, `append_as_bytes`, and
the transforming bang methods (`upcase!`, `gsub!`, `strip!`, `reverse!`,
...) -- across every storage shape: local aliases, array elements and hash
values (stored or read back, including mutation THROUGH a container read
like `arr[0].upcase!`), instance variables (with attr and hand-written
readers), method parameters (a callee's mutation stays visible through the
caller's aliases), returned values (including a string the callee also
retained), closure captures, and iteration variables. Frozen strings keep
raising FrozenError through every path; a hash string KEY is
snapshot-frozen on store, exactly CRuby's dup-and-freeze. Strings never
mutated in place, or mutated but never aliased, keep the plain value
representation (no cost).

#### `Range#step`, `Range#%` and `Numeric#step` return a materialized Array, not an ArithmeticSequence

CRuby's blockless `(1..10).step(2)`, `(1..10) % 2` and `1.step(5)` all return
an `Enumerator::ArithmeticSequence`: a lazy object with its own `inspect`
(`((1..10).%(2))`) and its own readers. Spinel has no ArithmeticSequence class
and materializes the stepped values at the call, as an Array. The values are
CRuby's, and so is everything computed from them: `to_a`, `each`, `map`,
`select`, `first`, `first(n)`, `size`, `sum`, `include?`, `each_slice`,
`reverse_each` and `==` all agree.

What differs is the object, not the values. `.class` answers `Array`, `p` on
the unforced sequence prints the array rather than `((1..10).%(2))`, and the
readers only an ArithmeticSequence has -- `begin`, `end`, `step`,
`exclude_end?`, `with_index` -- are not Array methods, so they raise
`NoMethodError` naming Array. A sequence that has to be described rather than
enumerated should be asked of the source range, which is unchanged.

Materializing also bounds what can be stepped at all. CRuby's sequence is lazy,
so `(1..).step(2).first(3)` and `(1..3_000_000_000).step(2).size` cost it
nothing; spinel would have to build every element, and refuses past 2**30 of
them with `RangeError: range too large to materialize`. An endless range hits
the same limit, its end being the largest representable integer.

Materializing at the call also decides *when* a bad stride is rejected. CRuby
defers the check to the point the sequence is enumerated, so `(1..10).step("x")`
returns an Enumerator, `.size` answers `nil`, and the `TypeError` arrives only
on `to_a` / `each` / `first`. Spinel raises the same `TypeError`, with CRuby's
message, at the call itself. A `String` stride on a String range differs
further: since 3.4 CRuby steps a non-numeric range by repeated `+`, so
`("a".."e").step("x")` diverges, while spinel takes every Nth element -- a
stride a String cannot name -- and raises.

#### Embedded NUL bytes: byte-exact core, C-string transforms

Strings store embedded NUL bytes, and the byte-exact core matches CRuby:
literals (`"a\0b"`), `length` / `bytesize` / `bytes`, `==` (`"a\0b" == "a"`
is false), Hash keys, slicing (`s[i]`, `s[a, n]`, ranges, `byteslice`),
`dup` / `clone`, concatenation, `0.chr`, `File.write` / `File.read`
round-trips, StringIO, pack/unpack, and Marshal.

The transform and search methods walk the C string and stop at the first
NUL: case ops (`upcase`, ...), `strip` family, `index` / `include?` /
`start_with?`, `sub` / `gsub` / `tr` / `delete` / `squeeze`, `split`,
`reverse`, `succ`, and interpolation / `%` formatting (`"x#{s}y"` drops
the NUL and its tail). `inspect` renders `\x00` where CRuby prints
`\u0000`. Treat embedded-NUL strings as byte containers, not text to
transform; full binary-safe transforms are a possible future project.

#### Nested modules named after a builtin class

`module Encoding` at the top level is CRuby's `TypeError` (`Encoding is not a
module`) and Spinel reports the same error at compile time. A *nested*
`module Foo::Encoding` (or `class Foo; module Encoding; end; end`) is legal
CRuby — it names a fresh constant — but Spinel's generated C type for a class
or module is its bare tail name, which collides with the runtime's own
`sp_Encoding` type. Spinel refuses these at compile time with
`unsupported module name '<Name>': collides with the builtin class of that
name` instead of failing with a raw C error. Renaming the nested module
avoids it. Builtin *modules* (`Comparable`, `Kernel`, `Math`, …) reopen
normally at any nesting level.

#### String-named `Struct` (the `Struct::Name` form)

The legacy form `Struct.new("Foo", :a, :b)` registers the new class as the
constant `Struct::Foo`. Spinel does not support this: a class is a compile-time
entity here, and the whole point of the string name — a class installed under
the `Struct::` namespace and reached through `Struct::Foo` — has no analogue in
the ahead-of-time model. Spinel refuses both the string-named definition and any
`Struct::Name` reference at compile time with `Struct.new with a string name …
is not supported; use \`Name = Struct.new(...)\``. Use the modern constant-
assignment form, which is equivalent and idiomatic:

```ruby
Foo = Struct.new(:a, :b)   # not Struct.new("Foo", :a, :b)
```

#### `Rational` precision and `Complex` components

`Rational` is stored as a pair of fixed `sp_int` numerator/denominator. The
arithmetic is exact while the reduced terms fit in `sp_int`; an operation whose
result would overflow raises `RangeError` rather than promoting to a Bigint as
CRuby does:

```ruby
Rational(10**18, 1) * Rational(10**18, 1)   # RangeError (CRuby: a Bigint Rational)
```

`Complex` stores its components as `sp_float` plus a per-component class tag,
so `#real` / `#imaginary` / `#abs2` and display report `Integer` components like
CRuby for integer-valued inputs. What the representation cannot express is a
`Rational` component: operations that would produce one compute in floats
instead. This applies to exact division and to mixed `Complex`/`Rational`
arithmetic and construction, which coerce the `Rational` via `#to_f` (the
operations work; only the component class -- and therefore the printed form --
differs from CRuby):

```ruby
Complex(1, 2).real                      # => 1     (matches CRuby)
Complex(1, 2) / Complex(3, -1)          # => (0.1+0.7i)       (CRuby: ((1/10)+(7/10)*i))
Complex(1, 2) + Rational(1, 2)          # => (1.5+2i)         (CRuby: ((3/2)+2i))
Rational(1, 2) + Complex(1, 2)          # => (1.5+2i)         (CRuby: ((3/2)+2i))
Complex(Rational(1, 2), Rational(1, 3)) # => (0.5+0.3333333333333333i)
                                        #                     (CRuby: ((1/2)+(1/3)*i))
Rational(3, 4).i                        # => (0+0.75i)        (CRuby: (0+(3/4)*i))
```

`Rational` and `Complex` values box into heterogeneous (poly) arrays and hashes
normally.

#### Negative Float `**` fractional exponent

CRuby promotes `(-2.0) ** 0.5` to a `Complex`. Spinel's Float stays a C
double, so that case raises `Math::DomainError` loudly (the class
`Math.sqrt(-1)` uses) rather than returning C's silent `NaN` or widening
every float power to a boxed union. Compute via `Complex(x) ** y` where the
complex result is really wanted.

#### `defined?(@ivar)` is answered at compile time

CRuby answers `defined?(@ivar)` from the object's runtime state: `nil` until
the instance variable is first assigned, `"instance-variable"` after — which
is what makes it usable as a memoization guard for falsy values
(`return @x if defined?(@x)`).

Spinel's instance variables are C struct fields. Every ivar the program
mentions exists in the object's layout from allocation, pre-filled with its
type's nil representation; there is no per-object "has been assigned" record
to consult. `defined?(@ivar)` therefore folds at compile time: it is truthy
iff the program contains an assignment to that ivar anywhere, regardless of
whether *this* object has been assigned yet at run time. The falsy-value
memoization pattern silently reads the unassigned slot on the first call:

```ruby
def foo
  return @foo if defined?(@foo)   # compile-time truthy: an @foo= exists below
  @foo = compute                  # never reached — foo returns nil forever
end
```

Tracking runtime assignment would need a shadow presence bit per ivar written
on every assignment — cost on every object and every ivar write to serve a
rare pattern. Use a nil check (`@foo = compute if @foo.nil?`, i.e. `||=`) when
`compute` never yields nil/false, or an explicit sentinel/flag ivar when it
can:

```ruby
def foo
  return @foo if @foo_set
  @foo_set = true
  @foo = compute
end
```

#### `Hash#compare_by_identity`

`compare_by_identity` is rejected at compile time (never silently ignored).
Spinel's hashes are typed storage variants keyed by VALUE -- string keys hash
and compare by content, and string literals are shared through a frozen pool.
Identity-keyed comparison cannot be honored on that representation: two
equal-content String keys may be the *same* object in Spinel where CRuby sees
two distinct ones, so even a dedicated identity mode would diverge from CRuby
on the exact programs that need it. Restructure identity-keyed lookups to use
an explicit unique key (an Integer id, a Symbol) instead.

#### `String#equal?` and literal identity

`equal?` on strings is pointer identity. Each literal OCCURRENCE compiles
to its own frozen static object (see the identity note under the
frozen-string-literal section): `"x".equal?("x")` is `false`, and
re-evaluating one occurrence (a literal in a loop) yields the same object.
Both facets are implementation-defined under `frozen_string_literal`
semantics and programs should not depend on them. Everything else about
identity is truthful: `s.freeze.equal?(s)` is `true` (freeze marks in
place), aliasing compares equal, `-lit` dedups interned content to one
object, and distinct-valued strings compare `false`.

#### `defined?`

`defined?` is resolved **statically at compile time** from the operand's
syntactic form and whole-program symbol presence, not from the runtime state of
the actual receiver or binding. It returns a fixed label string (or `nil`),
which matches CRuby for the common forms but differs in several cases. A full
runtime-accurate `defined?` would require carrying per-object/per-binding
definedness into the generated code; that cost is deliberately not paid.

Forms that match CRuby: a local variable (`"local-variable"`), a set/unset
instance variable, a set/unset global variable, a user or built-in constant
name (`"constant"`), a no-receiver call to a **user-defined** method
(`"method"`), `self`, `nil`/`true`/`false`, and the int/float/string/symbol/array
literals that report `"expression"`.

Where Spinel returns `nil` but CRuby returns a label:

| Operand | CRuby | Spinel |
| --- | --- | --- |
| `Foo::Bar` (constant path) | `"constant"` | `nil` |
| `puts` (built-in / Kernel method) | `"method"` | `nil` |
| `obj.meth` (call with a receiver) | `"method"` | `nil` |
| `1 + 1` (operator = method call) | `"method"` | `nil` |
| `{a: 1}`, `1..3` (hash/range and other general expressions) | `"expression"` | `nil` |
| `x = 1` (assignment) | `"assignment"` | `nil` |
| `yield`, `super` | `"yield"` / `"super"` | `nil` |
| Multi-encoding strings | Strings are UTF-8 or ASCII-8BIT, and one rule says which: a string is ASCII-8BIT when the program ASKED FOR BYTES (`pack`, `String#b`, `Marshal.dump`, `Random#bytes`, `binread`, `unpack`'s byte directives, `force_encoding` naming it). Everything else is UTF-8, including what CRuby calls US-ASCII (see below). A string carries one bit, not an encoding object; `#length`, `#[]`, `#inspect`, `#encoding` and the comparisons read it. A compiled binary's string paths (indexing, regexp, hashing) assume the two share a byte representation, and that assumption is load-bearing for their performance | write UTF-8; transcode at the boundary before the data enters the program |

Two forms report a label where CRuby would report `nil`, because the check is
syntactic rather than runtime:

- An instance variable reports `"instance-variable"` when **any** code in the
  program assigns that ivar name -- not whether it is set on the specific
  receiver at that point.
- A class variable read always reports `"class variable"`, with no
  definedness check, so `defined?(@@undefined)` is `"class variable"` in Spinel
  versus `nil` in CRuby.

#### `String#grapheme_clusters`

Correct Unicode extended-grapheme segmentation (`"á".grapheme_clusters # => ["á"]`)
requires shipping and maintaining the Unicode grapheme-break property tables,
which Spinel deliberately does not carry. `String#grapheme_clusters` and
`String#each_grapheme_cluster` are therefore not supported. For codepoint- or
byte-level iteration, use the supported `String#chars`, `#each_char`,
`#codepoints`, or `#bytes`.

#### `String#unicode_normalize`

Unicode normalization (`"é".unicode_normalize(:nfc) # => "é"`) requires
shipping and maintaining the Unicode decomposition/composition tables, which
Spinel deliberately does not carry -- the same limit as
`String#grapheme_clusters` above. `String#unicode_normalize`,
`#unicode_normalize!`, and `#unicode_normalized?` are therefore not supported,
and a call to them is rejected at compile time.

#### `Time` sub-nanosecond precision

Spinel's `Time` stores an `int64` second count and an `int32` nanosecond
fraction (nanosecond resolution). CRuby keeps the exact rational a `Float` or
`Rational` argument produces, so it carries bits below one nanosecond.
`#nsec` / `#usec` agree with CRuby (both truncate to the nanosecond), but two
things differ: `Time.at(f).to_f` does not always round-trip a `Float` (CRuby
rounds the exact rational to the nearest double; Spinel reconstructs from the
nanosecond value, so `Time.at(12345.678).to_f` is `12345.677999999`), and
`#subsec` returns a nanosecond-resolution `Rational` (`Time.at(2.2).subsec` is
`(1/5)`) rather than the exact binary fraction of the source `Float`.

#### Aliasing the regexp match globals

CRuby's `English` library aliases the punctuation match globals to readable
names (`alias $MATCH $&`, etc.). In Spinel the match globals (`$&`, `` $` ``,
`$'`, `$+`, `$~`) are not ordinary global-variable storage: a direct read lowers
to a special regexp runtime accessor. Supporting `alias $name $&` would require a
separate special-global alias mechanism plus broader `MatchData` compatibility,
outside the intended AOT subset. Aliasing one of these globals is rejected at
compile time rather than falling through to an undefined generated symbol:

```
$ spinel uses_english.rb
Error: global aliasing of regexp special globals is not supported (alias $MATCH $&)
```

Direct reads of the match globals work as usual; only aliasing them is
unsupported, so `require "English"` does not compile.

#### Flip-flop operator

CRuby supports the flip-flop operator (a `Range` used as a condition, toggled
between its two endpoints): `puts i if (i == 3)..(i == 5)`. This is a rarely used
feature with surprising hidden per-site state, and Spinel does not support it; a
program using it fails to compile rather than running with wrong behavior. Use an
explicit boolean state variable instead.

#### No `US-ASCII`

CRuby has three encodings where spinel has two. A string CRuby generated from
nothing -- `1.to_s`, `nil.to_s`, `:sym.to_s`, `1.chr`, `[1, 2].inspect`,
`Time#to_s` -- is US-ASCII; one derived from source text inherits the source's
encoding. Spinel calls both UTF-8.

The reason this costs nothing is that US-ASCII carries exactly one fact, "these
bytes are 7-bit", and nothing else. It is not needed for compatibility:
`rb_enc_compatible` keys on the CONTENT being 7-bit, not on the encoding's
identity, so an ASCII-only UTF-8 string concatenates with a Shift_JIS one
exactly as a US-ASCII string does. Across the operations that can tell two
same-byte strings apart -- `==`, `eql?`, `<=>`, `hash`, Hash keys, `length`,
`[]`, `chars`, `bytes`, `upcase`, `include?`, `index`, `sub`, `split`, regexp
matching, `to_sym`, `ascii_only?`, `valid_encoding?`, `encode`, `unpack`,
`force_encoding`, concatenation in both directions -- US-ASCII and ASCII-only
UTF-8 agree on every one. What differs:

```ruby
1.to_s.encoding      # CRuby: US-ASCII      Spinel: UTF-8
(1.to_s + "x").encoding
                     # CRuby: US-ASCII      Spinel: UTF-8
1.chr.inspect        # CRuby: "\x01"        Spinel: "\u0001"
```

The encoding's NAME, and `inspect`'s escape form for a non-printable byte.
CRuby needs the name because encodings are first-class objects and every string
must report one; a program cannot name an encoding in spinel, so there is
nothing for the third name to distinguish. The 7-bit fact itself is not lost --
spinel keeps it as a bit in the string header, where it makes indexing O(1)
rather than naming anything.

#### No `Encoding::CompatibilityError`

CRuby raises `Encoding::CompatibilityError` when a two-string operation
(`include?`, `index`, `+`, `sub`, `start_with?`, ...) is handed operands whose
encodings are incompatible and whose bytes are not all ASCII:

```ruby
"café".include?("é".b)   # CRuby: Encoding::CompatibilityError
                         # Spinel: true
```

The error guards against a byte match that is not a character match, which is
a real hazard when the two operands are, say, Shift_JIS and UTF-8: the same
bytes mean different characters. Spinel has two encodings, UTF-8 and
ASCII-8BIT, and they share one byte representation -- ASCII-8BIT is bytes with
no character interpretation at all, so there is no second interpretation for
the first one to disagree with. The failure the error exists to prevent cannot
happen here, so Spinel answers the byte question instead of refusing it.

Where CRuby produces a *value* rather than an error, Spinel matches it.
`String#==`, `#eql?`, `#<=>` and `#hash` follow CRuby's `rb_str_comparable`:
equal bytes are equal strings only when the encodings are comparable -- the
same encoding, or both operands ASCII only. That matters beyond the comparison
itself, because it decides whether a Hash keeps a binary blob and a text
string as one key or two.

The visible consequence of drawing the line there is an asymmetry:

```ruby
"café".include?("café".b)   # true  -- a byte search
"café" == "café".b          # false -- CRuby's answer, and the Hash-key rule
```

CRuby has the same pair; it just answers the first with an exception rather
than with `true`.

---

## Now supported (older write-ups are stale here)

These were limits in an earlier (Ruby self-hosted) version of the compiler and
now work on current master:

| Feature | Status |
|---|---|
| Mutable strings and aliased in-place mutation (`s = +"x"; s << "y"`; literals are frozen by default — see the String section) | works |
| Hash missing key → `nil` (string- and int-keyed, including `Hash.new(default)`) | works |
| `define_method(:name) { ... }` with a literal name | works |
| Block-param arity (un-yielded params are `nil`, not a sentinel) | works |
| Closures flowing through containers (`{op: ->(a,b){a+b}}[:op].call(2,3)`) | works |
| `String#oct` (`0x`/`0b`/`0o` prefixes) and `Array#first` on empty → `nil` | works |
| `send(:literal)` / `__send__("literal")` / `public_send(:literal)` on **implicit self** | works (resolved on the AST, so a `send(:` inside a string literal is left untouched) |
| Hash variant inference (a wrong initial guess widens to poly transparently) | correct (a perf cost, not a correctness limit) |

There is no Ruby self-host "bootstrap fixpoint" constraint: the C compiler is
the master implementation.

---

## Why this still works

Most real programs use the dynamic features above sparingly, in setup code, or
not at all. Spinel targets the large static core of Ruby — classes, methods,
blocks, the collection protocols, exceptions, mixins — and compiles it to fast
native code. When a program does need a feature in the *fundamental* table, that
program is not a fit for AOT; for everything else, the limits are either by
design or on the relaxable list.
