#include "codegen_internal.h"

Buf expr_buf(Compiler *c, int node) {
  Buf b; memset(&b, 0, sizeof b);
  emit_expr(c, node, &b);
  return b;
}

Buf *g_pre = NULL;

/* SP_COLLECT_ERRORS recovery: in collect mode a codegen gap longjmps back to a
   per-unit recovery point armed by the output driver (codegen.c), so one run
   surfaces every unsupported construct instead of aborting on the first. */
jmp_buf g_unsup_recover;
int g_unsup_armed = 0;
int g_unsup_probe = 0;   /* silent emittability probe: longjmp without printing/exiting */
int collect_mode(void) {
  static int collect = -1;
  if (collect < 0) collect = getenv("SP_COLLECT_ERRORS") ? 1 : 0;
  return collect;
}

void buf_putn(Buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + n + 1) nc *= 2;
    b->p = realloc(b->p, nc);
    b->cap = nc;
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = '\0';
}
void buf_puts(Buf *b, const char *s) { buf_putn(b, s, strlen(s)); }
/* Remove `n` bytes at `off`. Used to take back a prologue line the emitter had
   to write before it could know whether the body would need it. */
void buf_erase(Buf *b, size_t off, size_t n) {
  if (off > b->len || n == 0) return;
  if (off + n > b->len) n = b->len - off;
  memmove(b->p + off, b->p + off + n, b->len - off - n);
  b->len -= n;
  b->p[b->len] = '\0';
}
void buf_printf(Buf *b, const char *fmt, ...) {
  char tmp[512];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n < sizeof(tmp)) { buf_putn(b, tmp, (size_t)n); return; }
  char *big = malloc((size_t)n + 1);
  va_start(ap, fmt); vsnprintf(big, (size_t)n + 1, fmt, ap); va_end(ap);
  buf_putn(b, big, (size_t)n); free(big);
}
int  g_indent = 0;
/* Argument-hoist overrides: emit_args_filled pre-evaluates GC-hazardous
   call arguments into rooted temps; emit_expr then substitutes the temp
   name when it reaches the overridden node. */
int  g_argov_node[MAX_ARG_OVERRIDE];
char g_argov_text[MAX_ARG_OVERRIDE][16];
int  g_n_argov = 0;
/* Node id whose safe-nav (&.) guard is already emitted; the re-entrant
   emit_call skips the guard block for exactly this node. */
int  g_sn_skip = -1;
/* poly-dispatch builtin-arm re-entry marker: the call node whose dispatch is
   currently emitting its builtin-container arm, so the re-entered emission
   does not build the same dispatch again (#3459). */
int  g_pd_skip = -1;
/* Node whose Class-tag dispatch is emitting its non-Class arm, so the
   re-entered emission takes the ordinary path instead of rebuilding it. */
int  g_cls_tag_skip = -1;
/* True if evaluating the subtree at `id` may allocate (and so may trigger
   a GC): any call, container literal, or string interpolation qualifies. */
int subtree_may_allocate(const NodeTable *nt, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "CallNode") || sp_streq(ty, "ArrayNode") ||
      sp_streq(ty, "HashNode") || sp_streq(ty, "KeywordHashNode") ||
      sp_streq(ty, "InterpolatedStringNode") || sp_streq(ty, "SuperNode") ||
      sp_streq(ty, "ForwardingSuperNode") || sp_streq(ty, "YieldNode"))
    return 1;
  /* A NUL-containing (binary) string literal does not lower to an immortal
     rodata pointer: it allocates a heap string via sp_str_from_bytes (every
     evaluation when unfrozen, or once to fill a call-site cache when frozen).
     Either path can trigger a GC, so it must count as an allocating sibling
     so the operand-rooting logic protects a fresh operand next to it. A plain
     (NUL-free) literal is rodata and never allocates. */
  if (sp_streq(ty, "StringNode")) {
    const char *sc = nt_str(nt, id, "content");
    if (sc && nt_str_len(nt, id, "content") > strlen(sc)) return 1;
    return 0;
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_may_allocate(nt, nt_ref_at(nt, id, i))) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (subtree_may_allocate(nt, ids[j])) return 1;
  }
  return 0;
}
/* True if evaluating the subtree at `id` can be observed by, or can observe,
   a sibling argument's evaluation: any call (a user method, a mutating builtin,
   or an index read of a container someone else may write) or an assignment.
   Ruby fixes argument evaluation to left-to-right; C leaves a call's operand
   order unspecified, so such arguments have to be sequenced into temps. */
/* A builtin operator over scalars (`x + i`, `n < 3`) computes a value and
   touches nothing reachable, so no sibling argument can observe when it ran.
   It is a CallNode all the same, and counting it as an effect sequenced
   `cell_get(cells, x + ix, y + iy)` into temps for an ordering nobody can
   see -- 12% on the life benchmark. Gated on both the result and the
   receiver being scalar, so a user class's own `+` and `Array#<<` are
   effects as before. */
static int call_is_scalar_op(Compiler *c, int id) {
  static const char *const OPS[] = {
    "+","-","*","/","%","**","<",">","<=",">=","==","!=","<=>","&","|","^","<<",">>", NULL };
  const char *nm = nt_str(c->nt, id, "name");
  if (!nm) return 0;
  int hit = 0;
  for (int i = 0; OPS[i] && !hit; i++) if (sp_streq(nm, OPS[i])) hit = 1;
  if (!hit) return 0;
  if (nt_ref(c->nt, id, "block") >= 0) return 0;
  int recv = nt_ref(c->nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv), vt = comp_ntype(c, id);
  int scalar_r = (rt == TY_INT || rt == TY_FLOAT || rt == TY_BOOL);
  int scalar_v = (vt == TY_INT || vt == TY_FLOAT || vt == TY_BOOL);
  return scalar_r && scalar_v;
}

int subtree_has_side_effect(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "SuperNode") || sp_streq(ty, "ForwardingSuperNode") ||
      sp_streq(ty, "YieldNode") || strstr(ty, "WriteNode"))
    return 1;
  if (sp_streq(ty, "CallNode") && !call_is_scalar_op(c, id)) return 1;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_has_side_effect(c, nt_ref_at(nt, id, i))) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (subtree_has_side_effect(c, ids[j])) return 1;
  }
  return 0;
}
int  g_tmp = 0;
char g_ren_from[MAX_RENAME][96];
char g_ren_to[MAX_RENAME][112];
int  g_nren = 0;
int  g_block_id = -1;
/* comp_ntype's yield hook: while a literal block is spliced, a YieldNode's
   value is THAT block's tail, not the union the node cache holds over every
   call site -- a second site whose block answers another class was compiled
   as the first one's (#3784). Installed by codegen_main; NULL elsewhere. */
int (*sp_yield_site_type_hook)(const Compiler *c, int id, TyKind *out) = NULL;
/* `blk.call(...)` on the enclosing method's own block parameter, which the
   inliner splices exactly as it splices a `yield`. Its cached type is the one
   the proc form has -- a Proc call answers poly -- but the spliced form
   computes whatever the caller's literal block computes, so the two emissions
   of the one node want different types (#3916). */
static int blk_param_call(const Compiler *c, int id) {
  if (!g_block_param_name || !g_block_param_name[0]) return 0;
  const char *nm = nt_str(c->nt, id, "name");
  if (!nm || !sp_streq(nm, "call")) return 0;
  int r = nt_ref(c->nt, id, "receiver");
  if (r < 0 || nt_kind(c->nt, r) != NK_LocalVariableReadNode) return 0;
  const char *rn = nt_str(c->nt, r, "name");
  return rn && sp_streq(rn, g_block_param_name);
}
int sp_yield_site_type(const Compiler *c, int id, TyKind *out) {
  if (g_block_id < 0 || id < 0) return 0;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return 0;
  if (!sp_streq(ty, "YieldNode") &&
      !(sp_streq(ty, "CallNode") && blk_param_call(c, id))) return 0;
  int bbody = nt_ref(c->nt, g_block_id, "body");
  int bn = 0;
  const int *bb = bbody >= 0 ? nt_arr(c->nt, bbody, "body", &bn) : NULL;
  if (bn <= 0 || !bb) return 0;
  TyKind bt = c->ntype[bb[bn - 1]];
  /* only a CONCRETE per-site answer overrides the cache; an unresolved tail
     leaves the node's own (unified) type in place */
  if (bt == TY_UNKNOWN || bt == TY_VOID) return 0;
  *out = bt;
  return 1;
}
int  g_yield_block_fallback = -1;
/* rename-table depth at g_block_id's DEFINITION site: the spliced block body
   and its param names resolve against the entries below this mark only; the
   enclosing inline's renames above it must not capture same-named block
   locals (#3281). Paired with g_block_id / g_yield_block_fallback. */
int  g_block_nren = 0;
int  g_yield_block_fallback_nren = 0;
/* The (g_self, g_self_deref) that were active when the current g_block_id
   was captured -- i.e. the caller context of the innermost yield-method
   inline. A block spliced at a `yield` is caller code: emit_block_invoke
   emits its body under these instead of the inlined method's rebound self
   (`@map.vertices[...]` inside a block passed to an inlined method must
   read the CALLER's @map). Maintained by the inliners exactly like
   g_yield_block_fallback. */
const char *g_yield_self_fallback = NULL;
const char *g_yield_self_deref_fallback = NULL;
/* Companion to g_yield_self_fallback: the CALLER's emitting-class, so a
   block body spliced into an inlined callee resolves its implicit-self
   calls against the caller's class (the block is caller code). */
int g_yield_emitting_class_fallback = -1;
const char *g_block_param_name = NULL;
/* Inside an Enumerator.new { |y| ... } generator body, the name of the yielder
   block param. A `y << v` / `y.yield(v)` on it lowers to a Fiber.yield. */
const char *g_yielder_name = NULL;
const char *g_self = "self";
/* Member-access operator for `self`: "->" when self is a pointer (the usual
   heap object), "." when emitting a value-type method body (self is a value). */
const char *g_self_deref = "->";
/* When set, emit_inline_call_x binds self from this pre-hoisted expression
   instead of re-emitting the receiver node -- used by the poly-receiver block
   dispatch to bind self to a per-arm cast of the boxed receiver (#2448). */
const char *g_inline_recv_expr = NULL;
int g_inline_recv_class = -1;
int g_class_body_id = -1;
int g_emitting_class_id = -1;
const char *g_dm_subst_name = NULL;
int g_dm_subst_node = -1;
int g_ie_class_id = -1;
int g_ie_discard_value = 0;
const char *g_rescue_cls = NULL, *g_rescue_msg = NULL;
const char *g_retry_label = NULL;
int g_redo_stack[64];
int g_redo_depth = 0;
const char *g_loop_break_var = NULL;
/* When a direct instance_exec/eval splice is wrapped in a do{}while(0), this
   holds the C result temp so a top-level `next <v>` captures its value before
   continuing out of the splice (mirrors g_loop_break_var for `break`). */
const char *g_ie_next_var = NULL;
/* C-loop nesting depth within the current function body: `next` emits a plain
   `continue` only when inside a C loop; at depth 0 inside a proc function it
   is the proc's own return (Ruby block semantics), not a loop control. */
int g_c_loop_depth = 0;
int g_in_proc_body = 0;
/* Set while emitting an instance_exec/eval splice whose result temp is poly:
   a `break <v>` / `next <v>` carrying a scalar value must box it to match. */
int g_ie_res_poly = 0;
/* Set while emitting a block body wrapped in a valued-break setjmp scope:
   the C lvalue holding the enclosing scope's SERIAL, so a top-level
   `break <v>` lowers to sp_brk_throw(<serial>, v) rather than a C `break`.
   NULLed when entering a nested C loop (while/for/until/loop) or another
   emission context (method/proc/fiber body, instance_exec splice) whose own
   `break` must not target this scope. Non-lambda procs capture its value at
   creation as their break home. */
const char *g_brk_ser_var = NULL;
/* g_ensure_depth at the enclosing wrapper: a break at the SAME depth has no
   intervening ensure bodies and delivers by same-function `goto` (register-
   safe: a longjmp would roll back register-allocated locals mutated since
   the setjmp -- the known hazard of the catch/throw machinery); a deeper
   break longjmps via sp_brk_throw so the ensures run. */
int g_brk_ensure_base = 0;
/* The break-scope serial var bound to the CURRENT block (g_block_id): saved
   at the call site when a yielding method is inlined, re-installed while the
   block body is spliced at a yield site -- so a `break` in the block targets
   the call that received it even when the yield sits inside the method's own
   loops or nested iterators. Paired with g_yield_block_fallback the same way
   g_block_id is. */
const char *g_block_brk_var = NULL;
const char *g_yield_blk_brk_fallback = NULL;
int g_block_brk_ebase = 0;
int g_yield_blk_brk_efallback = 0;
/* Proc-literal body context: 1 = lambda (break returns from the lambda),
   2 = non-lambda proc with a break (targets its captured home scope). */
int g_proc_body_kind = 0;
/* C expression for the non-lambda proc's captured break-home serial. */
const char *g_proc_brk_home = NULL;
/* The CallNode id currently being emitted as the inner (unwrapped) call of its
   own break wrapper, so the wrapper is not re-entered recursively. */
int g_brk_skip_id = -1;
const char *g_result_var = NULL;
int g_result_poly = 0;
/* The TyKind of the slot g_result_var names, so a tail value can be checked
   against it (a diverging call may carry an unrelated C type). */
TyKind g_result_ty = TY_UNKNOWN;
/* Non-lambda proc `return` support. While emitting a method that owns a
   proc-return frame, g_method_pr_label / g_method_pr_var name the single-exit
   goto label and the value var, so an explicit `return` funnels there (popping
   the frame once) instead of returning directly. While emitting a returning
   proc's body, g_proc_return_home is the C expression for the home frame index
   (read from the proc capture), so `return` longjmps to the home method. */
const char *g_method_pr_label = NULL;
const char *g_method_pr_var = NULL;
const char *g_proc_return_home = NULL;
/* While a constructor's omitted defaults are emitted: the C text of the object
   being built, so a default that reads self (`def initialize(n = config_val)`)
   resolves against the new instance rather than the caller's self. */
const char *g_ctor_self = NULL;
const char *g_ctor_self_deref = NULL;
/* Emitting the body of a non-lambda proc created at top level: a `return`
   there is a TOP-LEVEL return, which ends the script (#3663). */
int g_proc_toplevel_return = 0;
/* Number of live setjmp exception frames (begin/rescue) enclosing the
   current emission point. A `return` from inside a try body must pop them
   (sp_exc_top -= N) before leaving -- a stale frame's jmp_buf points into
   a dead C stack frame, and the next raise longjmps into it (doom's
   SoundManager#[] early returns corrupted the stack this way).
   g_method_pr_exc_depth snapshots the depth at the return-funnel target so
   funnel gotos pop only the frames they actually exit. */
int g_exc_frame_depth = 0;
int g_loop_exc_base = 0;        /* frame depth at the innermost C-loop entry */
int g_loop_ensure_base = 0;     /* ensure depth at the innermost C-loop entry */
int g_brk_exc_base = 0;         /* frame depth at the valued-break wrapper */
int g_block_brk_exc_base = 0;   /* ... for yield-block re-entry (mirrors g_block_brk_ebase) */
int g_method_pr_exc_depth = 0;
/* Loop-invariant string-length hoisting: while a loop whose receiver string is
   not mutated in its body is being emitted, g_hoist_len_recv holds that
   receiver's AST local name and g_hoist_len_var the C temp caching its length;
   a matching `s.length`/`s.size` then emits the temp instead of strlen. */
const char *g_hoist_len_var = NULL;
const char *g_hoist_len_recv = NULL;
TyKind g_ret_type = TY_UNKNOWN;
/* Mirror of the REAL enclosing function's return funnel: yield-method inlining
   overrides g_method_pr_label/-_var/g_ret_type with a per-inline funnel, and a
   spliced block body (which lexically belongs to the real function, so its
   `return` exits that method) restores from these. Set wherever a fresh
   function context installs (or clears) its funnel. */
const char *g_fn_pr_label = NULL;
const char *g_fn_pr_var = NULL;
TyKind g_fn_ret_type = TY_UNKNOWN;
int g_current_scope_is_lowered = 0;
/* the scope being emitted has an --rbs-seeded return type (#3412) */
int g_ret_seeded = 0;
/* The block-param name of the lowered method currently being emitted (its
   declared &block name, or "__yblk__" when the lowering synthesized one);
   NULL outside a lowered-method emission. Read by emit_yblk_ref. */
const char *g_lowered_blk_name = NULL;
/* Set while emitting a lifted Thread/Fiber body that reaches the lowered
   method's block: the block lives in a cell the frame carries. */
int g_yblk_celled = 0;
/* The enclosing lowered context parked across an inline: an inlined callee's
   own yields splice the call-site block, so emit_inline_call_x clears the
   lowered pair for the callee body and parks it here; emit_block_invoke
   restores it around spliced CALLER code (whose yields do belong to the
   lowered method) -- the same discipline as g_yield_self_fallback. */
int g_yield_lowered_fallback = 0;
const char *g_yield_lowered_blk_fallback = NULL;
/* When a yielding method is inlined and its block is a forwarded REAL proc
   (the caller nil-checks its &block, so the block can't be an inlined literal),
   this holds the C expression for that proc; the inlined `yield` calls it via
   sp_proc_call instead of splicing a block body. NULL otherwise. */
const char *g_yield_proc_ref = NULL;
/* The inlined call's return-slot type while g_yield_proc_ref is set: sp_proc_call
   yields poly, but the slot may be concrete (the analyzer typed this forwarding
   context), so a value-position yield unboxes its result to this. */
TyKind g_yield_slot_ty = TY_UNKNOWN;
EnsureCtx g_ensure_stack[MAX_ENSURE_DEPTH];
int       g_ensure_depth = 0;
RescueSave g_rescue_save_stack[MAX_ENSURE_DEPTH];
int        g_rescue_save_depth = 0;

/* rescue bodies crossed by an exit to frame-depth pop_base: those entered at or
   deeper than pop_base (their exc_base >= pop_base). */
int rescues_crossed(int pop_base) {
  int k = 0;
  for (int i = 0; i < g_rescue_save_depth; i++)
    if (g_rescue_save_stack[i].exc_base >= pop_base) k++;
  return k;
}
/* Pop the k crossed rescue-body handlers (no frame pop). Used at sites whose
   frame-pop text is special (the begin..ensure deferred-return). */
void emit_cur_exc_restore(Buf *b, int pop_base) {
  int k = rescues_crossed(pop_base);
  if (k > 0) buf_printf(b, "sp_rescue_sp -= %d; ", k);
}
int emit_frame_unwind(Buf *b, int pop_base, const char *guard) {
  int pops = g_exc_frame_depth - pop_base;
  int k = rescues_crossed(pop_base);
  if (pops <= 0 && k == 0) return 0;
  if (guard) buf_printf(b, "if (%s) { ", guard);
  if (pops > 0) buf_printf(b, "sp_exc_top -= %d; ", pops);
  if (k > 0) buf_printf(b, "sp_rescue_sp -= %d; ", k);
  if (guard) buf_puts(b, "}");
  return 1;
}
Buf g_procs;
Buf g_proc_protos;
int g_proc_counter = 0;
int g_needs_proc_poly_argslot = 0; /* any proc takes a TY_POLY arg via _sp_proc_poly_args */
/* Fiber body functions accumulate here (similar to g_procs but void(*)(sp_Fiber*)). */
int g_fiber_counter = 0;
char **g_re_src; int *g_re_flg; int g_re_count, g_re_cap;
int re_engine_flags(int pf) {
  int f = 0;
  if (pf & 4) f |= 1;
  if (pf & 8) f |= 8;
  if (pf & 16) f |= 6;
  return f;
}
int re_has_captures(const char *src) {
  if (!src) return 0;
  int in_class = 0;
  for (const char *p = src; *p; p++) {
    if (*p == '\\') { if (p[1]) p++; continue; }
    /* a `(` inside a `[...]` character class is a literal paren, not a group
       (`/[()]/` has no captures) (#2912) */
    if (in_class) { if (*p == ']') in_class = 0; continue; }
    if (*p == '[') { in_class = 1; continue; }
    if (*p == '(') {
      if (p[1] != '?') return 1;
      /* (?<name>...) / (?'name'...) are named CAPTURE groups (lookbehinds
         (?<= (?<! are not) */
      if (p[1] == '?' && p[2] == '<' && p[3] != '=' && p[3] != '!') return 1;
      if (p[1] == '?' && p[2] == 0x27) return 1;
    }
  }
  return 0;
}
/* The RegularExpressionNode behind `nid`, or -1 when the pattern is only
   knowable at run time. A bare literal, a constant bound to one
   (`PAT = /re/[.freeze]`, possibly namespaced) and a regex-typed local bound to
   one all resolve.

   One resolver for every caller. re_lit_index, re_lit_src and re_lit_flags each
   carried their own copy and they disagreed about names: re_lit_src did not
   follow a local, so codegen picked the capturing `scan` emit for a pattern
   named by a constant while typing the same call from the non-capturing shape,
   and dropped the capture groups for one named by a local (#3391). */
int re_lit_node(Compiler *c, int nid) {
  if (nid < 0) return -1;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, nid);
  if (!ty) return -1;
  if (sp_streq(ty, "RegularExpressionNode")) return nid;
  int want_const = sp_streq(ty, "ConstantReadNode") || sp_streq(ty, "ConstantPathNode");
  int want_local = sp_streq(ty, "LocalVariableReadNode") && comp_ntype(c, nid) == TY_REGEX;
  if (!want_const && !want_local) return -1;
  const char *nm = nt_str(nt, nid, "name");
  if (!nm) return -1;
  for (int k = 0; k < nt->count; k++) {
    const char *kt = nt_type(nt, k);
    if (!kt) continue;
    if (want_const ? (!sp_streq(kt, "ConstantWriteNode") && !sp_streq(kt, "ConstantPathWriteNode"))
                   : !sp_streq(kt, "LocalVariableWriteNode"))
      continue;
    const char *kn = nt_str(nt, k, "name");
    if (!kn || !sp_streq(kn, nm)) continue;
    int v = nt_ref(nt, k, "value");
    if (want_const && v >= 0 && nt_type(nt, v) && sp_streq(nt_type(nt, v), "CallNode") &&
        nt_str(nt, v, "name") && sp_streq(nt_str(nt, v, "name"), "freeze"))
      v = nt_ref(nt, v, "receiver");
    if (v >= 0 && nt_type(nt, v) && sp_streq(nt_type(nt, v), "RegularExpressionNode")) return v;
  }
  return -1;
}
int re_lit_index(Compiler *c, int nid) {
  nid = re_lit_node(c, nid);
  if (nid < 0) return -1;
  const char *src = nt_str(c->nt, nid, "unescaped");
  if (!src) return -1;
  int flg = re_engine_flags((int)nt_int(c->nt, nid, "flags", 0));
  for (int i = 0; i < g_re_count; i++)
    if (g_re_flg[i] == flg && sp_streq(g_re_src[i], src)) return i;
  /* A literal is two constants, the pattern and its flags, so whether the
     engine can read it is known here rather than at the program's startup,
     where it used to surface as a RegexpError from a build that had reported
     nothing. CRuby reports it from the parse, as a SyntaxError. Checked once
     per distinct literal, after the table lookup above. */
  {
    const char *err = sp_re_literal_error(src, (int)strlen(src), flg);
    if (err) unsupported_feature(c, nid, err);
  }
  if (g_re_count >= g_re_cap) {
    g_re_cap = g_re_cap ? g_re_cap * 2 : 8;
    g_re_src = realloc(g_re_src, sizeof(char *) * (size_t)g_re_cap);
    g_re_flg = realloc(g_re_flg, sizeof(int) * (size_t)g_re_cap);
  }
  g_re_src[g_re_count] = (char *)src;
  g_re_flg[g_re_count] = flg;
  return g_re_count++;
}
const char *re_lit_src(Compiler *c, int nid) {
  nid = re_lit_node(c, nid);
  return nid < 0 ? NULL : nt_str(c->nt, nid, "unescaped");
}
/* Prism flags of a statically resolvable regexp (-1 if `nid` is not one). */
int re_lit_flags(Compiler *c, int nid) {
  nid = re_lit_node(c, nid);
  return nid < 0 ? -1 : (int)nt_int(c->nt, nid, "flags", 0);
}
void emit_interp(Compiler *c, int id, Buf *b);  /* forward */

/* Emit a regex pattern expression to `b`, handling both static literals and
   interpolated patterns. For interpolated patterns, setup is emitted to
   g_pre and a temp mrb_regexp_pattern* variable name is written to `b`.
   Returns 1 if handled, 0 if nid is not a recognizable regex. */
int emit_regex_pat_to_buf(Compiler *c, int nid, Buf *b) {
  int ri = re_lit_index(c, nid);
  if (ri >= 0) { buf_printf(b, "sp_re_pat_%d", ri); return 1; }
  const char *ty = nt_type(c->nt, nid);
  if (ty && sp_streq(ty, "InterpolatedRegularExpressionNode")) {
    int flg = re_engine_flags((int)nt_int(c->nt, nid, "flags", 0));
    int ts = ++g_tmp, tp = ++g_tmp;
    /* Emit the interpolated pattern into a local buffer: an embedded call that
       roots its own args pushes those decls to g_pre, which must land as whole
       statements before this temp's decl, not inside its initializer (#1498). */
    Buf pv; memset(&pv, 0, sizeof pv);
    emit_interp(c, nid, &pv);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "const char *_t%d = %s;\n", ts, pv.p ? pv.p : "\"\"");
    free(pv.p);
    emit_indent(g_pre, g_indent);
    /* the pattern text may hold a NUL (Regexp.new("a\0b")), and strlen would
       compile only the part before it */
    buf_printf(g_pre, "mrb_regexp_pattern *_t%d = re_compile(_t%d, (int64_t)sp_str_byte_len(_t%d), %d);\n", tp, ts, ts, flg);
    buf_printf(b, "_t%d", tp);
    return 1;
  }
  return 0;
}
int nameset_has(NameSet *s, const char *nm) {
  if (!nm) return 0;
  for (int i = 0; i < s->n; i++) if (sp_streq(s->v[i], nm)) return 1;
  return 0;
}
void nameset_add(NameSet *s, const char *nm) {
  if (!nm || nameset_has(s, nm)) return;
  if (s->n >= s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->v = realloc(s->v, sizeof(char *) * (size_t)s->cap); }
  s->v[s->n++] = nm;
}
const char *g_cap_struct = NULL;
NameSet *g_cap_names = NULL;
int g_needs_at_exit = 0;
int g_needs_class_machinery = 0;
int g_has_user_global_marks = 0;
int g_uses_symbols = 0;
int g_uses_marshal = 0;
int g_emit_sym_rt = 0;
int g_emit_class_names = 0;
int g_emit_obj_dispatch = 0;
int g_uses_program_name = 0;
int g_gen_obj_hash = 0;
int g_gen_obj_to_json = 0;
int g_gen_obj_to_h = 0;
int g_gen_obj_with = 0;
int g_uses_regex = 0;
int g_uses_argv = 0;
int g_uses_threads = 0;
int g_has_user_cmp = 0;
int g_has_user_binop = 0;
TyKind g_ie_next_ty = TY_UNKNOWN;
int g_has_user_coerce = 0;
int g_has_user_to_io = 0;
int g_gen_obj_hashkey = 0;
int g_gen_obj_valeq = 0;
int g_re_init_needed = 0;
void emit_local_ref(Compiler *c, int scope_node, const char *name, Buf *b) {
  if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, name)) {
    /* A TY_PROC capture is stored as (sp_int)(uintptr_t)sp_Proc* in the cell.
       Cast it back to sp_Proc* so call sites work. A heap-object cell is a real
       typed pointer, so its deref is already the right lvalue (no cast). */
    LocalVar *clv = scope_node >= 0 ? scope_local(comp_scope_of(c, scope_node), name) : NULL;
    if (clv && clv->type == TY_PROC)
      buf_printf(b, "(sp_Proc *)(uintptr_t)(*((%s *)_cap)->c_%s)", g_cap_struct, name);
    else
      buf_printf(b, "(*((%s *)_cap)->c_%s)", g_cap_struct, name);
    return;
  }
  LocalVar *lv = scope_node >= 0 ? scope_local(comp_scope_of(c, scope_node), name) : NULL;
  if (lv && lv->is_cell) {
    /* Through the rename map, exactly as the plain form below: a method
       INLINED at its call site renames its locals, and the cell form did not
       follow -- the prologue declared `lv__y1_n` while the body read
       `(*_cell_n)`, which nothing declared (#4088). Outside an inline the map
       is empty and this is the name itself. */
    const char *crn = rename_local(name);
    if (lv->type == TY_PROC) buf_printf(b, "(sp_Proc *)(uintptr_t)(*_cell_%s)", crn);
    else buf_printf(b, "(*_cell_%s)", crn);
    return;
  }
  buf_printf(b, "lv_%s", rename_local(name));
}
void emit_yblk_ref(Buf *b) {
  /* The lowered method's block param: the declared &block name when the def
     has one, else the synthetic __yblk__. */
  const char *nm =
      (g_lowered_blk_name && g_lowered_blk_name[0]) ? g_lowered_blk_name : "__yblk__";
  if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, nm)) {
    buf_printf(b, "(sp_Proc *)(uintptr_t)(*(((%s *)_cap)->c_%s))", g_cap_struct, nm);
  }
  /* A yield inside a lifted Thread/Fiber body reaches the forwarded block
     through the shared cell the body's frame carries, not a local (#3355). */
  else if (g_yblk_celled) {
    buf_printf(b, "(sp_Proc *)(uintptr_t)(*_cell_%s)", nm);
  }
  else {
    buf_printf(b, "lv_%s", nm);
  }
}
void emit_tail_lead(Buf *b) {
  if (g_result_var) buf_printf(b, "%s = ", g_result_var);
  else buf_puts(b, "return ");
}
/* The C representation of Ruby `nil` for a concretely-typed slot (vs
   default_value's zero-value): a fresh block-local starts nil, and several
   types carry an in-band nil sentinel (NULL string, SP_INT_NIL, NaN float,
   (sp_sym)-1). Types with no sentinel fall back to the zero value. */
const char *nil_value(TyKind t) {
  switch (t) {
    case TY_STRING: return "NULL";
    case TY_INT:    return "SP_INT_NIL";
    case TY_FLOAT:  return "sp_float_nil()";
    case TY_POLY:   return "sp_box_nil()";
    default:        return NULL;
  }
}

static int subtree_has_param_named(const NodeTable *nt, int id, const char *nm);
int subtree_has_param_named_pub(const NodeTable *nt, int id, const char *nm) {
  return subtree_has_param_named(nt, id, nm);
}
static int subtree_has_param_named(const NodeTable *nt, int id, const char *nm) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);

  /* numbered params (_1.._9): the NumberedParametersNode carries no child
     parameter nodes, yet the block's locals list contains the names. */
  if (ty && sp_streq(ty, "NumberedParametersNode") &&
      nm[0] == '_' && nm[1] >= '1' && nm[1] <= '9' && !nm[2]) return 1;
  if (ty && sp_streq(ty, "ItParametersNode") && sp_streq(nm, "it")) return 1;
  if (ty && (strstr(ty, "ParameterNode") || sp_streq(ty, "LocalVariableTargetNode"))) {
    const char *pn = nt_str(nt, id, "name");
    /* shadow-renaming rewrites a param's node-table name to NAME__bpNN;
       the parser's locals list keeps the raw NAME -- match both. */
    if (pn) {
      size_t nl = strlen(nm);
      if (sp_streq(pn, nm)) return 1;
      if (!strncmp(pn, nm, nl) && !strncmp(pn + nl, "__bp", 4)) return 1;
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_has_param_named(nt, nt_ref_at(nt, id, i), nm)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (subtree_has_param_named(nt, ids[j], nm)) return 1;
  }
  return 0;
}

void emit_block_locals_reset(Compiler *c, int blk, Buf *b, int indent) {
  if (blk < 0) return;
  const char *locs = nt_str(c->nt, blk, "locals");
  if (!locs || !*locs) return;
  char tmpn_buf[128];
  const char *p = locs;
  while (*p) {
    const char *e = strchr(p, ',');
    size_t l = e ? (size_t)(e - p) : strlen(p);
    if (l) {
      /* names longer than the stack buffer are legal Ruby; heap-fall back
         rather than silently skipping the reset */
      char *tmpn = l < sizeof tmpn_buf ? tmpn_buf : malloc(l + 1);
      if (!tmpn) break;
      memcpy(tmpn, p, l); tmpn[l] = 0;
      /* Skip every name bound by the block's parameter list, including
         destructured, optional, rest, and shadow (`; x`) declarations --
         collected straight from the parameters subtree, since
         block_param_name only covers plain leading params. */
      int is_param = subtree_has_param_named(c->nt, nt_ref(c->nt, blk, "parameters"), tmpn);

      if (!is_param) {
        Scope *sc = comp_scope_of(c, blk);
        LocalVar *lv = sc ? scope_local(sc, tmpn) : NULL;
        /* A captured (cell-backed) block-local gets a FRESH cell each
           invocation: the capture struct copies the current cell pointer at
           proc creation, so per-iteration closures each keep their own
           binding -- one shared cell made every closure observe the final
           iteration's value (#3230). The prologue's SP_GC_ROOT registered
           the cell VARIABLE's address, so the reassignment stays rooted;
           earlier cells stay live through their captures' scans. */
        if (lv && lv->is_cell) {
          const char *rn2 = rename_local(tmpn);
          emit_indent(b, indent);
          if (lv->type == TY_FLOAT) {
            buf_printf(b, "_cell_%s = (sp_float *)sp_gc_alloc(sizeof(sp_float), NULL, NULL); *_cell_%s = 0.0;\n", rn2, rn2);
          }
          else if (lv->type == TY_POLY) {
            buf_printf(b, "_cell_%s = (sp_RbVal *)sp_gc_alloc(sizeof(sp_RbVal), NULL, sp_cell_scan_rbval); *_cell_%s = sp_box_nil();\n", rn2, rn2);
          }
          else if (cell_value_struct(lv->type)) {
            /* a by-value struct rides a cell of its own type, as the two cell
               prologues already say; without this arm the reset fell through to
               the sp_int else and assigned an sp_int * to an sp_Class * */
            const char *vs = cell_value_struct(lv->type);
            buf_printf(b, "_cell_%s = (%s *)sp_gc_alloc(sizeof(%s), NULL, NULL); *_cell_%s = %s;\n",
                       rn2, vs, vs, rn2, cell_value_struct_empty(lv->type));
          }
          else if (lv->type != TY_PROC && lv->type != TY_INT && lv->type != TY_BOOL &&
                   lv->type != TY_SYMBOL && lv->type != TY_UNKNOWN && cell_is_typed_ptr(c, lv)) {
            const char *cell_scan = cell_scan_fn(lv->type);
            buf_printf(b, "_cell_%s = (", rn2); emit_ctype(c, lv->type, b);
            buf_puts(b, " *)sp_gc_alloc(sizeof(");
            emit_ctype(c, lv->type, b);
            buf_printf(b, "), NULL, %s); *_cell_%s = NULL;\n", cell_scan, rn2);
          }
          else if (lv->type == TY_PROC) {
            /* an int cell holding a collectable Proc still needs a scan (#4077) */
            buf_printf(b, "_cell_%s = (sp_int *)sp_gc_alloc(sizeof(sp_int), NULL, sp_cell_scan_procint); *_cell_%s = 0;\n", rn2, rn2);
          }
          else {
            buf_printf(b, "_cell_%s = (sp_int *)sp_gc_alloc(sizeof(sp_int), NULL, NULL); *_cell_%s = 0;\n", rn2, rn2);
          }
        }
        else if (lv && lv->type != TY_UNKNOWN && !lv->is_cell) {
          emit_indent(b, indent);
          /* A value-type object is stored inline (sp_X, not sp_X*), so its
             empty/nil reset is a zeroed struct -- default_value()'s blanket
             "NULL" would assign a pointer to a struct lvalue (#3267). */
          if (ty_is_object(lv->type) && c->classes[ty_object_class(lv->type)].is_value_type) {
            buf_printf(b, "lv_%s = (sp_%s){0};\n", rename_local(tmpn),
                       c->classes[ty_object_class(lv->type)].c_name);
          }
          else {
            const char *nv = nil_value(lv->type);
            if (!nv) nv = lv->type == TY_RANGE ? "(sp_Range){0}" : default_value(lv->type);
            buf_printf(b, "lv_%s = %s;\n", rename_local(tmpn), nv);
          }
        }
      }
      if (tmpn != tmpn_buf) free(tmpn);
    }
    if (!e) break;
    p = e + 1;
  }
}

/* The receiver's local name when it is a shared-mutable (TY_STRBUF) string
   local read, else NULL (#3227 shim gate). */
const char *strbuf_local_name(Compiler *c, int recv) {
  if (recv < 0) return NULL;
  const char *rty = nt_type(c->nt, recv);
  if (!rty || !sp_streq(rty, "LocalVariableReadNode")) return NULL;
  const char *rn = nt_str(c->nt, recv, "name");
  Scope *rs = rn ? comp_scope_of(c, recv) : NULL;
  LocalVar *rl = rs ? scope_local(rs, rn) : NULL;
  return (rl && rl->type == TY_STRBUF) ? rn : NULL;
}
/* The owning class slot of an ivar READ node, mirroring the read emitter's
   storage resolution: instance method -> its class; top-level method ->
   the Toplevel pseudo-class; class-method / instance_eval contexts return
   -1 (their storage is not a per-instance field). */
int strbuf_ivar_owner(Compiler *c, int node) {
  Scope *cs = comp_scope_of(c, node);
  if (!cs) return -1;
  if (cs->is_cmethod) return -1;
  if (cs->class_id >= 0) return cs->class_id;
  if (g_ie_class_id >= 0) return -1;
  return comp_class_index(c, "Toplevel");
}
/* Emit-side lvalue for a shared-mutable string receiver: lv_<x> for a
   strbuf local, <self>-><iv_x> (or civ_Toplevel_x) for a strbuf ivar.
   Returns 1 and fills `out`, or 0 when the receiver is neither (#3227). */
/* An empty `[]` / `{}` assigned into a typed slot has to be built at the SLOT's
   representation, not at the literal's own default: the literal carries no
   element or key type, so emit_expr answers an IntArray / StrPolyHash and the
   store lands on a slot of a different C type. The global and constant writes
   spell this out inline; class variables share the rule (#4054). Returns 1
   when it emitted. */
int emit_empty_container_for_slot(Compiler *c, int v, TyKind slot, Buf *b) {
  const NodeTable *nt = c->nt;
  if (v < 0) return 0;
  const char *vty = nt_type(nt, v);
  if (!vty) return 0;
  int n = 0;
  if (sp_streq(vty, "ArrayNode")) {
    nt_arr(nt, v, "elements", &n);
    if (n != 0) return 0;
    if (slot == TY_POLY_ARRAY) { buf_puts(b, "sp_PolyArray_new()"); return 1; }
    if (array_kind(slot)) { buf_printf(b, "sp_%sArray_new()", array_kind(slot)); return 1; }
    return 0;
  }
  if (sp_streq(vty, "HashNode") || sp_streq(vty, "KeywordHashNode")) {
    nt_arr(nt, v, "elements", &n);
    if (n != 0) return 0;
    const char *hcn = ty_is_hash(slot) ? ty_hash_cname(slot) : NULL;
    if (!hcn) return 0;
    buf_printf(b, "sp_%sHash_new()", hcn);
    return 1;
  }
  /* `Hash.new` with no arguments or block is the same empty producer, and the
     typed slot needs the same fresh `sp_XHash_new()` rather than the boxed
     value the untyped call would emit (the global write says this too). */
  if (sp_streq(vty, "CallNode") && ty_is_hash(slot)) {
    const char *cn = nt_str(nt, v, "name");
    int r = nt_ref(nt, v, "receiver");
    const char *rn = (r >= 0 && nt_kind(nt, r) == NK_ConstantReadNode) ? nt_str(nt, r, "name") : NULL;
    int a = nt_ref(nt, v, "arguments");
    int an = 0; if (a >= 0) nt_arr(nt, v, "arguments", &an);
    if (cn && rn && sp_streq(cn, "new") && sp_streq(rn, "Hash") && an == 0 &&
        nt_ref(nt, v, "block") < 0) {
      const char *hcn = ty_hash_cname(slot);
      if (!hcn) return 0;
      buf_printf(b, "sp_%sHash_new()", hcn);
      return 1;
    }
  }
  return 0;
}

int emit_poly_rhs_coerced(Compiler *c, TyKind slot, int v, Buf *b) {
  /* yield_site_type, not comp_ntype: a `yield` carries the union over every
     call site, and the block spliced HERE may already hand back the scalar
     this slot wants. Coercing that would unbox a value that is not boxed. */
  if (v < 0 || yield_site_type(c, v) != TY_POLY) return 0;
  /* int and string carry the implicit conversion protocol: this narrowing is
     the moment a boxed user object enters a typed slot, and reading it as 0 or
     as its #to_s rendering is the silent wrong answer. bool keeps the plain
     form -- an object in a bool slot is truthy, not a number. */
  /* The int slot needs nothing here: sp_poly_to_i carries the conversion
     protocol in its cold half, so a narrowing is correct for free. The string
     slot cannot -- sp_poly_to_s renders an object through #to_s, which is
     right for interpolation -- so it takes the protocol form, and only where
     the program defines a #to_str to reach: a narrowing lands wherever the
     analysis put it, including a hot loop, and the test is not free there.
     bool keeps the plain form: an object in a bool slot is truthy. */
  const char *fn = (slot == TY_INT || slot == TY_BOOL) ? "sp_poly_to_i"
                 : slot == TY_FLOAT  ? "sp_poly_to_f"
                 : slot == TY_STRING
                     ? (prog_has_conv_method(c, "to_str", TY_STRING) ? "sp_poly_arg_str" : "sp_poly_to_s") : NULL;
  if (!fn) return 0;
  buf_printf(b, "%s(", fn); emit_expr(c, v, b); buf_puts(b, ")");
  return 1;
}

/* Emit a shared-mutable string receiver for an operation that only READS its
   bytes: the live buffer, not the whole-buffer copy an ordinary value read
   makes (#3227). Answers 0 when the receiver is not such a slot, so the caller
   falls back to emit_expr. */
int emit_strbuf_read_ref(Compiler *c, int recv, Buf *b) {
  char sref[1024];
  int svm = c->strbuf_box[recv];
  c->strbuf_box[recv] = 1;
  int is_sb = strbuf_slot_ref(c, recv, sref, sizeof sref);
  c->strbuf_box[recv] = (unsigned char)svm;
  if (!is_sb) return 0;
  buf_printf(b, "sp_String_cstr(%s)", sref);
  return 1;
}
/* `cont[k]` where the container hands its elements out BOXED (a poly array, a
   hash): the read is an sp_RbVal, so a shared-handle destination has to unbox
   it rather than wrap it (#3941). */
int strbuf_boxed_elem_read(Compiler *c, int v) {
  if (!container_elem_read_p(c->nt, v)) return 0;
  int r = nt_ref(c->nt, v, "receiver");
  if (r < 0) return 0;
  TyKind rt = comp_ntype(c, r);
  return rt == TY_POLY || rt == TY_POLY_ARRAY || ty_is_hash(rt);
}
int strbuf_slot_ref(Compiler *c, int recv, char *out, size_t cap) {
  const char *rn = strbuf_local_name(c, recv);
  if (rn) {
    /* via emit_local_ref: a celled/captured local derefs its cell */
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_local_ref(c, recv, rn, &rb);
    snprintf(out, cap, "%s", rb.p ? rb.p : "");
    free(rb.p);
    return 1;
  }
  /* a demand-marked reader call typed as the handle (external reader
     mutation, e.g. `subs[0].topic << x`): the emitted read IS the sp_String*
     expression. Declined when it does not fit the caller's buffer (the
     branches then fall through to the value-form arms). */
  if (recv >= 0 && nt_kind(c->nt, recv) == NK_CallNode &&
      c->strbuf_box[recv] && comp_ntype(c, recv) == TY_STRBUF) {
    Buf rb2; memset(&rb2, 0, sizeof rb2);
    emit_expr(c, recv, &rb2);
    /* A container ELEMENT read comes back BOXED (a poly array element, a hash
       value), so the handle has to come out of the box; a reader call already
       emits the sp_String * itself (#3941). */
    int erecv = nt_ref(c->nt, recv, "receiver");
    const char *cnm = nt_str(c->nt, recv, "name");
    TyKind ert = erecv >= 0 ? comp_ntype(c, erecv) : TY_UNKNOWN;
    int boxed = cnm && sp_streq(cnm, "[]") &&
                (ert == TY_POLY || ert == TY_POLY_ARRAY || ty_is_hash(ert));
    int fit = rb2.p && strlen(rb2.p) + 24 <= cap;
    if (fit) snprintf(out, cap, boxed ? "sp_poly_as_strbuf(%s)" : "(%s)", rb2.p);
    free(rb2.p);
    return fit;
  }
  if (recv < 0 || nt_kind(c->nt, recv) != NK_InstanceVariableReadNode) return 0;
  const char *nm = nt_str(c->nt, recv, "name");
  if (!nm) return 0;
  int cid = strbuf_ivar_owner(c, recv);
  if (cid < 0) return 0;
  int iv = comp_ivar_index(&c->classes[cid], nm);
  if (iv < 0 || c->classes[cid].ivar_types[iv] != TY_STRBUF) return 0;
  Scope *cs = comp_scope_of(c, recv);
  if (cs && cs->class_id < 0)
    snprintf(out, cap, "civ_Toplevel_%s", nm + 1);
  else
    snprintf(out, cap, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
  return 1;
}
const char *rename_local(const char *nm) {
  /* Innermost first. A nested inline pushes its own locals above the caller's,
     and a same-named local belongs to the inner one -- scanning forward gave
     the parent's `yield a` the child's `a` in a super chain. The park
     mechanism bounds visibility by truncating g_nren, so a backward scan sees
     exactly the same entries. */
  for (int i = g_nren - 1; i >= 0; i--)
    if (sp_streq(g_ren_from[i], nm)) return g_ren_to[i];
  return nm;
}
/* Report a feature spinel deliberately does not support (docs/limitations.md).
   Unlike `unsupported`, which describes a codegen gap and dumps the node so the
   compiler can be debugged, this names the feature and stops: the internals are
   noise when the answer is "this is a documented limit". #2652 / #2667 / #2668 */
__attribute__((noreturn)) void unsupported_feature(Compiler *c, int id, const char *msg) {
  if (g_unsup_probe) longjmp(g_unsup_recover, 1);
  int ln = (int)nt_int(c->nt, id, "node_line", 0);
  char pos[1200]; pos[0] = 0;
  if (ln > 0) {
    int fid = (int)nt_int(c->nt, id, "node_file", 0);
    const char *file = nt_file_path(c->nt, fid);
    if (!file || !*file) file = c->nt->source_file;
    if (!file || !*file) file = "source.rb";
    snprintf(pos, sizeof pos, "%s:%d: ", file, ln);
  }
  fprintf(stderr, "spinel: %s%s\n", pos, msg);
  if (collect_mode() && g_unsup_armed) longjmp(g_unsup_recover, 1);
  exit(1);
}

__attribute__((noreturn)) void unsupported(Compiler *c, int id, const char *what) {
  /* Silent emittability probe (dynamic-send arm selection): unwind without a
     diagnostic, the caller just drops this arm. */
  if (g_unsup_probe) longjmp(g_unsup_recover, 1);
  const char *ty = nt_type(c->nt, id);
  /* Ruby-map the diagnostic (#1338): a codegen gap reports against the source
     line the parser stamped (the same position the #line machinery uses), so
     the message is anchored to the .rb file instead of an opaque node id.
     Falls back to the bare form when the position wasn't stamped. */
  int ln = (int)nt_int(c->nt, id, "node_line", 0);
  char pos[1200]; pos[0] = 0;
  if (ln > 0) {
    int fid = (int)nt_int(c->nt, id, "node_file", 0);
    const char *file = nt_file_path(c->nt, fid);
    if (!file || !*file) file = c->nt->source_file;
    if (!file || !*file) file = "source.rb";
    snprintf(pos, sizeof pos, "%s:%d: ", file, ln);
  }
  const char *mname = ty && sp_streq(ty, "CallNode") ? nt_str(c->nt, id, "name") : NULL;
  if (mname) {
    int recv = nt_ref(c->nt, id, "receiver");
    int args = nt_ref(c->nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(c->nt, args, "arguments", &ac) : NULL;
    /* A bare unresolved identifier (Prism variable-call: no receiver, no
       parens, no args) is CRuby's NameError -- an undefined local that fell
       through to method lookup. Name the enclosing class the way CRuby does. */
    if (recv < 0 && ac == 0 && nt_ref(c->nt, id, "block") < 0 &&
        nt_int(c->nt, id, "vcall", 0)) {
      const char *cn = g_emitting_class_id >= 0 ? class_ruby_name(c, g_emitting_class_id) : NULL;
      fprintf(stderr, "spinel: %sundefined local variable or method '%s' for %s%s (NameError)\n",
              pos, mname, cn ? "an instance of " : "main", cn ? cn : "");
      if (collect_mode() && g_unsup_armed) longjmp(g_unsup_recover, 1);
      exit(1);
    }
    /* A call on a typed user object whose class chain has no such method is
       not a compiler gap: it is the program's NoMethodError, caught ahead of
       time. Report it in CRuby's words instead of the internal node dump. */
    if (recv >= 0) {
      TyKind rvt = comp_ntype(c, recv);
      /* Same for a typed BUILTIN receiver: when CRuby's own surface for that
         class does not carry the name either, the call is the program's
         NoMethodError, proved at compile time rather than left for run time --
         so say so in CRuby's words. A name CRuby *does* have is a spinel gap
         and keeps the internal report (#3715). */
      {
        const char *bcn = rvt == TY_STRING || rvt == TY_STRBUF ? "String"
                        : rvt == TY_INT ? "Integer" : rvt == TY_FLOAT ? "Float"
                        : rvt == TY_SYMBOL ? "Symbol"
                        : rvt == TY_RANGE || rvt == TY_FLOAT_RANGE || rvt == TY_STR_RANGE ? "Range"
                        : rvt == TY_TIME ? "Time"
                        : ty_is_array(rvt) ? "Array" : ty_is_hash(rvt) ? "Hash" : NULL;
        if (bcn && !builtin_method_known(bcn, mname) &&
            !builtin_object_method_known(mname) &&
            !name_is_enumerable_module_method(mname) &&
            !an_user_defines_method(c, mname)) {
          fprintf(stderr, "spinel: %sundefined method '%s' for an instance of %s (NoMethodError)\n",
                  pos, mname, bcn);
          if (collect_mode() && g_unsup_armed) longjmp(g_unsup_recover, 1);
          exit(1);
        }
      }
      if (ty_is_object(rvt)) {
        int cid = ty_object_class(rvt);
        if (cid >= 0 && cid < c->nclasses && !c->classes[cid].is_native_class &&
            comp_method_in_chain(c, cid, mname, NULL) < 0) {
          const char *cn = class_ruby_name(c, cid);
          fprintf(stderr, "spinel: %sundefined method '%s' for an instance of %s (NoMethodError)\n",
                  pos, mname, cn ? cn : "Object");
          if (collect_mode() && g_unsup_armed) longjmp(g_unsup_recover, 1);
          exit(1);
        }
      }
    }
    fprintf(stderr, "spinel: %sunsupported %s: node %d (%s `%s`) recv=%s/ty%d argc=%d",
            pos, what, id, ty, mname,
            recv >= 0 ? nt_type(c->nt, recv) : "-",
            recv >= 0 ? (int)comp_ntype(c, recv) : -1, ac);
    if (ac > 0 && av) fprintf(stderr, " arg0ty%d", (int)comp_ntype(c, av[0]));
    fprintf(stderr, "\n");
  }
  else
    fprintf(stderr, "spinel: %sunsupported %s: node %d (%s)\n",
            pos, what, id, ty ? ty : "?");
  /* SP_COLLECT_ERRORS: don't abort on the first gap -- longjmp back to the
     driver's per-unit recovery point (when armed) so one run surfaces every
     unsupported construct, abandoning just this unit's (discarded) output.
     `unsupported` thus never returns: it exits, or longjmps. */
  if (collect_mode() && g_unsup_armed) longjmp(g_unsup_recover, 1);
  exit(1);
}

const char *c_type_name(TyKind t) {
  if (ty_is_obj_array(t)) return "sp_PtrArray *";
  switch (t) {
    case TY_INT:         return "sp_int";
    case TY_BIGINT:      return "sp_Bigint *";
    case TY_FLOAT:       return "sp_float";
    case TY_BOOL:        return "sp_bool";
    case TY_STRING:      return "const char *";
    case TY_SYMBOL:      return "sp_sym";
    case TY_RANGE:       return "sp_Range";
    case TY_FLOAT_RANGE: return "sp_FloatRange";
    case TY_STR_RANGE:   return "sp_StrRange";
    case TY_TIME:        return "sp_Time";
    case TY_COMPLEX:     return "sp_Complex";
    case TY_RATIONAL:    return "sp_Rational";
    case TY_MATCHDATA:   return "sp_MatchData *";
    case TY_REGEX:       return "mrb_regexp_pattern *";
    case TY_EXCEPTION:   return "sp_Exception *";
    case TY_STRBUF:      return "sp_String *";
    case TY_INT_ARRAY:   return "sp_IntArray *";
    case TY_FLOAT_ARRAY: return "sp_FloatArray *";
    case TY_STR_ARRAY:   return "sp_StrArray *";
    case TY_STR_INT_HASH: return "sp_StrIntHash *";
    case TY_STR_STR_HASH: return "sp_StrStrHash *";
    case TY_INT_INT_HASH: return "sp_IntIntHash *";
    case TY_INT_STR_HASH: return "sp_IntStrHash *";
    case TY_SYM_POLY_HASH:  return "sp_SymPolyHash *";
    case TY_STR_POLY_HASH:  return "sp_StrPolyHash *";
    case TY_POLY_POLY_HASH: return "sp_PolyPolyHash *";
    case TY_POLY:         return "sp_RbVal";
    case TY_POLY_ARRAY:   return "sp_PolyArray *";
    case TY_INT_ARRAY_ARRAY: return "sp_PtrArray *";
    case TY_PROC:         return "sp_Proc *";
    case TY_CURRY:        return "sp_Curry *";
    case TY_FIBER:        return "sp_Fiber *";
    case TY_THREAD:       return "sp_thread *";
    case TY_QUEUE:        return "sp_queue *";
    case TY_MUTEX:        return "sp_mutex *";
    case TY_CONDVAR:      return "sp_condvar *";
    case TY_RANDOM:       return "sp_Random *";
    case TY_DIR:          return "sp_Dir *";
    case TY_ADDRINFO:     return "sp_Addrinfo *";
    case TY_SOCKOPT:      return "sp_SockOpt *";
    case TY_TMS:          return "sp_Tms";
    case TY_OPENSTRUCT:   return "sp_OpenStruct *";
    case TY_METHOD:       return "sp_BoundMethod *";
    case TY_IO:           return "sp_File *";
    case TY_ARGF:         return "sp_Argf *";
    case TY_ENUMERATOR:   return "sp_Enumerator *";
    case TY_CLASS:        return "sp_Class";
    default:             return NULL;
  }
}
int is_scalar_ret(TyKind t) {
  return t == TY_INT || t == TY_BIGINT || t == TY_FLOAT || t == TY_BOOL || t == TY_STRING ||
         t == TY_SYMBOL || t == TY_RANGE || t == TY_FLOAT_RANGE || t == TY_STR_RANGE || t == TY_TIME || t == TY_COMPLEX || t == TY_RATIONAL || t == TY_MATCHDATA || t == TY_REGEX || t == TY_EXCEPTION ||
         t == TY_INT_ARRAY || t == TY_FLOAT_ARRAY || t == TY_STR_ARRAY || t == TY_INT_ARRAY_ARRAY ||
         t == TY_STRBUF ||
         t == TY_POLY || t == TY_POLY_ARRAY || t == TY_PROC || t == TY_CURRY || t == TY_FIBER || t == TY_THREAD || t == TY_QUEUE || t == TY_MUTEX || t == TY_CONDVAR || t == TY_RANDOM || t == TY_DIR || t == TY_ADDRINFO || t == TY_SOCKOPT || t == TY_METHOD || t == TY_IO || t == TY_ARGF || t == TY_ENUMERATOR || t == TY_CLASS || t == TY_OPENSTRUCT ||
         ty_is_hash(t) || ty_is_object(t) || ty_is_obj_array(t);
}
/* native binding (Path B): map a spinel type spec to the C type at the ABI
   boundary. any -> the boxed value; string -> the runtime string; scalars
   pass by value. */
/* Kinds whose C representation is a struct passed BY VALUE (see c_type_name).
   A struct never implicitly converts to or from anything else in C, so a
   parameter and an argument that disagree here can never be the same call --
   unlike the numeric scalars, where int-into-float is an ordinary conversion. */
int ty_is_struct_valued(TyKind t) {
  switch (t) {
    case TY_RANGE: case TY_FLOAT_RANGE: case TY_STR_RANGE:
    case TY_TIME: case TY_COMPLEX: case TY_RATIONAL:
    case TY_TMS: case TY_CLASS:
      return 1;
    default: return 0;
  }
}

const char *native_c_type(const char *spec) {
  if (!spec) return "void";
  if (sp_streq(spec, "any"))    return "sp_RbVal";
  if (sp_streq(spec, "ptr"))    return "void *";   /* raw pointer passthrough */
  if (sp_streq(spec, "string")) return "const char *";
  if (sp_streq(spec, "text"))   return "const char *";   /* write payload: the operand's #to_s */
  if (sp_streq(spec, "string?")) return "const char *";  /* nullable; call site wraps */
  if (sp_streq(spec, "nstring")) return "const char *";   /* NULL-able string, unboxed */
  if (sp_streq(spec, "cstring")) return "const char *";   /* borrowed C string; call site dups */
  if (sp_streq(spec, "cbinstr")) return "const char *";  /* borrowed C bytes; call site dups sp_ffi_bin_len of them */
  if (sp_streq(spec, "regexp")) return "mrb_regexp_pattern *";
  if (sp_streq(spec, "int"))    return "sp_int";
  if (sp_streq(spec, "float"))  return "double";
  if (sp_streq(spec, "bool"))   return "int";
  if (sp_streq(spec, "nil") || sp_streq(spec, "void")) return "void";
  return "sp_RbVal";
}

const char *ffi_c_type(const char *spec) {
  const FfiSpecInfo *info = ffi_spec_lookup(spec);
  return info ? info->c_type : "void";
}

/* The C type of one ffi_callback argument, used to build the trampoline's own
   pointer type. A :ptr callback arg is `const void*` -- the near-universal shape
   of C comparator/visitor callbacks (qsort, bsearch, ...) -- so the generated
   trampoline's type matches the header's declaration exactly (no
   incompatible-function-pointer error). */
const char *ffi_cb_arg_ctype(const char *spec) {
  if (sp_streq(spec, "ptr")) return "const void *";
  return ffi_c_type(spec);
}
/* Write into `out` the C test for "local `en` currently holds nil", and answer
   1. Answers 0 when the slot has no nil representation: every value the type
   can hold is truthy, and its zero is indistinguishable from a real one, so
   `x ||= v` on it genuinely is a no-op.

   The pointer-shaped kinds are exactly the ones declare_local initialises to
   NULL, so a slot still holding NULL has never been assigned -- which is nil,
   not "already truthy". Reading it as truthy is what dropped the assignment in
   `text ||= [...].join(" ")` (#3388). */
/* The initial value of a local's slot: its type's zero, or the type's nil
   sentinel when the local has no definite assignment anywhere (#3388). */
const char *local_init_value(Compiler *c, LocalVar *lv) {
  (void)c;
  if (lv->or_write_only && !lv->is_param && !lv->is_block_param) {
    const char *nv = nil_value(lv->type);
    if (nv) return nv;
  }
  return lv->type == TY_RANGE ? "(sp_Range){0}" : default_value(lv->type);
}
int local_nil_test(Compiler *c, LocalVar *lv, const char *ref, Buf *out) {
  if (!lv) return 0;
  TyKind t = lv->type;
  /* sp_int 0 and 0.0 are real values, so the slot only distinguishes nil when
     it was declared with the sentinel -- which declare_local does exactly when
     the local has no definite assignment anywhere. */
  int nil_init = lv->or_write_only && !lv->is_param && !lv->is_block_param;
  switch (t) {
    case TY_STRING: case TY_BIGINT: case TY_OPENSTRUCT:
      buf_printf(out, "!%s", ref); return 1;
    case TY_CLASS:
      buf_printf(out, "sp_class_nil_p(%s)", ref); return 1;
    case TY_INT:
      if (!nil_init) return 0;
      buf_printf(out, "%s == SP_INT_NIL", ref); return 1;
    case TY_FLOAT:
      if (!nil_init) return 0;
      buf_printf(out, "sp_float_is_nil(%s)", ref); return 1;
    /* value kinds with no in-band nil (and POLY/BOOL/SYMBOL, whose callers
       test their own sentinel before reaching here) */
    case TY_BOOL: case TY_SYMBOL: case TY_POLY:
    case TY_RANGE: case TY_FLOAT_RANGE: case TY_STR_RANGE: case TY_TIME:
    case TY_COMPLEX: case TY_RATIONAL: case TY_TMS:
      return 0;
    default:
      if (comp_ty_value_obj(c, t)) return 0;
      if (t != TY_UNKNOWN && is_scalar_ret(t)) { buf_printf(out, "!%s", ref); return 1; }
      return 0;
  }
}
/* The dead value closing a `({ ...; sp_raise_cls(...); V; })` arm. The raise
   never returns, so V only has to type-check in the slot: an UNKNOWN result
   flows as poly (default_value's "0" would not assign to sp_RbVal), and a
   Range wants its brace form. */
const char *raise_tail_value(TyKind t) {
  if (t == TY_UNKNOWN || t == TY_VOID) return "sp_box_nil()";
  return default_value(t);
}

/* Compiler-aware form: a by-value object class's C representation is a bare
   struct, where default_value's NULL would be ill-typed C. */
const char *raise_tail_value_c(Compiler *c, TyKind t) {
  if (ty_is_object(t) && comp_ty_value_obj(c, t)) {
    /* rotate: one static buffer would make two of these in a single
       buf_printf read the same text, and nothing in the signature says so */
    static char vbuf[4][128];
    static int vslot = 0;
    int cid = ty_object_class(t);
    if (cid >= 0 && cid < c->nclasses) {
      char *out = vbuf[vslot++ & 3];
      snprintf(out, sizeof vbuf[0], "((sp_%s){0})", c->classes[cid].c_name);
      return out;
    }
  }
  return raise_tail_value(t);
}

const char *default_value(TyKind t) {
  switch (t) {
    case TY_INT:    return "0";
    case TY_FLOAT:  return "0.0";
    case TY_BOOL:   return "0";
    case TY_STRING: return "(&(\"\\xff\")[1])";
    case TY_SYMBOL: return "((sp_sym)-1)";
    case TY_RANGE:  return "(sp_Range){0}";
    case TY_FLOAT_RANGE: return "(sp_FloatRange){0}";
    case TY_STR_RANGE:   return "(sp_StrRange){0}";
    case TY_TIME:   return "(sp_Time){0}";
    case TY_COMPLEX: return "(sp_Complex){0}";
    case TY_RATIONAL: return "(sp_Rational){0}";
    case TY_MATCHDATA:  return "NULL";
    case TY_REGEX:      return "NULL";
    case TY_EXCEPTION: return "NULL";
    case TY_STRBUF:    return "NULL";
    case TY_INT_ARRAY:
    case TY_FLOAT_ARRAY:
    case TY_STR_ARRAY:
    case TY_POLY_ARRAY:
    case TY_INT_ARRAY_ARRAY: return "NULL";
    case TY_PROC:    return "NULL";
    case TY_CURRY:   return "NULL";
    case TY_FIBER:   return "NULL";
    case TY_THREAD:  return "NULL";
    case TY_QUEUE:   return "NULL";
    case TY_MUTEX:   return "NULL";
    case TY_CONDVAR: return "NULL";
    case TY_RANDOM:  return "NULL";
    case TY_DIR:     return "NULL";
    case TY_ADDRINFO: return "NULL";
    case TY_SOCKOPT: return "NULL";
    case TY_TMS:     return "((sp_Tms){0})";
    case TY_OPENSTRUCT: return "NULL";
    case TY_METHOD:  return "NULL";
    case TY_IO:      return "NULL";
    case TY_ARGF:    return "NULL";
    case TY_ENUMERATOR: return "NULL";
    case TY_POLY:    return "sp_box_nil()";
    case TY_CLASS:   return "((sp_Class){-1})";
    default:        return (ty_is_hash(t) || ty_is_object(t) || ty_is_obj_array(t)) ? "NULL" : "0";
  }
}
/* The C type of class `cid`'s instances. A `native_struct` carries the name
   its declaration gave -- which need not be derived from the Ruby class name
   (`native_struct "Store", "sp_X509_Store"`) -- and every other class is the
   `sp_<c_name>` struct the generator defines for it. Four rotating buffers so
   one format string can name two classes. */
const char *class_ctype(Compiler *c, int cid) {
  static char bufs[4][160];
  static int turn = 0;
  if (cid < 0 || cid >= c->nclasses) return "void";
  ClassInfo *ci = &c->classes[cid];
  if (ci->is_native_class && ci->c_struct) return ci->c_struct;
  char *out = bufs[turn++ & 3];
  snprintf(out, sizeof bufs[0], "sp_%s", ci->c_name ? ci->c_name : "");
  return out;
}
void emit_ctype(Compiler *c, TyKind t, Buf *b) {
  if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    /* value-type classes are stored inline (sp_X); others are heap pointers */
    buf_printf(b, "%s %s", class_ctype(c, cid), c->classes[cid].is_value_type ? "" : "*");
  }
  else {
    const char *n = c_type_name(t);
    buf_puts(b, n ? n : "void");
  }
}
void emit_box_open(Compiler *c, TyKind t, Buf *b) {
  if (t == TY_INT)          buf_puts(b, "sp_box_int(");
  else if (t == TY_STRING)  buf_puts(b, "sp_box_str(");
  else if (t == TY_FLOAT)   buf_puts(b, "sp_box_float(");
  else if (t == TY_BOOL)    buf_puts(b, "sp_box_bool(");
  else if (t == TY_NIL)     buf_puts(b, "sp_box_nil(); (void)(");
  else if (t == TY_SYMBOL)  buf_puts(b, "sp_box_sym(");
  /* Array slots are nilable C pointers (a nil-defaulting param, `[x] if cond`
     in value position): box NULL as a proper nil, not a truthy OBJ wrapping
     NULL that passes truthy checks and then segfaults on the first access
     (#3275). Matches emit_boxed_text's array cases. */
  else if (t == TY_INT_ARRAY)   buf_puts(b, "sp_box_nullable_obj((void *)(");
  else if (t == TY_FLOAT_ARRAY) buf_puts(b, "sp_box_nullable_obj((void *)(");
  else if (t == TY_STR_ARRAY)   buf_puts(b, "sp_box_nullable_obj((void *)(");
  else if (t == TY_POLY_ARRAY)  buf_puts(b, "sp_box_nullable_obj((void *)(");
  else if (t == TY_CLASS) buf_puts(b, "sp_box_class(");
  else if (t == TY_COMPLEX)  buf_puts(b, "sp_box_complex(");
  else if (t == TY_RATIONAL) buf_puts(b, "sp_box_rational(");
  /* Reference-backed builtins are nilable C pointers: box NULL as nil. */
  else if (ty_nullable_builtin_id(t)) buf_puts(b, "sp_box_nullable_obj((void *)(");
  else if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    /* the struct typedef is sp_<c_name>; a bare `(<Name> *)` would never
       have compiled, so this arm was effectively unreachable as written */
    buf_printf(b, "sp_box_obj((%s *)( ", class_ctype(c, cid));
  }
  /* TY_POLY: already sp_RbVal, no prefix */
}
void emit_box_close(Compiler *c, TyKind t, Buf *b) {
  (void)c;
  if (t == TY_POLY || t == TY_UNKNOWN) return; /* no-op: already sp_RbVal */
  { const char *nbid = ty_nullable_builtin_id(t);
    if (nbid) { buf_printf(b, "), %s)", nbid); return; } }
  if (ty_is_object(t))        { buf_printf(b, "), %d)", ty_object_class(t)); return; }
  /* array open used sp_box_nullable_obj((void *)( ... -- close with the kind. */
  if (t == TY_INT_ARRAY)   { buf_puts(b, "), SP_BUILTIN_INT_ARRAY)"); return; }
  if (t == TY_FLOAT_ARRAY) { buf_puts(b, "), SP_BUILTIN_FLT_ARRAY)"); return; }
  if (t == TY_STR_ARRAY)   { buf_puts(b, "), SP_BUILTIN_STR_ARRAY)"); return; }
  if (t == TY_POLY_ARRAY)  { buf_puts(b, "), SP_BUILTIN_POLY_ARRAY)"); return; }
  buf_puts(b, ")");
}
const char *array_kind(TyKind t) {
  switch (t) {
    case TY_INT_ARRAY:   return "Int";
    case TY_FLOAT_ARRAY: return "Float";
    case TY_STR_ARRAY:   return "Str";
    default:             return NULL;
  }
}
void emit_c_escaped_n(Buf *b, const char *s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch == '\\' || ch == '"') buf_printf(b, "\\%c", ch);
    else if (ch == '\n') buf_puts(b, "\\n");
    else if (ch == '\t') buf_puts(b, "\\t");
    else if (ch == '\r') buf_puts(b, "\\r");
    else if (ch >= 0x20 && ch < 0x7f) buf_printf(b, "%c", ch);
    else buf_printf(b, "\\%03o", ch);
  }
}
void emit_c_escaped(Buf *b, const char *s) {
  if (s) emit_c_escaped_n(b, s, strlen(s));
}
/* Marker byte before string-literal data: 0xff is an immutable rodata literal
   (frozen? false, value semantics); `frozen` -- set from the node's `fzl` flag
   when its file has `# frozen_string_literal: true` -- switches to 0xf1 so
   `frozen?` is true and mutation raises FrozenError. Synthesized strings
   (symbol names, ivar names, ...) go through emit_str_literal, which is never
   frozen: the pragma only affects literals written in the source. */
/* Open/close a static frozen-literal object around caller-streamed escaped
   bytes. The 0xf1 marker promises a REAL sp_str_hdr immediately in front of
   the data (hash cache, mutation guards), so every frozen literal -- single
   or an adjacent-literal fold -- must carry this header (#1749). */
int emit_frozen_literal_open(Buf *b, size_t raw_len) {
  static int g_fzl_ctr = 0;
  int id = g_fzl_ctr++;
  size_t dl = raw_len + 1;
  buf_printf(b, "({ static struct { sp_str_hdr h; unsigned char m; char d[%zu]; } _fzl_%d = "
                "{ { NULL, %zu, %zu, 0 }, 0xf1, \"", dl, id, dl, raw_len);
  return id;
}
void emit_frozen_literal_close(Buf *b, int id) {
  buf_printf(b, "\" }; _fzl_%d.d; })", id);
}
void emit_str_literal_n(Buf *b, const char *content, size_t len, int frozen) {
  const char *mk = frozen ? "\\xf1" : "\\xff";
  /* A frozen literal must carry a REAL sp_str_hdr: the 0xf1 marker promises
     one immediately in front of the data (sp_str_hash caches the FNV hash
     through it, mutation guards and frozen? key off the marker). Baking the
     marker onto a bare rodata literal made that header read/write land in
     whatever rodata precedes the literal -- a garbage cached hash, so a
     Hash#[] with the literal key missed entries whose equal-content keys
     were built at runtime (#1749; ASAN: global-buffer-overflow). Emit a
     static header+marker+data object instead: the layout matches a heap
     string exactly (hdr | marker | bytes), the hash cache write hits our
     own static storage, and next=NULL keeps it off the sweep list. */
  if (frozen) {
    int id = emit_frozen_literal_open(b, content ? len : 0);
    if (content && len) emit_c_escaped_n(b, content, len);
    emit_frozen_literal_close(b, id);
    return;
  }
  if (!content || len == 0) { buf_printf(b, "(&(\"%s\")[1])", mk); return; }
  /* NUL-containing strings: use sp_str_from_bytes with explicit byte count.
     The heap string it builds is writable (0xfe), so a frozen literal is
     sealed with sp_str_freeze_val (flips the heap marker to 0xf1 in place). */
  if (len > strlen(content)) {
    /* A FROZEN NUL-containing literal is immortal (sp_str_sweep never frees a
       0xf1 string), so build it once into a call-site-local static and reuse it.
       This avoids re-allocating it on every evaluation -- which, besides the
       churn, made the literal a GC-triggering sibling that could sweep an
       unrooted operand mid-expression (e.g. the receiver in
       `data[8, 8].delete("\0")`, a use-after-free in doom's WAD name parse). */
    if (frozen) {
      static int g_binlit_ctr = 0;
      int id = g_binlit_ctr++;
      buf_printf(b, "({ static const char *_binlit_%d; _binlit_%d ? _binlit_%d : "
                    "(_binlit_%d = sp_str_freeze_val(sp_str_from_bytes(\"", id, id, id, id);
      emit_c_escaped_n(b, content, len);
      buf_printf(b, "\", %zu))); })", len);
      return;
    }
    buf_puts(b, "sp_str_from_bytes(\"");
    emit_c_escaped_n(b, content, len);
    buf_printf(b, "\", %zu)", len);
    return;
  }
  buf_printf(b, "(&(\"%s\" \"", mk);
  emit_c_escaped_n(b, content, len);
  buf_puts(b, "\")[1])");
}
void emit_str_literal(Buf *b, const char *content) {
  if (!content) { buf_puts(b, "(&(\"\\xff\")[1])"); return; }
  emit_str_literal_n(b, content, strlen(content), 0);
}
/* Ruby-source string literal (a StringNode): unlike the internal-constant
   emitter above, each OCCURRENCE gets its own static array, so two textually
   equal literals are two distinct objects and `equal?` (pointer identity)
   answers like CRuby. The plain `("\xff" "abc")` form let the C compiler merge
   equal literals into one address, making `"abc".equal?("abc")` true. A
   re-evaluated occurrence (a literal in a loop) still yields one address --
   the frozen-string-literal semantics spinel's immutable strings already have.
   Frozen and NUL-containing literals keep their existing per-site forms
   (_fzl_N / sp_str_from_bytes), which are already identity-correct. */
void emit_str_literal_src(Buf *b, const char *content, size_t len, int frozen) {
  static int g_slit_ctr = 0;
  if (frozen || (content && len > strlen(content))) {
    emit_str_literal_n(b, content, len, frozen);
    return;
  }
  int lid = g_slit_ctr++;
  buf_printf(b, "({ static const char _slit_%d[] = \"\\xff\" \"", lid);
  if (content && len) emit_c_escaped_n(b, content, len);
  buf_printf(b, "\"; &_slit_%d[1]; })", lid);
}
/* Emit a catch/throw tag expression; returns the tag KIND (0 = name tag
   matched by content, 1 = object tag matched by pointer identity). */
int emit_catch_tag(Compiler *c, int id, Buf *b) {
  const char *ty = nt_type(c->nt, id);
  if (ty && sp_streq(ty, "SymbolNode")) { emit_str_literal(b, nt_str(c->nt, id, "value")); return 0; }
  if (ty && sp_streq(ty, "StringNode")) { emit_str_literal(b, nt_str(c->nt, id, "unescaped")); return 0; }
  TyKind t = comp_ntype(c, id);
  if (t == TY_SYMBOL) {
    buf_puts(b, "sp_sym_to_s("); emit_expr(c, id, b); buf_puts(b, ")");
    return 0;
  }
  if (ty_is_object(t)) {
    /* a non-symbol object tag matches by identity: carry its pointer */
    buf_puts(b, "(const char *)(void *)("); emit_expr(c, id, b); buf_puts(b, ")");
    return 1;
  }
  if (t == TY_POLY) {
    /* boxed object tag: identity via the boxed pointer */
    buf_puts(b, "(const char *)("); emit_expr(c, id, b); buf_puts(b, ").v.p");
    return 1;
  }
  if (t == TY_STRING) {
    /* a dynamic string tag: a valid pointer, matched by content (like the
       StringNode-literal arm above) */
    emit_expr(c, id, b);
    return 0;
  }
  if (t == TY_INT) {
    /* an Integer tag: CRuby matches by identity, which for a Fixnum is value
       equality -- carry the value in the pointer slot and match by identity. */
    buf_puts(b, "(const char *)(intptr_t)("); emit_expr(c, id, b); buf_puts(b, ")");
    return 1;
  }
  /* A Float/boolean/nil/Bignum tag would emit a non-pointer (or a struct)
     into the const char* tag slot -- invalid C, and (0/1 truncation) a wrong
     match. These are vanishingly rare as catch/throw tags; reject loudly
     rather than miscompile. */
  unsupported(c, id, "catch/throw with a Float, boolean, nil, or Bignum tag (use a Symbol, String, Integer, or object tag)");
  return 0;
}
void emit_hash_key(Compiler *c, int key, TyKind kt, Buf *b) {
  TyKind actual = comp_ntype(c, key);
  /* A symbol key on a string-keyed hash (Hash.new{}'s StrPolyHash models
     symbol keys by their name) coerces to the symbol's string. A literal
     :sym becomes the name string directly; a symbol value uses sp_sym_to_s. */
  if (kt == TY_STRING && actual == TY_SYMBOL) {
    const char *kty = nt_type(c->nt, key);
    if (kty && sp_streq(kty, "SymbolNode")) {
      emit_str_literal(b, nt_str(c->nt, key, "value"));
    }
else {
      buf_puts(b, "sp_sym_to_s("); emit_expr(c, key, b); buf_puts(b, ")");
    }
    return;
  }
  if (actual == TY_POLY && kt != TY_POLY) {
    /* The union member is only valid when the tag agrees. A call site reached
       with a key of another kind -- the same method called with a String and
       with a Float -- read a Float's bits as a `const char *` and dereferenced
       them (#3810). A key of the wrong kind is simply not in the table, so
       answer a value no key can equal and let the lookup miss. */
    buf_puts(b, "({ sp_RbVal _hk = ");
    emit_boxed(c, key, b);
    if (kt == TY_STRING)      buf_puts(b, "; _hk.tag == SP_TAG_STR ? _hk.v.s : (const char *)0; })");
    else if (kt == TY_SYMBOL) buf_puts(b, "; _hk.tag == SP_TAG_SYM ? (sp_sym)_hk.v.i : (sp_sym)-1; })");
    else                      buf_puts(b, "; _hk.tag == SP_TAG_INT ? _hk.v.i : SP_INT_NIL; })");
    return;
  }
  if (kt == TY_POLY && actual != TY_POLY) {
    /* PolyPolyHash key: box the typed value into sp_RbVal */
    emit_boxed(c, key, b);
    return;
  }
  emit_expr(c, key, b);
}
int unwrap_parens(Compiler *c, int id) {
  while (id >= 0) {
    const char *ty = nt_type(c->nt, id);
    if (!ty || !sp_streq(ty, "ParenthesesNode")) break;
    int body = nt_ref(c->nt, id, "body");
    int n = 0;
    const int *bd = body >= 0 ? nt_arr(c->nt, body, "body", &n) : NULL;
    if (n != 1) break;
    id = bd[0];
  }
  return id;
}

/* 1 when the receiver is a range whose begin endpoint is statically a Float
   -- directly a (possibly parenthesized) RangeNode or through a
   sole-assignment local. CRuby raises TypeError "can't iterate from Float"
   when enumerating such a range (an int begin with a float end iterates
   fine); the int-backed sp_Range would otherwise silently truncate. */
int range_float_begin(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  int rn = unwrap_parens(c, recv);
  if (rn < 0) return 0;
  const char *rty = nt_type(nt, rn);
  if (!rty || !sp_streq(rty, "RangeNode")) {
    rn = local_sole_range_node(c, rn);
    if (rn < 0) return 0;
  }
  int lo = nt_ref(nt, rn, "left");
  return lo >= 0 && comp_ntype(c, lo) == TY_FLOAT;
}
const char *int_arith_fn(const char *op) {
  if (sp_streq(op, "+"))  return "sp_int_add";
  if (sp_streq(op, "-"))  return "sp_int_sub";
  if (sp_streq(op, "*"))  return "sp_int_mul";
  if (sp_streq(op, "/"))  return "sp_idiv";
  if (sp_streq(op, "%"))  return "sp_imod";
  if (sp_streq(op, "**")) return "sp_int_pow";
  return NULL;
}
const char *bigint_arith_fn(const char *op) {
  if (sp_streq(op, "+"))  return "sp_bigint_add";
  if (sp_streq(op, "-"))  return "sp_bigint_sub";
  if (sp_streq(op, "*"))  return "sp_bigint_mul";
  if (sp_streq(op, "/"))  return "sp_bigint_div";
  if (sp_streq(op, "%"))  return "sp_bigint_mod";
  if (sp_streq(op, "&"))  return "sp_bigint_and";
  if (sp_streq(op, "|"))  return "sp_bigint_or";
  if (sp_streq(op, "^"))  return "sp_bigint_xor";
  return NULL;
}
/* True if any user exception subclass overrides #message or #to_s, so the
   default exception message/to_s path must dispatch to the user method rather
   than reporting the stored message (which defaults to the class name). */
int exc_has_user_msg_override(Compiler *c) {
  for (int i = 0; i < c->nclasses; i++) {
    if (!class_is_exc_subclass(c, i)) continue;
    /* Only a string-returning override is dispatched (see codegen_program),
       so gate on TY_STRING to match -- otherwise a non-string override would
       route every exception query through an empty dispatcher for nothing. */
    int mi_msg = comp_method_in_chain(c, i, "message", NULL);
    if (mi_msg >= 0 && (TyKind)c->scopes[mi_msg].ret == TY_STRING)
      return 1;
    int mi_tos = comp_method_in_chain(c, i, "to_s", NULL);
    if (mi_tos >= 0 && (TyKind)c->scopes[mi_tos].ret == TY_STRING)
      return 1;
  }
  return 0;
}

/* An override that answers something other than a String: Exception#message
   is #to_s, so such a class carries a non-string value out of #message and the
   const char * dispatchers cannot represent it. A boxed pair of dispatchers is
   emitted instead, and the call sites type as poly (#3868). */
int exc_has_nonstring_msg_override(Compiler *c) {
  for (int i = 0; i < c->nclasses; i++) {
    if (!class_is_exc_subclass(c, i)) continue;
    for (int k = 0; k < 2; k++) {
      int mi = comp_method_in_chain(c, i, k ? "to_s" : "message", NULL);
      if (mi < 0) continue;
      TyKind rk = (TyKind)c->scopes[mi].ret;
      if (rk != TY_STRING && rk != TY_UNKNOWN) return 1;
    }
  }
  return 0;
}

/* The class an index-taking Array method was handed instead of an index, or
   NULL when the argument can serve as one. Ruby converts a Float through
   #to_int and takes a Range where the method has a slice form; a String,
   Symbol, Array, Hash, nil or boolean is a TypeError. A poly argument stays on
   the runtime path: its class is not settled here (#3923, #3924, #3925). */
const char *array_index_bad_class(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int ir = nt_ref(nt, id, "receiver");
  const char *inm = nt_str(nt, id, "name");
  int ia = nt_ref(nt, id, "arguments");
  int ic = 0; const int *iv = ia >= 0 ? nt_arr(nt, ia, "arguments", &ic) : NULL;
  TyKind irt = ir >= 0 ? comp_ntype(c, ir) : TY_UNKNOWN;
  if (ir < 0 || !inm || ic < 1 || !iv || iv[0] < 0) return NULL;
  if (!(ty_is_array(irt) || ty_is_obj_array(irt))) return NULL;
  if (user_defines_or_reads(c, inm)) return NULL;
  static const char *const IDX[] = { "at", "fetch", "first", "last", "take",
                                     "drop", "insert", "dig", "values_at",
                                     "rotate", "[]", "slice", "[]=", NULL };
  static const char *const SLICE_OK[] = { "[]", "slice", "[]=", "values_at", NULL };
  int is_idx = 0, range_ok = 0;
  for (int k = 0; IDX[k]; k++) if (sp_streq(inm, IDX[k])) { is_idx = 1; break; }
  if (!is_idx) return NULL;
  for (int k = 0; SLICE_OK[k]; k++) if (sp_streq(inm, SLICE_OK[k])) { range_ok = 1; break; }
  TyKind at4 = comp_ntype(c, iv[0]);
  if (at4 == TY_STRING || at4 == TY_STRBUF) return "String";
  if (at4 == TY_SYMBOL) return "Symbol";
  if (ty_is_array(at4) || ty_is_obj_array(at4)) return "Array";
  if (ty_is_hash(at4)) return "Hash";
  if (at4 == TY_NIL) return "nil";
  if (at4 == TY_BOOL) return "Boolean";
  if ((at4 == TY_RANGE || at4 == TY_FLOAT_RANGE || at4 == TY_STR_RANGE) && !range_ok)
    return "Range";
  return NULL;
}
const char *mc(const char *name) {
  static char buf[256];
  int j = 0;
  for (const char *p = name; *p && j < (int)sizeof buf - 8; p++) {
    char ch = *p;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_') { buf[j++] = ch; continue; }
    /* operator characters map to distinct tokens so that, e.g., `&` and `|`
       (or `<<` and `>>`) don't mangle to the same C identifier */
    const char *tok;
    switch (ch) {
      case '?': tok = "_p";     break;
      case '!': tok = "_bang";  break;
      case '=': tok = "_set";   break;
      case '+': tok = "_plus";  break;
      case '-': tok = "_minus"; break;
      case '*': tok = "_star";  break;
      case '/': tok = "_slash"; break;
      case '%': tok = "_pct";   break;
      case '<': tok = "_lt";    break;
      case '>': tok = "_gt";    break;
      case '&': tok = "_amp";   break;
      case '|': tok = "_bar";   break;
      case '^': tok = "_caret"; break;
      case '~': tok = "_tilde"; break;
      case '@': tok = "_at";    break;
      case '[': tok = "_lb";    break;
      case ']': tok = "_rb";    break;
      default:  tok = "_";      break;
    }
    size_t tl = strlen(tok);
    memcpy(buf + j, tok, tl); j += (int)tl;
  }
  buf[j] = '\0';
  return buf;
}

/* The names a top-level Ruby method must not take: generated from the runtime
   sources by the Makefile (build/csrc/sp_rt_names.h), because the set is a fact
   about those sources and a hand-kept copy of it goes stale -- `def gcd` and
   `def gets` collided with sp_gcd and sp_gets while the list said nothing. */
#include "sp_rt_names.h"

/* The mangled name of a top-level method: `mc(name)`, with an `rb_` infix when
   the plain form would sit in the runtime's own namespace.

   The segment compared is the whole name when it carries no underscore. A
   prefix names both a runtime NAMESPACE (sp_sym_intern, sp_int_add) and, for
   about a dozen of them, a runtime IDENTIFIER of its own -- the typedefs
   sp_sym and sp_int, sp_raise, sp_thread. Looking only in front of an
   underscore protected the namespace and left the identifier open, so `def
   sym` collided with the typedef and the program failed to build on a C
   diagnostic that never mentions the name the author wrote. */
const char *mc_top(Compiler *c, const char *name) {
  static char buf[272];
  const char *m = mc(name);
  /* A class or module of the same name owns sp_<Name> as its typedef, and
     `def Foo` beside `class Foo` is ordinary Ruby -- URI(), Integer(),
     Array() are all a method sharing a name with a type. Without this the two
     collide and the program fails to build on a C diagnostic that never
     mentions either. Same remedy as the runtime clash below. */
  if (c && comp_class_index(c, name) >= 0) {
    snprintf(buf, sizeof buf, "rb_%s", m);
    return buf;
  }
  const char *us = strchr(m, '_');
  size_t seg = (us && us > m) ? (size_t)(us - m) : strlen(m);
  if (seg > 0) {
    for (int i = 0; SP_RT_PREFIXES[i]; i++) {
      if (strlen(SP_RT_PREFIXES[i]) != seg) continue;
      if (strncmp(m, SP_RT_PREFIXES[i], seg) != 0) continue;
      snprintf(buf, sizeof buf, "rb_%s", m);
      return buf;
    }
  }
  return m;
}
/* Mangle an ivar/struct-member name (sans leading '@') to a valid C field
   identifier, so a Struct/Data member like `verbose?` becomes iv_verbose_p
   rather than the illegal iv_verbose? (#3110). Same character map as mc(), but
   a rotating buffer ring so the 2-4 uses of a field name on one buf_printf line
   don't clobber each other. A no-op for the usual all-identifier ivar names. */
const char *iv_c(const char *name) {
  static char bufs[8][256];
  static int which = 0;
  char *buf = bufs[(which++) & 7];
  int j = 0;
  for (const char *p = name; *p && j < (int)sizeof bufs[0] - 8; p++) {
    char ch = *p;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_') { buf[j++] = ch; continue; }
    const char *tok;
    switch (ch) {
      case '?': tok = "_p";     break;
      case '!': tok = "_bang";  break;
      case '=': tok = "_set";   break;
      default:  tok = "_";      break;
    }
    size_t tl = strlen(tok);
    memcpy(buf + j, tok, tl); j += (int)tl;
  }
  buf[j] = '\0';
  return buf;
}
int scope_is_shadowed(Compiler *c, int s) {
  Scope *sc = &c->scopes[s];
  if (sc->class_id < 0 || !sc->name) return 0;
  for (int k = s + 1; k < c->nscopes; k++) {
    Scope *o = &c->scopes[k];
    if (o->class_id == sc->class_id && o->is_cmethod == sc->is_cmethod &&
        o->name && sp_streq(o->name, sc->name)) return 1;
  }
  return 0;
}
/* True when scope `s` is emitted as a standalone `sp_Class_method` function, so
   a poly-dispatch `case` arm may call it without dangling at link. Mirrors the
   emission gate in codegen.c exactly: a yielding method is inlined at each call
   site (no symbol exists), and a pruned/shadowed/transplanted method is never
   defined. A dispatch arm that targets a scope failing this test references an
   undefined symbol (issues #1583 yields, #1576 pruned). */
/* ---- proc-form emission for yielding methods (#3399) ----
   A yielding method has no symbol: it is inlined at each call site with the
   block spliced in. A poly dispatch has no call site to splice into, so for the
   methods it names we emit a SECOND definition -- an ordinary function taking
   the block as an sp_Proc * -- and point the dispatch at that. Marked during
   dispatch emission (scope_mark_proc_form), emitted afterwards.

   The emission reuses the existing non-yielding shape rather than adding a
   mode: with `yields` cleared the signature already grows the sp_Proc* param
   and roots it, and with g_yield_proc_ref set every `yield` in the body already
   lowers to a call on that proc. begin/end swap those in and back. */
static char **g_pf_flag = NULL;
static int g_pf_cap = 0;
static char g_pf_synth[SP_MAX_PROC_FORM][32];

void scope_mark_proc_form(Compiler *c, int s) {
  if (s < 0 || s >= c->nscopes) return;
  if (!g_pf_flag || g_pf_cap < c->nscopes) {
    char **n = (char **)realloc(g_pf_flag, sizeof(char *) * (size_t)c->nscopes);
    if (!n) return;
    for (int i = g_pf_cap; i < c->nscopes; i++) n[i] = NULL;
    g_pf_flag = n; g_pf_cap = c->nscopes;
  }
  if (g_pf_flag[s] != (char *)2) g_pf_flag[s] = (char *)1;
}
void scope_veto_proc_form(Compiler *c, int s) {
  if (!g_pf_flag || s < 0 || s >= g_pf_cap || s >= c->nscopes) return;
  g_pf_flag[s] = (char *)2;   /* sticky: a later marking pass must not revive it */
}
/* The proc-form clone of scope `s`, or -1. Made in analyze (make_yield_proc_forms):
   a second scope named "<name>#pf" on the same class, holding an independently
   typed copy of the body whose yields answer poly. */
int scope_proc_form_of(Compiler *c, int s) {
  if (s < 0 || s >= c->nscopes) return -1;
  Scope *sc = &c->scopes[s];
  if (!sc->name || sc->class_id < 0 || !sc->yields) return -1;
  char pfname[192];
  snprintf(pfname, sizeof pfname, "%s#pf", sc->name);
  int pi = comp_method_in_class(c, sc->class_id, pfname);
  if (pi < 0 || !c->scopes[pi].is_proc_form) return -1;
  return pi;
}
int scope_needs_proc_form(Compiler *c, int s) {
  return scope_proc_form_of(c, s) >= 0;
}
static int g_pf_saved_yields;
static char *g_pf_saved_blk;
static const char *g_pf_saved_ypr;
static TyKind g_pf_saved_slot;
static TyKind g_pf_saved_ret;
static char g_pf_ref[64];
int g_pf_emitting = 0;
void scope_proc_form_begin(Compiler *c, int s) {
  Scope *sc = &c->scopes[s];
  g_pf_saved_yields = sc->yields;
  g_pf_saved_blk = sc->blk_param;
  g_pf_saved_ypr = g_yield_proc_ref;
  g_pf_saved_slot = g_yield_slot_ty;
  if (!sc->blk_param || !sc->blk_param[0]) {
    /* a bare `yield` names no block: give the parameter a name of its own */
    int idx = s < SP_MAX_PROC_FORM ? s : 0;
    snprintf(g_pf_synth[idx], sizeof g_pf_synth[0], "__pf_blk");
    sc->blk_param = g_pf_synth[idx];
  }
  sc->yields = 0;
  snprintf(g_pf_ref, sizeof g_pf_ref, "lv_%s", sc->blk_param);
  g_yield_proc_ref = g_pf_ref;
  /* The proc form returns POLY, and it has to: the same yielding method
     inlined at two call sites produces two different C types (an sp_int at
     one, a const char * at the other), because each site is monomorphised
     with its own block. One shared function cannot carry a per-call-site
     return type, so it carries the boxed one and the dispatch unboxes into
     its slot. sp_proc_call already answers through _sp_proc_poly_ret, so
     asking for TY_POLY here is what makes each yield yield a value at all --
     with the method's own (void, since it never needed one) the result was
     computed and dropped. */
  g_pf_saved_ret = sc->ret;
  sc->ret = TY_POLY;
  g_yield_slot_ty = TY_POLY;
  g_pf_emitting = 1;
}
void scope_proc_form_end(Compiler *c, int s) {
  Scope *sc = &c->scopes[s];
  sc->yields = g_pf_saved_yields;
  sc->blk_param = g_pf_saved_blk;
  sc->ret = g_pf_saved_ret;
  g_yield_proc_ref = g_pf_saved_ypr;
  g_yield_slot_ty = g_pf_saved_slot;
  g_pf_emitting = 0;
}
/* A module whose instance methods a TOP-LEVEL `include` makes callable: the
   bare-call path emits a direct call to the module's own function, so that
   function has to exist even though including the module into a class also
   copied it away (#3795). */
int scope_toplevel_included(Compiler *c, int s) {
  if (s < 0 || s >= c->nscopes) return 0;
  Scope *sc = &c->scopes[s];
  if (sc->is_cmethod || sc->class_id < 0) return 0;
  for (int i = 0; i < c->ntoplevel_includes; i++)
    if (c->toplevel_includes[i] == sc->class_id) return 1;
  return 0;
}

int scope_has_callable_symbol(Compiler *c, int s) {
  if (s < 0 || s >= c->nscopes) return 0;
  Scope *sc = &c->scopes[s];
  return sc->reachable && !sc->yields &&
         (!sc->is_transplanted_source || scope_toplevel_included(c, s)) &&
         !scope_is_shadowed(c, s);
}
int struct_kwarg_value(Compiler *c, int kwh, const char *name) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *els = nt_arr(nt, kwh, "elements", &n);
  for (int i = 0; i < n; i++) {
    if (!nt_type(nt, els[i]) || !sp_streq(nt_type(nt, els[i]), "AssocNode")) continue;
    int key = nt_ref(nt, els[i], "key");
    if (key >= 0 && nt_type(nt, key) && sp_streq(nt_type(nt, key), "SymbolNode")) {
      const char *kn = nt_str(nt, key, "value");
      if (kn && sp_streq(kn, name)) return nt_ref(nt, els[i], "value");
    }
  }
  return -1;
}
int eq_family(TyKind t) {
  if (ty_is_numeric(t)) return 1;
  if (t == TY_STRING) return 2;
  if (t == TY_BOOL) return 3;
  if (t == TY_SYMBOL) return 4;
  if (t == TY_RANGE) return 5;
  if (t == TY_FLOAT_RANGE) return 6;
  if (t == TY_STR_RANGE) return 7;
  return 0;
}
int ty_matches_class(TyKind t, const char *cn, int exact) {
  const char *self_cls = NULL;
  if (t == TY_STRING || t == TY_STRBUF) self_cls = "String";
  else if (t == TY_INT || t == TY_BIGINT) self_cls = "Integer";
  else if (t == TY_FLOAT) self_cls = "Float";
  else if (t == TY_SYMBOL) self_cls = "Symbol";
  else if (t == TY_RANGE || t == TY_FLOAT_RANGE || t == TY_STR_RANGE) self_cls = "Range";
  else if (ty_is_array(t)) self_cls = "Array";
  else if (ty_is_hash(t)) self_cls = "Hash";
  else if (t == TY_NIL) self_cls = "NilClass";
  else if (t == TY_BOOL) self_cls = "Boolean"; /* true/false split handled at call site */
  else if (t == TY_FIBER) self_cls = "Fiber";
  else if (t == TY_THREAD) self_cls = "Thread";
  else if (t == TY_QUEUE) self_cls = "Queue";
  else if (t == TY_MUTEX) self_cls = "Mutex";
  else if (t == TY_CONDVAR) self_cls = "ConditionVariable";
  else if (t == TY_ENUMERATOR) self_cls = "Enumerator";
  else if (t == TY_TIME) self_cls = "Time";
  else if (t == TY_COMPLEX) self_cls = "Complex";
  else if (t == TY_RATIONAL) self_cls = "Rational";
  else if (t == TY_REGEX) self_cls = "Regexp";
  else if (t == TY_MATCHDATA) self_cls = "MatchData";
  else if (t == TY_PROC) self_cls = "Proc";
  else if (t == TY_RANDOM) self_cls = "Random";
  else if (t == TY_IO) self_cls = "IO";
  if (!self_cls) return -1;
  if (sp_streq(cn, self_cls)) return 1;
  if (exact) return 0;
  if (sp_streq(cn, "Object") || sp_streq(cn, "BasicObject") || sp_streq(cn, "Kernel")) return 1;
  if (sp_streq(cn, "Comparable") && (t == TY_STRING || t == TY_STRBUF || t == TY_INT || t == TY_BIGINT ||
                                     t == TY_FLOAT || t == TY_SYMBOL || t == TY_TIME ||
                                     t == TY_COMPLEX || t == TY_RATIONAL)) return 1;
  if (sp_streq(cn, "Numeric") && (t == TY_INT || t == TY_BIGINT || t == TY_FLOAT ||
                                  t == TY_COMPLEX || t == TY_RATIONAL)) return 1;
  if (sp_streq(cn, "Enumerable") && (ty_is_array(t) || ty_is_hash(t) || t == TY_RANGE ||
                                     t == TY_ENUMERATOR)) return 1;
  return 0;
}

/* `if (frozen) raise FrozenError` guard preceding an ivar store on a
   freeze-observed class ("can't modify frozen <Name>: <inspect>"); emits
   nothing when instances of the class are never frozen. `selfexpr` is a C
   expression for the instance pointer, evaluated twice (bind a temp first
   when it has effects). */
void emit_frozen_obj_guard(Compiler *c, int cid, const char *selfexpr, Buf *b) {
  if (cid < 0 || cid >= c->nclasses) return;
  if (!c->classes[cid].freeze_observed || c->classes[cid].is_value_type) return;
  const char *rn = class_ruby_name(c, cid) ? class_ruby_name(c, cid) : c->classes[cid].name;
  buf_printf(b,
      "if (sp_gc_is_frozen((void *)%s)) "
      "sp_raise_frozen_obj(sp_box_obj((void *)%s, %d), (&(\"\\xff\" \"can't modify frozen %s\")[1])); ",
      selfexpr, selfexpr, cid, rn);
}

/* Root a temp whose C type came from a TyKind. A boxed-poly temp is an
   sp_RbVal, whose first word is a tag rather than a pointer, so it has to be
   rooted through the rbval macro -- rooting it as a raw pointer hands the mark
   walker a small integer and segfaults under GC pressure. Sites that emit a
   temp from a type the inference chose keep getting this wrong one at a time,
   so they go through here. */
void emit_gc_root_tmp(Compiler *c, TyKind t, int tmp, Buf *b) {
  if (!needs_root(t)) return;
  /* A value-type object lives in the temp itself, not behind it: rooting one
     hands the mark walker the struct's first field. */
  if (comp_ty_value_obj(c, t)) return;
  buf_printf(b, t == TY_POLY ? "SP_GC_ROOT_RBVAL(_t%d);" : "SP_GC_ROOT(_t%d);", tmp);
}
