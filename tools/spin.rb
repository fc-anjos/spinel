# spin — the Spinel project tool (M0: new/init/build/run/test/clean,
# path dependencies only, no network, no lockfile). Usage: docs/spin.md;
# design record: docs/internals/spin.md.

require_relative "spin/toml"

$spin_hasher = ""   # memoized content-hasher command (native_hasher)

SPIN_USAGE = <<USAGE
usage: spin <command> [args]
  new <name> [--lib]   scaffold an application (or a library with --lib)
  init                 write a spin.toml into the current directory
  add <name> [--version C | --git URL [--ref R] | --path DIR]
                       [--features A,B]  add + lock
  search [term]        find packages in the index (name, latest, repo)
  remove <name>        drop a dependency + relock
  lock | fetch | vendor  resolve deps / warm the cache / copy into vendor/
  build [target..]     build bin/ executables into build/bin/
                       (--debug / -g: debuggable -O0 build for lldb/gdb;
                        also SPIN_DEBUG=1; applies to build/run/test)
  run [target] [-- a]  build, then run one executable
  test [file..]        build and run test/*.rb against expectations
  trust <name>         always allow <name>'s declared native build steps
                       (one run: --allow-native-build / SPIN_ALLOW_NATIVE_BUILD=1)
  clean                remove build/
  flags                print the compiler flags this project implies, for a
                       build driven from outside spin (Makefile, script)
  list [--json]        resolved dependency set (name, version, source)
  tree [--json]        dependency tree from this package
  publish [--direct]   validate + test, then submit this release to the index
  install [name..]     build and copy bin/ executables to ~/.local/bin
                       (--prefix DIR, --uninstall)
USAGE

def spin_die(msg)
  $stderr.puts "spin: #{msg}"
  exit 1
end

# --- project discovery -------------------------------------------------------

def find_root(dir)
  d = dir
  while true
    return d if File.exist?(File.join(d, "spin.toml"))
    up = File.expand_path("..", d)
    return "" if up == d
    d = up
  end
end

# `command -v` for one name, without a shell: the PATH entry that would run.
# mkdir -p for one path, without a shell.
def mkdir_p_path(path)
  acc = path.start_with?("/") ? "/" : ""
  path.split("/").each do |seg|
    next if seg == ""
    acc = acc == "/" ? "/" + seg : (acc == "" ? seg : File.join(acc, seg))
    Dir.mkdir(acc) unless Dir.exist?(acc)
  end
  nil
end

def which(name)
  ENV["PATH"].to_s.split(":").each do |dir|
    next if dir == ""
    cand = File.join(dir, name)
    return cand if File.file?(cand) && File.executable?(cand)
  end
  ""
end

def spinel_bin
  # spin ships beside the compiler: <dir-of-$0>/spinel. It has to be an
  # executable FILE: a checkout of the compiler repo cloned next to the project
  # is a DIRECTORY of that name, and File.exist? said yes to it -- spin then
  # tried to run the directory ("Permission denied", or "is a directory" on
  # macOS) instead of falling through to the installed binary (#3407).
  me = File.expand_path($0)
  cand = File.join(File.expand_path("..", me), "spinel")
  return cand if File.file?(cand) && File.executable?(cand)
  "spinel"  # PATH fallback
end

# the compiler build revision ("spinel <sha>"), "" when unknown -- keys the
# R8 probe records; a git SHA is the toolchain version until semver exists
def spinel_rev
  f = sh_read(spinel_bin + " --version").split(" ")
  r = f.length >= 2 ? f[1] : ""
  r == "unknown" ? "" : r
end


# --- index (M3) ----------------------------------------------------------------
# The index is a git repo, not a server (R5): packages/<name>.toml maps a name
# its repo plus [[release]] version/ref entries. Selection is MVS: the LOWEST
# release satisfying every constraint gathered for the package, so a build without
# a lock is still deterministic; spin.lock then pins the outcome.

def spin_index_url
  u = ENV["SPIN_INDEX"].to_s
  u == "" ? "https://github.com/matz/spin-index" : u
end

def index_dir(offline)
  base = ENV["XDG_CACHE_HOME"].to_s
  base = File.join(ENV["HOME"].to_s, ".cache") if base == ""
  Dir.mkdir(base) unless Dir.exist?(base)   # a fresh XDG_CACHE_HOME
  d = File.join(base, "spin")
  Dir.mkdir(d) unless Dir.exist?(d)
  d = File.join(d, "index")
  return d if Dir.exist?(File.join(d, ".git"))
  spin_die("--offline: no cached index (spin fetch first)") if offline
  ok = system("git clone -q --depth 1 #{spin_index_url} #{d}")
  spin_die("cannot clone the index: " + spin_index_url) unless ok
  d
end

def index_refresh(offline)
  return if offline
  d = index_dir(false)
  system("git -C #{d} pull -q --ff-only 2>/dev/null")  # offline-tolerant: stale is usable
end

# "1.2.3" <=> "1.10" as numeric components; missing parts are 0
def vcmp(a, b)
  pa = a.split(".")
  pb = b.split(".")
  n = pa.length > pb.length ? pa.length : pb.length
  i = 0
  while i < n
    x = i < pa.length ? pa[i].to_i : 0
    y = i < pb.length ? pb[i].to_i : 0
    return -1 if x < y
    return 1 if x > y
    i += 1
  end
  0
end

# constraint: "*"/"" (any), "~> X.Y(.Z)" (pessimistic), ">= X", or exact "X.Y.Z"
def version_satisfies(v, cons)
  c = cons.strip
  return true if c == "" || c == "*"
  if c.start_with?("~>")
    floor = c[2..-1].to_s.strip
    return false if vcmp(v, floor) < 0
    parts = floor.split(".")
    return true if parts.length < 2
    ceil = ""
    i = 0
    while i < parts.length - 1
      ceil += "." unless ceil == ""
      ceil += (i == parts.length - 2 ? (parts[i].to_i + 1).to_s : parts[i])
      i += 1
    end
    return vcmp(v, ceil) < 0
  end
  return vcmp(v, c[2..-1].to_s.strip) >= 0 if c.start_with?(">=")
  vcmp(v, c) == 0
end

# MVS pick from packages/<name>.toml: lowest release satisfying the constraint.
# Returns "version\nrepo\nref" ("" when nothing matches).
def index_select(dep, cons, offline)
  gf = File.join(index_dir(offline), "packages", dep + ".toml")
  spin_die("not in the index: " + dep + " (spin add " + dep + " --git URL is the escape hatch)") unless File.exist?(gf)
  gdoc = TomlDoc.parse(File.read(gf))
  repo = gdoc.get("", "repo")
  spin_die("index entry for " + dep + " lacks a repo") if repo == ""
  n = gdoc.array_len("release")
  sel_v = ""
  sel_r = ""
  i = 0
  while i < n
    t = "release." + i.to_s
    v = gdoc.get(t, "version")
    r = gdoc.get(t, "ref")
    if v != "" && r != "" && version_satisfies(v, cons)
      if sel_v == "" || vcmp(v, sel_v) < 0
        sel_v = v
        sel_r = r
      end
    end
    i += 1
  end
  spin_die("no release of " + dep + " satisfies " + (cons == "" ? "*" : cons)) if sel_v == ""
  # R8: surface recorded probe results for the selected release. A fail
  # recorded against THIS compiler build is a strong warning; the newest
  # fail against any build still warns. Never an error -- the build tells.
  myrev = spinel_rev
  exact = ""
  latest = ""
  latest_detail = ""
  pi = 0
  while pi < gdoc.array_len("probe")
    t2 = "probe." + pi.to_s
    if gdoc.get(t2, "version") == sel_v
      r2 = gdoc.get(t2, "result")
      latest = r2
      latest_detail = gdoc.get(t2, "detail")
      exact = r2 if myrev != "" && gdoc.get(t2, "spinel") == myrev
    end
    pi += 1
  end
  if exact == "fail"
    $stderr.puts "spin: warning: " + dep + " " + sel_v + " is recorded FAILING with this compiler build" + (latest_detail == "" ? "" : " (" + latest_detail + ")")
  elsif exact == "" && latest == "fail"
    $stderr.puts "spin: warning: " + dep + " " + sel_v + "'s newest probe is a fail" + (latest_detail == "" ? "" : " (" + latest_detail + ")")
  end
  sel_v + "\n" + repo + "\n" + sel_r
end

# --- carried native C (M2) ----------------------------------------------------
# A package may carry .c/.h sources (R6). spin compiles each .c once into the
# shared cache, keyed by (package, version, toolchain), and hands the objects to
# spinel via --link; the compiler itself never touches package C. Objects are
# project-independent (carried C is not specialized by inference).

def native_cc
  c = ENV["CC"].to_s
  c == "" ? "cc" : c
end

# A filename-safe name for the compiler, for the object cache's directory. CC
# is a command line, not a path: "sccache gcc" and "/usr/lib/ccache/cc" both
# have to reduce to something a directory can be called.
def cc_cache_key
  k = ""
  native_cc.each_char do |ch|
    ok = (ch >= "a" && ch <= "z") || (ch >= "A" && ch <= "Z") ||
         (ch >= "0" && ch <= "9") || ch == "-" || ch == "_" || ch == "."
    k += ok ? ch : "_"
  end
  k
end

# The public runtime headers ship beside the compiler: dev tree ../lib,
# installed tree ./lib. $0 does not resolve symlinks, so when spin runs as
# the /usr/local/bin/spin symlink the sibling is the /usr/local/bin/spinel
# symlink -- probe the install layout (<prefix>/lib/spinel/lib) from there.
# Where the runtime headers are: package C includes "spinel/runtime.h", and
# the compiler ships them beside itself. Resolved from the compiler's own
# path, which is why a PATH-resolved compiler has to be located rather than
# given up on -- `spin install` puts spin in ~/.local/bin with no spinel
# beside it, and returning "" there dropped -I <spinel>/lib from every
# package-C compile. The package then failed on a missing runtime.h while the
# same package built fine from a tree where the two sat together (#4115).
#
# SPINEL_HDR_DIR overrides the search outright, for a layout this cannot guess.
def spinel_hdr_dir
  env = ENV["SPINEL_HDR_DIR"].to_s
  return env if env != "" && File.exist?(File.join(env, "spinel_rt.h"))
  bin = spinel_bin
  if bin == "spinel"
    found = which("spinel")
    # PATH returns the symlink as-is; the lib probe below then walks
    # relative to a symlinked path and misses the install tree.
    found = File.realpath(found) if found != "" && File.symlink?(found)
    bin = found
  end
  return "" if bin == ""
  d = File.expand_path("..", bin)
  a = File.join(d, "lib")
  return a if File.exist?(File.join(a, "spinel_rt.h"))
  up = File.expand_path("..", d)
  b = File.join(up, "lib")
  return b if File.exist?(File.join(b, "spinel_rt.h"))
  c = File.join(up, "lib/spinel/lib")
  return c if File.exist?(File.join(c, "spinel_rt.h"))
  ""
end

# Newline-packed absolute paths kept out of the per-file cc sweep and out of
# the staleness scan that decides whether the cached objects are current.
#
# Two sources. A [[build]] workdir is compiled by the package's own build
# system, so it is not carried C: the sweep must not touch it (no include
# path, and it would hard-fail before [[build]] ever runs -- the toy tinynn
# bounce, #1845). And `[package] exclude` is the author saying a path is not
# part of this build at all. Nothing else can say it: `.rb` enters the build by
# require-reachability, `.c` enters by presence, so an application whose
# repository also holds a C program of its own -- a `main()` beside the Ruby --
# had no way to keep it out (#4105).
#
# The globs are expanded here rather than matched at each candidate, so
# path_excluded? stays an exact compare, and naming a directory prunes its
# whole subtree (collect_c consults it before it recurses).
def native_excludes(dir)
  mf = File.join(dir, "spin.toml")
  return "" unless File.exist?(mf)
  toml = TomlDoc.parse(File.read(mf))
  out = ""
  i = 0
  while i < toml.array_len("build")
    wd = toml.get("build." + i.to_s, "workdir")
    if wd != ""
      out += "\n" unless out == ""
      out += File.expand_path(wd, dir)
    end
    i += 1
  end
  toml.get_array("package", "exclude").split("\n").each do |g|
    next if g == ""
    Dir.glob(File.join(dir, g)).each do |hit|
      out += "\n" unless out == ""
      out += hit
    end
  end
  out
end

def path_excluded?(p2, excl)
  excl.split("\n").each { |x| return true if x != "" && p2 == x }
  false
end

# newline-packed .c paths (an [] accumulator arg would go poly-array and
# box the paths -- same tuple trap as dep_srcs)
def collect_c(dir, excl)
  out = ""
  Dir.children(dir).each do |e|
    next if e.start_with?(".")
    next if e == "build" || e == "vendor" || e == "test"
    p2 = File.join(dir, e)
    next if path_excluded?(p2, excl)
    if File.directory?(p2)
      out += collect_c(p2, excl)
    elsif e.end_with?(".c")
      out += p2 + "\n"
    end
  end
  out
end

def newest_native_input(dir, newest, excl)
  Dir.children(dir).each do |e|
    next if e.start_with?(".")
    next if e == "build" || e == "vendor" || e == "test"
    p2 = File.join(dir, e)
    next if path_excluded?(p2, excl)
    if File.directory?(p2)
      newest = newest_native_input(p2, newest, excl)
    elsif e.end_with?(".c") || e.end_with?(".h")
      m = File.mtime(p2).to_i
      newest = m if m > newest
    end
  end
  newest
end

# Where a package's compiled objects live. Shared across projects and keyed by
# (package, version, toolchain), so the same package is not rebuilt for every
# consumer.
#
# SPIN_NATIVE_CACHE relocates it. Two reasons to want that, both from #4115:
# a build that must not write outside its own tree, and a debugging session
# where the answer differs depending on whether an object is already there --
# pointing it at a scratch directory makes every run start from the same
# state. SPIN_NO_NATIVE_CACHE=1 goes further and rebuilds every time.
def native_cache_dir(key)
  override = ENV["SPIN_NATIVE_CACHE"].to_s
  if override != ""
    d = File.expand_path(override)
    mkdir_p_path(d)
    d = File.join(d, key)
    Dir.mkdir(d) unless Dir.exist?(d)
    return d
  end
  base = ENV["XDG_CACHE_HOME"].to_s
  base = File.join(ENV["HOME"].to_s, ".cache") if base == ""
  Dir.mkdir(base) unless Dir.exist?(base)   # a fresh XDG_CACHE_HOME
  d = File.join(base, "spin")
  Dir.mkdir(d) unless Dir.exist?(d)
  d = File.join(d, "native")
  Dir.mkdir(d) unless Dir.exist?(d)
  d = File.join(d, key)
  Dir.mkdir(d) unless Dir.exist?(d)
  d
end

# Compile one package's carried C into the cache; returns the object list.
def native_objs_for(name, dir, version)
  excl = native_excludes(dir)
  cs = collect_c(dir, excl)
  return [] if cs == ""
  # The cache key names a DIRECTORY, so the compiler part of it has to be
  # spellable as one. A CC of "sccache gcc" or "ccache cc" -- what CI and most
  # developer setups use -- put a space in the path, and the unquoted -o
  # argument below then split into two words at it.
  odir = native_cache_dir(name + "-" + version + "-" + cc_cache_key)
  hdr = spinel_hdr_dir
  hnew = newest_native_input(dir, 0, excl)
  objs = []
  cs.split("\n").each do |c|
    rel = c[dir.length + 1..-1].to_s
    o = File.join(odir, rel.gsub("/", "_")[0..-3] + ".o")
    if !File.exist?(o) || File.mtime(o).to_i < hnew || ENV["SPIN_NO_NATIVE_CACHE"].to_s != ""
      cmd = native_cc + " -O2 -c '#{c}' -I '#{dir}'"
      cmd += " -I '#{hdr}'" if hdr != ""
      cmd += " -o '#{o}'"
      spin_die("native compile failed: " + rel + " (" + name + ")") unless system(cmd)
      # stderr, not stdout: `spin flags` prints a flag string on stdout and a
      # cold cache compiles here first, so progress on stdout would be spliced
      # into the flags the caller passes to the compiler (#4105).
      $stderr.puts "cc #{name}/#{rel}"
    end
    objs.push(o)
  end
  objs
end

# --- declared native build steps ([[build]]) ----------------------------------
#
# A package may vendor an external project with its own build system (cmake,
# make) that carried-C's per-file CC cannot express. `[[build]]` entries in
# its spin.toml declare the step: a command run in a scratch copy of
# `workdir`, optional `patches` applied to that copy first, and `artifacts`
# the run must produce. Entries run at DEPENDENT-APPLICATION BUILD TIME only
# (never at fetch, preserving R2), are consented explicitly, and cache into
# the shared content-keyed native cache. Artifacts reach the link line via
# `[native] libs` entries, where `${build.out}` expands to the package's
# artifact directory -- linking stays on the existing shape-2 surface.

def spin_config_dir
  base = ENV["XDG_CONFIG_HOME"].to_s
  base = File.join(ENV["HOME"].to_s, ".config") if base == ""
  Dir.mkdir(base) unless Dir.exist?(base)
  d = File.join(base, "spin")
  Dir.mkdir(d) unless Dir.exist?(d)
  d
end

def native_trust_file
  File.join(spin_config_dir, "trust")
end

def native_trusted?(name)
  f = native_trust_file
  return false unless File.exist?(f)
  File.read(f).split("\n").include?(name)
end

def native_trust!(name)
  f = native_trust_file
  have = File.exist?(f) ? File.read(f) : ""
  return if have.split("\n").include?(name)
  File.write(f, have + name + "\n")
end

# A package running an external build system is a trust decision the consumer
# makes: the flag/env for this run, a recorded `spin trust <name>`, or -- on
# an interactive terminal -- a per-run prompt. There is deliberately no
# silent default (cargo's build.rs runs unprompted; we don't copy that), and
# a non-interactive build (CI, pipes) never waits on a prompt.
def ensure_native_allowed(name, command)
  return if ENV["SPIN_ALLOW_NATIVE_BUILD"].to_s != ""
  return if native_trusted?(name)
  if $stdin.tty?
    $stderr.puts "spin: package '#{name}' declares a native build step:"
    $stderr.puts "  #{command}"
    $stderr.print "Allow? [y/N/always] "
    ans = $stdin.gets
    ans = ans.nil? ? "" : ans.strip.downcase
    if ans == "always"
      native_trust!(name)
      return
    end
    return if ans == "y" || ans == "yes"
    spin_die("native build not allowed (#{name})")
  end
  $stderr.puts "spin: package '#{name}' declares a native build step:"
  $stderr.puts "  #{command}"
  $stderr.puts "Allow it with --allow-native-build (this run),"
  $stderr.puts "SPIN_ALLOW_NATIVE_BUILD=1 (CI), or `spin trust #{name}` (always)."
  exit 1
end

# Content hasher: prefer sha256, fall back for platforms without coreutils
# naming (macOS ships shasum). cksum is the POSIX floor -- weak, but this is
# a cache key, not a security boundary.
def native_hasher
  return $spin_hasher if $spin_hasher != ""
  ["sha256sum", "shasum -a 256", "cksum"].each do |h|
    probe = sh_read("command -v " + h.split(" ")[0])
    if probe != ""
      $spin_hasher = h
      return h
    end
  end
  $spin_hasher = "cksum"
  "cksum"
end

def native_hash_pipe(shell_producer)
  out = sh_read("(" + shell_producer + ") | " + native_hasher)
  out.split(" ")[0].to_s
end

# Content hash of a directory tree: the sorted file list plus every file's
# bytes. Rename-only changes and content changes both move the key.
def native_tree_hash(dir, excludes = "")
  # dot-entries (.git and friends) are excluded, consistent with collect_c /
  # newest_mtime: a workdir that is a real git clone must not churn the key
  # on fetch metadata, and stale VCS state is not a build input. An entry's
  # own `exclude` globs (build output dirs riding inside a dev workdir) are
  # pruned the same way. -mindepth 1: find's first entry is `.` itself, which
  # matches -name '.*' and would prune the WHOLE tree (every workdir hashed
  # as empty input, so source edits never moved the key).
  pr = "-name '.*'"
  excludes.split("\n").each do |g|
    next if g == ""
    spin_die("[[build]] exclude may not contain quotes: #{g}") if g.include?("'")
    pr += " -o -path './#{g}'"
  end
  lst = "cd #{dir} && find . -mindepth 1 \\( #{pr} \\) -prune -o -type f -print | LC_ALL=C sort"
  native_hash_pipe(lst + " ; (" + lst + ") | while read f; do cat \"$f\"; done")
end

def native_out_dir(name, version, key)
  native_cache_dir(name + "-" + version + "-build-" + key)
end

# Run one package's [[build]] entries (consented, content-cached) and return
# its `[native] libs` for the link line, newline-packed, with ${build.out}
# expanded to the artifact directory. "" when the package declares no build.
# Entries gated on `features` run only when every named feature is in the
# package's own `[features] default` set (consumer-side enablement is a
# later slice); a lib entry whose artifact was feature-skipped is dropped.
def native_build_libs_for(name, dir, version, consumer_feats)
  mf = File.join(dir, "spin.toml")
  return "" unless File.exist?(mf)
  toml = TomlDoc.parse(File.read(mf))
  n = toml.array_len("build")
  return "" if n == 0
  # enabled = the package's own defaults plus what the consuming application
  # switched on for this dependency (cargo-style: features live in the
  # manifest, the source of truth; the lock stays resolution-only)
  enabled = toml.get_array("features", "default")
  consumer_feats.split("\n").each do |cf|
    next if cf == "" || enabled.split("\n").include?(cf)
    enabled += "\n" unless enabled == ""
    enabled += cf
  end

  # one artifact dir per package, keyed over every entry's inputs
  keysrc = "cc=" + File.basename(native_cc) + "\nfeatures=" + enabled
  i = 0
  while i < n
    t = "build." + i.to_s
    wd = toml.get(t, "workdir")
    spin_die("[[build]] entry #{i} of #{name}: workdir is required") if wd == ""
    wdir = File.join(dir, wd)
    spin_die("[[build]] entry #{i} of #{name}: no such workdir #{wd}") unless File.directory?(wdir)
    keysrc += "\nworkdir=" + wd + "@" + native_tree_hash(wdir, toml.get_array(t, "exclude"))
    keysrc += "\nexclude=" + toml.get_array(t, "exclude")
    keysrc += "\ncommand=" + toml.get(t, "command")
    keysrc += "\nartifacts=" + toml.get_array(t, "artifacts")
    keysrc += "\nfeatures=" + toml.get_array(t, "features")
    toml.get_array(t, "patches").split("\n").each do |pg|
      Dir.glob(File.join(dir, pg)).sort.each do |pf|
        keysrc += "\npatch=" + File.basename(pf) + "@" + native_hash_pipe("cat #{pf}")
      end
    end
    i += 1
  end
  keyf = "/tmp/spin_key_#{Process.pid}"
  File.write(keyf, keysrc)
  key = native_hash_pipe("cat " + keyf)[0..15]
  File.delete(keyf)
  out = native_out_dir(name, version, key)

  i = 0
  while i < n
    t = "build." + i.to_s
    gates = toml.get_array(t, "features")
    skip = false
    gates.split("\n").each { |g| skip = true unless enabled.split("\n").include?(g) }
    if skip
      i += 1
      next
    end
    arts = toml.get_array(t, "artifacts")
    spin_die("[[build]] entry #{i} of #{name}: artifacts is required") if arts == ""
    # Artifacts keep their entry-relative path under ${build.out} (a bare
    # `libggml.a` still lands at the top). Authors namespace colliding names
    # (a CPU and a CUDA libggml.a) by building/declaring them under distinct
    # subpaths -- no per-entry output dir needed (#1845 bounce 3).
    missing = false
    arts.split("\n").each { |a| missing = true unless File.exist?(File.join(out, a)) }
    unless missing
      i += 1
      next   # cached: every artifact already present for this key
    end
    cmdline = toml.get(t, "command")
    spin_die("[[build]] entry #{i} of #{name}: command is required") if cmdline == ""
    # ${build.out} expands in the command too, so a later entry can compile
    # against an earlier entry's artifacts (headers, archives) without
    # snapshotting them into its own tree (#1845 bounce 2). Entries run in
    # declaration order.
    cmdline = cmdline.gsub("${build.out}", out)
    ensure_native_allowed(name, cmdline)
    # scratch copy: the vendored tree stays a read-only input. Dot-entries
    # (.git and friends) are dropped from the copy, consistent with the tree
    # hash -- VCS state is not a build input and a real clone's .git is heavy.
    scratch = out + ".scratch"
    system("rm -rf #{scratch}")
    # -H dereferences the WORKDIR OPERAND itself: a symlinked workdir must
    # copy the pointed-to tree, or the "scratch" is a symlink and every
    # later step (patch, the build command) writes through into the live
    # checkout the link points at. Symlinks inside the tree stay links.
    spin_die("native build: cannot copy #{name}'s workdir") unless system("cp -RH #{File.join(dir, toml.get(t, 'workdir'))} #{scratch}")
    spin_die("native build: #{name}'s workdir copied as a symlink, not a tree") unless File.directory?(scratch) && !File.symlink?(scratch)
    system("find #{scratch} -mindepth 1 -name '.*' -prune -exec rm -rf {} + 2>/dev/null")
    toml.get_array(t, "exclude").split("\n").each do |g|
      next if g == ""
      system("cd #{scratch} && rm -rf #{g}") if File.directory?(scratch)
    end
    toml.get_array(t, "patches").split("\n").each do |pg|
      Dir.glob(File.join(dir, pg)).sort.each do |pf|
        unless system("patch -s -p1 -d #{scratch} < #{pf}")
          system("rm -rf #{scratch}")
          spin_die("native build: patch failed: #{File.basename(pf)} (#{name})")
        end
      end
    end
    puts "native #{name}: #{cmdline}"
    unless system("cd #{scratch} && ( #{cmdline} )")
      system("rm -rf #{scratch}")
      spin_die("native build failed (#{name})")
    end
    arts.split("\n").each do |a|
      built = File.join(scratch, a)
      unless File.exist?(built)
        system("rm -rf #{scratch}")
        spin_die("native build of #{name} did not produce declared artifact: #{a}")
      end
      dest = File.join(out, a)
      ddir = File.dirname(dest)
      system("mkdir -p #{ddir}") unless Dir.exist?(ddir)
      system("cp #{built} #{dest}")
    end
    system("rm -rf #{scratch}")
    i += 1
  end

  # Artifact names declared by feature-DISABLED entries: a [native] libs
  # reference to one of these silently drops out (the feature is off); a
  # missing path declared by NO entry at all is an authoring error and dies
  # loud (a stale path otherwise surfaces as hundreds of undefined symbols
  # at link time, #2010).
  gated_off = ""
  i = 0
  while i < n
    t = "build." + i.to_s
    gates = toml.get_array(t, "features")
    skip = false
    gates.split("\n").each { |g| skip = true unless enabled.split("\n").include?(g) }
    if skip
      toml.get_array(t, "artifacts").split("\n").each do |a|
        gated_off += "\n" unless gated_off == ""
        gated_off += a
      end
    end
    i += 1
  end
  libs = ""
  toml.get_array("native", "libs").split("\n").each do |l|
    path = l.gsub("${build.out}", out)
    unless File.exist?(path)
      rel = l.gsub("${build.out}/", "").gsub("${build.out}", "")
      next if gated_off.split("\n").include?(rel)   # feature-skipped: drops out
      spin_die("[native] libs of #{name}: #{l} was not produced by any [[build]] entry")
    end
    libs += "\n" unless libs == ""
    libs += path
  end
  libs
end

# --- shared cache & git sources (M1) -----------------------------------------

def cache_packages_dir
  base = ENV["XDG_CACHE_HOME"].to_s
  base = File.join(ENV["HOME"].to_s, ".cache") if base == ""
  Dir.mkdir(base) unless Dir.exist?(base)   # a fresh XDG_CACHE_HOME
  d = File.join(base, "spin")
  Dir.mkdir(d) unless Dir.exist?(d)
  g = File.join(d, "packages")
  Dir.mkdir(g) unless Dir.exist?(g)
  g
end

def sh_read(cmd)
  tmp = "/tmp/spin_out_#{Process.pid}"
  system(cmd + " > " + tmp + " 2>/dev/null")
  out = File.exist?(tmp) ? File.read(tmp).strip : ""
  File.delete(tmp) if File.exist?(tmp)
  out
end

def gem_version_of(dir)
  mf = File.join(dir, "spin.toml")
  return "0.0.0" unless File.exist?(mf)
  v = TomlDoc.parse(File.read(mf)).get("package", "version")
  v == "" ? "0.0.0" : v
end

# Fetch (or reuse) a git source; returns "dir\nversion\nsha".
def git_fetch(name, url, ref, want_sha)
  pkgs = cache_packages_dir
  # a previously locked SHA that is already cached wins (offline path)
  if want_sha != ""
    hits = Dir.glob(pkgs + "/" + name + "-*")
    hits.each do |h|
      stamp = File.join(h, ".spin-sha")
      next unless File.exist?(stamp)
      if File.read(stamp).strip == want_sha
        return h + "\n" + gem_version_of(h) + "\n" + want_sha
      end
    end
  end
  tmp = File.join(pkgs, ".fetch-" + name)
  system("rm -rf " + tmp)
  cloned = false
  if want_sha != ""
    # materialize the exact pinned/selected commit: fetch it directly
    # (works on file:// and GitHub); a server refusing SHA fetches falls
    # back to the full clone + checkout below.
    cloned = system("mkdir -p " + tmp) &&
             system("git -C " + tmp + " init -q 2>/dev/null") &&
             system("git -C " + tmp + " fetch -q --depth 1 " + url + " " + want_sha + " 2>/dev/null") &&
             system("git -C " + tmp + " checkout -q FETCH_HEAD 2>/dev/null")
    system("rm -rf " + tmp) unless cloned
  end
  unless cloned
    refarg = ref == "" ? "" : " --branch " + ref
    depth = want_sha == "" ? " --depth 1" : ""
    ok = system("git clone -q" + depth + refarg + " " + url + " " + tmp)
    spin_die("fetch failed: git clone " + url) unless ok
    if want_sha != ""
      okc = system("git -C " + tmp + " checkout -q " + want_sha)
      spin_die("fetch failed: " + name + " has no commit " + want_sha) unless okc
    end
  end
  sha = sh_read("git -C " + tmp + " rev-parse HEAD")
  spin_die("fetch failed: no HEAD sha for " + url) if sha == ""
  spin_die("fetch verify failed: wanted " + want_sha + ", got " + sha) if want_sha != "" && sha != want_sha
  ver = gem_version_of(tmp)
  final = File.join(pkgs, name + "-" + ver)
  system("rm -rf " + final)
  system("rm -rf " + File.join(tmp, ".git"))
  File.write(File.join(tmp, ".spin-sha"), sha + "\n")
  ok2 = system("mv " + tmp + " " + final)
  spin_die("fetch failed: cannot place " + final) unless ok2
  final + "\n" + ver + "\n" + sha
end

# --- spin.lock ----------------------------------------------------------------

def write_lock(root, lines)
  body = "# generated by spin lock -- diff me, don't edit me\n"
  lines.each { |l| body += l }
  File.write(File.join(root, "spin.lock"), body)
end

# Resolve all [dependencies] of the manifest at `root` (recursively through
# fetched gems), preferring SHAs recorded in spin.lock. Returns newline-packed
# records "name\tdir\tversion\tgit\tsha_or_path" joined by \n.
def resolve_deps(root, offline)
  root0 = root
  lf = File.join(root, "spin.lock")
  lock = TomlDoc.parse("")
  lock = TomlDoc.parse(File.read(lf)) if File.exist?(lf)
  out = ""
  seen = { "" => "" }
  queue = [root]
  qdirs = { root => "" }
  while queue.length > 0
    cur = queue.shift.to_s
    mf = File.join(cur, "spin.toml")
    next unless File.exist?(mf)
    toml = TomlDoc.parse(File.read(mf))
    toml.table_keys("dependencies").each do |dep|
      next if seen.key?(dep)
      seen[dep] = "1"
      pth = toml.get_inline("dependencies", dep, "path")
      url = toml.get_inline("dependencies", dep, "git")
      ref = toml.get_inline("dependencies", dep, "ref")
      if pth != ""
        d2 = File.expand_path(pth, cur)
        spin_die("dependency " + dep + ": path not found: " + pth) unless File.directory?(d2)
        out += dep + "\t" + d2 + "\t" + gem_version_of(d2) + "\tpath\t" + pth + "\n"
        queue.push(d2)
      elsif url != ""
        want = lock.get("lock." + dep, "ref")
        rec = ""
        if offline
          hit = ""
          Dir.glob(cache_packages_dir + "/" + dep + "-*").each do |h|
            st = File.join(h, ".spin-sha")
            hit = h if want != "" && File.exist?(st) && File.read(st).strip == want
          end
          if hit == ""
            Dir.glob(File.join(root0, "vendor/packages") + "/" + dep + "-*").each { |h| hit = h }
          end
          spin_die("--offline: " + dep + " not in cache or vendor (spin fetch/vendor first)") if hit == ""
          rec = hit + "\n" + gem_version_of(hit) + "\n" + want.to_s
        else
          rec = git_fetch(dep, url, ref, want)
        end
        parts = rec.split("\n")
        # want may be "" (no spin.lock yet): split drops the trailing empty
        # field, so read the SHA defensively rather than trusting parts[2].
        sha = parts.length > 2 ? parts[2] : ""
        out += dep + "\t" + parts[0] + "\t" + parts[1] + "\tgit\t" + url + "\x01" + sha + "\n"
        queue.push(parts[0])
      else
        # a plain string value is an index constraint: foo = "~> 1.2" / "*";
        # with per-dep features it moves inline: foo = { version = "*", features = [..] }
        cons = toml.get("dependencies", dep)
        cons = toml.get_inline("dependencies", dep, "version") if cons == ""
        sel = index_select(dep, cons, offline).split("\n")
        sel_v = sel[0]
        repo = sel[1]
        sel_r = sel.length > 2 ? sel[2] : ""
        # verify-not-select: a lock ref that still satisfies the constraint
        # pins the build; one that no longer does is reselected (spin lock
        # rewrites the pin).
        want = lock.get("lock." + dep, "ref")
        lv = lock.get("lock." + dep, "version")
        if want != "" && version_satisfies(lv, cons)
          sel_r = want
          sel_v = lv
        elsif want != ""
          $stderr.puts "spin: spin.lock pins " + dep + " " + lv + " outside " + cons + "; reselecting " + sel_v
        end
        rec = ""
        if offline
          hit = ""
          Dir.glob(cache_packages_dir + "/" + dep + "-*").each do |h|
            st = File.join(h, ".spin-sha")
            hit = h if sel_r != "" && File.exist?(st) && File.read(st).strip == sel_r
          end
          if hit == ""
            Dir.glob(File.join(root0, "vendor/packages") + "/" + dep + "-*").each { |h| hit = h }
          end
          spin_die("--offline: " + dep + " not in cache or vendor (spin fetch/vendor first)") if hit == ""
          rec = hit + "\n" + gem_version_of(hit) + "\n" + sel_r
        else
          rec = git_fetch(dep, repo, "", sel_r)
        end
        parts = rec.split("\n")
        sha = parts.length > 2 ? parts[2] : sel_r
        out += dep + "\t" + parts[0] + "\t" + sel_v + "\tindex\t" + repo + "\x01" + sha + "\x01" + cons + "\n"
        queue.push(parts[0])
      end
    end
  end
  out
end

# --- manifest ----------------------------------------------------------------

class Project
  attr_reader :root, :name, :dep_paths

  def initialize(root)
    @root = root
    toml = TomlDoc.parse(File.read(File.join(root, "spin.toml")))
    nm = toml.get("package", "name")
    if nm == ""
      base = File.basename(root)
      base = base[7..-1] if base.start_with?("spinel-")
      nm = base
    end
    @name = nm
    # per-dependency feature enablement from THIS manifest's [dependencies]
    # inline specs (dep = { ..., features = ["cuda"] }); root-level only --
    # transitive feature unification is out of scope
    @dep_features = { "" => "" }
    toml.table_keys("dependencies").each do |dep|
      f = toml.inline_array("dependencies", dep, "features")
      @dep_features[dep] = f if f != ""
    end
    @dep_paths = []
    @dep_records = resolve_deps(root, ENV["SPIN_OFFLINE"].to_s.length > 0)
    # tab-packed name\tdir\tversion records (an array of tuples would go
    # poly and poison the string params downstream)
    @dep_srcs = @name + "\t" + root + "\t" + gem_version_of(root)
    @dep_records.split("\n").each do |rec|
      next if rec == ""
      f = rec.split("\t")
      # prefer a vendored copy when present
      vd = File.join(root, "vendor/packages", f[0] + "-" + f[2])
      d = File.directory?(vd) ? vd : f[1]
      @dep_paths.push(d)
      @dep_srcs += "\n" + f[0] + "\t" + d + "\t" + f[2]
    end
  end

  # carried native C across the root package and every resolved dep (M2)
  def native_objs
    objs = []
    @dep_srcs.split("\n").each do |s|
      f = s.split("\t")
      native_objs_for(f[0], f[1], f[2]).each { |o| objs.push(o) }
    end
    objs
  end

  # declared [[build]] steps across the root package and every resolved dep:
  # runs (or reuses) each package's native build and returns the expanded
  # `[native] libs` link inputs, newline-packed. Memoized -- the staleness
  # check and compile() both consult it, and a cached build is cheap but a
  # cold one is not.
  def native_build_libs
    return @native_build_libs if @native_build_libs != nil
    libs = ""
    @dep_srcs.split("\n").each do |s|
      f = s.split("\t")
      cf = @dep_features.key?(f[0]) ? @dep_features[f[0]] : ""
      got = native_build_libs_for(f[0], f[1], f[2], cf)
      next if got == ""
      libs += "\n" unless libs == ""
      libs += got
    end
    @native_build_libs = libs
    libs
  end

  def dep_records
    @dep_records
  end

  def bins
    out = []
    bd = File.join(@root, "bin")
    return out unless File.directory?(bd)
    Dir.glob(bd + "/*.rb").each do |path|
      out.push(File.basename(path)[0..-4])
    end
    out
  end

  def tests
    out = []
    td = File.join(@root, "test")
    return out unless File.directory?(td)
    Dir.glob(td + "/*.rb").each do |path|
      out.push(File.basename(path))
    end
    out
  end
end

# --- staleness (newest input mtime vs output mtime) --------------------------

def newest_mtime(dir, newest)
  Dir.children(dir).each do |e|
    next if e.start_with?(".")   # .git and friends
    p2 = File.join(dir, e)
    next if e == "build" || e == "vendor"
    if File.directory?(p2)
      newest = newest_mtime(p2, newest)
    elsif e.end_with?(".rb") || e.end_with?(".rbs") || e.end_with?(".c") || e.end_with?(".h") || e == "spin.toml"
      m = File.mtime(p2).to_i
      newest = m if m > newest
    end
  end
  newest
end

def inputs_mtime(prj)
  newest = newest_mtime(prj.root, 0)
  prj.dep_paths.each { |d| newest = newest_mtime(d, newest) }
  sb = spinel_bin
  newest = File.mtime(sb).to_i if File.exist?(sb) && File.mtime(sb).to_i > newest
  newest
end

# --- build -------------------------------------------------------------------

# The spinel command line that compiles `entry` to `out`. Split out from
# compile() so `spin test` can collect commands and run them in parallel.
# Every compiler flag this project implies, with no entry file and no -o: what
# `spin build` passes, and what `spin flags` prints. One producer for both, so
# a Makefile driving the compiler itself gets exactly the build spin would have
# made rather than an approximation of it (#4105).
#
# Every path here is absolute -- find_root walks up from Dir.pwd, path
# dependencies go through File.expand_path, and cache objects are named from
# the cache root -- which matters for `spin flags`, whose caller's working
# directory is its own tree, not this project.
def spin_flags(prj)
  # Inside a spin project the dependency universe is fully known (manifest +
  # lock), so an unresolvable require is a bug, not a maybe: flip the
  # compiler's require gate from warning to hard error. This also makes
  # stdlib features require-gated, i.e. CRuby-style `require "stringio"`
  # before use.
  f = "--require-gate"
  prj.dep_paths.each { |d| f += " -I #{d}" }
  f += " -I #{prj.root}"
  # Feed .rbs sidecars to the compiler's --rbs seed machinery when the project
  # carries any (issue #1788). `.rbs` participates by extension, so a package's
  # type sidecars pin its public surfaces (e.g. a Router#match that would
  # otherwise infer poly) under `spin build`/`test`. --rbs takes one dir and its
  # extractor scans recursively, so the project root covers every sidecar.
  if Dir.glob(File.join(prj.root, "**", "*.rbs")).any?
    f += " --rbs #{prj.root}"
  end
  # Reading these compiles any carried C and runs any declared native build
  # that is not already cached, which is what makes the --link paths real.
  prj.native_objs.each { |o| f += " --link #{o}" }
  prj.native_build_libs.split("\n").each { |l| f += " --link #{l}" if l != "" }
  f
end

def compile_cmd(prj, entry, out, extra)
  cmd = "#{spinel_bin} #{entry} #{spin_flags(prj)}"
  cmd += " #{extra}" if extra != ""
  # `spin build --debug` / `-g` (or SPIN_DEBUG=1) forwards the compiler's
  # debug build (#line + -g -O0) so the emitted binary is steppable in
  # lldb/gdb; without it there is no way to get a debuggable build through
  # spin (it always compiles release).
  cmd += " --debug" if ENV["SPIN_DEBUG"].to_s != ""
  cmd += " -o #{out}"
  cmd
end

def compile(prj, entry, out, extra)
  ok = system(compile_cmd(prj, entry, out, extra))
  unless ok
    # close the add-a-package loop: wrap the compiler's unsatisfiable-require error
    $stderr.puts "spin: build failed (hint: an unresolved require may need a dependency: spin add <name> --path <dir>)"
    exit 1
  end
end

# The compiler modes that produce no executable. `spin build` writes one to
# build/bin/<target> and compile_cmd always appends that `-o`, so these
# contradict it -- and `-c` does so silently: spinel honours both flags and
# writes C SOURCE over the executable's path, which keeps its executable bit
# while the build reports success (#4098).
EMIT_ONLY_FLAGS = ["-c", "-S", "--emit-rbs", "--emit-types",
                   "--emit-symbol-map", "--dump-ast"]

def cmd_build(prj, targets, extra)
  extra.split(" ").each do |a|
    if EMIT_ONLY_FLAGS.include?(a)
      spin_die("`#{a}` produces no executable, and `spin build` writes one to " \
               "build/bin/ -- run the compiler directly for that")
    end
  end
  bins = prj.bins
  spin_die("no bin/*.rb executables to build (a library is exercised via `spin test`)") if bins.empty?
  targets = bins if targets.empty?
  need = inputs_mtime(prj)
  # Declared native builds run first (vendor/ is excluded from the mtime
  # sweep, so a rebuilt artifact's own mtime is what re-triggers the link).
  prj.native_build_libs.split("\n").each do |l|
    next if l == ""
    m = File.mtime(l).to_i
    need = m if m > need
  end
  Dir.mkdir(File.join(prj.root, "build")) unless Dir.exist?(File.join(prj.root, "build"))
  bindir = File.join(prj.root, "build/bin")
  Dir.mkdir(bindir) unless Dir.exist?(bindir)
  targets.each do |t|
    spin_die("no such executable: bin/#{t}.rb") unless bins.include?(t)
    out = File.join(bindir, t)
    # The freshness check has to know which KIND of binary is sitting there.
    # `--debug` rides an env var rather than `extra`, so without this a debug
    # build right after a release one reports "up to date" and hands back the
    # release binary -- silently, which is the one outcome a debug flag must
    # never produce. A stamp beside the binary records the mode and a mismatch
    # forces a rebuild in either direction.
    mode = ENV["SPIN_DEBUG"].to_s != "" ? "debug" : "release"
    stamp = File.join(bindir, ".#{t}.mode")
    had = File.exist?(stamp) ? File.read(stamp).strip : "release"
    if File.exist?(out) && File.mtime(out).to_i > need && extra == "" && had == mode
      puts "build #{t} (up to date)"
      next
    end
    puts "build #{t}"
    compile(prj, File.join(prj.root, "bin/#{t}.rb"), out, extra)
    File.write(stamp, mode)
  end
end

def cmd_run(prj, args)
  target = ""
  run_args = []
  seen_dd = false
  args.each do |a|
    if a == "--"
      seen_dd = true
    elsif seen_dd
      run_args.push(a)
    elsif target == ""
      target = a
    end
  end
  bins = prj.bins
  if target == ""
    spin_die("no executables in bin/") if bins.empty?
    spin_die("multiple executables (#{bins.join(', ')}): spin run <name>") if bins.length > 1
    target = bins[0]
  end
  cmd_build(prj, [target], "")
  cmd = File.join(prj.root, "build/bin", target)
  run_args.each { |a| cmd += " #{a}" }
  ok = system(cmd)
  exit(ok ? 0 : 1)
end

# --- test --------------------------------------------------------------------

def cmd_test(prj, files, regen)
  tests = prj.tests
  spin_die("no test/*.rb files") if tests.empty?
  tests = files.map { |f| File.basename(f) } unless files.empty?
  tdir = File.join(prj.root, "build/test")
  Dir.mkdir(File.join(prj.root, "build")) unless Dir.exist?(File.join(prj.root, "build"))
  Dir.mkdir(tdir) unless Dir.exist?(tdir)
  inc = ""
  prj.dep_paths.each { |d| inc += " -I #{d}" }
  inc += " -I #{prj.root}"
  tests.each do |t|
    spin_die("no such test: test/#{t}") unless File.exist?(File.join(prj.root, "test", t))
  end
  # regen rewrites each snapshot from CRuby; kept serial (it only runs ruby).
  # Both streams, merged: that is what a run is compared against below, and
  # what the no-snapshot path already captures from CRuby. Taking stdout alone
  # here wrote a snapshot the very next `spin test` could not match, for any
  # program that writes to stderr (#3405).
  if regen
    tests.each do |t|
      src = File.join(prj.root, "test", t)
      system("ruby#{inc} #{src} > #{src}.expected 2>&1")
      puts "regen #{t}"
    end
    return
  end
  # Incremental-build freshness bound: a test binary newer than every project
  # input AND the compiler binary may be reused without recompiling (#3202).
  need = inputs_mtime(prj)
  cm = File.exist?(spinel_bin) ? File.mtime(spinel_bin).to_i : 0
  need = cm if cm > need
  bins = tests.map { |t| File.join(tdir, t[0..-4]) }
  # A binary newer than the freshness bound is still valid -- reuse it (marked
  # "(cached)") and skip the dominant recompile cost.
  cached = tests.map { |t| b = File.join(tdir, t[0..-4]); File.exist?(b) && File.mtime(b).to_i > need }
  # Phase 1: compile every stale test in PARALLEL. cc is the whole cost (the
  # spinel frontend is ~1ms) and the files are independent, so hand the compile
  # commands to `xargs -P` and let it saturate the cores -- ~ncores faster than
  # the old one-at-a-time build. Test binaries run once to check output, not for
  # speed, so compile at -O1 not the -O2 release default: -O1 still prunes the
  # unreferenced runtime-header statics (unlike -O0) while skipping the expensive
  # passes (~2x less cc per file). (#3202)
  cmds = []
  tests.each_index do |i|
    next if cached[i]
    # Drop the previous binary before compiling. A failed compile leaves it
    # where it was, and the run phase below reads File.exist? as "it built":
    # `spin test` printed the parse error, then ran the executable an OLDER
    # source had produced, compared ITS output against the snapshot, and
    # answered ok with exit 0 (#4085). What exists after this phase is now
    # what this phase produced.
    File.delete(bins[i]) if File.exist?(bins[i])
    cmds.push(compile_cmd(prj, File.join(prj.root, "test", tests[i]), bins[i], "-O 1"))
  end
  unless cmds.empty?
    # `nproc` is GNU coreutils; BSD/macOS answers via sysctl.
    nproc = `nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null`.to_i
    nproc = 1 if nproc < 1
    cf = File.join(tdir, ".compile-cmds")
    File.write(cf, cmds.join("\n") + "\n")
    # One compile command per input line. GNU xargs takes the delimiter
    # explicitly (-d); BSD xargs has no -d, but -I already reads one
    # line per command -- it just caps the substituted command at 255
    # bytes unless -S raises the limit (65522 is the documented max),
    # and GNU rejects -S. Pick the flag set by flavor.
    flavor = `xargs --version 2>/dev/null`.include?("GNU") ? "-d '\\n'" : "-S 65522"
    system("xargs -P #{nproc} #{flavor} -I CMD sh -c CMD < #{cf}")
    File.delete(cf) if File.exist?(cf)
  end
  # Phase 2: run each binary and compare, serial and in order (execution is
  # milliseconds; only the compile above was worth parallelizing).
  fails = 0
  tests.each_index do |i|
    t = tests[i]
    bin = bins[i]
    unless File.exist?(bin)
      puts "FAIL #{t} (build failed)"
      fails += 1
      next
    end
    exp = File.join(prj.root, "test", t) + ".expected"
    outf = bin + ".out"
    system("#{bin} > #{outf} 2>&1")
    actual = File.exist?(outf) ? File.read(outf) : ""
    if File.exist?(exp)
      expected = File.read(exp)
    else
      # no snapshot: diff directly against CRuby (the subset-parity check)
      cexp = bin + ".cruby"
      system("ruby#{inc} #{File.join(prj.root, "test", t)} > #{cexp} 2>&1")
      expected = File.exist?(cexp) ? File.read(cexp) : ""
    end
    if actual == expected
      puts "ok   #{t}" + (cached[i] ? " (cached)" : "")
    else
      puts "FAIL #{t}"
      puts "--- expected"
      print expected
      puts "--- actual"
      print actual
      fails += 1
    end
  end
  puts "#{tests.length - fails}/#{tests.length} passed"
  exit 1 if fails > 0
end


# --- add / lock / fetch / vendor (M1) ------------------------------------------

def lock_from_records(prj)
  lines = []
  prj.dep_records.split("\n").each do |rec|
    next if rec == ""
    f = rec.split("\t")
    lines.push("\n[lock." + f[0] + "]\nversion = \"" + f[2] + "\"\n")
    if f[3] == "git" || f[3] == "index"
      us = f[4].split("\x01")
      lines.push("git = \"" + us[0] + "\"\nref = \"" + us[1] + "\"\n")
    else
      lines.push("path = \"" + f[4] + "\"\n")
    end
  end
  write_lock(prj.root, lines)
  puts "locked " + prj.dep_paths.length.to_s + " package(s)"
end

def jq_str(s)
  out = ""
  s.split("").each do |ch|
    if ch == "\\" || ch == "\""
      out += "\\" + ch
    else
      out += ch
    end
  end
  "\"" + out + "\""
end

def cmd_list(prj, json)
  if json
    out = "["
    first = true
    prj.dep_records.split("\n").each do |rec|
      next if rec == ""
      f = rec.split("\t")
      src = f[3] == "path" ? f[4] : f[4].split("\x01")[0]
      out += "," unless first
      first = false
      out += "{\"name\":" + jq_str(f[0]) + ",\"version\":" + jq_str(f[2]) +
             ",\"kind\":" + jq_str(f[3]) + ",\"source\":" + jq_str(src) + "}"
    end
    puts out + "]"
  else
    prj.dep_records.split("\n").each do |rec|
      next if rec == ""
      f = rec.split("\t")
      src = f[3] == "path" ? f[4] : f[4].split("\x01")[0]
      puts f[0] + " " + f[2] + " (" + f[3] + " " + src + ")"
    end
  end
end

# name -> resolved dir, for walking each package's own manifest
def tree_children(dir)
  mf = File.join(dir, "spin.toml")
  return [] unless File.exist?(mf)
  TomlDoc.parse(File.read(mf)).table_keys("dependencies")
end

def tree_walk(prj, name, dir, version, indent, seen, json)
  out = ""
  if json
    out = "{\"name\":" + jq_str(name) + ",\"version\":" + jq_str(version) + ",\"deps\":["
  else
    puts indent + name + " " + version
  end
  first = true
  tree_children(dir).each do |dep|
    # resolved location/version from the flat record set
    ddir = ""
    dver = ""
    prj.dep_records.split("\n").each do |rec|
      f = rec.split("\t")
      if f[0] == dep
        ddir = f[1]
        dver = f[2]
      end
    end
    next if ddir == ""
    if seen.include?("|" + dep + "|")
      puts indent + "  " + dep + " " + dver + " (...)" unless json
      next
    end
    sub = tree_walk(prj, dep, ddir, dver, indent + "  ", seen + "|" + dep + "|", json)
    if json
      out += "," unless first
      first = false
      out += sub
    end
  end
  json ? out + "]}" : ""
end

def cmd_tree(prj, json)
  r = tree_walk(prj, prj.name, prj.root, gem_version_of(prj.root), "", "|" + prj.name + "|", json)
  puts r if json
end

def cmd_add(root, name, url, ref, pth, cons, feats)
  spin_die("usage: spin add <name> [--version C | --git URL [--ref R] | --path DIR] [--features A,B]") if name == ""
  mf = File.join(root, "spin.toml")
  text = File.read(mf)
  flist = ""
  feats.split(",").each do |ft|
    next if ft.strip == ""
    flist += ", " unless flist == ""
    flist += "\"" + ft.strip + "\""
  end
  fmember = flist == "" ? "" : ", features = [" + flist + "]"
  spec = ""
  if pth != ""
    spec = "{ path = \"" + pth + "\"" + fmember + " }"
  elsif url != ""
    spec = ref == "" ? "{ git = \"" + url + "\"" + fmember + " }"
                     : "{ git = \"" + url + "\", ref = \"" + ref + "\"" + fmember + " }"
  else
    # index form: bare name takes any release, --version narrows it; with
    # features the constraint moves inline as `version`
    index_refresh(false)
    c2 = cons == "" ? "*" : cons
    spec = flist == "" ? "\"" + c2 + "\""
                       : "{ version = \"" + c2 + "\"" + fmember + " }"
  end
  line = name + " = " + spec + "\n"
  if text.include?("\n[dependencies]\n")
    text = text.sub("\n[dependencies]\n", "\n[dependencies]\n" + line)
  elsif text.start_with?("[dependencies]\n")
    text = "[dependencies]\n" + line + text[15..-1]
  else
    text += "\n[dependencies]\n" + line
  end
  File.write(mf, text)
  prj = Project.new(root)
  lock_from_records(prj)
  puts "added " + name
end

def cmd_search(term)
  index_refresh(false)
  d = File.join(index_dir(false), "packages")
  found = 0
  Dir.glob(d + "/*.toml").sort.each do |gf|
    nm = File.basename(gf)[0..-6]
    next unless term == "" || nm.include?(term)
    gdoc = TomlDoc.parse(File.read(gf))
    best = ""
    i = 0
    while i < gdoc.array_len("release")
      v = gdoc.get("release." + i.to_s, "version")
      best = v if best == "" || vcmp(v, best) > 0
      i += 1
    end
    puts nm + " " + best + " " + gdoc.get("", "repo")
    found += 1
  end
  puts "no matches" if found == 0
end

def cmd_remove(root, name)
  mf = File.join(root, "spin.toml")
  out = ""
  File.read(mf).split("\n").each do |l|
    out += l + "\n" unless l.strip.start_with?(name + " ") || l.strip.start_with?(name + "=")
  end
  File.write(mf, out)
  prj = Project.new(root)
  lock_from_records(prj)
  puts "removed " + name
end

# --- publish (index PR automation) --------------------------------------------
# `spin publish` folds "get my release into the index" into one command:
# validate identity + a pushed, version-consistent commit, run the tests as a
# hard gate (R8), write packages/<name>.toml, then submit -- straight push with
# --direct (index write access), a gh-driven fork + PR when gh is available,
# or printed instructions otherwise. No tarballs, no accounts: the git
# identity is the identity, and nothing executes at fetch time.

def publish_repo_url(root, override)
  u = override
  u = sh_read("git -C " + root + " remote get-url origin") if u == ""
  spin_die("publish: no git remote (set one or pass --repo URL)") if u == ""
  # normalize the GitHub ssh form; consumers clone anonymously
  if u.start_with?("git@github.com:")
    u = "https://github.com/" + u[15..-1].to_s
  end
  u = u[0..-5] if u.end_with?(".git")
  spin_die("publish: " + u + " is not fetchable by others (file:// and local paths cannot be published)") if u.start_with?("file://") || u.start_with?("/") || u.start_with?(".")
  u
end

# `spin install`: build this package's executables and copy them onto PATH --
# the last step of "I wrote a CLI and now I use it". Local sources only;
# installing a tool from the index is a separate (deferred) verb, so the
# rubygems reading of `install <name>` never collides with bin/<name>.
def install_dir(prefix)
  return prefix if prefix != ""
  d = ENV["XDG_BIN_HOME"].to_s
  return d if d != ""
  File.join(ENV["HOME"].to_s, ".local/bin")
end

def cmd_install(prj, targets, prefix, uninstall)
  bins = prj.bins
  spin_die("no bin/*.rb executables (a library has nothing to install)") if bins.empty?
  targets = bins if targets.empty?
  targets.each { |t| spin_die("no such executable: bin/#{t}.rb") unless bins.include?(t) }
  d = install_dir(prefix)
  if uninstall
    targets.each do |t|
      f = File.join(d, t)
      if File.exist?(f)
        File.delete(f)
        puts "uninstalled " + f
      else
        puts "not installed: " + f
      end
    end
    return
  end
  cmd_build(prj, targets, "")
  system("mkdir -p " + d)
  targets.each do |t|
    src = File.join(prj.root, "build/bin", t)
    dst = File.join(d, t)
    spin_die("install: copy failed for " + t) unless system("install -m 755 " + src + " " + dst)
    puts "installed " + t + " -> " + dst
  end
end

def cmd_publish(root, repo_override, ref_override, direct)
  toml = TomlDoc.parse(File.read(File.join(root, "spin.toml")))
  name = toml.get("package", "name")
  version = toml.get("package", "version")
  spin_die("publish makes identity mandatory: set [package] name and version in spin.toml") if name == "" || version == ""
  repo = publish_repo_url(root, repo_override)

  dirty = sh_read("git -C " + root + " status --porcelain")
  spin_die("publish: uncommitted changes (commit and push first)") if dirty != "" && ref_override == ""
  ref = ref_override == "" ? sh_read("git -C " + root + " rev-parse HEAD") : ref_override
  spin_die("publish: cannot resolve HEAD (is this a git repo?)") if ref == ""

  # the commit must be reachable by consumers: some remote branch contains it
  reach = sh_read("git -C " + root + " branch -r --contains " + ref)
  spin_die("publish: commit " + ref[0..11].to_s + " is not on any remote branch (git push first)") if reach == ""

  # the tree at the release ref must carry the version being published
  tv = ""
  sh_read("git -C " + root + " show " + ref + ":spin.toml").split("\n").each do |l|
    tv = TomlDoc.parse(l + "\n").get("", "version") if l.strip.start_with?("version")
  end
  spin_die("publish: spin.toml at " + ref[0..11].to_s + " says version \"" + tv + "\", manifest says \"" + version + "\"") if tv != version

  # hard test gate (R8): a package publishes with passing tests or not at all
  prj = Project.new(root)
  spin_die("publish requires tests: add test/*.rb (spin test)") if prj.tests.empty?
  cmd_test(prj, [], false)   # exits non-zero on any failure

  # write the index entry
  index_refresh(false)
  idir = index_dir(false)
  gf = File.join(idir, "packages", name + ".toml")
  entry = ""
  if File.exist?(gf)
    gdoc = TomlDoc.parse(File.read(gf))
    erepo = gdoc.get("", "repo")
    spin_die("publish: index name \"" + name + "\" belongs to " + erepo + " (same name means the same library; rename per the name policy)") if erepo != repo
    i = 0
    while i < gdoc.array_len("release")
      spin_die("publish: " + name + " " + version + " is already in the index") if gdoc.get("release." + i.to_s, "version") == version
      i += 1
    end
    entry = File.read(gf)
  else
    entry = "name = \"" + name + "\"\nrepo = \"" + repo + "\"\n"
  end
  entry += "\n[[release]]\nversion = \"" + version + "\"\nref = \"" + ref + "\"\n"
  rev = spinel_rev
  if rev != ""
    entry += "\n[[probe]]\nversion = \"" + version + "\"\nspinel = \"" + rev + "\"\nresult = \"pass\"\ndate = \"" + Time.now.strftime("%Y-%m-%d") + "\"\n"
  end

  if direct
    File.write(gf, entry)
    ok = system("git -C " + idir + " add packages/" + name + ".toml") &&
         system("git -C " + idir + " -c user.email=spin@publish -c user.name=spin commit -qm \"" + name + " " + version + "\"") &&
         system("git -C " + idir + " push -q origin HEAD")
    spin_die("publish --direct: push to the index failed (no write access?)") unless ok
    puts "published " + name + " " + version + " (direct)"
    return
  end

  if system("gh --version > /dev/null 2>&1")
    # work in a scratch clone so the cache index stays on main
    tmp = File.join(File.dirname(idir), ".publish-" + name)
    system("rm -rf " + tmp)
    spin_die("publish: cannot clone the index") unless system("git clone -q " + idir + " " + tmp)
    File.write(File.join(tmp, "packages", name + ".toml"), entry)
    br = "publish-" + name + "-" + version.gsub(".", "-")
    login = sh_read("gh api user -q .login")
    spin_die("publish: gh is installed but not authenticated (gh auth login)") if login == ""
    system("gh repo fork " + spin_index_url + " --clone=false > /dev/null 2>&1")
    ok = system("git -C " + tmp + " checkout -qb " + br) &&
         system("git -C " + tmp + " add packages/" + name + ".toml") &&
         system("git -C " + tmp + " -c user.email=spin@publish -c user.name=spin commit -qm \"" + name + " " + version + "\"") &&
         system("git -C " + tmp + " push -q https://github.com/" + login + "/spin-index.git " + br + ":" + br)
    spin_die("publish: pushing the fork branch failed") unless ok
    body = "spin publish: " + name + " " + version + "%0A%0Arepo: " + repo + "%0Aref: " + ref + "%0Atests: pass (spin test gate)"
    body = body.gsub("%0A", "\n")
    okpr = system("gh pr create --repo " + spin_index_url.sub("https://github.com/", "") +
                  " --head " + login + ":" + br +
                  " --title \"" + name + " " + version + "\"" +
                  " --body \"" + body + "\"")
    system("rm -rf " + tmp)
    spin_die("publish: gh pr create failed") unless okpr
    puts "published " + name + " " + version + " (PR opened)"
    return
  end

  puts "gh not found -- open a pull request against " + spin_index_url
  puts "adding this as packages/" + name + ".toml:"
  puts ""
  puts entry
end

def cmd_vendor(prj)
  vg = File.join(prj.root, "vendor")
  Dir.mkdir(vg) unless Dir.exist?(vg)
  vg = File.join(vg, "packages")
  Dir.mkdir(vg) unless Dir.exist?(vg)
  prj.dep_records.split("\n").each do |rec|
    next if rec == ""
    f = rec.split("\t")
    dst = File.join(vg, f[0] + "-" + f[2])
    system("rm -rf " + dst)
    system("cp -a " + f[1] + " " + dst)
    puts "vendored " + f[0] + "-" + f[2]
  end
end

# --- scaffold ----------------------------------------------------------------

APP_MANIFEST = <<TOML
# spin manifest — an application needs no [package] identity.
# Add dependencies like:
#   [dependencies]
#   ansi = { path = "../spinel-ansi" }
TOML

def cmd_new(name, lib)
  spin_die("usage: spin new <name> [--lib]") if name == ""
  spin_die("#{name}: already exists") if File.exist?(name)
  Dir.mkdir(name)
  Dir.mkdir(File.join(name, "test"))
  if lib
    File.write(File.join(name, "spin.toml"),
               "[package]\nname = \"#{name}\"\nversion = \"0.1.0\"\n\n# published repos are conventionally named spinel-#{name}\n")
    File.write(File.join(name, "#{name}.rb"), "# #{name}: library entry (require \"#{name}\")\n")
  else
    File.write(File.join(name, "spin.toml"), APP_MANIFEST)
    Dir.mkdir(File.join(name, "bin"))
    File.write(File.join(name, "bin/#{name}.rb"), "puts \"Hello from #{name}\"\n")
  end
  File.write(File.join(name, ".gitignore"), "/build/\n")
  system("git -C #{name} init -q")
  puts "created #{name}/#{lib ? " (library)" : ""}"
end

def cmd_init
  spin_die("spin.toml already exists") if File.exist?("spin.toml")
  File.write("spin.toml", APP_MANIFEST)
  puts "wrote spin.toml"
end

# --- main --------------------------------------------------------------------

cmd = ARGV.empty? ? "" : ARGV[0]
rest = []
i = 1
while i < ARGV.length
  rest.push(ARGV[i])
  i += 1
end

case cmd
when "new"
  lib = rest.include?("--lib")
  names = rest.reject { |a| a.start_with?("--") }
  cmd_new(names.empty? ? "" : names[0], lib)
when "init"
  cmd_init
when "add"
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found") if root == ""
  nm = ""
  url = ""
  ref = ""
  pth = ""
  cons = ""
  feats = ""
  i2 = 0
  while i2 < rest.length
    a = rest[i2]
    if a == "--git"
      i2 += 1
      url = rest[i2].to_s
    elsif a == "--ref"
      i2 += 1
      ref = rest[i2].to_s
    elsif a == "--path"
      i2 += 1
      pth = rest[i2].to_s
    elsif a == "--version"
      i2 += 1
      cons = rest[i2].to_s
    elsif a == "--features"
      i2 += 1
      feats = rest[i2].to_s
    elsif !a.start_with?("--") && nm == ""
      nm = a
    end
    i2 += 1
  end
  cmd_add(root, nm, url, ref, pth, cons, feats)
when "remove"
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found") if root == ""
  cmd_remove(root, rest.empty? ? "" : rest[0])
when "lock", "fetch", "vendor"
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found") if root == ""
  prj = Project.new(root)
  lock_from_records(prj) if cmd == "lock"
  puts "fetched " + prj.dep_paths.length.to_s + " package(s)" if cmd == "fetch"
  cmd_vendor(prj) if cmd == "vendor"
when "flags"
  # The handoff: spin resolves the dependencies and warms the native cache,
  # then hands the compiler flags to whoever is driving the build. An
  # application whose repository spin does not own -- Ruby and C side by side
  # under one Makefile -- keeps its layout and still consumes packages (#4105).
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found") if root == ""
  puts spin_flags(Project.new(root))
when "search"
  cmd_search(rest.empty? ? "" : rest[0])
when "install"
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found") if root == ""
  prj = Project.new(root)
  pfx = ""
  names = []
  i4 = 0
  while i4 < rest.length
    a4 = rest[i4]
    if a4 == "--prefix"
      i4 += 1
      pfx = rest[i4].to_s
    elsif !a4.start_with?("--")
      names.push(a4)
    end
    i4 += 1
  end
  cmd_install(prj, names, pfx, rest.include?("--uninstall"))
when "publish"
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found") if root == ""
  rp = ""
  rf = ""
  i3 = 0
  while i3 < rest.length
    a3 = rest[i3]
    if a3 == "--repo"
      i3 += 1
      rp = rest[i3].to_s
    elsif a3 == "--ref"
      i3 += 1
      rf = rest[i3].to_s
    end
    i3 += 1
  end
  cmd_publish(root, rp, rf, rest.include?("--direct"))
when "list", "tree"
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found") if root == ""
  prj = Project.new(root)
  json = rest.include?("--json")
  cmd_list(prj, json) if cmd == "list"
  cmd_tree(prj, json) if cmd == "tree"
when "trust"
  spin_die("usage: spin trust <name>") if rest.empty?
  native_trust!(rest[0])
  puts "trusted: #{rest[0]} (native build steps run without prompting)"
when "build", "run", "test", "clean"
  root = find_root(Dir.pwd)
  spin_die("no spin.toml found (run `spin init`, or `spin new <name>`)") if root == ""
  if rest.include?("--allow-native-build")
    ENV["SPIN_ALLOW_NATIVE_BUILD"] = "1"
    rest = rest.reject { |a| a == "--allow-native-build" }
  end
  # Only args BEFORE a `--` are spin's; everything after belongs to the target
  # program. `-g` is a plausible flag for a program to want, and `spin run app
  # -- -g` must pass it through rather than turn into a debug build.
  dd = rest.index("--")
  spin_args = dd ? rest[0, dd] : rest
  if spin_args.include?("--debug") || spin_args.include?("-g")
    ENV["SPIN_DEBUG"] = "1"
    tail = dd ? rest[dd..] : []
    rest = spin_args.reject { |a| a == "--debug" || a == "-g" } + tail
  end
  prj = Project.new(root)
  case cmd
  when "build"
    extra = ""
    if rest.include?("--")
      di = rest.index("--").to_i
      extra = rest[(di + 1)..-1].join(" ")
      # `rest[0, di]`, not `rest[0..(di - 1)]`: with `--` first (`spin build --
      # --profile`, no target named) di is 0 and `rest[0..-1]` is the WHOLE
      # array, so `--` survives as a build target and the run dies with
      # "no such executable: bin/--.rb" instead of building every bin with the
      # extra flag.
      rest = rest[0, di]
    end
    cmd_build(prj, rest, extra)
  when "run"
    cmd_run(prj, rest)
  when "test"
    regen = rest.include?("--regen")
    files = rest.reject { |a| a.start_with?("--") }
    cmd_test(prj, files, regen)
  when "clean"
    system("rm -rf #{File.join(prj.root, 'build')}")
    puts "cleaned"
  end
when "--version"
  puts "spin"
else
  puts SPIN_USAGE
  exit(cmd == "" || cmd == "help" || cmd == "--help" ? 0 : 3)
end
