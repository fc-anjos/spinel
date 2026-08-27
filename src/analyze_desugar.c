/* analyze_desugar.c -- the AST rewrites analyze_program drives, split out of
   analyze_pass.c. Pure code movement, no logic change: the functions keep
   their order, and analyze_program's call sequence is untouched.

   Five desugars stay in analyze_pass.c because they use file-static helpers
   that inference also uses (subtree_has_kind, subtree_rename_local, the
   bdp_/ie_ pairs); moving those would widen their linkage for no reason but
   this file split. */
#include "analyze_internal.h"
#include <stdio.h>
#include <stdlib.h>

/* Required-param count of a forwarded callable expression `ex`, or -1 if it
   cannot be determined statically. Chooses the hash-pair calling convention: a
   1-param callable receives the [k,v] pair as one array, a 2-param one is called
   positionally (matching CRuby's proc auto-splat of the yielded pair). */
static int fwd_callable_arity(Compiler *c, int ex) {
  NodeTable *nt = (NodeTable *)c->nt;
  const char *exty = nt_type(nt, ex);
  if (!exty) return -1;
  int create = -1;
  if (sp_streq(exty, "LambdaNode") || is_proc_create(c, ex)) create = ex;
  else if (sp_streq(exty, "LocalVariableReadNode")) {
    const char *vn = nt_str(nt, ex, "name");
    Scope *sc = vn ? comp_scope_of(c, ex) : NULL;
    for (int w = 0; vn && w < nt->count; w++) {
      const char *wty = nt_type(nt, w);
      if (!wty || !sp_streq(wty, "LocalVariableWriteNode")) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != sc) continue;
      int val = nt_ref(nt, w, "value");
      if (val >= 0 && is_proc_create(c, val)) { create = val; break; }
    }
  }
  if (create < 0) return -1;
  int pn = a_proc_params_node(c, create);
  if (pn < 0) return -1;
  int rn = 0; nt_arr(nt, pn, "requireds", &rn);
  return rn;
}

/* A method call on a local statically holding one BUILTIN class constant
   dispatches like the constant itself: retarget the receiver at the AST so
   `k = Array; k.new(3, 0)` rides every Array.new arm (#2715). User classes
   already resolve through class_var_static_ci at the dispatch sites. */
static unsigned bcv_key_hash(const char *name, const Scope *sc) {
  unsigned h = 5381;
  for (const char *p = name; *p; p++) h = h * 33u + (unsigned char)*p;
  size_t s = (size_t)(const void *)sc;
  for (unsigned b = 0; b < sizeof s; b++) h = h * 33u + (unsigned char)(s >> (b * 8));
  return h;
}

int desugar_builtin_class_var_recv(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;

  /* Index every LocalVariableWriteNode by (variable name, scope) once, so
     resolving a receiver's static class scans only the writes that could
     actually bind it instead of the whole node table per receiver. Turns the
     pass from O(receivers * N) into O(N) -- the quadratic that stalled the
     lobsters tree (#3115). Scope belongs in the key because a hot name reused
     across many scopes otherwise leaves one long chain whose every entry needs
     its scope resolved. Inlines the old builtin_class_var_static_name
     resolution over the index. */
  int nbuckets = 16;
  while (nbuckets < n0) nbuckets <<= 1;
  int *head = malloc((size_t)nbuckets * sizeof(int));
  int *wnext = malloc((size_t)(n0 > 0 ? n0 : 1) * sizeof(int));
  if (!head || !wnext) { free(head); free(wnext); return 0; }
  for (int i = 0; i < nbuckets; i++) head[i] = -1;
  unsigned mask = (unsigned)nbuckets - 1;
  for (int w = 0; w < n0; w++) {
    if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
    const char *wn = nt_str(nt, w, "name");
    if (!wn) continue;
    unsigned h = bcv_key_hash(wn, comp_scope_of(c, w)) & mask;
    wnext[w] = head[h];
    head[h] = w;
  }

  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_LocalVariableReadNode) continue;
    const char *vn = nt_str(nt, recv, "name");
    if (!vn) continue;
    Scope *sc = comp_scope_of(c, recv);
    unsigned h = bcv_key_hash(vn, sc) & mask;
    /* every write of this local in this scope must assign the SAME builtin (or
       user-class) constant, else the local is dynamic and does not retarget */
    const char *cn = NULL;
    int bail = 0;
    for (int w = head[h]; w >= 0; w = wnext[w]) {
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != sc) continue;
      int val = nt_ref(nt, w, "value");
      const char *vcn = (val >= 0 && nt_kind(nt, val) == NK_ConstantReadNode)
                        ? nt_str(nt, val, "name") : NULL;
      if (!vcn || !(is_builtin_class_name(vcn) || comp_class_index(c, vcn) >= 0)) { bail = 1; break; }
      if (cn && !sp_streq(cn, vcn)) { bail = 1; break; }
      cn = vcn;
    }
    if (bail || !cn) continue;
    int cr = nt_new_node(nt, "ConstantReadNode");
    if (cr < 0) continue;
    nt_node_set_str(nt, cr, "name", cn);
    comp_grow_node_arrays(c);
    c->nscope[cr] = c->nscope[id];
    nt_node_set_ref(nt, id, "receiver", cr);
    changed = 1;
  }
  free(head); free(wnext);
  return changed;
}

/* Proc#>> / #<< with a Method operand: wrap the Method side in #to_proc at the
   AST, so composition always runs proc-to-proc. The to_proc emission builds a
   real trampoline proc that publishes its boxed result through the return
   slot; the raw sp_method_to_proc tramp does not, which is why composing the
   Method directly mis-typed the intermediate (#2692). */
int desugar_compose_method_operand(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, ">>") && !sp_streq(nm, "<<"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (recv < 0 || an != 1 || !av) continue;
    TyKind rt = infer_type(c, recv), at = infer_type(c, av[0]);
    int r_m = rt == TY_METHOD, a_m = at == TY_METHOD;
    if (!r_m && !a_m) continue;
    /* The other operand may be a Proc read out of a container, which arrives
       boxed: the composition arm unwraps it. Refusing the shape left a Method
       composed with a container-read Proc unemittable (#3884). A poly `<<` is
       unambiguous here -- the Method side says this is a composition. */
    if (!(rt == TY_METHOD || rt == TY_PROC || rt == TY_POLY) ||
        !(at == TY_METHOD || at == TY_PROC || at == TY_POLY)) continue;
    int a0 = av[0];
    int base = nt->count;
    if (r_m) {
      int tp = nt_new_node(nt, "CallNode");
      if (tp < 0) continue;
      nt_node_set_ref(nt, tp, "receiver", recv);
      nt_node_set_str(nt, tp, "name", "to_proc");
      nt_node_set_ref(nt, tp, "arguments", -1);
      nt_node_set_ref(nt, tp, "block", -1);
      nt_node_set_ref(nt, id, "receiver", tp);
    }
    if (a_m) {
      int tp = nt_new_node(nt, "CallNode");
      int na = nt_new_node(nt, "ArgumentsNode");
      if (tp < 0 || na < 0) continue;
      nt_node_set_ref(nt, tp, "receiver", a0);
      nt_node_set_str(nt, tp, "name", "to_proc");
      nt_node_set_ref(nt, tp, "arguments", -1);
      nt_node_set_ref(nt, tp, "block", -1);
      nt_node_set_arr(nt, na, "arguments", &tp, 1);
      nt_node_set_ref(nt, id, "arguments", na);
    }
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* method.curry -> method.to_proc.curry: the Proc curry machinery (arity
   typing, boxed accumulation, param widening) then applies unchanged. The
   rewrite fires once: afterwards curry's receiver is the synthesized
   to_proc call, which infers TY_PROC. */
int desugar_method_curry(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "curry")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    if (infer_type(c, recv) != TY_METHOD) continue;
    int base = nt->count;
    int tp = nt_new_node(nt, "CallNode");
    if (tp < 0) continue;
    nt_node_set_ref(nt, tp, "receiver", recv);
    nt_node_set_str(nt, tp, "name", "to_proc");
    nt_node_set_ref(nt, tp, "arguments", -1);
    nt_node_set_ref(nt, tp, "block", -1);
    nt_node_set_ref(nt, id, "receiver", tp);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* n.times.with_index / upto / downto (and with_object/each_with_index):
   a blockless Integer enumerator types as a range, which has no with_index
   arm. Interpose `.each` -- range.each stays an external Enumerator ahead
   of these chains, whose machinery already serves both the blockless and
   the block forms. Fires once: afterwards the receiver is the each call. */
int desugar_int_enum_with_index(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "with_index") && !sp_streq(nm, "with_object") &&
                !sp_streq(nm, "each_with_index"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_CallNode) continue;
    if (nt_ref(nt, recv, "block") >= 0) continue;
    const char *rnm = nt_str(nt, recv, "name");
    if (!rnm || (!sp_streq(rnm, "times") && !sp_streq(rnm, "upto") &&
                 !sp_streq(rnm, "downto"))) continue;
    if (infer_type(c, recv) != TY_RANGE) continue;
    int base = nt->count;
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0 && (sp_streq(nm, "with_index") || sp_streq(nm, "each_with_index"))) {
      /* Block form returns the Integer RECEIVER (CRuby: the enumerator's
         underlying each return), not the range: hoist the receiver into a
         temp, run the chain for effect, and make the original call a
         transparent `.itself` on `(t = n; t.times.each.with_index {..}; t)`
         so the value is the int evaluated once. */
      int ircv = nt_ref(nt, recv, "receiver");
      if (ircv < 0) continue;
      char tmpn[32]; snprintf(tmpn, sizeof tmpn, "_spwi%d", id);
      /* register_locals already ran: intern the temp into the enclosing
         scope now; its type comes from the following inference passes. */
      { int encl0 = c->nscope[id];
        if (encl0 >= 0 && encl0 < c->nscopes)
          scope_local_intern(&c->scopes[encl0], tmpn); }
      int w = nt_new_node(nt, "LocalVariableWriteNode");
      int rd1 = nt_new_node(nt, "LocalVariableReadNode");
      int rd2 = nt_new_node(nt, "LocalVariableReadNode");
      int ec = nt_new_node(nt, "CallNode");
      int inner = nt_new_node(nt, "CallNode");
      int stmts = nt_new_node(nt, "StatementsNode");
      int paren = nt_new_node(nt, "ParenthesesNode");
      if (w < 0 || rd1 < 0 || rd2 < 0 || ec < 0 || inner < 0 ||
          stmts < 0 || paren < 0) continue;
      nt_node_set_str(nt, w, "name", tmpn);
      nt_node_set_ref(nt, w, "value", ircv);
      nt_node_set_str(nt, rd1, "name", tmpn);
      nt_node_set_str(nt, rd2, "name", tmpn);
      nt_node_set_ref(nt, recv, "receiver", rd1);
      nt_node_set_ref(nt, ec, "receiver", recv);
      nt_node_set_str(nt, ec, "name", "each");
      nt_node_set_ref(nt, ec, "arguments", -1);
      nt_node_set_ref(nt, ec, "block", -1);
      nt_node_set_str(nt, inner, "name", nm);
      nt_node_set_ref(nt, inner, "receiver", ec);
      nt_node_set_ref(nt, inner, "arguments", nt_ref(nt, id, "arguments"));
      nt_node_set_ref(nt, inner, "block", blk);
      { int items[3] = { w, inner, rd2 };
        nt_node_set_arr(nt, stmts, "body", items, 3); }
      nt_node_set_ref(nt, paren, "body", stmts);
      nt_node_set_str(nt, id, "name", "itself");
      nt_node_set_ref(nt, id, "receiver", paren);
      nt_node_set_ref(nt, id, "block", -1);
      nt_node_set_ref(nt, id, "arguments", -1);
    }
    else {
      int ec = nt_new_node(nt, "CallNode");
      if (ec < 0) continue;
      nt_node_set_ref(nt, ec, "receiver", recv);
      nt_node_set_str(nt, ec, "name", "each");
      nt_node_set_ref(nt, ec, "arguments", -1);
      nt_node_set_ref(nt, ec, "block", -1);
      nt_node_set_ref(nt, id, "receiver", ec);
    }
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

int desugar_reduce_proc_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "reduce") && !sp_streq(nm, "inject"))) continue;
    if (nt_ref(nt, id, "receiver") < 0) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || nt_kind(nt, blk) != NK_BlockArgumentNode) continue;
    int ex = nt_ref(nt, blk, "expression");
    if (ex < 0) continue;
    const char *exty = nt_type(nt, ex);
    int simple = exty && (sp_streq(exty, "LocalVariableReadNode") ||
                          sp_streq(exty, "InstanceVariableReadNode") ||
                          sp_streq(exty, "LambdaNode"));
    if (!simple || infer_type(c, ex) != TY_PROC) continue;

    int base = nt->count;
    char pn[2][40]; int reqs[2], reads[2];
    int ok = 1;
    for (int k = 0; k < 2 && ok; k++) {
      snprintf(pn[k], sizeof pn[k], "__fold_%d_%d", id, k);
      reqs[k] = nt_new_node(nt, "RequiredParameterNode");
      reads[k] = nt_new_node(nt, "LocalVariableReadNode");
      if (reqs[k] < 0 || reads[k] < 0) { ok = 0; break; }
      nt_node_set_str(nt, reqs[k], "name", pn[k]);
      nt_node_set_str(nt, reads[k], "name", pn[k]);
    }
    if (!ok) continue;
    int params = nt_new_node(nt, "ParametersNode");
    int bparams = nt_new_node(nt, "BlockParametersNode");
    int callargs = nt_new_node(nt, "ArgumentsNode");
    int callnode = nt_new_node(nt, "CallNode");
    int body = nt_new_node(nt, "StatementsNode");
    int blocknode = nt_new_node(nt, "BlockNode");
    if (params < 0 || bparams < 0 || callargs < 0 || callnode < 0 || body < 0 || blocknode < 0) continue;
    nt_node_set_arr(nt, params, "requireds", reqs, 2);
    nt_node_set_ref(nt, bparams, "parameters", params);
    nt_node_set_arr(nt, callargs, "arguments", reads, 2);
    nt_node_set_ref(nt, callnode, "receiver", ex);
    nt_node_set_str(nt, callnode, "name", "call");
    nt_node_set_ref(nt, callnode, "arguments", callargs);
    nt_node_set_ref(nt, callnode, "block", -1);
    nt_node_set_arr(nt, body, "body", &callnode, 1);
    nt_node_set_ref(nt, blocknode, "parameters", bparams);
    nt_node_set_ref(nt, blocknode, "body", body);
    nt_node_set_ref(nt, id, "block", blocknode);

    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    Scope *bs = comp_scope_of(c, blocknode);
    for (int k = 0; k < 2; k++) {
      LocalVar *lv = scope_local_intern(bs, pn[k]);
      if (lv) lv->is_block_param = 1;
    }
    changed = 1;
  }
  return changed;
}

/* ENV's enumeration/read-only surface rides a StrStr-hash snapshot: retarget
   the receiver at a receiverless __env_to_h call, and the whole Hash machinery
   serves keys/each/select/count{...}/inspect/... (#2742). Mutators and the
   direct read/write arms (\[\], \[\]=, fetch sans block, delete, store) stay on
   the real environment. */
static int env_enum_method(const char *n) {
  static const char *const M[] = {
    "keys", "values", "each", "each_pair", "each_key", "each_value",
    "each_entry", "to_h", "to_a", "select", "filter", "reject", "any?",
    "all?", "none?", "one?", "find", "detect", "find_all", "min_by", "max_by",
    "sort", "sort_by", "map", "collect", "flat_map", "filter_map", "group_by",
    "partition", "sum", "reduce", "inject", "invert", "key", "rassoc",
    "assoc", "slice", "except", "values_at", "count", "inspect", "hash",
    "empty?",
    /* the wider Enumerable/query surface (#2832) */
    "to_hash", "entries", "first", "min", "max", "minmax", "tally", "uniq",
    "zip", "take", "take_while", "drop", "drop_while", "each_slice",
    "each_cons", "each_with_index", "each_with_object", "find_index", "grep",
    "chunk", "chunk_while", "slice_when", "collect_concat",
    "reverse_each", "value?", "has_value?", "lazy", NULL };
  for (int i = 0; M[i]; i++) if (sp_streq(n, M[i])) return 1;
  return 0;
}

/* `a !~ b` where a's class defines `=~` (and no `!~` of its own): Object#!~ is
   !(a =~ b). Rewrite the CallNode into `!` over a FRESH `=~` node -- renaming
   in codegen poisoned the node-type cache (`!~` bool vs `=~` any, see #3018's
   note), a fresh node types independently (#3019). Receivers without a user
   `=~` (regex operands, bool/nil raises) keep their dedicated paths. */
int desugar_user_not_match(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "!~")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    TyKind rt = infer_type(c, recv);
    if (!ty_is_object(rt)) continue;
    int cid = ty_object_class(rt);
    if (cid < 0 || comp_method_in_chain(c, cid, "=~", NULL) < 0) continue;
    if (comp_method_in_chain(c, cid, "!~", NULL) >= 0) continue;  /* user !~ wins */
    int inner = nt_new_node(nt, "CallNode");
    nt_node_set_str(nt, inner, "name", "=~");
    nt_node_set_ref(nt, inner, "receiver", recv);
    nt_node_set_ref(nt, inner, "arguments", nt_ref(nt, id, "arguments"));
    nt_node_set_ref(nt, inner, "block", -1);
    nt_node_set_str(nt, id, "name", "!");
    nt_node_set_ref(nt, id, "receiver", inner);
    nt_node_set_ref(nt, id, "arguments", -1);
    comp_grow_node_arrays(c);
    c->nscope[inner] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

int desugar_env_enum(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    if (!nm || recv < 0 || nt_kind(nt, recv) != NK_ConstantReadNode) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || !sp_streq(rn, "ENV")) continue;
    int is_enum = env_enum_method(nm);
    /* fetch WITH a block rides the snapshot's block-aware Hash#fetch (#2745) */
    if (!is_enum && sp_streq(nm, "fetch") && nt_ref(nt, id, "block") >= 0) is_enum = 1;
    if (!is_enum) continue;
    /* ENV keys are Strings at the C level: a statically non-String argument to
       the string-keyed queries is CRuby's TypeError, not a silent miss (#3000).
       Rewrite the call into the raise before the snapshot desugar. */
    if (sp_streq(nm, "assoc") || sp_streq(nm, "key") || sp_streq(nm, "slice") ||
        sp_streq(nm, "values_at")) {
      int qargs = nt_ref(nt, id, "arguments");
      int qn = 0; const int *qav = qargs >= 0 ? nt_arr(nt, qargs, "arguments", &qn) : NULL;
      const char *badc = NULL;
      for (int k = 0; k < qn && !badc; k++) {
        TyKind at = infer_type(c, qav[k]);
        badc = at == TY_SYMBOL ? "Symbol" : at == TY_INT ? "Integer"
             : at == TY_FLOAT ? "Float" : at == TY_NIL ? "nil"
             : at == TY_BOOL ? "Boolean" : NULL;
      }
      if (badc) {
        char msg[128];
        snprintf(msg, sizeof msg, "no implicit conversion of %s into String", badc);
        int ecn = nt_new_node(nt, "ConstantReadNode");
        nt_node_set_str(nt, ecn, "name", "TypeError");
        int emn = nt_new_node(nt, "StringNode");
        nt_node_set_str(nt, emn, "content", msg);
        int ea[2] = { ecn, emn };
        int eargs = nt_new_node(nt, "ArgumentsNode");
        nt_node_set_arr(nt, eargs, "arguments", ea, 2);
        nt_node_set_str(nt, id, "name", "raise");
        nt_node_set_ref(nt, id, "receiver", -1);
        nt_node_set_ref(nt, id, "arguments", eargs);
        nt_node_set_ref(nt, id, "block", -1);
        comp_grow_node_arrays(c);
        c->nscope[ecn] = c->nscope[emn] = c->nscope[eargs] = c->nscope[id];
        changed = 1;
        continue;
      }
    }
    int snap = nt_new_node(nt, "CallNode");
    if (snap < 0) continue;
    nt_node_set_str(nt, snap, "name", "__env_to_h");
    nt_node_set_ref(nt, snap, "receiver", -1);
    nt_node_set_ref(nt, snap, "arguments", -1);
    nt_node_set_ref(nt, snap, "block", -1);
    comp_grow_node_arrays(c);
    c->nscope[snap] = c->nscope[id];
    /* the plain-Enumerable names ride the pair ARRAY (the typed-hash surface
       does not carry them); hash-native names stay on the snapshot (#2832) */
    {
      static const char *const VIA_A[] = {
        "first", "min", "max", "minmax", "tally", "uniq", "zip", "take",
        "take_while", "drop", "drop_while", "each_slice", "each_cons",
        "each_with_index", "each_with_object", "find_index", "grep", "chunk",
        "chunk_while", "slice_when", "collect_concat", "reverse_each",
        "lazy", NULL };
      int via_a = 0;
      for (int q = 0; VIA_A[q]; q++) if (sp_streq(nm, VIA_A[q])) { via_a = 1; break; }
      if (via_a) {
        int toa = nt_new_node(nt, "CallNode");
        if (toa < 0) continue;
        nt_node_set_str(nt, toa, "name", "to_a");
        nt_node_set_ref(nt, toa, "receiver", snap);
        nt_node_set_ref(nt, toa, "arguments", -1);
        nt_node_set_ref(nt, toa, "block", -1);
        comp_grow_node_arrays(c);
        c->nscope[toa] = c->nscope[id];
        nt_node_set_ref(nt, id, "receiver", toa);
      }
      else nt_node_set_ref(nt, id, "receiver", snap);
    }
    /* aliases the hash surface spells differently */
    if (sp_streq(nm, "to_hash")) nt_node_set_str(nt, id, "name", "to_h");
    else if (sp_streq(nm, "entries")) nt_node_set_str(nt, id, "name", "to_a");
    else if (sp_streq(nm, "value?")) nt_node_set_str(nt, id, "name", "has_value?");
    else if (sp_streq(nm, "collect_concat")) nt_node_set_str(nt, id, "name", "flat_map");
    changed = 1;
  }
  return changed;
}

int desugar_public_method(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "public_method")) continue;
    if (!method_sym_arg(c, id)) continue;   /* literal symbol/string arg only */
    nt_node_set_str(nt, id, "name", "method");
    changed = 1;
  }
  return changed;
}

/* Is `n` a value spinel can materialize with #to_a -- i.e. an operand a chain
   may concatenate? Arrays/ranges/hashes/enumerators answer directly; a user
   object qualifies when its class defines #each (the Enumerable contract). */
static int chain_operand_ok(Compiler *c, int n) {
  if (n < 0) return 0;
  TyKind t = infer_type(c, n);
  if (ty_is_array(t) || ty_is_hash(t) || t == TY_RANGE || t == TY_ENUMERATOR) return 1;
  /* An empty array literal never narrows, so `a = []; a.chain.to_a` leaves the
     receiver UNKNOWN (#2474 / #2468). #chain is Enumerable-specific and a
     user-defined #chain is excluded by the caller, so an untyped operand is
     taken at its word; if it turns out to have no #to_a, that call reports it. */
  if (t == TY_UNKNOWN) return 1;
  if (ty_is_object(t)) {
    int ci = ty_object_class(t);
    return ci >= 0 && comp_method_in_chain(c, ci, "each", NULL) >= 0;
  }
  return 0;
}

/* Synthesize `<n>.to_a` (a fresh CallNode), or -1 on node-table OOM. */
static int chain_mk_to_a(Compiler *c, int n) {
  NodeTable *nt = (NodeTable *)c->nt;
  int call = nt_new_node(nt, "CallNode");
  if (call < 0) return -1;
  nt_node_set_ref(nt, call, "receiver", n);
  nt_node_set_str(nt, call, "name", "to_a");
  nt_node_set_ref(nt, call, "arguments", -1);
  nt_node_set_ref(nt, call, "block", -1);
  return call;
}

/* Synthesize `<a> + <b>`, or -1 on node-table OOM. */
static int chain_mk_concat(Compiler *c, int a, int b) {
  NodeTable *nt = (NodeTable *)c->nt;
  int args = nt_new_node(nt, "ArgumentsNode");
  if (args < 0) return -1;
  nt_node_set_arr(nt, args, "arguments", &b, 1);
  int call = nt_new_node(nt, "CallNode");
  if (call < 0) return -1;
  nt_node_set_ref(nt, call, "receiver", a);
  nt_node_set_str(nt, call, "name", "+");
  nt_node_set_ref(nt, call, "arguments", args);
  nt_node_set_ref(nt, call, "block", -1);
  return call;
}

/* `recv.chain(a, b)` and `enum + enum` -> `__enum_chain(recv.to_a + a.to_a + b.to_a)`.
   Ruby's chain is lazy over its sources; spinel materializes them at build time
   and hands the concatenation to a snapshot enumerator, which serves every
   terminal the sources support (#to_a, #each, #map, #next, ...). Reusing #to_a
   is what lets a Struct, a user Enumerable, or another enumerator be an operand:
   each already knows how to materialize itself. #2545 / #2548 / #2551 */
int desugar_enumerable_chain(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    if (!nm || recv < 0) continue;
    if (nt_ref(nt, id, "block") >= 0) continue;   /* chain{} is not a thing; leave it */
    int args = nt_ref(nt, id, "arguments");
    int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;

    int is_chain = sp_streq(nm, "chain");
    /* Enumerator#+ only: `+` is overwhelmingly numeric/array/string, so require
       BOTH operands to be enumerators before touching it. */
    int is_plus = sp_streq(nm, "+") && argc == 1 && argv &&
                  infer_type(c, recv) == TY_ENUMERATOR &&
                  infer_type(c, argv[0]) == TY_ENUMERATOR;
    if (!is_chain && !is_plus) continue;
    /* `arr.chain` with no argument is just the receiver's own elements (#2468),
       so argc == 0 is valid for chain (but `+` always has its operand). */
    if (argc > 32 || (argc > 0 && !argv) || (is_plus && argc != 1)) continue;

    if (is_chain) {
      /* a user-defined #chain wins over Enumerable's */
      TyKind rt = infer_type(c, recv);
      if (ty_is_object(rt)) {
        int ci = ty_object_class(rt);
        if (ci >= 0 && comp_method_in_chain(c, ci, "chain", NULL) >= 0) continue;
      }
      if (!chain_operand_ok(c, recv)) continue;
    }
    int ok = 1;
    for (int k = 0; k < argc && ok; k++) if (!chain_operand_ok(c, argv[k])) ok = 0;
    if (!ok) continue;

    int saved[32];
    for (int k = 0; k < argc; k++) saved[k] = argv[k];  /* copy before realloc */
    int base = nt->count;
    int acc = chain_mk_to_a(c, recv);
    for (int k = 0; k < argc && acc >= 0; k++) {
      int t = chain_mk_to_a(c, saved[k]);
      acc = (t >= 0) ? chain_mk_concat(c, acc, t) : -1;
    }
    if (acc < 0) continue;   /* node-table OOM: leave the call alone */
    int newargs = nt_new_node(nt, "ArgumentsNode");
    if (newargs < 0) continue;
    nt_node_set_arr(nt, newargs, "arguments", &acc, 1);
    nt_node_set_str(nt, id, "name", "__enum_chain");
    nt_node_set_ref(nt, id, "receiver", -1);
    nt_node_set_ref(nt, id, "arguments", newargs);

    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

int desugar_implicit_send(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;        /* implicit self only */
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "send") && !sp_streq(nm, "__send__") &&
                !sp_streq(nm, "public_send"))) continue;
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;
    const char *a0ty = nt_type(nt, argv[0]);
    const char *mname = NULL;
    if (a0ty && sp_streq(a0ty, "SymbolNode")) mname = nt_str(nt, argv[0], "value");
    else if (a0ty && sp_streq(a0ty, "StringNode")) mname = nt_str(nt, argv[0], "content");
    if (!mname || !*mname) continue;                      /* non-literal name: leave it */
    if (sp_streq(mname, "send") || sp_streq(mname, "__send__") ||
        sp_streq(mname, "public_send")) continue;          /* don't re-trigger next pass */
    int nrest = argc - 1;
    if (nrest > 64) continue;                             /* absurd arity: leave it */
    int rest[64];
    for (int k = 0; k < nrest; k++) rest[k] = argv[k + 1];  /* copy before realloc */
    char namebuf[256];
    snprintf(namebuf, sizeof namebuf, "%s", mname);        /* copy before realloc */
    /* public_send dispatches only public methods: stamp the retargeted call
       so codegen raises NoMethodError for a private/protected target */
    int vis_enf = sp_streq(nm, "public_send");
    int base = nt->count;
    int newargs = nt_new_node(nt, "ArgumentsNode");
    if (newargs < 0) continue;
    nt_node_set_arr(nt, newargs, "arguments", rest, nrest);
    nt_node_set_str(nt, id, "name", namebuf);              /* retarget the call */
    nt_node_set_ref(nt, id, "arguments", newargs);         /* drop the name arg */
    if (vis_enf) nt_node_set_str(nt, id, "vis_enforce", "1");
    else nt_node_set_str(nt, id, "send_blind", "1");   /* see desugar_public_send_recv */
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* `recv.public_send(:m, args)` with a literal name -> a direct `recv.m(args)`
   call stamped `vis_enforce`, so codegen raises NoMethodError for a
   private/protected target. send/__send__ keep the visibility-blind textual
   rewrite in spinel_parse.c (CRuby's send ignores visibility). Mirrors
   desugar_implicit_send's node-retarget model. */
int desugar_public_send_recv(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    if (nt_ref(nt, id, "receiver") < 0) continue;          /* explicit receiver only */
    const char *nm = nt_str(nt, id, "name");
    /* Also handle send/__send__ here, but ONLY when they target another
       send-family method (the nested `d.send(:send, :greet)` case, #2688):
       simple `d.send(:m)` is already lowered textually in spinel_parse.c, and
       the send-of-send it leaves behind unwinds one layer per pass here. */
    int is_pub = nm && sp_streq(nm, "public_send");
    int is_snd = nm && (sp_streq(nm, "send") || sp_streq(nm, "__send__"));
    if (!is_pub && !is_snd) continue;
    /* A blank-slate receiver has no #send / #public_send (only __send__ is
       BasicObject's): leave the call unretargeted, and the blank-slate gate
       raises CRuby's NoMethodError for the send itself (#2725). */
    if (!sp_streq(nm, "__send__")) {
      int bsrecv = nt_ref(nt, id, "receiver");
      TyKind bsrt = bsrecv >= 0 ? infer_type(c, bsrecv) : TY_UNKNOWN;
      if (ty_is_object(bsrt) && class_is_blank_slate(c, ty_object_class(bsrt))) continue;
      /* A socket's #send is the datagram write, not Object#send: CRuby picks
         by the receiver's class, and `u.send("ping", 0, host, port)` would
         otherwise retarget to a method named "ping" (#2922). */
      if (sp_streq(nm, "send") && bsrt == TY_IO && sp_feature_required("socket")) continue;
    }
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;
    const char *a0ty = nt_type(nt, argv[0]);
    const char *mname = NULL;
    if (a0ty && sp_streq(a0ty, "SymbolNode")) mname = nt_str(nt, argv[0], "value");
    else if (a0ty && sp_streq(a0ty, "StringNode")) mname = nt_str(nt, argv[0], "content");
    if (!mname || !*mname) continue;                       /* runtime name: dyn_send_arms */
    int m_is_send = sp_streq(mname, "send") || sp_streq(mname, "__send__") ||
                    sp_streq(mname, "public_send");
    (void)m_is_send;
    /* send/__send__ that spinel_parse.c already lowered never reach here;
       the ones it leaves are the send-of-send residue (`d.send(:greet)` after
       stripping the outer :send), which we finish retargeting. */
    int nrest = argc - 1;
    if (nrest > 64) continue;
    int rest[64];
    for (int k = 0; k < nrest; k++) rest[k] = argv[k + 1];
    char namebuf[256];
    snprintf(namebuf, sizeof namebuf, "%s", mname);
    int base = nt->count;
    int newargs = nt_new_node(nt, "ArgumentsNode");
    if (newargs < 0) continue;
    nt_node_set_arr(nt, newargs, "arguments", rest, nrest);
    nt_node_set_str(nt, id, "name", namebuf);
    nt_node_set_ref(nt, id, "arguments", newargs);
    if (is_pub && !m_is_send) nt_node_set_str(nt, id, "vis_enforce", "1");
    /* `x.send(:m)` ignores visibility, which is the whole point of it: a
       top-level `def` is Object's PRIVATE instance method, so a plain
       `x.m` cannot reach it but a send can. The retargeted call carries
       that permission, since nothing else distinguishes it from the
       ordinary call it now looks like (#4070 follow-up). */
    if (is_snd) nt_node_set_str(nt, id, "send_blind", "1");
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* n.step(to: X[, by: Y]) is the keyword form of n.step(X, Y) (by defaults to 1).
   The step passes read positional arguments, so a lone KeywordHashNode argument
   is otherwise mis-read as an integer limit (an int-from-pointer miscompile).
   Rewrite the to:/by: form into the positional list before those passes run. */
/* `expr => pattern` where the pattern NESTS another pattern, rewritten to the
   one-arm `case expr; in pattern; end` it is defined to mean. The rightward
   form has its own destructuring emitter, and that one binds direct local
   targets only: a nested class pattern (`{ left: Lit(value: lv) }`) bound
   nothing at all and raised nothing either, so the local came out nil (#4047).
   The case form's emitter recurses, and answers the same NoMatchingPatternError
   on a miss. Only the nesting shapes are rewritten -- the flat ones the
   dedicated emitter handles keep going through it. */
static int pattern_nests(const NodeTable *nt, int pat, int depth) {
  if (pat < 0 || depth > 8) return 0;
  const char *ty = nt_type(nt, pat);
  if (!ty || !sp_streq(ty, "HashPatternNode")) return 0;
  int en = 0; const int *el = nt_arr(nt, pat, "elements", &en);
  for (int i = 0; i < en; i++) {
    if (!nt_type(nt, el[i]) || !sp_streq(nt_type(nt, el[i]), "AssocNode")) continue;
    int v = nt_ref(nt, el[i], "value");
    const char *vt = v >= 0 ? nt_type(nt, v) : NULL;
    if (vt && !sp_streq(vt, "LocalVariableTargetNode")) return 1;
  }
  return 0;
}

int desugar_rightward_nested_pattern(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_MatchRequiredNode) continue;
    int value = nt_ref(nt, id, "value");
    int pattern = nt_ref(nt, id, "pattern");
    if (value < 0 || pattern < 0) continue;
    if (!pattern_nests(nt, pattern, 0)) continue;
    int inn = nt_new_node(nt, "InNode");
    int st = nt_new_node(nt, "StatementsNode");
    if (inn < 0 || st < 0) continue;
    nt_node_set_ref(nt, inn, "pattern", pattern);
    nt_node_set_arr(nt, st, "body", NULL, 0);
    nt_node_set_ref(nt, inn, "statements", st);
    nt_node_set_type(nt, id, "CaseMatchNode");
    nt_node_set_ref(nt, id, "predicate", value);
    nt_node_set_arr(nt, id, "conditions", &inn, 1);
    nt_node_set_ref(nt, id, "else_clause", -1);
    comp_grow_node_arrays(c);
    c->nscope[inn] = c->nscope[id];
    c->nscope[st] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

int desugar_step_kwargs(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "step")) continue;
    if (nt_ref(nt, id, "receiver") < 0) continue;         /* Numeric#step has a receiver */
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int ac = 0; const int *av = nt_arr(nt, args, "arguments", &ac);
    if (ac != 1 || !av) continue;
    int kh = av[0];
    if (!nt_type(nt, kh) || !sp_streq(nt_type(nt, kh), "KeywordHashNode")) continue;
    int to_v = -1, by_v = -1, other = 0;
    int en = 0; const int *els = nt_arr(nt, kh, "elements", &en);
    for (int i = 0; i < en; i++) {
      if (!nt_type(nt, els[i]) || !sp_streq(nt_type(nt, els[i]), "AssocNode")) { other = 1; break; }
      int key = nt_ref(nt, els[i], "key");
      const char *kn = (key >= 0 && nt_type(nt, key) && sp_streq(nt_type(nt, key), "SymbolNode"))
                       ? nt_str(nt, key, "value") : NULL;
      if (kn && sp_streq(kn, "to")) to_v = nt_ref(nt, els[i], "value");
      else if (kn && sp_streq(kn, "by")) by_v = nt_ref(nt, els[i], "value");
      else { other = 1; break; }
    }
    if (other || to_v < 0) continue;   /* only the to:[/by:] form; `to` is required */
    int sc = c->nscope[id];
    int base = nt->count;
    if (by_v < 0) {
      by_v = nt_new_node(nt, "IntegerNode");
      if (by_v < 0) continue;
      nt_node_set_int(nt, by_v, "value", 1);
    }
    int pos[2] = { to_v, by_v };
    nt_node_set_arr(nt, args, "arguments", pos, 2);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = sc;
    changed = 1;
  }
  return changed;
}

/* A receiverless `instance_exec(&b)` at top level (or in a free function) has an
   implicit self, so instance_exec rebinds self to the current self -- i.e. it
   does not change self at all, and is exactly `<block>.call(<args>)`. Rewrite it
   so the value form (`x = run { }`) lowers like any block-call forward instead of
   stranding an un-emittable top-level instance_exec (which links to an undefined
   function). Class-level instance_exec forwarders DO rebind self to the instance
   and are handled by their own trampoline splice, so they are left untouched. */
int desugar_toplevel_instance_exec(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;         /* implicit self only */
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "instance_exec")) continue;
    int sc = c->nscope[id];
    if (sc < 0 || sc >= c->nscopes || c->scopes[sc].class_id >= 0) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockArgumentNode")) continue;
    int bexpr = nt_ref(nt, blk, "expression");
    if (bexpr < 0) continue;                               /* anonymous `&`: no name to call */
    nt_node_set_ref(nt, id, "receiver", bexpr);            /* receiver = forwarded block */
    nt_node_set_str(nt, id, "name", "call");
    nt_node_set_ref(nt, id, "block", -1);                  /* the block is now the receiver */
    changed = 1;
  }
  return changed;
}

/* `binding.local_variable_get(:name)` with a literal symbol naming an in-scope
   local is the idiom for reading a reserved-word parameter (`def f(then:);
   binding.local_variable_get(:then); end`), the only way to reference such a
   name -- a bare `then` is a keyword. An AOT compiler has no reified Binding, but
   this statically-decidable form is exactly the value of that local, so rewrite
   it to `<local>.itself` (an identity that yields the local's value). Other
   binding uses have no static answer and are rejected in codegen. */
int desugar_binding_lvget(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "local_variable_get")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) continue;
    if (nt_ref(nt, recv, "receiver") >= 0) continue;      /* binding must be receiverless */
    const char *rnm = nt_str(nt, recv, "name");
    if (!rnm || !sp_streq(rnm, "binding")) continue;
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (ac != 1 || !av || !nt_type(nt, av[0]) || !sp_streq(nt_type(nt, av[0]), "SymbolNode")) continue;
    const char *vn = nt_str(nt, av[0], "value");
    if (!vn) continue;
    int sc = c->nscope[id];
    if (sc < 0 || sc >= c->nscopes || !scope_local(&c->scopes[sc], vn)) continue;
    char *vnbuf = malloc(strlen(vn) + 1);   /* copy before nt_new_node may realloc vn's storage */
    if (!vnbuf) continue;
    strcpy(vnbuf, vn);
    int base = nt->count;
    int lread = nt_new_node(nt, "LocalVariableReadNode");
    if (lread < 0) { free(vnbuf); continue; }
    nt_node_set_str(nt, lread, "name", vnbuf);
    free(vnbuf);
    nt_node_set_ref(nt, id, "receiver", lread);            /* <local>.itself */
    nt_node_set_str(nt, id, "name", "itself");
    nt_node_set_ref(nt, id, "arguments", -1);
    /* The `binding` receiver is now orphaned; rename it to a sentinel no pass
       matches so the binding reject (which scans all nodes, including
       unreferenced ones) does not fire on it. */
    nt_node_set_str(nt, recv, "name", "__orphaned__");
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = sc;
    changed = 1;
  }
  return changed;
}

/* `recv.send(name_expr, args)` with a NON-literal name and an explicit receiver:
   lower it to a static dispatch over the method names that appear as symbol
   literals in the program. For each candidate name `m` we synthesize an ordinary
   `recv.m(args)` call; analyze types each (honoring arity), and codegen keeps the
   ones that resolve on the receiver's type and emits `name == :m1 ? recv.m1(args)
   : ... : NoMethodError` (result poly). A runtime name that is not one of those
   literals -- or whose call does not resolve on the receiver -- is not
   dispatchable and raises NoMethodError. The literal-name forms are rewritten
   earlier (spinel_parse.c / desugar_implicit_send); this covers a name known only
   at runtime but drawn from the program's closed set of symbol literals. The arm
   node ids are stashed on the send under "dyn_send_arms" for codegen. */
int desugar_dynamic_send(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count;
  int changed = 0;
  static const char *const sends[] = { "send", "__send__", "public_send", NULL };
  /* a user-defined method named send/etc. resolves normally; don't intercept */
  for (int s = 0; s < c->nscopes; s++) { const char *sn = c->scopes[s].name;
    if (sn) for (int k = 0; sends[k]; k++) if (sp_streq(sn, sends[k])) return 0; }
  /* quick out: nothing to do unless some not-yet-lowered explicit-receiver send
     with a runtime name exists (the common case has none, so skip the scans). */
  { int any = 0;
    for (int id = 0; id < n0 && !any; id++) {
      if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
      const char *nm = nt_str(nt, id, "name"); if (!nm) continue;
      int is = 0; for (int k = 0; sends[k]; k++) if (sp_streq(nm, sends[k])) { is = 1; break; }
      if (!is || nt_ref(nt, id, "receiver") < 0) continue;
      int dn = 0; nt_arr(nt, id, "dyn_send_arms", &dn); if (dn > 0) continue;
      int a = nt_ref(nt, id, "arguments"); if (a < 0) continue;
      int ac = 0; const int *av = nt_arr(nt, a, "arguments", &ac);
      if (ac < 1 || !av) continue;
      const char *a0 = nt_type(nt, av[0]);
      if (a0 && (sp_streq(a0, "SymbolNode") || sp_streq(a0, "StringNode"))) continue;
      any = 1;
    }
    if (!any) return 0;
  }
  /* collect distinct symbol/string-literal names = candidate method names (send
     accepts either; a string name interns to the same symbol at the call). */
  char **cand = NULL; int ncand = 0, candcap = 0;
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    const char *v = NULL;
    if (ty && sp_streq(ty, "SymbolNode")) v = nt_str(nt, id, "value");
    else if (ty && sp_streq(ty, "StringNode")) v = nt_str(nt, id, "content");
    if (!v || !*v) continue;
    int skip = 0;
    for (int k = 0; sends[k]; k++) if (sp_streq(v, sends[k])) { skip = 1; break; }  /* avoid send-of-send recursion */
    for (int k = 0; !skip && k < ncand; k++) if (sp_streq(cand[k], v)) skip = 1;
    if (skip) continue;
    if (ncand == candcap) { candcap = candcap ? candcap * 2 : 16; cand = (char **)realloc(cand, sizeof(char *) * candcap); }
    cand[ncand++] = strdup(v);
  }
  if (ncand == 0 || ncand > 128) { for (int k = 0; k < ncand; k++) free(cand[k]); free(cand); return 0; }
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int is_send = 0; for (int k = 0; sends[k]; k++) if (sp_streq(nm, sends[k])) { is_send = 1; break; }
    if (!is_send) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;                            /* implicit self handled elsewhere */
    { int dn = 0; nt_arr(nt, id, "dyn_send_arms", &dn); if (dn > 0) continue; }  /* already lowered */
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;
    const char *a0 = nt_type(nt, argv[0]);
    if (a0 && (sp_streq(a0, "SymbolNode") || sp_streq(a0, "StringNode"))) continue;  /* literal: handled earlier */
    int nrest = argc - 1;
    if (nrest > 64) continue;
    int rest[64]; for (int k = 0; k < nrest; k++) rest[k] = argv[k + 1];  /* copy before realloc */
    int base = nt->count;
    int arms[128]; int narm = 0;
    for (int k = 0; k < ncand; k++) {
      int na = nt_new_node(nt, "ArgumentsNode"); if (na < 0) break;
      if (nrest) nt_node_set_arr(nt, na, "arguments", rest, nrest);
      int call = nt_new_node(nt, "CallNode"); if (call < 0) break;
      nt_node_set_ref(nt, call, "receiver", recv);
      nt_node_set_str(nt, call, "name", cand[k]);
      /* The dispatch keys each arm on the NAME it was built for, so a later
         desugar that rewrites the name (`first` -> `[]`) leaves the arm
         unreachable and the send raises. Mark them as owned. */
      nt_node_set_int(nt, call, "dyn_arm", 1);
      nt_node_set_ref(nt, call, "arguments", na);
      /* public_send arms enforce visibility at the dispatch site */
      if (sp_streq(nm, "public_send")) nt_node_set_str(nt, call, "vis_enforce", "1");
      arms[narm++] = call;
    }
    nt_node_set_arr(nt, id, "dyn_send_arms", arms, narm);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  for (int k = 0; k < ncand; k++) free(cand[k]);
  free(cand);
  return changed;
}

/* `recv.respond_to?(:m)` with an explicit receiver and a literal method name:
   synthesize a probe `recv.m` call. The analyze fixpoint types the probe with
   the ordinary resolver, so its inferred type tells codegen whether spinel can
   actually dispatch `m` on that receiver (UNKNOWN = it cannot). The probe id is
   stashed on the respond_to? node under "rt_probes" and is analysis-only -- it is
   never emitted. The codegen fold reads it for primitive/builtin receivers,
   deriving the answer from the real dispatch instead of a hand-maintained method
   list; user-object receivers keep their visibility-aware chain resolution. The
   probe carries no arguments: builtin method inference keys on the receiver type
   and name (not arity), so an arg-taking method like `+`/`[]` still types. */
int desugar_respond_to_probe(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  /* a user-defined respond_to? resolves normally; don't intercept */
  for (int s = 0; s < c->nscopes; s++) { const char *sn = c->scopes[s].name;
    if (sn && sp_streq(sn, "respond_to?")) return 0; }
  int n0 = nt->count;
  int changed = 0;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "respond_to?")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;                         /* implicit self handled in the fold */
    { int pn = 0; nt_arr(nt, id, "rt_probes", &pn); if (pn > 0) continue; }  /* already probed */
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;
    const char *aty = nt_type(nt, argv[0]);
    const char *qm = NULL;
    if (aty && sp_streq(aty, "SymbolNode")) qm = nt_str(nt, argv[0], "value");
    else if (aty && sp_streq(aty, "StringNode")) {
      qm = nt_str(nt, argv[0], "content");
      if (!qm) qm = nt_str(nt, argv[0], "unescaped");
    }
    if (!qm || !*qm) continue;                      /* non-literal name: not foldable */
    int base = nt->count;
    /* Probe two call shapes and let codegen answer true if EITHER types, since a
       single shape cannot satisfy every method: a block method (`each`, `map`)
       rejects a positional argument but needs a block, while an operator (`+`,
       `[]`) needs an argument. One probe carries an empty block (no arg), the
       other one dummy argument (the receiver, always type-available). A plain
       no-arg method (`upcase`) types under either. Builtin method inference keys
       on the receiver type and name, so a recognized method types by name; an
       unrecognized one is UNKNOWN under both. */
    int probes[3]; int np = 0;
    /* shape 0: recv.m  -- no argument, no block. Resolves the blockless
       enumerator forms (`each`, `reverse_each` infer TY_ENUMERATOR) and plain
       no-arg methods, using only codegen-safe inference. Every probe carries
       the rt_probe flag: it is analysis-only, and the param-binding passes
       must not let its dummy shapes type real lambda/method parameters. */
    {
      int na = nt_new_node(nt, "ArgumentsNode");
      int probe = nt_new_node(nt, "CallNode");
      if (na >= 0 && probe >= 0) {
        nt_node_set_ref(nt, probe, "receiver", recv);
        nt_node_set_str(nt, probe, "name", qm);
        nt_node_set_ref(nt, probe, "arguments", na);
        nt_node_set_int(nt, probe, "rt_probe", 1);
        probes[np++] = probe;
      }
    }
    /* shape 1: recv.m { }  -- block, no argument */
    {
      int na = nt_new_node(nt, "ArgumentsNode");
      int blkbody = nt_new_node(nt, "StatementsNode");
      int blk = nt_new_node(nt, "BlockNode");
      int probe = nt_new_node(nt, "CallNode");
      if (na >= 0 && blkbody >= 0 && blk >= 0 && probe >= 0) {
        nt_node_set_ref(nt, blk, "body", blkbody);
        nt_node_set_ref(nt, probe, "receiver", recv);
        nt_node_set_str(nt, probe, "name", qm);
        nt_node_set_ref(nt, probe, "arguments", na);
        nt_node_set_ref(nt, probe, "block", blk);
        nt_node_set_int(nt, probe, "rt_probe", 1);
        probes[np++] = probe;
      }
    }
    /* shape 2: recv.m(recv)  -- one dummy argument, no block */
    {
      int dummy_args[1] = { recv };
      int na = nt_new_node(nt, "ArgumentsNode");
      int probe = nt_new_node(nt, "CallNode");
      if (na >= 0 && probe >= 0) {
        nt_node_set_arr(nt, na, "arguments", dummy_args, 1);
        nt_node_set_ref(nt, probe, "receiver", recv);
        nt_node_set_str(nt, probe, "name", qm);
        nt_node_set_ref(nt, probe, "arguments", na);
        nt_node_set_int(nt, probe, "rt_probe", 1);
        probes[np++] = probe;
      }
    }
    /* Sync the parallel arrays for every node allocated in this iteration --
       even a partial shape (an allocation failed mid-shape, so no probe was
       added) leaves nodes past `base` whose c->nscope would otherwise stay
       uninitialized, desyncing the arrays from nt->count for later passes. */
    if (nt->count > base) {
      comp_grow_node_arrays(c);
      int encl = c->nscope[id];
      for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    }
    if (np == 0) continue;
    nt_node_set_arr(nt, id, "rt_probes", probes, np);
    changed = 1;
  }
  return changed;
}

/* `recv.at(i)` is `recv[i]` for a single argument -- Array#at takes exactly
   one integer and answers what #[] does. Written as its own name it reached
   neither the typed array arms nor the boxed dispatch, so an Array read out
   of a container answered NoMethodError (#3821). Rewritten here, every path
   that knows #[] knows it. */
/* `arr.first` / `arr.last` on a statically ARRAY receiver are `arr[0]` and
   `arr[-1]`, exactly -- both answer nil on an empty array. Rewriting them onto
   the index route is not a shortcut: the shared-mutable-string machinery keys
   its alias analysis off the element read, and only `[]` carried a local
   binding through it, so `a = b.first; a << "Z"` bound a COPY and the
   container never saw the append (#4013). One route, one behaviour.
   The count forms (`first(2)`) answer a new Array and are left alone, as are
   Hash / Range / Enumerator / poly receivers, whose #first is a different
   method. */
int desugar_array_first_last(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int user_fl = 0;
  for (int k = 0; k < c->nclasses && !user_fl; k++)
    if (comp_method_in_chain(c, k, "first", NULL) >= 0 ||
        comp_reader_in_chain(c, k, "first", NULL) ||
        comp_method_in_chain(c, k, "last", NULL) >= 0 ||
        comp_reader_in_chain(c, k, "last", NULL)) user_fl = 1;
  if (user_fl) return 0;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (!sp_streq(nm, "first") && !sp_streq(nm, "last"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_ref(nt, id, "block") >= 0) continue;
    if (nt_int(nt, id, "dyn_arm", 0)) continue;   /* a dynamic-send arm keeps its name */
    int args = nt_ref(nt, id, "arguments");
    int argc = 0; if (args >= 0) nt_arr(nt, args, "arguments", &argc);
    if (argc != 0) continue;
    TyKind rt = infer_type(c, recv);
    if (!ty_is_array(rt) || ty_is_obj_array(rt)) continue;
    int idx = nt_new_node(nt, "IntegerNode");
    if (idx < 0) continue;
    nt_node_set_int(nt, idx, "value", sp_streq(nm, "first") ? 0 : -1);
    int ia = nt_new_node(nt, "ArgumentsNode");
    if (ia < 0) continue;
    nt_node_set_arr(nt, ia, "arguments", &idx, 1);
    comp_grow_node_arrays(c);
    c->nscope[idx] = c->nscope[id];
    c->nscope[ia] = c->nscope[id];
    nt_node_set_ref(nt, id, "arguments", ia);
    nt_node_set_str(nt, id, "name", "[]");
    changed = 1;
  }
  return changed;
}

int desugar_array_at(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  /* a user class owning the name keeps its own dispatch */
  int user_at = 0;
  for (int k = 0; k < c->nclasses && !user_at; k++)
    if (comp_method_in_chain(c, k, "at", NULL) >= 0 ||
        comp_reader_in_chain(c, k, "at", NULL)) user_at = 1;
  if (!user_at)
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "at")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_ref(nt, id, "block") >= 0) continue;
    if (nt_int(nt, id, "dyn_arm", 0)) continue;   /* same reason as first/last */
    int args = nt_ref(nt, id, "arguments");
    int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    if (argc != 1 || !argv) continue;
    const char *aty = nt_type(nt, argv[0]);
    if (aty && sp_streq(aty, "SplatNode")) continue;
    TyKind rt = infer_type(c, recv);
    /* Time#at and a Struct's own member reader are different methods */
    if (rt == TY_TIME || rt == TY_CLASS || ty_is_object(rt)) continue;
    /* Array#at takes an index, never a Range: rewriting it to #[] handed the
       slice form a call CRuby answers with a TypeError (#3924). */
    { TyKind aat = infer_type(c, argv[0]);
      if (aat == TY_RANGE || aat == TY_FLOAT_RANGE || aat == TY_STR_RANGE) continue; }
    nt_node_set_str(nt, id, "name", "[]");
    changed = 1;
  }
  return changed;
}

/* `recv.attr op= value` where the writer is a hand-written `def attr=`.
   Ruby desugars this into a reader call and a writer call; the emitter's own
   lowering goes straight to the backing ivar, which is right for an
   attr_accessor and wrong for a writer with a body, so it refused the shape
   outright (#3809). Rewrite it into the two calls Ruby means and let the
   ordinary call machinery handle them; the accessor case is left alone, where
   the direct ivar store is worth keeping.

   The receiver is evaluated twice, so only a form with no work behind it and
   no side effect qualifies: a local, self, an ivar or a constant. */
int desugar_call_op_write(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallOperatorWriteNode")) continue;
    int recv = nt_ref(nt, id, "receiver");
    const char *attr = nt_str(nt, id, "name");
    const char *op = nt_str(nt, id, "binary_operator");
    int val = nt_ref(nt, id, "value");
    if (recv < 0 || !attr || !op || val < 0) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || !(sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "SelfNode") ||
                  sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "ConstantReadNode")))
      continue;
    char wname[300];
    snprintf(wname, sizeof wname, "%s=", attr);
    int has_def_writer = 0;
    for (int k = 0; k < c->nclasses && !has_def_writer; k++)
      if (comp_method_in_chain(c, k, wname, NULL) >= 0) has_def_writer = 1;
    if (!has_def_writer) continue;                 /* attr_writer: keep the store */
    char aname[300]; snprintf(aname, sizeof aname, "%s", attr);
    char opname[64]; snprintf(opname, sizeof opname, "%s", op);
    int recv2 = nt_clone_subtree(nt, recv);
    if (recv2 < 0) continue;
    int base = nt->count;
    int rd = nt_new_node(nt, "CallNode");
    int binargs = nt_new_node(nt, "ArgumentsNode");
    int bin = nt_new_node(nt, "CallNode");
    int wargs = nt_new_node(nt, "ArgumentsNode");
    if (rd < 0 || binargs < 0 || bin < 0 || wargs < 0) continue;
    nt_node_set_ref(nt, rd, "receiver", recv);
    nt_node_set_str(nt, rd, "name", aname);
    { int one[1]; one[0] = val; nt_node_set_arr(nt, binargs, "arguments", one, 1); }
    nt_node_set_ref(nt, bin, "receiver", rd);
    nt_node_set_str(nt, bin, "name", opname);
    nt_node_set_ref(nt, bin, "arguments", binargs);
    { int one[1]; one[0] = bin; nt_node_set_arr(nt, wargs, "arguments", one, 1); }
    nt_node_set_type(nt, id, "CallNode");
    nt_node_set_ref(nt, id, "receiver", recv2);
    nt_node_set_str(nt, id, "name", wname);
    nt_node_set_ref(nt, id, "arguments", wargs);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = recv2; j < nt->count; j++) c->nscope[j] = encl;
    (void)base;
    changed = 1;
  }
  return changed;
}

/* `:sym.to_proc.call(recv, *args)` -> `recv.sym(*args)`. An explicit Symbol#to_proc
   followed by a call applies the named method to the first argument; with both the
   symbol and the call site statically known, it rewrites to an ordinary method call
   and the normal dispatch handles it. (The `&:sym` block form lowers separately; a
   to_proc whose receiver isn't a literal symbol, or that isn't immediately called,
   is left alone.) Mirrors desugar_implicit_send's node-retarget model. */
int desugar_symbol_to_proc_call(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;  /* snapshot: synthetic nodes are appended past here */
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "call")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) continue;
    const char *rnm = nt_str(nt, recv, "name");
    if (!rnm || !sp_streq(rnm, "to_proc")) continue;
    int rargs = nt_ref(nt, recv, "arguments");
    if (rargs >= 0) { int rc = 0; nt_arr(nt, rargs, "arguments", &rc); if (rc != 0) continue; }
    int sym = nt_ref(nt, recv, "receiver");
    if (sym < 0 || !nt_type(nt, sym) || !sp_streq(nt_type(nt, sym), "SymbolNode")) continue;
    const char *mname = nt_str(nt, sym, "value");
    if (!mname || !*mname) continue;
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;            /* needs the receiver argument */
    int newrecv = argv[0];
    int nrest = argc - 1;
    if (nrest > 64) continue;
    int rest[64];
    for (int k = 0; k < nrest; k++) rest[k] = argv[k + 1];  /* copy before realloc */
    char namebuf[256];
    snprintf(namebuf, sizeof namebuf, "%s", mname);         /* copy before realloc */
    int base = nt->count;
    int newargs = nt_new_node(nt, "ArgumentsNode");
    if (newargs < 0) continue;
    nt_node_set_arr(nt, newargs, "arguments", rest, nrest);
    nt_node_set_ref(nt, id, "receiver", newrecv);           /* receiver = first arg */
    nt_node_set_str(nt, id, "name", namebuf);               /* call the named method */
    nt_node_set_ref(nt, id, "arguments", newargs);          /* drop the receiver arg */
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* `recv.to_h { |e| [k, v] }` -> `recv.map { |e| [k, v] }.to_h`. The block-taking
   to_h maps each element to a [key, value] pair and collects the pairs into a
   hash; map already lowers the block for any iterable and the blockless to_h
   already builds a typed hash from an array of pairs, so rewriting onto that
   pair reuses both instead of adding a bespoke hash-building iterator. */
int desugar_to_h_block(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "to_h")) continue;
    int recv = nt_ref(nt, id, "receiver");
    int blk = nt_ref(nt, id, "block");
    if (recv < 0 || blk < 0) continue;                 /* need a receiver and a block */
    if (!nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockNode")) continue;
    /* Only builtin iterables lower onto map{}.to_h. A Struct/Data or other user
       object with a block-taking to_h has its own member-pair path; rewriting it
       onto map would change the element protocol and mistype the result. */
    TyKind rt = infer_type(c, recv);
    if (!ty_is_array(rt) && !ty_is_hash(rt) && rt != TY_RANGE && rt != TY_ENUMERATOR) continue;
    int base = nt->count;
    int mapargs = nt_new_node(nt, "ArgumentsNode");
    int mapcall = nt_new_node(nt, "CallNode");
    if (mapargs < 0 || mapcall < 0) continue;          /* node-table OOM: leave as-is */
    nt_node_set_arr(nt, mapargs, "arguments", NULL, 0); /* map takes no positional args */
    nt_node_set_ref(nt, mapcall, "receiver", recv);
    nt_node_set_str(nt, mapcall, "name", "map");
    nt_node_set_ref(nt, mapcall, "arguments", mapargs);
    nt_node_set_ref(nt, mapcall, "block", blk);
    nt_node_set_ref(nt, id, "receiver", mapcall);      /* to_h now consumes the mapped pairs */
    nt_node_set_ref(nt, id, "block", -1);              /* and no longer carries the block */
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl; /* new nodes share the scope */
    changed = 1;
  }
  return changed;
}

/* Descend `root`'s subtree (bounded to scope `sc`) tracking the nearest enclosing
   StatementsNode entry (curr_st/curr_idx). On reaching `target`, report that entry
   -- the innermost same-scope top-level statement whose subtree contains target.
   A single O(N) pass that prunes at nested scope boundaries. */
static int tp_find_stmt(Compiler *c, int root, int target, int sc,
                        int curr_st, int curr_idx, int *out_st, int *out_idx) {
  if (root < 0 || c->nscope[root] != sc) return 0;   /* out of scope -> prune */
  if (root == target) {
    if (curr_st < 0) return 0;                        /* no enclosing statement */
    *out_st = curr_st; *out_idx = curr_idx; return 1;
  }
  NodeTable *nt = (NodeTable *)c->nt;
  int is_stmt = nt_type(nt, root) && sp_streq(nt_type(nt, root), "StatementsNode");
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) {
    int ch = nt_ref_at(nt, root, i);
    if (ch >= 0 && tp_find_stmt(c, ch, target, sc, curr_st, curr_idx, out_st, out_idx)) return 1;
  }
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *a = nt_arr_at(nt, root, i, &n);
    for (int k = 0; k < n; k++) {
      if (a[k] < 0) continue;
      int nst = is_stmt ? root : curr_st, nidx = is_stmt ? k : curr_idx;
      if (tp_find_stmt(c, a[k], target, sc, nst, nidx, out_st, out_idx)) return 1;
    }
  }
  return 0;
}

/*out_idx set. */
static int tp_enclosing_stmt(Compiler *c, int id, int *out_st, int *out_idx) {
  int sc = c->nscope[id];
  if (sc < 0 || sc >= c->nscopes) return 0;
  return tp_find_stmt(c, c->scopes[sc].body, id, sc, -1, -1, out_st, out_idx);
}

/* `recv.iter(&obj)` where obj is a user object defining `to_proc`: Ruby calls
   obj.to_proc exactly ONCE to obtain the block. Hoist `__tproc_N = obj.to_proc`
   to the statement enclosing the call and rewrite the block argument to the
   hoisted local, so the value-callable desugar below forwards the once-computed
   proc (mirroring Ruby's `&obj` => `obj.to_proc` model, evaluated once). Declines
   -- leaving a loud reject -- when the enclosing statement can't be located. */
int desugar_to_proc_block_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockArgumentNode")) continue;
    int ex = nt_ref(nt, blk, "expression");
    if (ex < 0) continue;
    const char *exty = nt_type(nt, ex);
    if (!exty || sp_streq(exty, "SymbolNode")) continue;  /* &:sym lowers separately */
    /* Only a user object defining #to_proc (not a Proc/Method value or symbol). */
    TyKind ct = infer_type(c, ex);
    if (!ty_is_object(ct)) continue;
    int cid = ty_object_class(ct);
    if (cid < 0 || comp_method_in_chain(c, cid, "to_proc", NULL) < 0) continue;
    int st = -1, idx = -1;
    if (!tp_enclosing_stmt(c, id, &st, &idx)) continue;  /* can't hoist -> leave (reject) */
    int encl = c->nscope[id];
    int base = nt->count;
    int exclone = nt_clone_subtree(nt, ex);
    int tpcall = nt_new_node(nt, "CallNode");
    int wnode = nt_new_node(nt, "LocalVariableWriteNode");
    int rd = nt_new_node(nt, "LocalVariableReadNode");
    if (exclone < 0 || tpcall < 0 || wnode < 0 || rd < 0) continue;
    char tpname[48];
    snprintf(tpname, sizeof tpname, "__tproc_%d", id);
    nt_node_set_ref(nt, tpcall, "receiver", exclone);
    nt_node_set_str(nt, tpcall, "name", "to_proc");
    nt_node_set_ref(nt, tpcall, "arguments", -1);
    nt_node_set_ref(nt, tpcall, "block", -1);
    nt_node_set_str(nt, wnode, "name", tpname);
    nt_node_set_ref(nt, wnode, "value", tpcall);
    nt_node_set_str(nt, rd, "name", tpname);
    nt_node_set_ref(nt, blk, "expression", rd);  /* block arg now &__tproc_N */
    /* insert `wnode` before body[idx] in the enclosing StatementsNode */
    int bn = 0; const int *body = nt_arr(nt, st, "body", &bn);
    int *nb = malloc(sizeof(int) * (size_t)(bn + 1));
    if (!nb) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int k = 0; k < idx; k++) nb[k] = body[k];
    nb[idx] = wnode;
    for (int k = idx; k < bn; k++) nb[k + 1] = body[k];
    nt_node_set_arr(nt, st, "body", nb, bn + 1);
    free(nb);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    LocalVar *lv = scope_local_intern(comp_scope_of(c, id), tpname);
    lv->type = TY_PROC;
    changed = 1;
  }
  return changed;
}

/* `m(&(a >> b))`: an inline proc-composition (or any Proc-valued expression
   that is not already a simple read) as a block argument. The block-argument
   lowering wants a value it can name, so hoist the expression into a temp on
   the enclosing statement and pass that. The same composition assigned to a
   local first already compiled; this makes the inline form agree. (#3117) */
int desugar_proc_expr_block_arg(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockArgumentNode")) continue;
    int ex = nt_ref(nt, blk, "expression");
    if (ex < 0) continue;
    const char *exty = nt_type(nt, ex);
    if (!exty || !sp_streq(exty, "CallNode")) continue;
    /* only the Proc combinators: everything else keeps its own lowering
       (&:sym, &obj.to_proc, &method(:m), a lambda literal, ...) */
    const char *exn = nt_str(nt, ex, "name");
    if (!exn || (!sp_streq(exn, ">>") && !sp_streq(exn, "<<"))) continue;
    if (infer_type(c, ex) != TY_PROC) continue;
    int st = -1, idx = -1;
    if (!tp_enclosing_stmt(c, id, &st, &idx)) continue;
    int encl = c->nscope[id];
    int base = nt->count;
    int wnode = nt_new_node(nt, "LocalVariableWriteNode");
    int rd = nt_new_node(nt, "LocalVariableReadNode");
    if (wnode < 0 || rd < 0) continue;
    char bname[48];
    snprintf(bname, sizeof bname, "__blkexpr_%d", id);
    nt_node_set_str(nt, wnode, "name", bname);
    nt_node_set_ref(nt, wnode, "value", ex);
    nt_node_set_str(nt, rd, "name", bname);
    nt_node_set_ref(nt, blk, "expression", rd);
    int bn = 0; const int *body = nt_arr(nt, st, "body", &bn);
    int *nb = malloc(sizeof(int) * (size_t)(bn + 1));
    if (!nb) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int k = 0; k < idx; k++) nb[k] = body[k];
    nb[idx] = wnode;
    for (int k = idx; k < bn; k++) nb[k + 1] = body[k];
    nt_node_set_arr(nt, st, "body", nb, bn + 1);
    free(nb);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    LocalVar *lv = scope_local_intern(comp_scope_of(c, id), bname);
    lv->type = TY_PROC;
    changed = 1;
  }
  return changed;
}

/* `f(**obj)` where obj is a user object defining `#to_hash`: Ruby converts it
   through to_hash. Rewrite the splat's value from `obj` to `obj.to_hash` so the
   existing double-splat machinery (which pre-evaluates the source hash into a
   temp -- once) forwards the converted hash. Mirrors the `to_ary` splice
   coercion; once rewritten the value is a to_hash call and is not revisited. */
int desugar_to_hash_splat(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "AssocSplatNode")) continue;
    int val = nt_ref(nt, id, "value");
    if (val < 0) continue;
    if (nt_type(nt, val) && sp_streq(nt_type(nt, val), "CallNode") &&
        nt_str(nt, val, "name") && sp_streq(nt_str(nt, val, "name"), "to_hash")) continue;
    TyKind t = infer_type(c, val);
    if (!ty_is_object(t)) continue;
    int cid = ty_object_class(t);
    if (cid < 0 || comp_method_in_chain(c, cid, "to_hash", NULL) < 0) continue;
    int base = nt->count;
    int clone = nt_clone_subtree(nt, val);
    int call = nt_new_node(nt, "CallNode");
    if (clone < 0 || call < 0) {
      /* A partial allocation (clone appended nodes but the call node failed)
         leaves nodes past `base` whose c->nscope would otherwise stay
         uninitialized, desyncing the arrays from nt->count for later passes. */
      if (nt->count > base) {
        comp_grow_node_arrays(c);
        int encl = c->nscope[id];
        for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
      }
      continue;
    }
    nt_node_set_ref(nt, call, "receiver", clone);
    nt_node_set_str(nt, call, "name", "to_hash");
    nt_node_set_ref(nt, call, "arguments", -1);
    nt_node_set_ref(nt, call, "block", -1);
    nt_node_set_ref(nt, id, "value", call);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* `def lz; [1,2,3].lazy.map { }; end; lz.first` -- a lazy chain returned from a
   parameterless method. A lazy value has no runtime representation to return,
   so the method body is not emittable at all; splice a clone of the chain into
   the call site instead, which is exactly what a local alias already gets. The
   method then has no callers left and drops out as dead code. Restricted by
   lazy_method_chain to a self-free body, so the clone means the same thing
   where it lands. */
int desugar_lazy_method_call(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_CallNode) continue;
    int chain = lazy_method_chain(c, recv);
    if (chain < 0) continue;
    int base = nt->count;
    int clone = nt_clone_subtree(nt, chain);
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    if (clone < 0) continue;
    nt_node_set_ref(nt, id, "receiver", clone);
    /* the cloned stages carry block parameters that were interned in the
       CALLEE's scope; re-intern them here or their locals are never declared.
       Locals ASSIGNED inside a cloned block body need it too -- a block that
       writes a temp before using it emitted an undeclared identifier (#3367). */
    for (int j = base; j < nt->count; j++) {
      NodeKind jk = nt_kind(nt, j);
      if (jk == NK_BlockNode) {
        for (int k = 0; k < 4; k++) {
          const char *bp = block_param_name(c, j, k);
          if (!bp) break;
          scope_local_intern(&c->scopes[encl], bp);
        }
        continue;
      }
      if (jk == NK_LocalVariableWriteNode || jk == NK_LocalVariableTargetNode ||
          jk == NK_LocalVariableOperatorWriteNode || jk == NK_LocalVariableOrWriteNode ||
          jk == NK_LocalVariableAndWriteNode) {
        const char *wn = nt_str(nt, j, "name");
        if (wn) scope_local_intern(&c->scopes[encl], wn);
      }
    }
    changed = 1;
  }
  return changed;
}

int desugar_value_callable_forwards(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;  /* snapshot: synthetic nodes are appended past here */
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockArgumentNode")) continue;
    int ex = nt_ref(nt, blk, "expression");
    if (ex < 0) continue;  /* anonymous `&`: inline-forward path, not a value */
    const char *exty = nt_type(nt, ex);
    if (!exty) continue;
    /* a constant read is as deterministic and side-effect-free as a local one,
       so `&SOME_LAMBDA` forwards the same way (#3689) */
    int simple_ref = sp_streq(exty, "LocalVariableReadNode") ||
                     sp_streq(exty, "InstanceVariableReadNode") ||
                     sp_streq(exty, "ConstantReadNode") ||
                     sp_streq(exty, "ConstantPathNode");
    /* `&method(:m)`: a deterministic method-object lookup, safe to re-evaluate */
    int method_obj = sp_streq(exty, "CallNode") && nt_str(nt, ex, "name") &&
                     sp_streq(nt_str(nt, ex, "name"), "method");
    /* `&->(x){...}`: an inline lambda literal, equivalent to the block itself;
       building it per element has no observable side effect */
    int inline_lambda = sp_streq(exty, "LambdaNode");
    if (!simple_ref && !method_obj && !inline_lambda) continue;
    TyKind ct = infer_type(c, ex);
    /* A poly local can hold a callable produced by an operation whose static
       type stays poly -- e.g. `procs.reduce(:>>)`, a composed Proc. Forward it
       as a value callable too (its `.call` dispatches at runtime); restricted
       to a bare local/ivar read so re-evaluation is side-effect-free (#3167). */
    if (ct != TY_PROC && ct != TY_METHOD && !(ct == TY_POLY && simple_ref)) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    int encl = c->nscope[id];
    TyKind rt = infer_type(c, recv);
    /* Hash `each`/`each_pair` yields the [k,v] pair, forwarded as a single array
       argument `c.call([k, v])` (built below) -- correct for every arity: a
       1-param callable gets the pair, a 2-param proc auto-splats it, a 2-param
       lambda raises exactly as CRuby's `Hash#each(&lambda)` does. A Method object
       takes the pair via its array ABI; a proc/lambda value's param is typed as
       the pair array by the call-site argument inference (a container arg
       overrides the bare-int default). */
    TyKind pty[4];
    int arity;
    if (sp_streq(name, "each_with_object")) {
      /* each_with_object(init) { |elem, memo| }: two params, the element and the
         accumulator. Array receivers only (a `{}` hash memo is unsupported even
         for a literal block). The memo type is recovered from how the callable
         fills it (ewo_memo_elem_type) by infer_block_params, so seed it UNKNOWN;
         the wrap_pair / hash logic below does not apply. */
      if (!ty_is_array(rt)) continue;
      arity = 2;
      pty[0] = ty_array_elem(rt);
      pty[1] = TY_UNKNOWN;
      /* an empty `{}` memo makes the accumulator a general boxed hash, so the
         memo param is typed accordingly (an empty `[]` stays UNKNOWN and is
         recovered from its fills by ewo_memo_elem_type). */
      int ewo_a = nt_ref(nt, id, "arguments");
      int ewo_ac = 0; const int *ewo_av = ewo_a >= 0 ? nt_arr(nt, ewo_a, "arguments", &ewo_ac) : NULL;
      if (ewo_ac >= 1 && ewo_av) {
        const char *seedty = nt_type(nt, ewo_av[0]);
        int seed_n = 0;
        if (seedty && sp_streq(seedty, "HashNode") &&
            (nt_arr(nt, ewo_av[0], "elements", &seed_n), seed_n == 0))
          pty[1] = TY_POLY_POLY_HASH;
        /* An empty `[]` memo the block never fills directly -- it hands it to a
           callable instead -- has no element evidence to recover, so type it as
           the general boxed array rather than leaving it unresolved (#3657). */
        if (seedty && sp_streq(seedty, "ArrayNode") &&
            (nt_arr(nt, ewo_av[0], "elements", &seed_n), seed_n == 0) &&
            ewo_memo_elem_type(c, id) == TY_UNKNOWN)
          pty[1] = TY_POLY_ARRAY;
      }
    }
    else {
      arity = ty_block_yield(rt, name, pty, 4);
      if (arity < 1) continue;  /* not a context-free iterator (or recv unresolved) */
    }

    /* Hash forwarding. A Method object takes any hash iterator's yield directly
       through its array ABI (the [k,v] pair as one array for `each`, the bare
       key/value for `each_key`/`each_value`). A proc/lambda VALUE is reliable
       only for the `each` pair forwarded to a single param, whose pair-array
       type the call-site inference recovers (a container overriding the bare-int
       default). Every other hash + proc/lambda combination -- inline lambdas
       (cloned, no write to type from), multi-param procs (CRuby auto-splat), and
       the scalar `each_key`/`each_value` yields -- needs cross-procedural param
       typing not yet modeled, so decline to the pre-existing path. */
    int wrap_pair = 0;
    if (ty_is_hash(rt)) {
      if (ct == TY_METHOD) {
        wrap_pair = (arity == 2);  /* each: pair as array; each_key/value: bare value */
      }
      else {
        /* a proc/lambda (value or inline): the call-site inference types its
           params from the forwarded call. `each` to a 1-param callable gets the
           [k,v] pair as one array; to a 2-param one, k and v positionally
           (auto-splat by arity); each_key/each_value pass the bare key/value. */
        int cpc = fwd_callable_arity(c, ex);
        if (arity == 2 && cpc == 1) wrap_pair = 1;
        else if (arity == 2 && cpc == 2) wrap_pair = 0;
        else if (arity == 1) wrap_pair = 0;
        else continue;  /* arity-2 with unresolved callable arity */
      }
    }

    int base = nt->count;
    int proc_clone = nt_clone_subtree(nt, ex);  /* re-read the proc per element */
    if (proc_clone < 0) continue;

    int reqs[4], reads[4];
    char pn[48];
    int alloc_ok = 1;
    for (int k = 0; k < arity; k++) {
      snprintf(pn, sizeof pn, "__fwd_%d_%d", id, k);
      reqs[k] = nt_new_node(nt, "RequiredParameterNode");
      nt_node_set_str(nt, reqs[k], "name", pn);
      reads[k] = nt_new_node(nt, "LocalVariableReadNode");
      nt_node_set_str(nt, reads[k], "name", pn);
      if (reqs[k] < 0 || reads[k] < 0) { alloc_ok = 0; break; }
    }
    if (!alloc_ok) continue;  /* node-table OOM: leave the call in its &-form */
    int params = nt_new_node(nt, "ParametersNode");
    nt_node_set_arr(nt, params, "requireds", reqs, arity);
    int bparams = nt_new_node(nt, "BlockParametersNode");
    nt_node_set_ref(nt, bparams, "parameters", params);

    int callargs = nt_new_node(nt, "ArgumentsNode");
    if (wrap_pair) {
      int pairarr = nt_new_node(nt, "ArrayNode");
      nt_node_set_arr(nt, pairarr, "elements", reads, 2);
      nt_node_set_arr(nt, callargs, "arguments", &pairarr, 1);
    }
    else nt_node_set_arr(nt, callargs, "arguments", reads, arity);
    int callnode = nt_new_node(nt, "CallNode");
    nt_node_set_ref(nt, callnode, "receiver", proc_clone);
    nt_node_set_str(nt, callnode, "name", "call");
    nt_node_set_ref(nt, callnode, "arguments", callargs);
    nt_node_set_ref(nt, callnode, "block", -1);

    int body = nt_new_node(nt, "StatementsNode");
    nt_node_set_arr(nt, body, "body", &callnode, 1);
    int blocknode = nt_new_node(nt, "BlockNode");
    if (params < 0 || bparams < 0 || callargs < 0 || callnode < 0 || body < 0 ||
        blocknode < 0)
      continue;  /* node-table OOM: a -1 id is an out-of-bounds node index below */
    nt_node_set_ref(nt, blocknode, "parameters", bparams);
    nt_node_set_ref(nt, blocknode, "body", body);

    nt_node_set_ref(nt, id, "block", blocknode);  /* call now takes a literal block */

    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;

    Scope *bs = comp_scope_of(c, blocknode);
    for (int k = 0; k < arity; k++) {
      snprintf(pn, sizeof pn, "__fwd_%d_%d", id, k);
      LocalVar *lv = scope_local_intern(bs, pn);
      lv->is_block_param = 1;
      lv->type = pty[k];
    }
    changed = 1;
  }
  return changed;
}

/* `|x,|` -- a trailing comma in a BLOCK's parameter list -- is Ruby's way of
   saying "destructure the element and take the leading names, drop the rest".
   Prism spells the comma as an ImplicitRestNode in the rest slot, and nothing
   downstream read it, so `|x,|` behaved as `|x|` and bound the whole element:
   `[[1, 2]].map { |x,| x }` answered `[[1, 2]]` where Ruby answers `[1]`.
   Give the list a trailing parameter nobody names, which is what the rest is,
   and every multi-parameter path destructures it from there. A method
   definition's trailing comma means nothing and is left alone. */
int desugar_block_implicit_rest(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  /* A lambda is strict about its parameter list, and there the trailing comma
     changes nothing: `lambda { |a,| }` takes exactly one argument and raises
     on two. Only a block (or a proc, which is lenient) destructures. */
  char *is_lambda_params = (char *)calloc(n0 > 0 ? (size_t)n0 : 1, 1);
  if (!is_lambda_params) return 0;
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    int bp = -1;
    if (ty && sp_streq(ty, "LambdaNode")) bp = nt_ref(nt, id, "parameters");
    else if (ty && sp_streq(ty, "CallNode")) {
      const char *cn = nt_str(nt, id, "name");
      if (!cn || !sp_streq(cn, "lambda")) continue;
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) bp = nt_ref(nt, blk, "parameters");
    }
    if (bp >= 0 && bp < n0) is_lambda_params[bp] = 1;
  }
  for (int id = 0; id < n0; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "BlockParametersNode")) continue;
    if (is_lambda_params[id]) continue;
    int pn = nt_ref(nt, id, "parameters");
    if (pn < 0) continue;
    int rest = nt_ref(nt, pn, "rest");
    const char *rty = rest >= 0 ? nt_type(nt, rest) : NULL;
    if (!rty || !sp_streq(rty, "ImplicitRestNode")) continue;
    int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
    if (!reqs || rn < 1 || rn > 30) continue;
    int copy[32];
    for (int k = 0; k < rn; k++) copy[k] = reqs[k];
    int p = nt_new_node(nt, "RequiredParameterNode");
    if (p < 0) continue;
    char nm[32]; snprintf(nm, sizeof nm, "__implicit_rest_%d", id);
    nt_node_set_str(nt, p, "name", nm);
    copy[rn] = p;
    nt_node_set_arr(nt, pn, "requireds", copy, rn + 1);
    nt_node_set_ref(nt, pn, "rest", -1);
    comp_grow_node_arrays(c);
    changed = 1;
  }
  free(is_lambda_params);
  return changed;
}

/* `xs.sort_by.with_index { |v, i| key }`: a blockless sort_by answers an
   Enumerator whose with_index feeds the index to the key block. There is no
   Enumerator arm for the *_by family, so rewrite the chain into the equivalent
   spelling over pairs and take the values back out:

     xs.each_with_index.sort_by { |v, i| key }.map { |p| p[0] }

   Only the no-offset form: `with_index(1)` would need the index to start
   somewhere each_with_index cannot. */
int desugar_sort_by_with_index(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "with_index")) continue;
    /* with_index(offset) shifts every index; the pair map below applies it */
    int wi_off = -1;
    {
      int wa = nt_ref(nt, id, "arguments");
      if (wa >= 0) {
        int wn = 0; const int *wv = nt_arr(nt, wa, "arguments", &wn);
        if (wn != 1 || !wv) continue;
        wi_off = wv[0];
      }
    }
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || !sp_streq(nt_type(nt, blk), "BlockNode")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_CallNode) continue;
    const char *rnm = nt_str(nt, recv, "name");
    if (!rnm || !sp_streq(rnm, "sort_by")) continue;
    if (nt_ref(nt, recv, "block") >= 0 || nt_ref(nt, recv, "arguments") >= 0) continue;
    int src = nt_ref(nt, recv, "receiver");
    if (src < 0) continue;

    int base = nt->count;
    /* src.each_with_index */
    int ewi = nt_new_node(nt, "CallNode");
    nt_node_set_ref(nt, ewi, "receiver", src);
    nt_node_set_str(nt, ewi, "name", "each_with_index");
    nt_node_set_ref(nt, ewi, "arguments", -1);
    nt_node_set_ref(nt, ewi, "block", -1);
    /* with_index(off): shift the pair indexes before the key block reads them,
       `ewi.map { |v, i| [v, i + off] }` (#3763) */
    int pairs = ewi;
    if (wi_off >= 0) {
      char vn[48], inm[48];
      snprintf(vn, sizeof vn, "__wi_v_%d", id);
      snprintf(inm, sizeof inm, "__wi_i_%d", id);
      int vreq = nt_new_node(nt, "RequiredParameterNode");
      nt_node_set_str(nt, vreq, "name", vn);
      int ireq = nt_new_node(nt, "RequiredParameterNode");
      nt_node_set_str(nt, ireq, "name", inm);
      int oreqs[2] = { vreq, ireq };
      int oparams = nt_new_node(nt, "ParametersNode");
      nt_node_set_arr(nt, oparams, "requireds", oreqs, 2);
      int obp = nt_new_node(nt, "BlockParametersNode");
      nt_node_set_ref(nt, obp, "parameters", oparams);
      int vread = nt_new_node(nt, "LocalVariableReadNode");
      nt_node_set_str(nt, vread, "name", vn);
      int iread = nt_new_node(nt, "LocalVariableReadNode");
      nt_node_set_str(nt, iread, "name", inm);
      int offargs = nt_new_node(nt, "ArgumentsNode");
      nt_node_set_arr(nt, offargs, "arguments", &wi_off, 1);
      int shifted = nt_new_node(nt, "CallNode");
      nt_node_set_ref(nt, shifted, "receiver", iread);
      nt_node_set_str(nt, shifted, "name", "+");
      nt_node_set_ref(nt, shifted, "arguments", offargs);
      nt_node_set_ref(nt, shifted, "block", -1);
      int pair[2] = { vread, shifted };
      int parr = nt_new_node(nt, "ArrayNode");
      nt_node_set_arr(nt, parr, "elements", pair, 2);
      int obody = nt_new_node(nt, "StatementsNode");
      nt_node_set_arr(nt, obody, "body", &parr, 1);
      int oblk = nt_new_node(nt, "BlockNode");
      nt_node_set_ref(nt, oblk, "parameters", obp);
      nt_node_set_ref(nt, oblk, "body", obody);
      pairs = nt_new_node(nt, "CallNode");
      nt_node_set_ref(nt, pairs, "receiver", ewi);
      nt_node_set_str(nt, pairs, "name", "map");
      nt_node_set_ref(nt, pairs, "arguments", -1);
      nt_node_set_ref(nt, pairs, "block", oblk);
      if (pairs < 0) continue;
    }
    /* .sort_by { |v, i| key } -- the with_index block, moved across */
    int sb = nt_new_node(nt, "CallNode");
    nt_node_set_ref(nt, sb, "receiver", pairs);
    nt_node_set_str(nt, sb, "name", "sort_by");
    nt_node_set_ref(nt, sb, "arguments", -1);
    nt_node_set_ref(nt, sb, "block", blk);
    /* .map { |p| p[0] } */
    char pn[48]; snprintf(pn, sizeof pn, "__wi_pair_%d", id);
    int preq = nt_new_node(nt, "RequiredParameterNode");
    nt_node_set_str(nt, preq, "name", pn);
    int params = nt_new_node(nt, "ParametersNode");
    nt_node_set_arr(nt, params, "requireds", &preq, 1);
    int bparams = nt_new_node(nt, "BlockParametersNode");
    nt_node_set_ref(nt, bparams, "parameters", params);
    int pread = nt_new_node(nt, "LocalVariableReadNode");
    nt_node_set_str(nt, pread, "name", pn);
    int zero = nt_new_node(nt, "IntegerNode");
    nt_node_set_int(nt, zero, "value", 0);
    int idxargs = nt_new_node(nt, "ArgumentsNode");
    nt_node_set_arr(nt, idxargs, "arguments", &zero, 1);
    int idx = nt_new_node(nt, "CallNode");
    nt_node_set_ref(nt, idx, "receiver", pread);
    nt_node_set_str(nt, idx, "name", "[]");
    nt_node_set_ref(nt, idx, "arguments", idxargs);
    nt_node_set_ref(nt, idx, "block", -1);
    int mbody = nt_new_node(nt, "StatementsNode");
    nt_node_set_arr(nt, mbody, "body", &idx, 1);
    int mblk = nt_new_node(nt, "BlockNode");
    if (ewi < 0 || sb < 0 || preq < 0 || params < 0 || bparams < 0 || pread < 0 ||
        zero < 0 || idxargs < 0 || idx < 0 || mbody < 0 || mblk < 0) continue;
    nt_node_set_ref(nt, mblk, "parameters", bparams);
    nt_node_set_ref(nt, mblk, "body", mbody);

    /* the with_index call BECOMES the map, so the parent link stays put */
    int line = (int)nt_int(nt, id, "node_line", 0);
    int file = (int)nt_int(nt, id, "node_file", 0);
    nt_node_reset(nt, id, "CallNode");
    nt_node_set_ref(nt, id, "receiver", sb);
    nt_node_set_str(nt, id, "name", "map");
    nt_node_set_ref(nt, id, "arguments", -1);
    nt_node_set_ref(nt, id, "block", mblk);
    if (line) nt_node_set_int(nt, id, "node_line", line);
    if (file) nt_node_set_int(nt, id, "node_file", file);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

/* 1 = route whatever the call's shape; 2 = only with a block, because the
   blockless form answers an Enumerator that keeps its source (`(1..6)
   .each_slice(2)` inspects as `1..6:each_slice(2)`, not as its elements). */
static int enum_via_to_a_name(const char *n) {
  static const char *always[] = {
    "take_while", "drop_while", "grep", "grep_v", "chunk_while", "slice_when",
    "slice_before", "slice_after", "sort", "minmax", "minmax_by", "zip",
    "find_index", "uniq", "flat_map", "collect_concat", NULL
  };
  static const char *with_block[] = {
    "each_slice", "each_cons", "each_entry", "cycle", NULL
  };
  for (int i = 0; always[i]; i++) if (sp_streq(n, always[i])) return 1;
  for (int i = 0; with_block[i]; i++) if (sp_streq(n, with_block[i])) return 2;
  return 0;
}

int desugar_enumerable_via_to_a(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    int how = nm ? enum_via_to_a_name(nm) : 0;
    if (!how) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    /* skip one we already rewrote (its receiver IS the to_a) */
    const char *rnm = nt_kind(nt, recv) == NK_CallNode ? nt_str(nt, recv, "name") : NULL;
    if (rnm && sp_streq(rnm, "to_a")) continue;
    TyKind rt = comp_ntype(c, recv);
    if (!ty_is_hash(rt) && rt != TY_RANGE) continue;
    /* A blockless `.each` receiver is an Enumerator, whatever the chain rules
       type it as; the with-block group answers that Enumerator, and routing it
       through to_a answered the elements instead (#3857). */
    { int er = recv;
      if (er >= 0 && nt_kind(nt, er) == NK_CallNode && nt_ref(nt, er, "block") < 0) {
        const char *ernm = nt_str(nt, er, "name");
        if (ernm && (sp_streq(ernm, "each") || sp_streq(ernm, "each_with_index") ||
                     sp_streq(ernm, "reverse_each"))) continue;
      } }
    /* A one-sided Range cannot become an array at all, and find / detect /
       take_while have their own walk from the bounded end: routing them
       through to_a turned a working search into a RangeError (#3863). The
       other names have no such walk and keep the (faithfully raising) hop. */
    if (rt == TY_RANGE &&
        (sp_streq(nm, "find") || sp_streq(nm, "detect") || sp_streq(nm, "take_while"))) {
      int rn7 = recv;
      while (rn7 >= 0 && nt_type(nt, rn7) && sp_streq(nt_type(nt, rn7), "ParenthesesNode")) {
        int pb7 = nt_ref(nt, rn7, "body"); int pn7 = 0;
        const int *pp7 = pb7 >= 0 ? nt_arr(nt, pb7, "body", &pn7) : NULL;
        rn7 = pn7 == 1 ? pp7[0] : -1;
      }
      if (rn7 >= 0 && nt_type(nt, rn7) && sp_streq(nt_type(nt, rn7), "RangeNode") &&
          nt_ref(nt, rn7, "right") < 0) continue;
    }
    /* Only where the call found no arm at all. A form that IS wired -- a
       blockless each_slice answering an Enumerator, say -- has a type, and
       rerouting it through to_a would answer an Array instead. */
    if (comp_ntype(c, id) != TY_UNKNOWN) continue;
    /* A blockless each_* over a Range has an arm already, and its Enumerator
       keeps the Range as its source (`(1..6).each_slice(2)` inspects as
       `1..6:each_slice(2)`); routing it would answer the elements instead. A
       Hash has no such arm, so it routes either way. */
    if (how == 2 && rt == TY_RANGE && nt_ref(nt, id, "block") < 0) continue;
    int base = nt->count;
    int toa = nt_new_node(nt, "CallNode");
    if (toa < 0) continue;
    nt_node_set_ref(nt, toa, "receiver", recv);
    nt_node_set_str(nt, toa, "name", "to_a");
    nt_node_set_ref(nt, toa, "arguments", -1);
    nt_node_set_ref(nt, toa, "block", -1);
    /* The with-block group answers the RECEIVER, not the pairs it walked, so
       mark the synthesized hop: inference and the value emitter read it to
       yield the original Hash / Range (#3842). A `to_a` the program wrote
       itself carries no mark and keeps answering its array. */
    if (how == 2) nt_node_set_str(nt, toa, "enum_recv", "1");
    nt_node_set_ref(nt, id, "receiver", toa);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[id];
    changed = 1;
  }
  return changed;
}

/* `def m(a, ...) = callee(a, ...)` with a rest/kwrest callee becomes
   `def m(a, *, **) = callee(a, *, **)`. The __fwd_N model (#1288) binds the
   callee's params positionally, which is exact for fixed params and flattens
   a rest/kwrest. `*` / `**` follow the callee's params; blocks are not
   forwarded by this form, so any block involvement keeps the __fwd_N model. */
static int fwd_node_is(const NodeTable *nt, int id, const char *ty) {
  return id >= 0 && nt_type(nt, id) && sp_streq(nt_type(nt, id), ty);
}
/* highest node id under `id`; ids are pre-order, so [id, max] is the subtree */
static int fwd_subtree_max(const NodeTable *nt, int id) {
  if (id < 0 || id >= nt->count) return -1;
  const SpNode *nd = &nt->nodes[id];
  int mx = id;
  for (int j = 0; j < nd->nr; j++) { int m = fwd_subtree_max(nt, nd->r[j].ref); if (m > mx) mx = m; }
  for (int j = 0; j < nd->na; j++)
    for (int k = 0; k < nd->a[j].n; k++) { int m = fwd_subtree_max(nt, nd->a[j].ids[k]); if (m > mx) mx = m; }
  return mx;
}
static int fwd_subtree_uses_yield_or_block(const NodeTable *nt, int def) {
  int pn = nt_ref(nt, def, "parameters");
  if (pn >= 0 && nt_ref(nt, pn, "block") >= 0) return 1;
  int hi = fwd_subtree_max(nt, def);
  for (int id = def; id <= hi; id++) {
    if (fwd_node_is(nt, id, "YieldNode")) return 1;
    if (fwd_node_is(nt, id, "CallNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm && sp_streq(nm, "block_given?")) return 1;
    }
  }
  return 0;
}
/* bit 1 = positional forwarding, bit 2 = keyword forwarding, bit 4 =
   block/yield; -1 no def, -2 defs disagree. Fixed parameters count too:
   forwarding to `def f(*a, k: 0)` needs both channels, as does forwarding to
   `def f(x, **k)`. */
static int def_shape_by_name(const NodeTable *nt, const char *name) {
  int shape = -1;
  for (int id = 0; id < nt->count; id++) {
    if (!fwd_node_is(nt, id, "DefNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, name)) continue;
    int pn = nt_ref(nt, id, "parameters");
    int sh = 0;
    if (pn >= 0) {
      int rn = 0; nt_arr(nt, pn, "requireds", &rn);
      int on = 0; nt_arr(nt, pn, "optionals", &on);
      int postn = 0; nt_arr(nt, pn, "posts", &postn);
      int kn = 0; nt_arr(nt, pn, "keywords", &kn);
      if (rn > 0 || on > 0 || postn > 0) sh |= 1;
      if (kn > 0) sh |= 2;
      if (fwd_node_is(nt, nt_ref(nt, pn, "rest"), "RestParameterNode")) sh |= 1;
      if (fwd_node_is(nt, nt_ref(nt, pn, "keyword_rest"), "KeywordRestParameterNode")) sh |= 2;
    }
    if (fwd_subtree_uses_yield_or_block(nt, id)) sh |= 4;
    if (shape >= 0 && shape != sh) return -2;
    shape = sh;
  }
  return shape;
}
static int fwd_new_node_like(NodeTable *nt, int like, const char *ty) {
  int id = nt_new_node(nt, ty);
  if (id < 0) return -1;
  nt_node_set_int(nt, id, "node_line", nt_int(nt, like, "node_line", 0));
  nt_node_set_int(nt, id, "node_file", nt_int(nt, like, "node_file", 0));
  nt_node_set_int(nt, id, "node_col", nt_int(nt, like, "node_col", 0));
  return id;
}
static int any_call_passes_block(const NodeTable *nt, const char *name) {
  for (int id = 0; id < nt->count; id++) {
    if (!fwd_node_is(nt, id, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (nm && sp_streq(nm, name) && nt_ref(nt, id, "block") >= 0) return 1;
  }
  return 0;
}
int desugar_forwarding_to_rest_callee(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int def = 0; def < n0; def++) {
    if (!fwd_node_is(nt, def, "DefNode")) continue;
    int pn = nt_ref(nt, def, "parameters");
    if (pn < 0 || !fwd_node_is(nt, nt_ref(nt, pn, "keyword_rest"), "ForwardingParameterNode")) continue;
    const char *dname = nt_str(nt, def, "name");
    if (!dname) continue;
    int hi = fwd_subtree_max(nt, def) + 1;
    int calls[n0]; int ncalls = 0; int shape = 0; int ok = 1;
    for (int id = def + 1; id < hi && id < n0; id++) {
      if (!fwd_node_is(nt, id, "CallNode")) continue;
      int args = nt_ref(nt, id, "arguments");
      int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
      if (ac < 1 || !av || !fwd_node_is(nt, av[ac - 1], "ForwardingArgumentsNode")) continue;
      const char *cn = nt_str(nt, id, "name");
      int sh = (nt_ref(nt, id, "receiver") < 0 && cn) ? def_shape_by_name(nt, cn) : -1;
      if (sh < 0 || nt_ref(nt, id, "block") >= 0) { ok = 0; break; }
      if (ncalls && sh != shape) { ok = 0; break; }
      shape = sh;
      calls[ncalls++] = id;
    }
    if (!ok || !ncalls || !(shape & 3) || (shape & 4)) continue;
    if (any_call_passes_block(nt, dname)) continue;
    int base = nt->count;
    /* def m(a, ...) -> def m(a, *, **) */
    if (shape & 1) {
      int rp = fwd_new_node_like(nt, pn, "RestParameterNode");
      if (rp < 0) continue;
      nt_node_set_ref(nt, pn, "rest", rp);
    }
    if (shape & 2) {
      int kp = fwd_new_node_like(nt, pn, "KeywordRestParameterNode");
      if (kp < 0) continue;
      nt_node_set_ref(nt, pn, "keyword_rest", kp);
    } else nt_node_set_ref(nt, pn, "keyword_rest", -1);
    /* callee(x, ...) -> callee(x, *, **) */
    for (int k = 0; k < ncalls; k++) {
      int call = calls[k];
      int args = nt_ref(nt, call, "arguments");
      int ac = 0; const int *av = nt_arr(nt, args, "arguments", &ac);
      int nargs[ac + 1]; int nn = 0;
      for (int i = 0; i < ac - 1; i++) nargs[nn++] = av[i];
      if (shape & 1) {
        int sp = fwd_new_node_like(nt, call, "SplatNode");
        if (sp < 0) continue;
        nt_node_set_ref(nt, sp, "expression", -1);
        nargs[nn++] = sp;
      }
      if (shape & 2) {
        int kh = fwd_new_node_like(nt, call, "KeywordHashNode");
        int as = fwd_new_node_like(nt, call, "AssocSplatNode");
        if (kh < 0 || as < 0) continue;
        nt_node_set_ref(nt, as, "value", -1);
        nt_node_set_arr(nt, kh, "elements", &as, 1);
        nargs[nn++] = kh;
      }
      nt_node_set_arr(nt, args, "arguments", nargs, nn);
    }
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[def];
    changed = 1;
  }
  return changed;
}
