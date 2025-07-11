#include "build.h"

#include "../alloc.h"
#include "../hashtbl.h"
#include "../log.h"
#include "../typechk.h"
#include "../util.h"
#include "../var_table.h"
#include "../vector.h"
#include "ir.h"
#include "var_refs.h"

// break/continues will add an entry into the jumps vector
// and then at the end of the loop these will be traversed and fixed to point to
// the correct basicblock the special value IR_JUMP_TY_NEW_LOOP indicates the
// start of a loop
enum ir_jump_ty {
  IR_JUMP_TY_NEW_LOOP,

  IR_JUMP_TY_BREAK,
  IR_JUMP_TY_CONTINUE
};

struct ir_jump {
  enum ir_jump_ty ty;

  struct ir_basicblock *basicblock;
};

enum ir_case_ty { IR_CASE_TY_NEW_SWITCH, IR_CASE_TY_CASE, IR_CASE_TY_DEFAULT };

struct ir_case {
  enum ir_case_ty ty;

  struct ir_split_case split_case;
};

// linked list of label -> bb mappings
struct ir_label {
  const char *name;
  struct ir_basicblock *basicblock;

  struct ir_label *succ;
};

enum ir_defer_ty {
  IR_DEFER_TY_DEFER,
  IR_DEFER_TY_NEW_SCOPE,
};

struct ir_defer {
  enum ir_defer_ty ty;

  struct td_deferstmt *deferstmt;
};

struct ir_var_builder {
  struct arena_allocator *arena;
  struct typechk *tchk;

  struct ir_unit *unit;
  struct ir_var *var;

  struct var_refs *global_var_refs;
};

struct ir_func_builder {
  enum ir_build_flags flags;

  struct arena_allocator *arena;
  struct typechk *tchk;

  struct ir_unit *unit;
  struct ir_func *func;

  // the global for `__func__`
  struct ir_glb *func_name_cnst;

  struct var_refs *var_refs;
  struct var_refs *global_var_refs;

  struct ir_label *labels;

  struct hashtbl *var_writes;
  struct hashtbl *phis;

  struct vector *jumps;
  struct vector *switch_cases;
  struct vector *defers;
};

static struct ir_label *add_label(struct ir_func_builder *irb, ustr_t name,
                                  struct ir_basicblock *basicblock) {
  struct ir_label *label = aralloc(irb->arena, sizeof(*label));

  label->name = aralloc_strndup(irb->arena, name.str, name.len);
  label->basicblock = basicblock;
  label->succ = irb->labels;

  irb->labels = label;

  return label;
}

static struct var_key get_var_key(const struct td_var *var,
                                  struct ir_basicblock *basicblock) {
  return (struct var_key){var->identifier, var->scope,
                          .basicblock = basicblock};
}

static void get_var_ref(struct ir_func_builder *irb,
                        struct ir_basicblock *basicblock,
                        const struct td_var *var, struct var_key *key,
                        struct var_ref **ref) {
  *ref = NULL;

  // this is when we are _reading_ from the var
  *key = get_var_key(var, basicblock);

  *ref = var_refs_get(irb->var_refs, key);

  if (*ref) {
    return;
  }

  *ref = var_refs_get(irb->var_refs, key);
  if (*ref && (*ref)->op->lcl) {
    return;
  }

  *ref = var_refs_get(irb->global_var_refs, key);
  if (*ref) {
    return;
  }

  // HACK: because functions always have global scope, when we look up at global
  // level force scope
  struct var_key glb = *key;
  glb.scope = SCOPE_GLOBAL;

  *ref = var_refs_get(irb->global_var_refs, &glb);
  if (*ref) {
    *key = glb;
  }
}

static bool ir_var_ty_needs_cast_op(struct ir_func_builder *irb,
                                    const struct ir_var_ty *l,
                                    const struct ir_var_ty *r) {
  // note: `l` is TO, `r` is FROM, (as this is in the context of `l <- r`)

  if (l->ty == IR_VAR_TY_TY_NONE) {
    // void casts are nop
    return false;
  }

  if (ir_var_ty_is_aggregate(l) && ir_var_ty_is_aggregate(r)) {
    // casting between these could require conversion,
    // but never a cast op
    return false;
  }

  if (ir_var_ty_eq(l, r)) {
    return false;
  }

  if ((l->ty == IR_VAR_TY_TY_FUNC && r->ty == IR_VAR_TY_TY_POINTER) ||
      (r->ty == IR_VAR_TY_TY_FUNC && l->ty == IR_VAR_TY_TY_POINTER)) {
    return false;
  }

  if ((l->ty == IR_VAR_TY_TY_POINTER || l->ty == IR_VAR_TY_TY_ARRAY) &&
      (r->ty == IR_VAR_TY_TY_POINTER || r->ty == IR_VAR_TY_TY_ARRAY)) {
    // pointers/arrays need no cast instr
    return false;
  }

  struct ir_var_ty ptr_ty = ir_var_ty_for_pointer_size(irb->unit);

  struct ir_var_ty_info ptr_info = ir_var_ty_info(irb->unit, &ptr_ty);
  size_t pointer_size = ptr_info.size;

  if (l->ty == IR_VAR_TY_TY_PRIMITIVE &&
      l->primitive == IR_VAR_PRIMITIVE_TY_I1) {
    return true;
  }

  if (l->ty == IR_VAR_TY_TY_PRIMITIVE &&
      l->primitive == IR_VAR_PRIMITIVE_TY_I8 &&
      r->ty == IR_VAR_TY_TY_PRIMITIVE &&
      r->primitive == IR_VAR_PRIMITIVE_TY_I1) {
    // i1 -> i8 is nop as bool must be 0/1
    return false;
  }

  if (((l->ty == IR_VAR_TY_TY_PRIMITIVE && ir_var_ty_is_integral(l) &&
        ir_var_ty_info(irb->unit, l).size == pointer_size) ||
       l->ty == IR_VAR_TY_TY_POINTER) &&
      ((r->ty == IR_VAR_TY_TY_PRIMITIVE && ir_var_ty_is_integral(r) &&
        ir_var_ty_info(irb->unit, r).size == pointer_size) ||
       r->ty == IR_VAR_TY_TY_POINTER)) {
    // same size int -> pointer needs no cast
    return false;
  }

  return true;
}

static enum ir_var_primitive_ty
ir_primitive_ty_for_well_known_ty(struct ir_unit *iru, enum well_known_ty wkt) {
  switch (wkt) {
  case WELL_KNOWN_TY_BOOL:
    return IR_VAR_PRIMITIVE_TY_I1;
  case WELL_KNOWN_TY_CHAR:
  case WELL_KNOWN_TY_SIGNED_CHAR:
  case WELL_KNOWN_TY_UNSIGNED_CHAR:
    return IR_VAR_PRIMITIVE_TY_I8;
  case WELL_KNOWN_TY_SIGNED_SHORT:
  case WELL_KNOWN_TY_UNSIGNED_SHORT:
    return IR_VAR_PRIMITIVE_TY_I16;
  case WELL_KNOWN_TY_SIGNED_INT:
  case WELL_KNOWN_TY_UNSIGNED_INT:
    return IR_VAR_PRIMITIVE_TY_I32;
  case WELL_KNOWN_TY_SIGNED_LONG:
  case WELL_KNOWN_TY_UNSIGNED_LONG:
    switch (iru->target->lp_sz) {
    case TARGET_LP_SZ_LP32:
      return IR_VAR_PRIMITIVE_TY_I32;
    case TARGET_LP_SZ_LP64:
      return IR_VAR_PRIMITIVE_TY_I64;
    }
  case WELL_KNOWN_TY_SIGNED_LONG_LONG:
  case WELL_KNOWN_TY_UNSIGNED_LONG_LONG:
    return IR_VAR_PRIMITIVE_TY_I64;
  case WELL_KNOWN_TY_HALF:
    return IR_VAR_PRIMITIVE_TY_F16;
  case WELL_KNOWN_TY_FLOAT:
    return IR_VAR_PRIMITIVE_TY_F32;
  case WELL_KNOWN_TY_DOUBLE:
  case WELL_KNOWN_TY_LONG_DOUBLE:
    return IR_VAR_PRIMITIVE_TY_F64;
  case WELL_KNOWN_TY_INT128:
  case WELL_KNOWN_TY_UINT128:
    return IR_VAR_PRIMITIVE_TY_I128;
  }
}

static struct ir_var_ty
ir_var_ty_for_td_var_ty_impl(struct ir_unit *iru,
                             const struct td_var_ty *var_ty,
                             bool allow_incomplete) {
  switch (var_ty->ty) {
  case TD_VAR_TY_TY_UNKNOWN:
    BUG("shouldn't reach IR gen with unknown type");
  case TD_VAR_TY_TY_INCOMPLETE_AGGREGATE:
    if (allow_incomplete) {
      // FIXME: is this okay? we just lie and say pointer
      return IR_VAR_TY_POINTER;
    }
    BUG("shouldn't reach IR gen with incomplete type (%.*s)",
        (int)var_ty->incomplete_aggregate.name.len,
        var_ty->incomplete_aggregate.name.str);
  case TD_VAR_TY_TY_AGGREGATE: {
    struct td_ty_aggregate aggregate = var_ty->aggregate;

    struct ir_var_ty ty;
    switch (aggregate.ty) {
    case TD_TY_AGGREGATE_TY_STRUCT:
      ty.ty = IR_VAR_TY_TY_STRUCT;
      ty.aggregate.num_fields = aggregate.num_fields;
      ty.aggregate.fields = aralloc(iru->arena, sizeof(struct ir_var_ty) *
                                                    ty.aggregate.num_fields);

      for (size_t i = 0; i < ty.aggregate.num_fields; i++) {
        // handle nested types

        ty.aggregate.fields[i] = ir_var_ty_for_td_var_ty_impl(
            iru, &aggregate.fields[i].var_ty, allow_incomplete);
      }
      break;
    case TD_TY_AGGREGATE_TY_UNION:
      ty.ty = IR_VAR_TY_TY_UNION;
      ty.aggregate.num_fields = aggregate.num_fields;
      ty.aggregate.fields = aralloc(iru->arena, sizeof(struct ir_var_ty) *
                                                    ty.aggregate.num_fields);

      for (size_t i = 0; i < ty.aggregate.num_fields; i++) {
        // handle nested types

        ty.aggregate.fields[i] = ir_var_ty_for_td_var_ty_impl(
            iru, &aggregate.fields[i].var_ty, allow_incomplete);
      }
      break;
    }

    return ty;
  }
  case TD_VAR_TY_TY_VOID:
    return IR_VAR_TY_NONE;
  case TD_VAR_TY_TY_VARIADIC:
    return IR_VAR_TY_VARIADIC;
  case TD_VAR_TY_TY_WELL_KNOWN: {
    struct ir_var_ty ty;
    ty.ty = IR_VAR_TY_TY_PRIMITIVE;
    ty.primitive = ir_primitive_ty_for_well_known_ty(iru, var_ty->well_known);
    return ty;
  }
  case TD_VAR_TY_TY_FUNC: {
    bool variadic = var_ty->func.ty == TD_TY_FUNC_TY_VARIADIC;

    struct ir_var_ty ty;
    ty.ty = IR_VAR_TY_TY_FUNC;
    ty.func.ret_ty = aralloc(iru->arena, sizeof(*ty.func.ret_ty));
    *ty.func.ret_ty =
        ir_var_ty_for_td_var_ty_impl(iru, var_ty->func.ret, allow_incomplete);

    // from IR onwards, variadic is no longer a param of the function but
    // instead a flag
    ty.func.num_params = var_ty->func.num_params;
    ty.func.params =
        aralloc(iru->arena, sizeof(struct ir_op) * ty.func.num_params);

    ty.func.flags = IR_VAR_FUNC_TY_FLAG_NONE;
    if (variadic) {
      ty.func.flags |= IR_VAR_FUNC_TY_FLAG_VARIADIC;
    }

    for (size_t i = 0; i < ty.func.num_params; i++) {
      ty.func.params[i] = ir_var_ty_for_td_var_ty_impl(
          iru, &var_ty->func.params[i].var_ty, allow_incomplete);
    }

    return ty;
  }
  case TD_VAR_TY_TY_POINTER: {
    return IR_VAR_TY_POINTER;
  }
  case TD_VAR_TY_TY_ARRAY: {
    struct ir_var_ty underlying = ir_var_ty_for_td_var_ty_impl(
        iru, var_ty->array.underlying, allow_incomplete);

    return ir_var_ty_mk_array(iru, &underlying, var_ty->array.size);
  }
  }
}

static struct ir_var_ty
ir_var_ty_for_td_var_ty(struct ir_unit *iru, const struct td_var_ty *var_ty) {
  return ir_var_ty_for_td_var_ty_impl(iru, var_ty, false);
}

static struct ir_var_ty
ir_var_ty_for_decl_td_var_ty(struct ir_unit *iru,
                             const struct td_var_ty *var_ty) {
  return ir_var_ty_for_td_var_ty_impl(iru, var_ty, true);
}

UNUSED struct ir_var_ty static var_ty_return_ty_for_td_var_ty(
    struct ir_func_builder *irb, const struct td_var_ty *ty_ref) {
  invariant_assert(ty_ref->ty == TD_VAR_TY_TY_FUNC,
                   "passed non-func to `return_ty_for_td_var_ty`");

  struct ir_var_ty func_ty = ir_var_ty_for_td_var_ty(irb->unit, ty_ref);
  return *func_ty.func.ret_ty;
}

struct ir_cast_info {
  bool cmp_nz; // don't generate `cast` IR; generate `>= 0` instead
  enum ir_op_cast_op_ty cast_ty;
};

static struct ir_cast_info cast_ty_for_td_var_ty(struct ir_func_builder *irb,
                                                 const struct td_var_ty *from,
                                                 const struct td_var_ty *to) {
  struct ir_var_ty from_var_ty = ir_var_ty_for_td_var_ty(irb->unit, from);
  struct ir_var_ty to_var_ty = ir_var_ty_for_td_var_ty(irb->unit, to);

  if (from_var_ty.ty == IR_VAR_TY_TY_POINTER &&
      to_var_ty.ty == IR_VAR_TY_TY_POINTER) {
    BUG("cast between pointer types is implicit");
  }

  if (to_var_ty.ty == IR_VAR_TY_TY_PRIMITIVE &&
      to_var_ty.primitive == IR_VAR_PRIMITIVE_TY_I1) {
    return (struct ir_cast_info){.cmp_nz = true};
  }

  if (from_var_ty.ty == IR_VAR_TY_TY_PRIMITIVE &&
      to_var_ty.ty == IR_VAR_TY_TY_POINTER) {
    // primitive -> pointer
    enum ir_var_primitive_ty pointer =
        ir_var_ty_pointer_primitive_ty(irb->unit);
    if (from_var_ty.primitive == pointer) {
      BUG("cast between primitive & pointer type of same size is implicit");
    }

    if (pointer < from_var_ty.primitive) {
      return (struct ir_cast_info){.cmp_nz = false,
                                   .cast_ty = IR_OP_CAST_OP_TY_TRUNC};
    } else if (WKT_IS_SIGNED(from->well_known)) {
      return (struct ir_cast_info){.cmp_nz = false,
                                   .cast_ty = IR_OP_CAST_OP_TY_SEXT};
    } else {
      return (struct ir_cast_info){.cmp_nz = false,
                                   .cast_ty = IR_OP_CAST_OP_TY_ZEXT};
    }
  }

  if (from_var_ty.ty == IR_VAR_TY_TY_POINTER &&
      to_var_ty.ty == IR_VAR_TY_TY_PRIMITIVE) {
    return (struct ir_cast_info){.cmp_nz = false,
                                 .cast_ty = IR_OP_CAST_OP_TY_TRUNC};
  }

  if (from_var_ty.ty != IR_VAR_TY_TY_PRIMITIVE ||
      to_var_ty.ty != IR_VAR_TY_TY_PRIMITIVE) {
    TODO("casts for non prims/pointers (from %u -> %u)", from_var_ty.ty,
         to_var_ty.ty);
  }

  if (td_var_ty_is_fp_ty(from) && td_var_ty_is_fp_ty(to)) {
    return (struct ir_cast_info){.cmp_nz = false,
                                 .cast_ty = IR_OP_CAST_OP_TY_CONV};
  }

  if (td_var_ty_is_fp_ty(from) || td_var_ty_is_fp_ty(to)) {
    // one (but not both) is fp
    // we need to generate `uconv`/`iconv` depending on the sign of the integral
    // type

    invariant_assert(from->ty == TD_VAR_TY_TY_WELL_KNOWN ||
                         to->ty == TD_VAR_TY_TY_WELL_KNOWN,
                     "other type must be an integer for float conversion");

    bool is_signed = td_var_ty_is_fp_ty(from) ? WKT_IS_SIGNED(to->well_known)
                                              : WKT_IS_SIGNED(from->well_known);

    return (struct ir_cast_info){.cmp_nz = false,
                                 .cast_ty = is_signed ? IR_OP_CAST_OP_TY_SCONV
                                                      : IR_OP_CAST_OP_TY_UCONV};
  }

  if (to_var_ty.primitive < from_var_ty.primitive) {
    return (struct ir_cast_info){.cmp_nz = false,
                                 .cast_ty = IR_OP_CAST_OP_TY_TRUNC};
  } else {
    invariant_assert(from_var_ty.primitive != to_var_ty.primitive,
                     "cast not needed for types of same size");
    if (WKT_IS_SIGNED(from->well_known)) {
      return (struct ir_cast_info){.cmp_nz = false,
                                   .cast_ty = IR_OP_CAST_OP_TY_SEXT};
    } else {
      return (struct ir_cast_info){.cmp_nz = false,
                                   .cast_ty = IR_OP_CAST_OP_TY_ZEXT};
    }
  }
}

static struct ir_op *build_ir_for_expr(struct ir_func_builder *irb,
                                       struct ir_stmt **stmt,
                                       struct td_expr *expr);

static struct ir_op *insert_ir_for_cast(struct ir_func_builder *irb,
                                        struct ir_stmt *stmt, struct ir_op *op,
                                        const struct ir_var_ty *to,
                                        struct ir_cast_info info) {

  if (info.cmp_nz) {
    struct ir_op *zero = ir_alloc_op(irb->func, stmt);
    ir_mk_zero_constant(irb->unit, zero, &op->var_ty);

    struct ir_op *cast = ir_alloc_op(irb->func, stmt);
    cast->ty = IR_OP_TY_BINARY_OP;
    cast->var_ty = *to;
    cast->binary_op = (struct ir_op_binary_op){
        .ty = ir_var_ty_is_fp(&op->var_ty) ? IR_OP_BINARY_OP_TY_FNEQ
                                           : IR_OP_BINARY_OP_TY_NEQ,
        .lhs = op,
        .rhs = zero};

    return cast;
  } else {
    struct ir_op *cast = ir_alloc_op(irb->func, stmt);
    cast->ty = IR_OP_TY_CAST_OP;
    cast->var_ty = *to;
    cast->cast_op.ty = info.cast_ty;
    cast->cast_op.value = op;

    return cast;
  }
}

static struct ir_op *insert_ir_for_cast_if_needed(struct ir_func_builder *irb,
                                                  struct ir_stmt *stmt,
                                                  struct ir_op *op,
                                                  const struct td_var_ty *from,
                                                  const struct td_var_ty *to) {
  struct ir_var_ty from_ir = ir_var_ty_for_td_var_ty(irb->unit, from);
  struct ir_var_ty to_ir = ir_var_ty_for_td_var_ty(irb->unit, to);

  if (ir_var_ty_needs_cast_op(irb, &to_ir, &from_ir)) {
    return insert_ir_for_cast(irb, stmt, op, &to_ir,
                              cast_ty_for_td_var_ty(irb, from, to));
  } else {
    op->var_ty = to_ir;
    return op;
  }
}

struct ir_build_binaryop {
  enum td_binary_op_ty ty;
  struct td_var_ty result_ty;

  struct td_var_ty lhs_ty, rhs_ty;
  struct ir_op *lhs, *rhs;
};

static struct ir_op *alloc_binaryop(struct ir_func_builder *irb,
                                    struct ir_stmt *stmt,
                                    const struct ir_build_binaryop *args) {

  enum td_binary_op_ty ty = args->ty;
  struct td_var_ty lhs_ty = args->lhs_ty;
  struct td_var_ty rhs_ty = args->rhs_ty;
  struct ir_op *lhs = args->lhs;
  struct ir_op *rhs = args->rhs;
  const struct td_var_ty *td_var_ty = &args->result_ty;

  invariant_assert(lhs->var_ty.ty != IR_VAR_TY_TY_ARRAY ||
                       rhs->var_ty.ty != IR_VAR_TY_TY_ARRAY,
                   "array should have decayed to ptr");

  struct ir_var_ty var_ty = ir_var_ty_for_td_var_ty(irb->unit, td_var_ty);

  if (!td_binary_op_is_comparison(ty) && (lhs_ty.ty == TD_VAR_TY_TY_POINTER ||
                                          rhs_ty.ty == TD_VAR_TY_TY_POINTER)) {
    if (td_var_ty->ty == TD_VAR_TY_TY_WELL_KNOWN) {
      struct td_var_ty *pointer_ty =
          lhs_ty.ty == TD_VAR_TY_TY_POINTER ? &lhs_ty : &rhs_ty;

      struct ir_var_ty el_ty =
          ir_var_ty_for_td_var_ty(irb->unit, pointer_ty->pointer.underlying);
      struct ir_var_ty_info el_info = ir_var_ty_info(irb->unit, &el_ty);

      struct ir_op *el_size_op = ir_alloc_op(irb->func, stmt);
      ir_mk_pointer_constant(irb->unit, el_size_op, el_info.size);

      struct ir_op *diff = ir_alloc_op(irb->func, stmt);
      diff->ty = IR_OP_TY_BINARY_OP;
      diff->var_ty = var_ty;
      diff->binary_op.ty = IR_OP_BINARY_OP_TY_SUB;
      diff->binary_op.lhs = lhs;
      diff->binary_op.rhs = rhs;

      struct ir_op *op = ir_alloc_op(irb->func, stmt);
      op->ty = IR_OP_TY_BINARY_OP;
      op->var_ty = var_ty;
      op->binary_op.ty = IR_OP_BINARY_OP_TY_SDIV;
      op->binary_op.lhs = diff;
      op->binary_op.rhs = el_size_op;

      return op;
    } else if (ty == TD_BINARY_OP_TY_ADD) {
      DEBUG_ASSERT(td_var_ty->ty == TD_VAR_TY_TY_POINTER, "non pointer");

      // need to multiply rhs by the element size
      struct ir_var_ty el_ty =
          ir_var_ty_for_td_var_ty(irb->unit, td_var_ty->pointer.underlying);
      struct ir_var_ty_info el_info = ir_var_ty_info(irb->unit, &el_ty);

      struct ir_op *op = ir_alloc_op(irb->func, stmt);
      op->ty = IR_OP_TY_ADDR_OFFSET;
      op->var_ty = var_ty;
      op->addr_offset = (struct ir_op_addr_offset){
          .base = lhs, .index = rhs, .scale = el_info.size};

      return op;
    } else {
      // generate binary op for pointer sub. we could try and make `addr_offset`
      // unsigned but involves codegen changes

      struct ir_var_ty el_ty =
          ir_var_ty_for_td_var_ty(irb->unit, td_var_ty->pointer.underlying);
      struct ir_var_ty_info el_info = ir_var_ty_info(irb->unit, &el_ty);

      struct ir_op *el_size_op = ir_alloc_op(irb->func, stmt);
      ir_mk_pointer_constant(irb->unit, el_size_op, el_info.size);

      struct ir_op *rhs_mul = ir_alloc_op(irb->func, stmt);
      rhs_mul->ty = IR_OP_TY_BINARY_OP;
      rhs_mul->var_ty = var_ty;
      rhs_mul->binary_op.ty = IR_OP_BINARY_OP_TY_MUL;
      rhs_mul->binary_op.lhs = el_size_op;
      rhs_mul->binary_op.rhs = rhs;

      struct ir_op *op = ir_alloc_op(irb->func, stmt);
      op->ty = IR_OP_TY_BINARY_OP;
      op->var_ty = var_ty;
      op->binary_op.ty = IR_OP_BINARY_OP_TY_SUB;
      op->binary_op.lhs = lhs;
      op->binary_op.rhs = rhs_mul;

      return op;
    }
  }

  struct ir_op *op = ir_alloc_op(irb->func, stmt);
  op->ty = IR_OP_TY_BINARY_OP;
  op->var_ty = var_ty;

  struct ir_op_binary_op *b = &op->binary_op;

  b->lhs = lhs;
  b->rhs = rhs;

  bool is_fp = ir_var_ty_is_fp(&op->binary_op.lhs->var_ty);

  bool is_sgn = args->lhs_ty.ty == TD_VAR_TY_TY_WELL_KNOWN &&
                WKT_IS_SIGNED(args->lhs_ty.well_known);
  DEBUG_ASSERT(is_fp == ir_var_ty_is_fp(&op->binary_op.rhs->var_ty),
               "type mismatch between lhs/rhs");
  DEBUG_ASSERT(is_fp == ir_var_ty_is_fp(&op->binary_op.rhs->var_ty),
               "type mismatch between lhs/rhs");

  invariant_assert(
      td_var_ty->ty == TD_VAR_TY_TY_WELL_KNOWN ||
          td_var_ty->ty == TD_VAR_TY_TY_POINTER,
      "non primitives/well-knowns/pointers cannot be used in binary "
      "expression by point IR is reached!");

  switch (ty) {
  case TD_BINARY_OP_TY_LOGICAL_AND:
  case TD_BINARY_OP_TY_LOGICAL_OR:
    BUG("logical and/or must be handled outside (as they need basicblock "
        "adjustment)");
  case TD_BINARY_OP_TY_EQ:
    b->ty = is_fp ? IR_OP_BINARY_OP_TY_FEQ : IR_OP_BINARY_OP_TY_EQ;
    break;
  case TD_BINARY_OP_TY_NEQ:
    b->ty = is_fp ? IR_OP_BINARY_OP_TY_FNEQ : IR_OP_BINARY_OP_TY_NEQ;
    break;
  case TD_BINARY_OP_TY_GT:
    if (is_fp) {
      b->ty = IR_OP_BINARY_OP_TY_FGT;
    } else if (is_sgn) {
      b->ty = IR_OP_BINARY_OP_TY_SGT;
    } else {
      b->ty = IR_OP_BINARY_OP_TY_UGT;
    }
    break;
  case TD_BINARY_OP_TY_GTEQ:
    if (is_fp) {
      b->ty = IR_OP_BINARY_OP_TY_FGTEQ;
    } else if (is_sgn) {
      b->ty = IR_OP_BINARY_OP_TY_SGTEQ;
    } else {
      b->ty = IR_OP_BINARY_OP_TY_UGTEQ;
    }
    break;
  case TD_BINARY_OP_TY_LT:
    if (is_fp) {
      b->ty = IR_OP_BINARY_OP_TY_FLT;
    } else if (is_sgn) {
      b->ty = IR_OP_BINARY_OP_TY_SLT;
    } else {
      b->ty = IR_OP_BINARY_OP_TY_ULT;
    }
    break;
  case TD_BINARY_OP_TY_LTEQ:
    if (is_fp) {
      b->ty = IR_OP_BINARY_OP_TY_FLTEQ;
    } else if (is_sgn) {
      b->ty = IR_OP_BINARY_OP_TY_SLTEQ;
    } else {
      b->ty = IR_OP_BINARY_OP_TY_ULTEQ;
    }
    break;
  case TD_BINARY_OP_TY_RSHIFT:
    if (is_sgn) {
      b->ty = IR_OP_BINARY_OP_TY_SRSHIFT;
    } else {
      b->ty = IR_OP_BINARY_OP_TY_URSHIFT;
    }
    break;
  case TD_BINARY_OP_TY_LSHIFT:
    b->ty = IR_OP_BINARY_OP_TY_LSHIFT;
    break;
  case TD_BINARY_OP_TY_AND:
    b->ty = IR_OP_BINARY_OP_TY_AND;
    break;
  case TD_BINARY_OP_TY_OR:
    b->ty = IR_OP_BINARY_OP_TY_OR;
    break;
  case TD_BINARY_OP_TY_XOR:
    b->ty = IR_OP_BINARY_OP_TY_XOR;
    break;
  case TD_BINARY_OP_TY_ADD:
    b->ty = is_fp ? IR_OP_BINARY_OP_TY_FADD : IR_OP_BINARY_OP_TY_ADD;
    break;
  case TD_BINARY_OP_TY_SUB:
    b->ty = is_fp ? IR_OP_BINARY_OP_TY_FSUB : IR_OP_BINARY_OP_TY_SUB;
    break;
  case TD_BINARY_OP_TY_MUL:
    b->ty = is_fp ? IR_OP_BINARY_OP_TY_FMUL : IR_OP_BINARY_OP_TY_MUL;
    break;
  case TD_BINARY_OP_TY_DIV:
    if (is_fp) {
      b->ty = IR_OP_BINARY_OP_TY_FDIV;
    } else if (is_sgn) {
      b->ty = IR_OP_BINARY_OP_TY_SDIV;
    } else {
      b->ty = IR_OP_BINARY_OP_TY_UDIV;
    }
    break;
  case TD_BINARY_OP_TY_MOD:
    if (is_sgn) {
      b->ty = IR_OP_BINARY_OP_TY_SMOD;
    } else {
      b->ty = IR_OP_BINARY_OP_TY_UMOD;
    }
    break;
  }

  return op;
}

static struct ir_op *build_ir_for_array_address(struct ir_func_builder *irb,
                                                struct ir_stmt **stmt,
                                                struct td_expr *lhs_expr,
                                                struct td_expr *rhs_expr);

static struct ir_op *
build_ir_for_member_address(struct ir_func_builder *irb, struct ir_stmt **stmt,
                            struct td_expr *lhs_expr, ustr_t member_name,
                            bool *member_is_bitfield,
                            struct ir_bitfield *member_bitfield);

static struct ir_op *
build_ir_for_pointer_address(struct ir_func_builder *irb, struct ir_stmt **stmt,
                             struct td_expr *lhs_expr, ustr_t member_name,
                             bool *member_is_bitfield,
                             struct ir_bitfield *member_bitfield);

static struct ir_op *build_ir_for_var(struct ir_func_builder *irb,
                                      struct ir_stmt **stmt,
                                      struct ir_var_ty var_ty,
                                      struct td_var *var);

static struct ir_op *build_ir_for_addressof_var(struct ir_func_builder *irb,
                                                struct ir_stmt **stmt,
                                                struct td_var *var) {
  struct var_key key;
  struct var_ref *ref;
  get_var_ref(irb, NULL, var, &key, &ref);

  struct ir_var_ty var_ty = IR_VAR_TY_POINTER;

  struct ir_op *op = ir_alloc_op(irb->func, *stmt);
  op->ty = IR_OP_TY_ADDR;

  switch (ref->ty) {
  case VAR_REF_TY_SSA: {
    struct ir_op *to_spill = build_ir_for_var(irb, stmt, ref->op->var_ty, var);

    if (to_spill) {
      ir_spill_op(irb->func, to_spill);
      ref->lcl = to_spill->lcl;
    } else {
      ref->lcl = ir_add_local(irb->func, &var_ty);
    }

    // HACK:
    // we don't really mean a "spill" here anyway
    to_spill->lcl = NULL;
    to_spill->flags &= ~IR_OP_FLAG_SPILLED;
    op->lcl = NULL;

    ref->ty = VAR_REF_TY_LCL;

    op->var_ty = var_ty;
    op->addr = (struct ir_op_addr){.ty = IR_OP_ADDR_TY_LCL, .lcl = ref->lcl};
    break;
  }
  case VAR_REF_TY_LCL:
    DEBUG_ASSERT(ref->lcl, "VAR_REF_TY_LCL but no lcl");
    // ref->lcl = ir_add_local(irb->func, &var_ty);

    op->var_ty = var_ty;
    op->addr = (struct ir_op_addr){.ty = IR_OP_ADDR_TY_LCL, .lcl = ref->lcl};
    break;
  case VAR_REF_TY_GLB:
    op->var_ty = var_ty;
    op->addr = (struct ir_op_addr){.ty = IR_OP_ADDR_TY_GLB, .glb = ref->glb};
    break;
  }

  return op;
}

enum build_compoundliteral_mode {
  BUILD_COMPOUNDLITERAL_MODE_ADDR,
  BUILD_COMPOUNDLITERAL_MODE_LOAD,
};

static struct ir_op *build_ir_for_compoundliteral(
    struct ir_func_builder *irb, struct ir_stmt **stmt, struct ir_op *address,
    enum build_compoundliteral_mode mode, struct td_expr *expr);

static struct ir_op *build_ir_for_cnst(struct ir_func_builder *irb,
                                       struct ir_stmt **stmt,
                                       struct ir_var_ty var_ty,
                                       struct td_expr *expr);

static struct ir_op *build_ir_for_addressof(struct ir_func_builder *irb,
                                            struct ir_stmt **stmt,
                                            struct td_expr *expr) {
  // address of does not actually "read" its underlying expression
  // so we do not build the expression

  switch (expr->ty) {
  case TD_EXPR_TY_ARRAYACCESS:
    return build_ir_for_array_address(irb, stmt, expr->array_access.lhs,
                                      expr->array_access.rhs);
  case TD_EXPR_TY_MEMBERACCESS:
    return build_ir_for_member_address(irb, stmt, expr->member_access.lhs,
                                       expr->member_access.member, NULL, NULL);
  case TD_EXPR_TY_POINTERACCESS:
    return build_ir_for_pointer_address(irb, stmt, expr->pointer_access.lhs,
                                        expr->pointer_access.member, NULL,
                                        NULL);
  case TD_EXPR_TY_COMPOUND_LITERAL:
    return build_ir_for_compoundliteral(irb, stmt, NULL,
                                        BUILD_COMPOUNDLITERAL_MODE_ADDR, expr);
  case TD_EXPR_TY_VAR:
    return build_ir_for_addressof_var(irb, stmt, &expr->var);
  case TD_EXPR_TY_CNST:
    // must be string literal
    // `&"foo"` is same as `"foo"`
    DEBUG_ASSERT(expr->cnst.ty == TD_CNST_TY_STRING, "expected str for &cnst");
    struct ir_var_ty ir_var_ty =
        ir_var_ty_for_td_var_ty(irb->unit, &expr->var_ty);
    return build_ir_for_cnst(irb, stmt, ir_var_ty, expr);
  case TD_EXPR_TY_CALL: {
    struct ir_op *value = build_ir_for_expr(irb, stmt, expr);

    // spill call, and address spill
    struct ir_lcl *lcl = ir_add_local(irb->func, &value->var_ty);

    struct ir_op *store =
        ir_append_op(irb->func, *stmt, IR_OP_TY_STORE, IR_VAR_TY_NONE);
    store->store = (struct ir_op_store){
        .ty = IR_OP_STORE_TY_LCL, .lcl = lcl, .value = value};

    struct ir_op *addr =
        ir_append_op(irb->func, *stmt, IR_OP_TY_ADDR, IR_VAR_TY_POINTER);
    addr->addr = (struct ir_op_addr){
        .ty = IR_OP_ADDR_TY_LCL,
        .lcl = lcl,
    };

    return addr;
  }
  case TD_EXPR_TY_UNARY_OP:
    if (expr->unary_op.ty == TD_UNARY_OP_TY_INDIRECTION) {
      // &*, so cancel
      return build_ir_for_expr(irb, stmt, expr->unary_op.expr);
    } else if (expr->unary_op.ty == TD_UNARY_OP_TY_CAST) {
      struct ir_var_ty from_ty =
          ir_var_ty_for_td_var_ty(irb->unit, &expr->unary_op.cast.var_ty);
      struct ir_var_ty to_ty =
          ir_var_ty_for_td_var_ty(irb->unit, &expr->var_ty);

      if (ir_var_ty_needs_cast_op(irb, &to_ty, &from_ty)) {
        BUG("can't take addressof cast op that requires conversion");
      }

      return build_ir_for_addressof(irb, stmt, expr->unary_op.expr);
    }
    break;
  case TD_EXPR_TY_COMPOUNDEXPR: {
    // we can hit this if you do `(foo, bar)(args)`
    // as it implicitly takes address of `bar` (even though `&(foo, bar)`) is
    // not legal

    struct td_compoundexpr *compound_expr = &expr->compound_expr;

    DEBUG_ASSERT(compound_expr->num_exprs > 1,
                 "compound expr must have >1 exprs");
    for (size_t i = 0; i < compound_expr->num_exprs - 1; i++) {
      build_ir_for_expr(irb, stmt, &compound_expr->exprs[i]);

      // compound expressions create a sequence point
      *stmt = ir_alloc_stmt(irb->func, (*stmt)->basicblock);
    }

    return build_ir_for_addressof(
        irb, stmt, &compound_expr->exprs[compound_expr->num_exprs - 1]);
  }

  default:
    break;
  }

  TODO("unknown type for addressof (%u) (file %s line %zu)", expr->ty,
       expr->span.start.file, expr->span.start.line);
}

static struct ir_op *build_ir_for_assg(struct ir_func_builder *irb,
                                       struct ir_stmt **stmt,
                                       struct td_expr *expr);

static struct ir_op *build_ir_for_unaryop(struct ir_func_builder *irb,
                                          struct ir_stmt **stmt,
                                          struct td_expr *expr) {
  struct td_unary_op *unary_op = &expr->unary_op;

  struct ir_var_ty var_ty = ir_var_ty_for_td_var_ty(irb->unit, &expr->var_ty);

  if (unary_op->ty == TD_UNARY_OP_TY_ADDRESSOF) {
    return build_ir_for_addressof(irb, stmt, unary_op->expr);
  }

  struct ir_op *ir_expr = build_ir_for_expr(irb, stmt, unary_op->expr);

  if (unary_op->ty == TD_UNARY_OP_TY_INDIRECTION) {
    // does not generate a unary op instead generates a LOAD_ADDR
    struct ir_op *op = ir_alloc_op(irb->func, *stmt);
    op->ty = IR_OP_TY_LOAD;
    op->var_ty = var_ty;
    op->load = (struct ir_op_load){.ty = IR_OP_LOAD_TY_ADDR, .addr = ir_expr};

    return op;
  }

  bool is_postfix;
  enum td_assg_ty assg_ty;

  switch (unary_op->ty) {
  case TD_UNARY_OP_TY_PREFIX_DEC:
  case TD_UNARY_OP_TY_PREFIX_INC:
    is_postfix = false;
    assg_ty = unary_op->ty == TD_UNARY_OP_TY_PREFIX_INC ? TD_ASSG_TY_ADD
                                                        : TD_ASSG_TY_SUB;

    goto inc_dec;
  case TD_UNARY_OP_TY_POSTFIX_INC:
  case TD_UNARY_OP_TY_POSTFIX_DEC:
    is_postfix = true;
    assg_ty = unary_op->ty == TD_UNARY_OP_TY_POSTFIX_INC ? TD_ASSG_TY_ADD
                                                         : TD_ASSG_TY_SUB;
    goto inc_dec;

  inc_dec: {
    // if we are decrementing a pointer/array, we need to make sure we don't
    // build an expr that is PTR - PTR as this will do a "pointer subtract"
    // rather than "pointer minus integer" so we give the constant a
    // pointer-sized-integer-type, rather than pointer type
    struct td_var_ty cnst_ty;
    if (unary_op->expr->var_ty.ty == TD_VAR_TY_TY_POINTER ||
        unary_op->expr->var_ty.ty == TD_VAR_TY_TY_ARRAY) {
      cnst_ty = td_var_ty_pointer_sized_int(irb->tchk, false);
    } else {
      cnst_ty = unary_op->expr->var_ty;
    }

    struct td_expr one;
    if (td_var_ty_is_fp_ty(&unary_op->expr->var_ty)) {
      enum ap_float_ty ty;

      switch (unary_op->expr->var_ty.well_known) {
      case WELL_KNOWN_TY_HALF:
        ty = AP_FLOAT_TY_F16;
        break;
      case WELL_KNOWN_TY_FLOAT:
        ty = AP_FLOAT_TY_F32;
        break;
      case WELL_KNOWN_TY_DOUBLE:
        ty = AP_FLOAT_TY_F64;
        break;
      case WELL_KNOWN_TY_LONG_DOUBLE:
        // FIXME: long double
        ty = AP_FLOAT_TY_F64;
        break;
      default:
        unreachable();
      }

      one = (struct td_expr){.ty = TD_EXPR_TY_CNST,
                             .var_ty = cnst_ty,
                             .cnst = (struct td_cnst){
                                 .ty = TD_CNST_TY_NUM,
                                 .num_value = MK_AP_VAL_FLT(ap_float_one(ty))}};
    } else {
      one = (struct td_expr){
          .ty = TD_EXPR_TY_CNST,
          .var_ty = cnst_ty,
          .cnst = (struct td_cnst){.ty = TD_CNST_TY_NUM,
                                   .num_value = MK_AP_VAL_INT(ap_int_one(8))}};
    }

    struct td_assg td_assg = {
        .ty = assg_ty,
        .cast_assignee = false,
        .cast_result = false,
        .assignee_var_ty = unary_op->expr->var_ty,
        .result_var_ty = unary_op->expr->var_ty,
        .expr = &one,
        .assignee = unary_op->expr,
    };

    struct td_expr td_expr = {.ty = TD_EXPR_TY_ASSG,
                              .var_ty = expr->var_ty,
                              .assg = td_assg,
                              .span = expr->span};

    struct ir_op *assg = build_ir_for_assg(irb, stmt, &td_expr);

    if (is_postfix) {
      return ir_expr;
    } else {
      return assg;
    }
  }
  case TD_UNARY_OP_TY_PLUS:
    // no work needed, build_expr will handle type conversion
    return ir_expr;
  case TD_UNARY_OP_TY_CAST:
    if (expr->var_ty.ty == TD_VAR_TY_TY_VOID) {
      // do nothing, just let it be an unused node
      return ir_expr;
    } else if (ir_var_ty_needs_cast_op(irb, &var_ty, &ir_expr->var_ty)) {
      return insert_ir_for_cast(
          irb, *stmt, ir_expr, &var_ty,
          cast_ty_for_td_var_ty(irb, &unary_op->expr->var_ty, &expr->var_ty));
    } else {
      ir_expr->var_ty = ir_var_ty_for_td_var_ty(irb->unit, &expr->var_ty);
      return ir_expr;
    }
  default:
    break;
  }

  enum ir_op_unary_op_ty unary_op_ty;
  switch (unary_op->ty) {
  case TD_UNARY_OP_TY_MINUS:
    unary_op_ty = td_var_ty_is_fp_ty(&expr->var_ty) ? IR_OP_UNARY_OP_TY_FNEG
                                                    : IR_OP_UNARY_OP_TY_NEG;
    break;
  case TD_UNARY_OP_TY_LOGICAL_NOT:
    unary_op_ty = IR_OP_UNARY_OP_TY_LOGICAL_NOT;
    break;
  case TD_UNARY_OP_TY_NOT:
    unary_op_ty = IR_OP_UNARY_OP_TY_NOT;
    break;
  default:
    BUG("unexpected unary_op_ty in `%s`", __func__);
  }

  struct ir_op *op = ir_alloc_op(irb->func, *stmt);
  op->ty = IR_OP_TY_UNARY_OP;
  op->var_ty = var_ty;
  op->unary_op.ty = unary_op_ty;
  op->unary_op.value = ir_expr;

  return op;
}

static struct ir_op *build_ir_for_binaryop(struct ir_func_builder *irb,
                                           struct ir_stmt **stmt,
                                           struct td_expr *expr) {
  struct td_binary_op *binary_op = &expr->binary_op;
  struct ir_var_ty var_ty = ir_var_ty_for_td_var_ty(irb->unit, &expr->var_ty);

  struct ir_op *lhs = build_ir_for_expr(irb, stmt, binary_op->lhs);

  if (binary_op->ty == TD_BINARY_OP_TY_LOGICAL_AND ||
      binary_op->ty == TD_BINARY_OP_TY_LOGICAL_OR) {
    struct ir_basicblock *entry_bb = (*stmt)->basicblock;
    struct ir_basicblock *rhs_bb = ir_alloc_basicblock(irb->func);
    struct ir_basicblock *true_bb = ir_alloc_basicblock(irb->func);
    struct ir_basicblock *false_bb = ir_alloc_basicblock(irb->func);
    struct ir_basicblock *end_bb = ir_alloc_basicblock(irb->func);

    if (binary_op->ty == TD_BINARY_OP_TY_LOGICAL_AND) {
      ir_make_basicblock_split(irb->func, entry_bb, rhs_bb, false_bb);
    } else {
      ir_make_basicblock_split(irb->func, entry_bb, true_bb, rhs_bb);
    }

    struct ir_stmt *entry_stmt = ir_alloc_stmt(irb->func, entry_bb);
    struct ir_op *lhs_br = ir_alloc_op(irb->func, entry_stmt);
    lhs_br->ty = IR_OP_TY_BR_COND;
    lhs_br->var_ty = IR_VAR_TY_NONE;
    lhs_br->br_cond = (struct ir_op_br_cond){.cond = lhs};

    struct ir_stmt *rhs_stmt = ir_alloc_stmt(irb->func, rhs_bb);
    struct ir_op *rhs = build_ir_for_expr(irb, &rhs_stmt, binary_op->rhs);

    struct ir_basicblock *rhs_stmt_bb = rhs_stmt->basicblock;
    struct ir_stmt *rhs_br_stmt = ir_alloc_stmt(irb->func, rhs_stmt_bb);
    struct ir_op *rhs_br = ir_alloc_op(irb->func, rhs_br_stmt);
    rhs_br->ty = IR_OP_TY_BR_COND;
    rhs_br->var_ty = IR_VAR_TY_NONE;
    rhs_br->br_cond = (struct ir_op_br_cond){.cond = rhs};

    ir_make_basicblock_split(irb->func, rhs_stmt_bb, true_bb, false_bb);

    struct ir_stmt *true_stmt = ir_alloc_stmt(irb->func, true_bb);
    struct ir_op *true_op = ir_alloc_op(irb->func, true_stmt);
    ir_mk_integral_constant(irb->unit, true_op, IR_VAR_PRIMITIVE_TY_I32, 1);
    ir_make_basicblock_merge(irb->func, true_bb, end_bb);

    struct ir_stmt *true_br_stmt = ir_alloc_stmt(irb->func, true_bb);
    struct ir_op *true_br = ir_alloc_op(irb->func, true_br_stmt);
    true_br->ty = IR_OP_TY_BR;
    true_br->var_ty = IR_VAR_TY_NONE;

    struct ir_stmt *false_stmt = ir_alloc_stmt(irb->func, false_bb);
    struct ir_op *false_op = ir_alloc_op(irb->func, false_stmt);
    ir_mk_integral_constant(irb->unit, false_op, IR_VAR_PRIMITIVE_TY_I32, 0);
    ir_make_basicblock_merge(irb->func, false_bb, end_bb);

    struct ir_stmt *false_br_stmt = ir_alloc_stmt(irb->func, false_bb);
    struct ir_op *false_br = ir_alloc_op(irb->func, false_br_stmt);
    false_br->ty = IR_OP_TY_BR;
    false_br->var_ty = IR_VAR_TY_NONE;

    struct ir_op *phi = ir_insert_phi(irb->func, end_bb, var_ty);
    struct ir_stmt *end_stmt = ir_alloc_stmt(irb->func, end_bb);

    phi->phi = (struct ir_op_phi){
        .num_values = 2,
        .values = aralloc(irb->arena, sizeof(struct ir_phi_entry) * 2)};

    phi->phi.values[0] = (struct ir_phi_entry){
        .basicblock = true_op->stmt->basicblock, .value = true_op};
    phi->phi.values[1] = (struct ir_phi_entry){
        .basicblock = false_op->stmt->basicblock, .value = false_op};

    *stmt = end_stmt;
    return phi;
  }

  struct ir_op *rhs = build_ir_for_expr(irb, stmt, binary_op->rhs);

  struct ir_build_binaryop args = {
      .ty = binary_op->ty,
      .result_ty = expr->var_ty,
      .lhs_ty = binary_op->lhs->var_ty,
      .rhs_ty = binary_op->rhs->var_ty,
      .lhs = lhs,
      .rhs = rhs,
  };

  return alloc_binaryop(irb, *stmt, &args);
}

static struct ir_op *build_ir_for_sizeof(struct ir_func_builder *irb,
                                         struct ir_stmt **stmt,
                                         struct td_expr *expr) {
  struct td_sizeof *size_of = &expr->size_of;
  struct ir_var_ty var_ty = ir_var_ty_for_td_var_ty(irb->unit, &expr->var_ty);

  struct ir_var_ty size_var_ty =
      ir_var_ty_for_td_var_ty(irb->unit, &size_of->var_ty);

  struct ir_var_ty_info info = ir_var_ty_info(irb->unit, &size_var_ty);

  struct ir_op *op = ir_alloc_op(irb->func, *stmt);
  op->ty = IR_OP_TY_CNST;
  op->var_ty = var_ty;
  op->cnst =
      (struct ir_op_cnst){.ty = IR_OP_CNST_TY_INT, .int_value = info.size};

  return op;
}

static struct ir_op *build_ir_for_alignof(struct ir_func_builder *irb,
                                          struct ir_stmt **stmt,
                                          struct td_expr *expr) {
  struct td_alignof *align_of = &expr->align_of;
  struct ir_var_ty var_ty = ir_var_ty_for_td_var_ty(irb->unit, &expr->var_ty);

  struct ir_var_ty align_var_ty =
      ir_var_ty_for_td_var_ty(irb->unit, &align_of->var_ty);
  struct ir_var_ty_info info = ir_var_ty_info(irb->unit, &align_var_ty);

  struct ir_op *op = ir_alloc_op(irb->func, *stmt);
  op->ty = IR_OP_TY_CNST;
  op->var_ty = var_ty;
  op->cnst =
      (struct ir_op_cnst){.ty = IR_OP_CNST_TY_INT, .int_value = info.alignment};

  return op;
}

static struct ir_var_str build_ir_str(const struct td_cnst *cnst,
                                      struct ir_var_ty *char_ty,
                                      bool *is_data) {
  switch (cnst->str_value.ty) {
  case TD_CNST_STR_TY_ASCII: {
    // data if contains null char
    ustr_t sized = {
        .str = cnst->str_value.ascii.value,
        .len = cnst->str_value.ascii.len,
    };
    *is_data = !ustr_nullsafe(sized);
    *char_ty = IR_VAR_TY_I8;
    return (struct ir_var_str){
        .value = cnst->str_value.ascii.value,
        .len = cnst->str_value.ascii.len,
    };
  }
  case TD_CNST_STR_TY_WIDE:
    *is_data = true;
    *char_ty = IR_VAR_TY_I32;

    return (struct ir_var_str){
        .value = cnst->str_value.wide.value,
        .len = cnst->str_value.wide.len,
    };
  }
}

static struct ir_glb *build_str_literal(struct ir_unit *iru,
                                        const struct td_var_ty *td_var_ty,
                                        const struct td_cnst *cnst) {
  DEBUG_ASSERT(cnst->ty == TD_CNST_TY_STRING, "expected string");

  struct ir_var_ty *chr = aralloc(iru->arena, sizeof(*chr));

  // if string literal contains null chars (or is wide char), it will mess up
  // counting and so put it in data
  bool is_data;
  struct ir_var_str str_value = build_ir_str(cnst, chr, &is_data);

  if (td_var_ty->ty != TD_VAR_TY_TY_POINTER ||
      !(td_var_ty->type_qualifiers & TD_TYPE_QUALIFIER_FLAG_CONST)) {
    is_data = true;
  }

  struct ir_var_ty var_ty = {
      .ty = IR_VAR_TY_TY_ARRAY,
      .array = {.underlying = chr,
                .num_elements = str_value.len + 1 /* null */}};

  struct ir_glb *glb =
      ir_add_global(iru, IR_GLB_TY_DATA, &var_ty, IR_GLB_DEF_TY_DEFINED, NULL);

  glb->var = aralloc(iru->arena, sizeof(*glb->var));

  if (is_data) {
    *glb->var = (struct ir_var){.unit = iru,
                                .ty = IR_VAR_TY_CONST_DATA,
                                .var_ty = var_ty,
                                .value = {.ty = IR_VAR_VALUE_TY_STR,
                                          .var_ty = var_ty,
                                          .str_value = str_value}};

  } else {
    *glb->var = (struct ir_var){.unit = iru,
                                .ty = IR_VAR_TY_STRING_LITERAL,
                                .var_ty = var_ty,
                                .value = {.ty = IR_VAR_VALUE_TY_STR,
                                          .var_ty = var_ty,
                                          .str_value = str_value}};
  }

  return glb;
}

static struct ir_op *build_ir_for_cnst(struct ir_func_builder *irb,
                                       struct ir_stmt **stmt,
                                       struct ir_var_ty var_ty,
                                       struct td_expr *expr) {
  struct ir_op *op = ir_alloc_op(irb->func, *stmt);

  switch (expr->cnst.ty) {
  case TD_CNST_TY_NUM:
    switch (expr->cnst.num_value.ty) {
    case AP_VAL_TY_INT:
      op->ty = IR_OP_TY_CNST;
      op->var_ty = var_ty;
      op->cnst.ty = IR_OP_CNST_TY_INT;
      op->cnst.int_value = ap_int_as_ull(expr->cnst.num_value.ap_int);
      break;
    case AP_VAL_TY_FLOAT:
      op->ty = IR_OP_TY_CNST;
      op->var_ty = var_ty;
      op->cnst.ty = IR_OP_CNST_TY_FLT;
      op->cnst.flt_value = ap_float_as_ld(expr->cnst.num_value.ap_float);
      break;
    case AP_VAL_TY_INVALID:
      BUG("INVALID should not reach ir gen");
    }
    break;
  case TD_CNST_TY_STRING: {
    struct ir_glb *glb =
        build_str_literal(irb->unit, &expr->var_ty, &expr->cnst);

    op->ty = IR_OP_TY_ADDR;
    op->var_ty = IR_VAR_TY_POINTER;
    op->addr = (struct ir_op_addr){.ty = IR_OP_ADDR_TY_GLB, .glb = glb};
    break;
  }
  }

  // FIXME: the user needs to load from the address if they want to get the
  // value principally in `const char[] = "foo"`

  return op;
}

static struct ir_op *
build_ir_for_compoundexpr(struct ir_func_builder *irb, struct ir_stmt **stmt,
                          UNUSED_ARG(struct ir_var_ty var_ty),
                          struct td_compoundexpr *compound_expr) {
  struct ir_op *op = NULL;
  for (size_t i = 0; i < compound_expr->num_exprs; i++) {
    op = build_ir_for_expr(irb, stmt, &compound_expr->exprs[i]);

    // compound expressions create a sequence point
    *stmt = ir_alloc_stmt(irb->func, (*stmt)->basicblock);
  }

  return op;
}

// a ?: b
static struct ir_op *build_ir_for_two_ternary(struct ir_func_builder *irb,
                                              struct ir_stmt **stmt,
                                              struct ir_var_ty var_ty,
                                              struct td_ternary *ternary) {
  // this was quickly written and not thoroughly tested

  struct ir_op *cond = build_ir_for_expr(irb, stmt, ternary->cond);
  struct ir_stmt *br_cond_stmt = ir_alloc_stmt(irb->func, (*stmt)->basicblock);
  struct ir_op *br_cond = ir_alloc_op(irb->func, br_cond_stmt);
  br_cond->ty = IR_OP_TY_BR_COND;
  br_cond->var_ty = IR_VAR_TY_NONE;
  br_cond->br_cond = (struct ir_op_br_cond){.cond = cond};

  struct ir_basicblock *pre_cond_bb = (*stmt)->basicblock;
  struct ir_basicblock *false_bb = ir_alloc_basicblock(irb->func);
  struct ir_basicblock *end_bb = ir_alloc_basicblock(irb->func);

  ir_make_basicblock_split(irb->func, pre_cond_bb, end_bb, false_bb);

  struct ir_stmt *false_stmt = ir_alloc_stmt(irb->func, false_bb);
  struct ir_op *false_op =
      build_ir_for_expr(irb, &false_stmt, ternary->false_expr);

  struct ir_stmt *false_br_stmt =
      ir_alloc_stmt(irb->func, false_stmt->basicblock);
  ir_make_basicblock_merge(irb->func, false_stmt->basicblock, end_bb);
  struct ir_op *false_br = ir_alloc_op(irb->func, false_br_stmt);
  false_br->ty = IR_OP_TY_BR;
  false_br->var_ty = IR_VAR_TY_NONE;

  // need to handle the case of `foo ? aggregate : aggregate`
  // in which case we want to do a phi of the _addresses_ not the loads
  // themselves

  bool gen_load = false;
  struct ir_var_ty load_ty;
  if (false_op && ir_var_ty_is_aggregate(&false_op->var_ty)) {
    gen_load = true;
    load_ty = false_op->var_ty;

    if (false_op->ty == IR_OP_TY_CALL) {
      // need to spill
      false_op = ir_spill_op(irb->func, false_op);
    }

    if (cond->ty == IR_OP_TY_CALL) {
      // need to spill
      cond = ir_spill_op(irb->func, cond);
    }

    struct ir_op *false_addr = ir_build_addr(irb->func, false_op);
    struct ir_op *true_addr = ir_build_addr(irb->func, cond);

    false_op = false_addr;
    cond = true_addr;
  }

  struct ir_op *phi = ir_insert_phi(irb->func, end_bb, var_ty);
  phi->phi = (struct ir_op_phi){
      .num_values = 2,
      .values = aralloc(irb->arena, sizeof(struct ir_op_phi) * 2),
  };

  phi->phi.values[0] = (struct ir_phi_entry){
      .basicblock = false_op->stmt->basicblock, .value = false_op};
  phi->phi.values[1] = (struct ir_phi_entry){
      .basicblock = cond->stmt->basicblock, .value = cond};

  struct ir_stmt *end_stmt = ir_alloc_stmt(irb->func, end_bb);
  *stmt = end_stmt;

  if (gen_load) {
    phi->var_ty = IR_VAR_TY_POINTER;

    struct ir_op *load =
        ir_append_op(irb->func, end_stmt, IR_OP_TY_LOAD, load_ty);
    load->load = (struct ir_op_load){.ty = IR_OP_LOAD_TY_ADDR, .addr = phi};

    return load;
  }

  return phi;
}

static struct ir_op *build_ir_for_ternary(struct ir_func_builder *irb,
                                          struct ir_stmt **stmt,
                                          struct ir_var_ty var_ty,
                                          struct td_ternary *ternary) {
  // lhs/rhs can be UNKNOWN if the ternary is mistyped but compile-time constant
  // (we allow this)
  if (ternary->true_expr &&
      ternary->true_expr->var_ty.ty == TD_VAR_TY_TY_UNKNOWN) {
    DEBUG_ASSERT(ternary->false_expr->var_ty.ty != TD_VAR_TY_TY_UNKNOWN,
                 "both ternary sides unknown!");

    return build_ir_for_expr(irb, stmt, ternary->false_expr);
  } else if (ternary->false_expr->var_ty.ty == TD_VAR_TY_TY_UNKNOWN) {
    DEBUG_ASSERT(ternary->true_expr->var_ty.ty != TD_VAR_TY_TY_UNKNOWN,
                 "both ternary sides unknown!");

    return build_ir_for_expr(irb, stmt, ternary->true_expr);
  }

  if (!ternary->true_expr) {
    return build_ir_for_two_ternary(irb, stmt, var_ty, ternary);
  }

  struct ir_op *cond = build_ir_for_expr(irb, stmt, ternary->cond);
  struct ir_stmt *br_cond_stmt = ir_alloc_stmt(irb->func, (*stmt)->basicblock);
  struct ir_op *br_cond = ir_alloc_op(irb->func, br_cond_stmt);
  br_cond->ty = IR_OP_TY_BR_COND;
  br_cond->var_ty = IR_VAR_TY_NONE;
  br_cond->br_cond = (struct ir_op_br_cond){.cond = cond};

  struct ir_basicblock *pre_cond_bb = (*stmt)->basicblock;
  struct ir_basicblock *true_bb = ir_alloc_basicblock(irb->func);
  struct ir_basicblock *false_bb = ir_alloc_basicblock(irb->func);
  struct ir_basicblock *end_bb = ir_alloc_basicblock(irb->func);

  ir_make_basicblock_split(irb->func, pre_cond_bb, true_bb, false_bb);

  struct ir_stmt *true_stmt = ir_alloc_stmt(irb->func, true_bb);
  struct ir_op *true_op =
      build_ir_for_expr(irb, &true_stmt, ternary->true_expr);

  struct ir_stmt *true_br_stmt =
      ir_alloc_stmt(irb->func, true_stmt->basicblock);
  ir_make_basicblock_merge(irb->func, true_br_stmt->basicblock, end_bb);
  struct ir_op *true_br = ir_alloc_op(irb->func, true_br_stmt);
  true_br->ty = IR_OP_TY_BR;
  true_br->var_ty = IR_VAR_TY_NONE;

  struct ir_stmt *false_stmt = ir_alloc_stmt(irb->func, false_bb);
  struct ir_op *false_op =
      build_ir_for_expr(irb, &false_stmt, ternary->false_expr);

  struct ir_stmt *false_br_stmt =
      ir_alloc_stmt(irb->func, false_stmt->basicblock);
  ir_make_basicblock_merge(irb->func, false_stmt->basicblock, end_bb);
  struct ir_op *false_br = ir_alloc_op(irb->func, false_br_stmt);
  false_br->ty = IR_OP_TY_BR;
  false_br->var_ty = IR_VAR_TY_NONE;

  // need to handle the case of `foo ? aggregate : aggregate`
  // in which case we want to do a phi of the _addresses_ not the loads
  // themselves

  bool gen_load = false;
  struct ir_var_ty load_ty;
  if ((false_op && ir_var_ty_is_aggregate(&false_op->var_ty) && true_op &&
       ir_var_ty_is_aggregate(&true_op->var_ty))) {
    DEBUG_ASSERT(ir_var_ty_eq(&false_op->var_ty, &true_op->var_ty),
                 "expected branches to have same ty");

    gen_load = true;
    load_ty = false_op->var_ty;

    if (false_op->ty == IR_OP_TY_CALL) {
      // need to spill
      false_op = ir_spill_op(irb->func, false_op);
    }

    if (true_op->ty == IR_OP_TY_CALL) {
      // need to spill
      true_op = ir_spill_op(irb->func, true_op);
    }

    struct ir_op *false_addr = ir_build_addr(irb->func, false_op);
    struct ir_op *true_addr = ir_build_addr(irb->func, true_op);

    false_op = false_addr;
    true_op = true_addr;
  }

  struct ir_op *phi = ir_insert_phi(irb->func, end_bb, var_ty);
  phi->phi = (struct ir_op_phi){
      .num_values = 2,
      .values = aralloc(irb->arena, sizeof(struct ir_op_phi) * 2),
  };

  phi->phi.values[0] = (struct ir_phi_entry){
      .basicblock = false_op->stmt->basicblock, .value = false_op};
  phi->phi.values[1] = (struct ir_phi_entry){
      .basicblock = true_op->stmt->basicblock, .value = true_op};

  struct ir_stmt *end_stmt = ir_alloc_stmt(irb->func, end_bb);
  *stmt = end_stmt;

  if (gen_load) {
    phi->var_ty = IR_VAR_TY_POINTER;

    struct ir_op *load =
        ir_append_op(irb->func, end_stmt, IR_OP_TY_LOAD, load_ty);
    load->load = (struct ir_op_load){.ty = IR_OP_LOAD_TY_ADDR, .addr = phi};

    return load;
  }

  return phi;
}

static void add_var_write(struct ir_func_builder *irb, struct ir_op *op,
                          struct td_var *var);

static const char *mangle_static_name(struct ir_var_builder *irb,
                                      struct ir_func *func, ustr_t name) {
  // need to mangle the name as statics cannot interfere with others
  size_t base_len = name.len;

  size_t len = base_len + 2; // null char and leading "."

  size_t func_name_len = 0;
  if (func) {
    func_name_len = strlen(func->name);
    len += func_name_len;
    len++; // for "."
  }

  char *buff = aralloc(irb->arena, sizeof(*name.str) * len);
  size_t head = 0;

  buff[head++] = '.';

  if (func) {
    memcpy(&buff[head], func->name, func_name_len);
    head += func_name_len;
    buff[head++] = '.';
  }

  memcpy(&buff[head], name.str, base_len);
  head += base_len;
  buff[head++] = '\0';

  DEBUG_ASSERT(head == len, "string/buff length mismatch");

  return buff;
}

static struct ir_op *build_ir_for_var(struct ir_func_builder *irb,
                                      struct ir_stmt **stmt,
                                      struct ir_var_ty var_ty,
                                      struct td_var *var) {
  if (ustr_eq(var->identifier, MK_USTR("__func__"))) {
    if (!irb->func_name_cnst) {
      const char *value = irb->func->name;
      struct ir_var_ty str_var_ty = ir_var_ty_mk_array(
          irb->unit, &IR_VAR_TY_I8, strlen(irb->func->name) + 1);
      const char *name =
          mangle_static_name(&(struct ir_var_builder){.arena = irb->arena},
                             irb->func, MK_USTR("__func__"));

      struct ir_glb *glb = ir_add_global(irb->unit, IR_GLB_TY_DATA, &str_var_ty,
                                         IR_GLB_DEF_TY_DEFINED, name);
      glb->linkage = IR_LINKAGE_INTERNAL;
      glb->var = aralloc(irb->arena, sizeof(*glb->var));
      *glb->var = (struct ir_var){
          .ty = IR_VAR_TY_STRING_LITERAL,
          .unit = irb->unit,
          .var_ty = str_var_ty,
          .value = {.var_ty = str_var_ty,
                    .ty = IR_VAR_VALUE_TY_STR,
                    .str_value = {.value = value, .len = strlen(value)}}};

      irb->func_name_cnst = glb;
    }

    struct ir_op *op = ir_alloc_op(irb->func, *stmt);
    op->ty = IR_OP_TY_ADDR;
    op->var_ty = IR_VAR_TY_POINTER;
    op->addr = (struct ir_op_addr){.ty = IR_OP_ADDR_TY_GLB,
                                   .glb = irb->func_name_cnst};
    return op;
  }

  // if `a` is an array/function, then reading `a` is actually `&a[0]`/&a
  // same with functions
  if (var_ty.ty == IR_VAR_TY_TY_ARRAY || var_ty.ty == IR_VAR_TY_TY_FUNC) {
    return build_ir_for_addressof_var(irb, stmt, var);
  }

  if (var->ty == TD_VAR_VAR_TY_ENUMERATOR) {
    struct ir_op *op = ir_alloc_op(irb->func, *stmt);
    op->ty = IR_OP_TY_CNST;
    op->var_ty = var_ty;
    op->cnst = (struct ir_op_cnst){.ty = IR_OP_CNST_TY_INT,
                                   .int_value = var->enumerator};

    return op;
  }

  struct var_key key;
  struct var_ref *ref;
  get_var_ref(irb, (*stmt)->basicblock, var, &key, &ref);

  switch (var->ty) {
  case TD_VAR_VAR_TY_ENUMERATOR:
    unreachable();

  case TD_VAR_VAR_TY_VAR: {
    // this is when we are _reading_ from the var
    if (ref) {
      switch (ref->ty) {
      case VAR_REF_TY_SSA:
        return ref->op;
      case VAR_REF_TY_LCL: {
        // DEBUG_ASSERT(ref->lcl, "VAR_REF_TY_LCL but op %zu had no lcl",
        //              ref->op->id);

        // if `a` is an array/function, then reading `a` is actually `&a[0]`/&a
        // same with functions
        if (ref->lcl->var_ty.ty == IR_VAR_TY_TY_ARRAY ||
            ref->lcl->var_ty.ty == IR_VAR_TY_TY_FUNC) {
          return build_ir_for_addressof_var(irb, stmt, var);
        }

        struct ir_op *op = ir_alloc_op(irb->func, *stmt);
        op->ty = IR_OP_TY_LOAD;

        if (var_ty.ty == IR_VAR_TY_TY_ARRAY) {
          // pointer decay
          op->var_ty = IR_VAR_TY_POINTER;
        } else {
          op->var_ty = var_ty;
        }

        op->load =
            (struct ir_op_load){.ty = IR_OP_LOAD_TY_LCL, .lcl = ref->lcl};

        return op;
      }
      case VAR_REF_TY_GLB: {
        if (ref->glb->var_ty.ty == IR_VAR_TY_TY_ARRAY ||
            ref->glb->var_ty.ty == IR_VAR_TY_TY_FUNC) {
          return build_ir_for_addressof_var(irb, stmt, var);
        }

        struct ir_op *op = ir_alloc_op(irb->func, *stmt);
        op->ty = IR_OP_TY_LOAD;
        op->var_ty = var_ty;
        op->load =
            (struct ir_op_load){.ty = IR_OP_LOAD_TY_GLB, .glb = ref->glb};

        return op;
      }
      }
    }
  }
  }

  // we generate an empty phi and then after all blocks are built we insert the
  // correct values
  // all phis appear at the start of their bb as they execute ""
  struct ir_op *phi = ir_insert_phi(irb->func, (*stmt)->basicblock, var_ty);
  phi->phi = (struct ir_op_phi){0};

  add_var_write(irb, phi, var);

  phi->metadata = aralloc(irb->arena, sizeof(struct td_var));
  *(struct td_var *)phi->metadata = *var;

  key = get_var_key(var, (*stmt)->basicblock);
  struct var_ref *new_ref = var_refs_add(irb->var_refs, &key, VAR_REF_TY_SSA);
  new_ref->ty = VAR_REF_TY_SSA;
  new_ref->op = phi;

  return phi;
}

static struct ir_op *store_load_if_needed(struct ir_func_builder *irb,
                                          struct ir_stmt **stmt,
                                          struct ir_op *op) {

  if (!ir_var_ty_is_aggregate(&op->var_ty) || op->ty == IR_OP_TY_LOAD) {
    return op;
  }

  struct ir_lcl *lcl = ir_add_local(irb->func, &op->var_ty);

  struct ir_op *store = ir_alloc_op(irb->func, *stmt);
  store->ty = IR_OP_TY_STORE;
  store->var_ty = IR_VAR_TY_NONE;

  store->store =
      (struct ir_op_store){.ty = IR_OP_STORE_TY_LCL, .value = op, .lcl = lcl};

  struct ir_op *load = ir_alloc_op(irb->func, *stmt);
  load->ty = IR_OP_TY_LOAD;
  load->var_ty = op->var_ty;

  load->load = (struct ir_op_load){.ty = IR_OP_LOAD_TY_LCL, .lcl = lcl};

  return load;
}

static struct ir_op *build_ir_for_intrinsic(struct ir_func_builder *irb,
                                            struct ir_stmt **stmt,
                                            struct td_expr *expr) {
  struct td_call *call = &expr->call;
  struct td_expr *target = call->target;

  if (target->ty != TD_EXPR_TY_VAR) {
    return NULL;
  }

  struct td_var var = target->var;

  if (var.scope != SCOPE_GLOBAL || var.ty != TD_VAR_VAR_TY_VAR) {
    return NULL;
  }

  struct ir_var_ty ret_ty = ir_var_ty_for_td_var_ty(irb->unit, &expr->var_ty);

  if (ustr_eq(var.identifier, MK_USTR("fabs")) ||
      ustr_eq(var.identifier, MK_USTR("fabsf")) ||
      ustr_eq(var.identifier, MK_USTR("fabsl"))) {
    DEBUG_ASSERT(call->arg_list.num_args == 1, "more than 1 arg to fabs");

    struct ir_op *value = build_ir_for_expr(irb, stmt, &call->arg_list.args[0]);

    struct ir_op *op =
        ir_append_op(irb->func, *stmt, IR_OP_TY_UNARY_OP, ret_ty);
    op->unary_op =
        (struct ir_op_unary_op){.ty = IR_OP_UNARY_OP_TY_FABS, .value = value};

    return op;
  } else if (ustr_eq(var.identifier, MK_USTR("sqrt")) ||
             ustr_eq(var.identifier, MK_USTR("sqrtf")) ||
             ustr_eq(var.identifier, MK_USTR("sqrtl"))) {
    DEBUG_ASSERT(call->arg_list.num_args == 1, "more than 1 arg to fabs");

    struct ir_op *value = build_ir_for_expr(irb, stmt, &call->arg_list.args[0]);

    struct ir_op *op =
        ir_append_op(irb->func, *stmt, IR_OP_TY_UNARY_OP, ret_ty);
    op->unary_op =
        (struct ir_op_unary_op){.ty = IR_OP_UNARY_OP_TY_FSQRT, .value = value};

    return op;
  }

  return NULL;
}

static struct ir_op *build_ir_for_call(struct ir_func_builder *irb,
                                       struct ir_stmt **stmt,
                                       struct td_expr *expr) {
  struct ir_op *intrinsic = build_ir_for_intrinsic(irb, stmt, expr);
  if (intrinsic) {
    return intrinsic;
  }
  // need to generate args and target IR first to keep IR in order
  struct td_call *call = &expr->call;

  if (call->target->ty == TD_EXPR_TY_BUILTIN) {
    ustr_t builtin = call->target->builtin.identifier;

    if (ustr_eq(builtin, MK_USTR("__builtin_va_start"))) {
      irb->func->flags |= IR_FUNC_FLAG_USES_VA_ARGS;

      struct td_expr *list = &call->arg_list.args[0];
      struct ir_op *list_addr = build_ir_for_addressof(irb, stmt, list);

      struct ir_op *va_start =
          ir_append_op(irb->func, *stmt, IR_OP_TY_VA_START, IR_VAR_TY_NONE);
      va_start->va_start = (struct ir_op_va_start){.list_addr = list_addr};
      return va_start;
    } else if (ustr_eq(builtin, MK_USTR("__builtin_va_end"))) {
      // nop
      return NULL;
    } else if (ustr_eq(builtin, MK_USTR("__builtin_va_copy"))) {
      // we require all backends to support bitwise copy of `va_list`
      struct td_expr *lhs = &call->arg_list.args[0];
      struct td_expr *rhs = &call->arg_list.args[1];

      struct ir_op *lhs_addr = build_ir_for_addressof(irb, stmt, lhs);
      struct ir_op *rhs_op = build_ir_for_expr(irb, stmt, rhs);

      if (irb->unit->target->variadic_info.flags &
          TARGET_VARIADIC_INFO_FLAG_VA_LIST_BYREF) {
        struct td_var_ty td_load_ty =
            td_var_ty_get_underlying(irb->tchk, &rhs->var_ty);
        struct ir_var_ty load_ty =
            ir_var_ty_for_td_var_ty(irb->unit, &td_load_ty);

        struct ir_op *load =
            ir_append_op(irb->func, *stmt, IR_OP_TY_LOAD, load_ty);
        load->load =
            (struct ir_op_load){.ty = IR_OP_LOAD_TY_ADDR, .addr = rhs_op};

        rhs_op = load;
      }

      struct ir_op *store =
          ir_append_op(irb->func, *stmt, IR_OP_TY_STORE, IR_VAR_TY_NONE);
      store->store = (struct ir_op_store){
          .ty = IR_OP_STORE_TY_ADDR, .addr = lhs_addr, .value = rhs_op};

      return store;
    } else if (ustr_eq(builtin, MK_USTR("__builtin_popcount")) ||
               ustr_eq(builtin, MK_USTR("__builtin_popcountl")) ||
               ustr_eq(builtin, MK_USTR("__builtin_popcountll"))) {
      struct ir_op *value =
          build_ir_for_expr(irb, stmt, &call->arg_list.args[0]);

      struct ir_op *popcnt =
          ir_append_op(irb->func, *stmt, IR_OP_TY_UNARY_OP, IR_VAR_TY_I32);
      popcnt->unary_op = (struct ir_op_unary_op){.ty = IR_OP_UNARY_OP_TY_POPCNT,
                                                 .value = value};

      return popcnt;
    } else if (ustr_eq(builtin, MK_USTR("__builtin_clz")) ||
               ustr_eq(builtin, MK_USTR("__builtin_clzl")) ||
               ustr_eq(builtin, MK_USTR("__builtin_clzll"))) {
      struct ir_op *value =
          build_ir_for_expr(irb, stmt, &call->arg_list.args[0]);

      struct ir_op *popcnt =
          ir_append_op(irb->func, *stmt, IR_OP_TY_UNARY_OP, IR_VAR_TY_I32);
      popcnt->unary_op =
          (struct ir_op_unary_op){.ty = IR_OP_UNARY_OP_TY_CLZ, .value = value};

      return popcnt;
    } else if (ustr_eq(builtin, MK_USTR("__builtin_ctz")) ||
               ustr_eq(builtin, MK_USTR("__builtin_ctzl")) ||
               ustr_eq(builtin, MK_USTR("__builtin_ctzll"))) {
      struct ir_op *value =
          build_ir_for_expr(irb, stmt, &call->arg_list.args[0]);

      struct ir_op *popcnt =
          ir_append_op(irb->func, *stmt, IR_OP_TY_UNARY_OP, IR_VAR_TY_I32);
      popcnt->unary_op =
          (struct ir_op_unary_op){.ty = IR_OP_UNARY_OP_TY_CTZ, .value = value};

      return popcnt;
    } else if (ustr_eq(builtin, MK_USTR("__builtin_bswap16")) ||
               ustr_eq(builtin, MK_USTR("__builtin_bswap32")) ||
               ustr_eq(builtin, MK_USTR("__builtin_bswap64"))) {
      struct ir_op *value =
          build_ir_for_expr(irb, stmt, &call->arg_list.args[0]);

      struct ir_op *popcnt =
          ir_append_op(irb->func, *stmt, IR_OP_TY_UNARY_OP, value->var_ty);
      popcnt->unary_op =
          (struct ir_op_unary_op){.ty = IR_OP_UNARY_OP_TY_REV, .value = value};

      return popcnt;
    } else if (ustr_eq(builtin, MK_USTR("__builtin_memset"))) {
      struct ir_op *dest =
          build_ir_for_expr(irb, stmt, &call->arg_list.args[0]);
      struct ir_op *ch = build_ir_for_expr(irb, stmt, &call->arg_list.args[1]);
      struct ir_op *len = build_ir_for_expr(irb, stmt, &call->arg_list.args[2]);

      struct ir_op *op = ir_alloc_op(irb->func, *stmt);
      return ir_mk_wk_memset(irb->func, op, dest, ch, len);
    } else if (ustr_eq(builtin, MK_USTR("__builtin_memmove"))) {
      struct ir_op *dest =
          build_ir_for_expr(irb, stmt, &call->arg_list.args[0]);
      struct ir_op *source =
          build_ir_for_expr(irb, stmt, &call->arg_list.args[1]);
      struct ir_op *len = build_ir_for_expr(irb, stmt, &call->arg_list.args[2]);

      struct ir_op *op = ir_alloc_op(irb->func, *stmt);
      return ir_mk_wk_memmove(irb->func, op, dest, source, len);
    } else if (ustr_eq(builtin, MK_USTR("__builtin_memcpy"))) {
      struct ir_op *dest =
          build_ir_for_expr(irb, stmt, &call->arg_list.args[0]);
      struct ir_op *source =
          build_ir_for_expr(irb, stmt, &call->arg_list.args[1]);
      struct ir_op *len = build_ir_for_expr(irb, stmt, &call->arg_list.args[2]);

      struct ir_op *op = ir_alloc_op(irb->func, *stmt);
      return ir_mk_wk_memcpy(irb->func, op, dest, source, len);
    } else if (ustr_eq(builtin, MK_USTR("__builtin_memcmp"))) {
      struct ir_op *lhs = build_ir_for_expr(irb, stmt, &call->arg_list.args[0]);
      struct ir_op *rhs = build_ir_for_expr(irb, stmt, &call->arg_list.args[1]);
      struct ir_op *len = build_ir_for_expr(irb, stmt, &call->arg_list.args[2]);

      struct ir_op *op = ir_alloc_op(irb->func, *stmt);
      return ir_mk_wk_memcmp(irb->func, op, lhs, rhs, len);
    } else if (ustr_eq(builtin, MK_USTR("__builtin_unreachable"))) {
      // TODO: unreachable in IR
      return NULL;
    } else {
      BUG("unrecognised builtin '%.*s'", (int)builtin.len, builtin.str);
    }
  }

  struct ir_op **args =
      aralloc(irb->arena, sizeof(struct ir_op *) * call->arg_list.num_args);

  struct ir_var_ty *arg_var_tys =
      aralloc(irb->arena, sizeof(struct ir_var_ty) * call->arg_list.num_args);

  size_t num_non_variadic_args = call->target->var_ty.func.num_params;

  struct td_expr *target_expr = call->target;

  struct ir_var_ty func_ty;

  // one level deref can occur
  if (target_expr->var_ty.ty == TD_VAR_TY_TY_POINTER ||
      target_expr->var_ty.ty == TD_VAR_TY_TY_ARRAY) {
    struct td_var_ty underlying =
        td_var_ty_get_underlying(irb->tchk, &target_expr->var_ty);
    func_ty = ir_var_ty_for_td_var_ty(irb->unit, &underlying);
  } else {
    func_ty = ir_var_ty_for_td_var_ty(irb->unit, &target_expr->var_ty);
  }

  DEBUG_ASSERT(func_ty.ty == IR_VAR_TY_TY_FUNC,
               "expected target to be func ty");

  for (size_t i = 0; i < call->arg_list.num_args; i++) {
    args[i] = build_ir_for_expr(irb, stmt, &call->arg_list.args[i]);
    args[i] = store_load_if_needed(irb, stmt, args[i]);

    arg_var_tys[i] = args[i]->var_ty;

    if (i >= num_non_variadic_args) {
      args[i]->flags |= IR_OP_FLAG_VARIADIC_PARAM;
    }
  }

  // if the target is a function name, we want to take address
  // else, we want to use value
  struct ir_op *target;

  if (call->target->var_ty.ty == TD_VAR_TY_TY_POINTER) {
    target = build_ir_for_expr(irb, stmt, call->target);
  } else {
    target = build_ir_for_addressof(irb, stmt, call->target);
  }

  irb->func->flags |= IR_FUNC_FLAG_MAKES_CALL;
  struct ir_op *op = ir_alloc_op(irb->func, *stmt);

  op->ty = IR_OP_TY_CALL;
  op->var_ty = *func_ty.func.ret_ty;

  op->call.func_ty = func_ty;
  op->call.target = target;
  op->call.num_args = call->arg_list.num_args;
  op->call.args = args;
  op->call.arg_var_tys = arg_var_tys;

  return op;
}

static void add_var_write(struct ir_func_builder *irb, struct ir_op *op,
                          struct td_var *var) {
  // TODO: consider merging this with var_refs
  // FIXME: inefficient, we need an easy lookup then insert in hashtbl
  struct vector **p = hashtbl_lookup(irb->var_writes, var);
  struct vector *writes;
  if (p) {
    writes = *p;
  } else {
    writes = vector_create_in_arena(sizeof(struct ir_op *), irb->arena);
    hashtbl_insert(irb->var_writes, var, &writes);
  }

  vector_push_back(writes, &op);
}

static struct ir_op *var_assg(struct ir_func_builder *irb, struct ir_stmt *stmt,
                              struct ir_op *op, struct td_var *var) {
  struct var_key key;
  struct var_ref *ref;
  get_var_ref(irb, stmt->basicblock, var, &key, &ref);

  if (!ref) {
    ref = var_refs_add(irb->var_refs, &key, VAR_REF_TY_SSA);
  }

  switch (ref->ty) {
  case VAR_REF_TY_SSA:
    ref->op = op;

    add_var_write(irb, op, var);
    return op;
  case VAR_REF_TY_LCL: {
    // FIXME: is this right
    struct ir_op *store = ir_alloc_op(irb->func, stmt);
    store->ty = IR_OP_TY_STORE;
    store->var_ty = IR_VAR_TY_NONE;
    store->store = (struct ir_op_store){
        .ty = IR_OP_STORE_TY_LCL, .lcl = ref->lcl, .value = op};

    // its okay that we use the thing assigned to the global, rather than
    // reloading the global
    return op;
  }
  case VAR_REF_TY_GLB: {
    // FIXME: is this right
    struct ir_op *store = ir_alloc_op(irb->func, stmt);
    store->ty = IR_OP_TY_STORE;
    store->var_ty = IR_VAR_TY_NONE;
    store->store = (struct ir_op_store){
        .ty = IR_OP_STORE_TY_GLB, .glb = ref->glb, .value = op};

    // its okay that we use the thing assigned to the global, rather than
    // reloading the global
    return op;
  }
  }
}

static bool try_get_member_info(struct ir_unit *iru,
                                const struct td_var_ty *aggregate,
                                ustr_t member_name, struct ir_var_ty *member_ty,
                                size_t *member_idx, size_t *member_offset,
                                bool *member_is_bitfield,
                                struct ir_bitfield *member_bitfield,
                                struct td_var_ty *td_member_ty) {
  DEBUG_ASSERT(aggregate->ty == TD_VAR_TY_TY_AGGREGATE, "expected aggregate");

  *member_ty = IR_VAR_TY_NONE;

  size_t idx;
  if (!member_idx) {
    member_idx = &idx;
  }

  *member_idx = 0;
  *member_offset = 0;
  for (; *member_idx < aggregate->aggregate.num_fields; (*member_idx)++) {
    struct td_struct_field *field = &aggregate->aggregate.fields[*member_idx];
    if (!field->identifier.len) {
      // anonymous field
      size_t anon_member_idx;
      size_t anon_member_offset;

      if (!try_get_member_info(iru, &field->var_ty, member_name, member_ty,
                               &anon_member_idx, &anon_member_offset,
                               member_is_bitfield, member_bitfield,
                               td_member_ty)) {
        continue;
      }

      DEBUG_ASSERT(*member_idx < aggregate->aggregate.num_fields,
                   "member_idx out of range");

      struct ir_var_ty ir_aggregate = ir_var_ty_for_td_var_ty(iru, aggregate);
      struct ir_var_ty_info info = ir_var_ty_info(iru, &ir_aggregate);

      // offsets are null for a union
      *member_offset += anon_member_offset;
      *member_offset += info.offsets ? info.offsets[*member_idx] : 0;
      return true;
    } else if (ustr_eq(field->identifier, member_name)) {
      if (member_bitfield) {
        if (field->flags & TD_STRUCT_FIELD_FLAG_BITFIELD) {
          *member_is_bitfield = true;
          *member_bitfield = (struct ir_bitfield){
              .offset = 0, // all bitfields are their own fields for now
              .width = field->bitfield_width};
        } else {
          *member_is_bitfield = false;
        }
      }

      if (td_member_ty) {
        *td_member_ty = field->var_ty;
      }

      *member_ty = ir_var_ty_for_td_var_ty(iru, &field->var_ty);
      if (member_ty->ty == IR_VAR_TY_TY_ARRAY) {
        // pointer decay
        *member_ty = *member_ty->array.underlying;
      }

      DEBUG_ASSERT(*member_idx < aggregate->aggregate.num_fields,
                   "member_idx out of range");

      struct ir_var_ty ir_aggregate = ir_var_ty_for_td_var_ty(iru, aggregate);
      struct ir_var_ty_info info = ir_var_ty_info(iru, &ir_aggregate);

      // offsets are null for a union
      *member_offset += info.offsets ? info.offsets[*member_idx] : 0;
      return true;
    }
  }

  return false;
}

static void get_member_info(struct ir_unit *iru,
                            const struct td_var_ty *aggregate,
                            ustr_t member_name, struct ir_var_ty *member_ty,
                            size_t *member_idx, size_t *member_offset,
                            bool *member_is_bitfield,
                            struct ir_bitfield *member_bitfield,
                            struct td_var_ty *td_member_ty) {
  if (try_get_member_info(iru, aggregate, member_name, member_ty, member_idx,
                          member_offset, member_is_bitfield, member_bitfield,
                          td_member_ty)) {
    return;
  }

  unreachable();
}

static size_t get_member_address_offset(
    struct ir_unit *unit, const struct td_var_ty *aggregate, ustr_t member_name,
    struct ir_var_ty *member_ty, bool *member_is_bitfield,
    struct ir_bitfield *member_bitfield, struct td_var_ty *td_member_ty) {

  size_t member_offset;
  size_t idx;
  get_member_info(unit, aggregate, member_name, member_ty, &idx, &member_offset,
                  member_is_bitfield, member_bitfield, td_member_ty);

  return member_offset;
}

static struct ir_op *
build_ir_for_member_address(struct ir_func_builder *irb, struct ir_stmt **stmt,
                            struct td_expr *lhs_expr, ustr_t member_name,
                            bool *member_is_bitfield,
                            struct ir_bitfield *member_bitfield) {
  struct ir_op *lhs = build_ir_for_addressof(irb, stmt, lhs_expr);

  struct ir_var_ty member_ty;
  size_t offset = get_member_address_offset(
      irb->unit, &lhs_expr->var_ty, member_name, &member_ty, member_is_bitfield,
      member_bitfield, NULL);

  if (!offset) {
    return lhs;
  }

  struct ir_op *op = ir_alloc_op(irb->func, *stmt);
  op->ty = IR_OP_TY_ADDR_OFFSET;
  op->var_ty = IR_VAR_TY_POINTER;
  op->addr_offset =
      (struct ir_op_addr_offset){.base = lhs, .offset = offset, .index = NULL};

  return op;
}

static struct ir_op *
build_ir_for_pointer_address(struct ir_func_builder *irb, struct ir_stmt **stmt,
                             struct td_expr *lhs_expr, ustr_t member_name,
                             bool *member_is_bitfield,
                             struct ir_bitfield *member_bitfield) {
  DEBUG_ASSERT(lhs_expr->var_ty.ty == TD_VAR_TY_TY_POINTER,
               "makes no sense except on LHS pointer");

  struct ir_op *lhs = build_ir_for_expr(irb, stmt, lhs_expr);

  struct ir_var_ty member_ty;
  size_t offset = get_member_address_offset(
      irb->unit, lhs_expr->var_ty.pointer.underlying, member_name, &member_ty,
      member_is_bitfield, member_bitfield, NULL);

  if (!offset) {
    return lhs;
  }

  struct ir_op *op = ir_alloc_op(irb->func, *stmt);
  op->ty = IR_OP_TY_ADDR_OFFSET;
  op->var_ty = IR_VAR_TY_POINTER;
  op->addr_offset =
      (struct ir_op_addr_offset){.base = lhs, .offset = offset, .index = NULL};

  return op;
}

static struct ir_op *build_ir_for_array_address(struct ir_func_builder *irb,
                                                struct ir_stmt **stmt,
                                                struct td_expr *lhs_expr,
                                                struct td_expr *rhs_expr) {
  struct td_var_ty pointer_ty;
  struct td_var_ty lhs_ty;

  struct ir_op *lhs;
  if (lhs_expr->var_ty.ty == TD_VAR_TY_TY_ARRAY) {
    // need to decay the type to pointer
    struct td_var_ty *underlying = lhs_expr->var_ty.array.underlying;
    lhs = build_ir_for_addressof(irb, stmt, lhs_expr);
    pointer_ty = td_var_ty_make_pointer(irb->tchk, underlying,
                                        TD_TYPE_QUALIFIER_FLAG_NONE);

    lhs_ty = pointer_ty;
  } else {
    lhs = build_ir_for_expr(irb, stmt, lhs_expr);
    pointer_ty = lhs_expr->var_ty;
    lhs_ty = lhs_expr->var_ty;
  }

  // need to promote rhs to pointer size int
  DEBUG_ASSERT(rhs_expr->var_ty.ty == TD_VAR_TY_TY_WELL_KNOWN,
               "expected well-known ty rhs");

  struct ir_op *rhs = build_ir_for_expr(irb, stmt, rhs_expr);

  struct td_var_ty underlying = td_var_ty_get_underlying(irb->tchk, &lhs_ty);
  struct ir_var_ty el_ty = ir_var_ty_for_td_var_ty(irb->unit, &underlying);
  struct ir_var_ty_info info = ir_var_ty_info(irb->unit, &el_ty);

  struct ir_op *addr = ir_alloc_op(irb->func, *stmt);
  addr->ty = IR_OP_TY_ADDR_OFFSET;
  addr->var_ty = IR_VAR_TY_POINTER;
  addr->addr_offset =
      (struct ir_op_addr_offset){.base = lhs, .scale = info.size, .index = rhs};

  return addr;
}

static struct ir_op *build_ir_for_assg(struct ir_func_builder *irb,
                                       struct ir_stmt **stmt,
                                       struct td_expr *expr) {
  struct td_assg *assg = &expr->assg;

  struct ir_op *value;
  enum td_binary_op_ty ty;
  switch (assg->ty) {
  case TD_ASSG_TY_BASIC:
    value = build_ir_for_expr(irb, stmt, assg->expr);
    break;

  case TD_ASSG_TY_ADD:
    ty = TD_BINARY_OP_TY_ADD;
    goto compound_assg;
  case TD_ASSG_TY_SUB:
    ty = TD_BINARY_OP_TY_SUB;
    goto compound_assg;
  case TD_ASSG_TY_MUL:
    ty = TD_BINARY_OP_TY_MUL;
    goto compound_assg;
  case TD_ASSG_TY_DIV:
    ty = TD_BINARY_OP_TY_DIV;
    goto compound_assg;
  case TD_ASSG_TY_MOD:
    ty = TD_BINARY_OP_TY_MOD;
    goto compound_assg;
  case TD_ASSG_TY_AND:
    ty = TD_BINARY_OP_TY_AND;
    goto compound_assg;
  case TD_ASSG_TY_OR:
    ty = TD_BINARY_OP_TY_OR;
    goto compound_assg;
  case TD_ASSG_TY_XOR:
    ty = TD_BINARY_OP_TY_XOR;
    goto compound_assg;
  case TD_ASSG_TY_LSHIFT:
    ty = TD_BINARY_OP_TY_LSHIFT;
    goto compound_assg;
  case TD_ASSG_TY_RSHIFT:
    ty = TD_BINARY_OP_TY_RSHIFT;
    goto compound_assg;

  compound_assg: {
    struct ir_op *assignee = build_ir_for_expr(irb, stmt, assg->assignee);

    struct ir_op *lhs;

    if (assg->cast_assignee) {
      lhs = insert_ir_for_cast_if_needed(irb, *stmt, assignee,
                                         &assg->assignee->var_ty,
                                         &assg->assignee_var_ty);
    } else {
      lhs = assignee;
    }

    struct ir_op *rhs = build_ir_for_expr(irb, stmt, assg->expr);

    struct ir_build_binaryop args = {
        .ty = ty,
        .result_ty = assg->result_var_ty,
        .lhs_ty = assg->assignee_var_ty,
        .rhs_ty = assg->expr->var_ty,
        .lhs = lhs,
        .rhs = rhs,
    };

    value = alloc_binaryop(irb, *stmt, &args);

    if (assg->cast_result) {
      value = insert_ir_for_cast_if_needed(
          irb, *stmt, value, &assg->result_var_ty, &assg->assignee->var_ty);
    }

    break;
  }
  }

  bool is_bitfield = false;
  struct ir_bitfield bitfield;

  struct ir_op *address = NULL;
  switch (assg->assignee->ty) {
  case TD_EXPR_TY_VAR:
    return var_assg(irb, *stmt, value, &assg->assignee->var);
  case TD_EXPR_TY_ARRAYACCESS: {
    struct td_arrayaccess *access = &assg->assignee->array_access;
    address = build_ir_for_array_address(irb, stmt, access->lhs, access->rhs);
    break;
  }
  case TD_EXPR_TY_MEMBERACCESS: {
    struct td_memberaccess *access = &assg->assignee->member_access;
    address = build_ir_for_member_address(
        irb, stmt, access->lhs, access->member, &is_bitfield, &bitfield);

    break;
  }
  case TD_EXPR_TY_POINTERACCESS: {
    struct td_pointeraccess *access = &assg->assignee->pointer_access;
    address = build_ir_for_pointer_address(
        irb, stmt, access->lhs, access->member, &is_bitfield, &bitfield);
    break;
  }
  case TD_EXPR_TY_UNARY_OP: {
    if (assg->assignee->unary_op.ty == TD_UNARY_OP_TY_INDIRECTION) {
      address = build_ir_for_expr(irb, stmt, assg->assignee->unary_op.expr);
    }
    break;
  }
  default:
    TODO("non var assignments");
  }

  if (!address) {
    TODO("non var assignments");
  }

  if (is_bitfield) {
    struct ir_op *store = ir_alloc_op(irb->func, *stmt);
    store->ty = IR_OP_TY_STORE_BITFIELD;
    store->var_ty = IR_VAR_TY_NONE;
    store->store_bitfield =
        (struct ir_op_store_bitfield){.ty = IR_OP_STORE_TY_ADDR,
                                      .addr = address,
                                      .value = value,
                                      .bitfield = bitfield};
  } else {
    struct ir_op *store = ir_alloc_op(irb->func, *stmt);
    store->ty = IR_OP_TY_STORE;
    store->var_ty = IR_VAR_TY_NONE;
    store->store = (struct ir_op_store){
        .ty = IR_OP_STORE_TY_ADDR, .addr = address, .value = value};
  }

  return value;
}

static struct ir_op *
build_ir_for_arrayaccess(struct ir_func_builder *irb, struct ir_stmt **stmt,
                         struct td_arrayaccess *array_access) {
  struct td_var_ty underlying =
      td_var_ty_get_underlying(irb->tchk, &array_access->lhs->var_ty);
  struct ir_var_ty var_ty = ir_var_ty_for_td_var_ty(irb->unit, &underlying);

  struct ir_op *address = build_ir_for_array_address(
      irb, stmt, array_access->lhs, array_access->rhs);

  if (var_ty.ty == IR_VAR_TY_TY_ARRAY) {
    // md array, don't dereference
    return address;
  }

  struct ir_op *op = ir_alloc_op(irb->func, *stmt);
  op->ty = IR_OP_TY_LOAD;
  op->var_ty = var_ty;
  op->load = (struct ir_op_load){.ty = IR_OP_LOAD_TY_ADDR, .addr = address};

  return op;
}

static struct ir_op *
build_ir_for_memberaccess(struct ir_func_builder *irb, struct ir_stmt **stmt,
                          struct td_memberaccess *member_access,
                          const struct td_var_ty *member_ty) {
  struct ir_var_ty var_ty = ir_var_ty_for_td_var_ty(irb->unit, member_ty);

  bool is_bitfield;
  struct ir_bitfield bitfield;

  struct ir_op *address = build_ir_for_member_address(
      irb, stmt, member_access->lhs, member_access->member, &is_bitfield,
      &bitfield);

  if (is_bitfield) {
    struct ir_op *op = ir_alloc_op(irb->func, *stmt);
    op->ty = IR_OP_TY_LOAD_BITFIELD;
    op->var_ty = var_ty;
    op->load_bitfield = (struct ir_op_load_bitfield){
        .ty = IR_OP_LOAD_TY_ADDR, .addr = address, .bitfield = bitfield};

    return op;
  } else {
    struct ir_op *op = ir_alloc_op(irb->func, *stmt);
    op->ty = IR_OP_TY_LOAD;
    op->var_ty = var_ty;
    op->load = (struct ir_op_load){.ty = IR_OP_LOAD_TY_ADDR, .addr = address};

    return op;
  }
}

static struct ir_op *
build_ir_for_pointeraccess(struct ir_func_builder *irb, struct ir_stmt **stmt,
                           struct td_pointeraccess *pointer_access,
                           const struct td_var_ty *member_ty) {
  struct ir_var_ty var_ty = ir_var_ty_for_td_var_ty(irb->unit, member_ty);

  bool is_bitfield;
  struct ir_bitfield bitfield;

  struct ir_op *address = build_ir_for_pointer_address(
      irb, stmt, pointer_access->lhs, pointer_access->member, &is_bitfield,
      &bitfield);

  if (is_bitfield) {
    struct ir_op *op = ir_alloc_op(irb->func, *stmt);
    op->ty = IR_OP_TY_LOAD_BITFIELD;
    op->var_ty = var_ty;
    op->load_bitfield = (struct ir_op_load_bitfield){
        .ty = IR_OP_LOAD_TY_ADDR, .addr = address, .bitfield = bitfield};

    return op;
  } else {
    struct ir_op *op = ir_alloc_op(irb->func, *stmt);
    op->ty = IR_OP_TY_LOAD;
    op->var_ty = var_ty;
    op->load = (struct ir_op_load){.ty = IR_OP_LOAD_TY_ADDR, .addr = address};

    return op;
  }
}

static void build_ir_for_init_list(struct ir_func_builder *irb,
                                   struct ir_stmt **stmt, struct ir_op *address,
                                   struct td_init_list *init_list);

static struct ir_op *build_ir_for_compoundliteral(
    struct ir_func_builder *irb, struct ir_stmt **stmt, struct ir_op *address,
    enum build_compoundliteral_mode mode, struct td_expr *expr) {
  struct td_compound_literal *compound_literal = &expr->compound_literal;

  if (mode == BUILD_COMPOUNDLITERAL_MODE_LOAD &&
      td_var_ty_is_scalar_ty(&expr->var_ty)) {
    DEBUG_ASSERT(expr->compound_literal.init_list.num_inits == 1,
                 "expected 1 init");
    return build_ir_for_expr(
        irb, stmt, &expr->compound_literal.init_list.inits->init->expr);
  }

  struct ir_var_ty var_ty =
      ir_var_ty_for_td_var_ty(irb->unit, &compound_literal->var_ty);

  if (!address) {
    struct ir_lcl *lcl = ir_add_local(irb->func, &var_ty);

    address = ir_alloc_op(irb->func, *stmt);
    address->ty = IR_OP_TY_ADDR;
    address->var_ty = IR_VAR_TY_POINTER;
    address->addr = (struct ir_op_addr){.ty = IR_OP_ADDR_TY_LCL, .lcl = lcl};
  }

  build_ir_for_init_list(irb, stmt, address, &compound_literal->init_list);

  if (mode == BUILD_COMPOUNDLITERAL_MODE_LOAD) {
    struct ir_op *load = ir_alloc_op(irb->func, *stmt);
    load->ty = IR_OP_TY_LOAD;
    load->var_ty = var_ty;
    load->load = (struct ir_op_load){.ty = IR_OP_LOAD_TY_ADDR, .addr = address};

    return load;
  }

  return address;
}

static struct ir_op *build_ir_for_va_arg(struct ir_func_builder *irb,
                                         struct ir_stmt **stmt,
                                         struct td_va_arg *va_arg) {
  struct ir_op *list_addr = build_ir_for_addressof(irb, stmt, va_arg->list);

  struct ir_var_ty var_ty = ir_var_ty_for_td_var_ty(irb->unit, &va_arg->var_ty);

  struct ir_op *op = ir_append_op(irb->func, *stmt, IR_OP_TY_VA_ARG, var_ty);
  op->va_arg = (struct ir_op_va_arg){.arg_ty = var_ty, .list_addr = list_addr};

  return op;
}

static struct ir_basicblock *build_ir_for_stmt(struct ir_func_builder *irb,
                                               struct ir_basicblock *basicblock,
                                               struct td_stmt *stmt);

static struct ir_op *build_ir_for_expr(struct ir_func_builder *irb,
                                       struct ir_stmt **stmt,
                                       struct td_expr *expr) {
  struct ir_op *op;

  struct ir_var_ty var_ty = ir_var_ty_for_td_var_ty(irb->unit, &expr->var_ty);

  switch (expr->ty) {
  case TD_EXPR_TY_INVALID:
    BUG("invalid expr should not reach ir gen");
  case TD_EXPR_TY_BUILTIN:
    BUG("builtin should have been handled by call");
    break;
  case TD_EXPR_TY_COMPOUND_STMT: {
    struct td_stmt *last =
        &expr->compound_stmt.stmts[expr->compound_stmt.num_stmts - 1];

    size_t num_stmt;
    if (last->ty == TD_STMT_TY_EXPR) {
      num_stmt = expr->compound_stmt.num_stmts > 1
                     ? expr->compound_stmt.num_stmts - 1
                     : 0;
    } else {
      num_stmt = expr->compound_stmt.num_stmts;
    }

    struct ir_basicblock *basicblock = (*stmt)->basicblock;
    for (size_t i = 0; i < num_stmt; i++) {
      basicblock =
          build_ir_for_stmt(irb, basicblock, &expr->compound_stmt.stmts[i]);
    }

    *stmt = ir_alloc_stmt(irb->func, basicblock);

    if (last->ty == TD_STMT_TY_EXPR) {
      op = build_ir_for_expr(
          irb, stmt,
          &expr->compound_stmt.stmts[expr->compound_stmt.num_stmts - 1].expr);
    } else {
      op = NULL;
    }
    break;
  }
  case TD_EXPR_TY_VA_ARG:
    op = build_ir_for_va_arg(irb, stmt, &expr->va_arg);
    break;
  case TD_EXPR_TY_TERNARY:
    op = build_ir_for_ternary(irb, stmt, var_ty, &expr->ternary);
    break;
  case TD_EXPR_TY_VAR:
    op = build_ir_for_var(irb, stmt, var_ty, &expr->var);
    break;
  case TD_EXPR_TY_CNST:
    op = build_ir_for_cnst(irb, stmt, var_ty, expr);
    break;
  case TD_EXPR_TY_COMPOUNDEXPR:
    op = build_ir_for_compoundexpr(irb, stmt, var_ty, &expr->compound_expr);
    break;
  case TD_EXPR_TY_CALL:
    op = build_ir_for_call(irb, stmt, expr);
    break;
  case TD_EXPR_TY_UNARY_OP:
    op = build_ir_for_unaryop(irb, stmt, expr);
    break;
  case TD_EXPR_TY_BINARY_OP:
    op = build_ir_for_binaryop(irb, stmt, expr);
    break;
  case TD_EXPR_TY_ARRAYACCESS:
    op = build_ir_for_arrayaccess(irb, stmt, &expr->array_access);
    break;
  case TD_EXPR_TY_MEMBERACCESS:
    op = build_ir_for_memberaccess(irb, stmt, &expr->member_access,
                                   &expr->var_ty);
    break;
  case TD_EXPR_TY_POINTERACCESS:
    op = build_ir_for_pointeraccess(irb, stmt, &expr->pointer_access,
                                    &expr->var_ty);
    break;
  case TD_EXPR_TY_ASSG:
    op = build_ir_for_assg(irb, stmt, expr);
    break;
  case TD_EXPR_TY_SIZEOF:
    op = build_ir_for_sizeof(irb, stmt, expr);
    break;
  case TD_EXPR_TY_ALIGNOF:
    op = build_ir_for_alignof(irb, stmt, expr);
    break;
  case TD_EXPR_TY_COMPOUND_LITERAL:
    op = build_ir_for_compoundliteral(irb, stmt, NULL,
                                      BUILD_COMPOUNDLITERAL_MODE_LOAD, expr);
    break;
  }

  return op;
}

static void build_ensure_deferred(struct ir_func_builder *irb,
                                  struct ir_stmt **stmt) {
  *stmt = ir_alloc_stmt(irb->func, (*stmt)->basicblock);

  size_t i = vector_length(irb->defers);
  while (i > 1) {
    const struct ir_defer *defer = vector_get(irb->defers, i - 1);
    i--;

    if (defer->ty == IR_DEFER_TY_NEW_SCOPE) {
      break;
    }

    struct ir_basicblock *basicblock =
        build_ir_for_stmt(irb, (*stmt)->basicblock, defer->deferstmt->stmt);
    *stmt = basicblock->last;
  }
}

static struct ir_basicblock *
build_ir_for_compoundstmt(struct ir_func_builder *irb,
                          struct ir_basicblock *basicblock,
                          struct td_compoundstmt *compound_stmt) {
  vector_push_back(irb->defers,
                   &(struct ir_defer){.ty = IR_DEFER_TY_NEW_SCOPE});

  for (size_t i = 0; i < compound_stmt->num_stmts; i++) {
    basicblock = build_ir_for_stmt(irb, basicblock, &compound_stmt->stmts[i]);
  }

  while (true) {
    const struct ir_defer *defer = vector_pop(irb->defers);

    if (defer->ty == IR_DEFER_TY_NEW_SCOPE) {
      break;
    }

    basicblock = build_ir_for_stmt(irb, basicblock, defer->deferstmt->stmt);
  }

  return basicblock;
}

static struct ir_basicblock *build_ir_for_if(struct ir_func_builder *irb,
                                             struct ir_basicblock *basicblock,
                                             struct td_ifstmt *if_stmt) {

  struct ir_stmt *cond_stmt = ir_alloc_stmt(irb->func, basicblock);
  struct ir_op *cond = build_ir_for_expr(irb, &cond_stmt, &if_stmt->cond);

  struct ir_basicblock *pre_if_basicblock = cond->stmt->basicblock;

  // basic block for if body
  struct ir_basicblock *if_start_basicblock = ir_alloc_basicblock(irb->func);

  struct ir_stmt *br_cond_stmt =
      ir_alloc_stmt(irb->func, cond_stmt->basicblock);
  struct ir_op *br_cond = ir_alloc_op(irb->func, br_cond_stmt);
  br_cond->ty = IR_OP_TY_BR_COND;
  br_cond->var_ty = IR_VAR_TY_NONE;
  br_cond->br_cond.cond = cond;

  struct ir_basicblock *if_end_basicblock =
      build_ir_for_stmt(irb, if_start_basicblock, if_stmt->body);

  // we add a redundant branch to keep the nice property that all BBs end in a
  // branch
  struct ir_stmt *br_stmt = ir_alloc_stmt(irb->func, if_end_basicblock);
  struct ir_op *br = ir_alloc_op(irb->func, br_stmt);
  br->ty = IR_OP_TY_BR;
  br->var_ty = IR_VAR_TY_NONE;

  // basic block for *after* if body
  struct ir_basicblock *after_if_basicblock = ir_alloc_basicblock(irb->func);

  ir_make_basicblock_split(irb->func, pre_if_basicblock, if_start_basicblock,
                           after_if_basicblock);

  ir_make_basicblock_merge(irb->func, if_end_basicblock, after_if_basicblock);

  return after_if_basicblock;
}

static struct ir_basicblock *
build_ir_for_ifelse(struct ir_func_builder *irb,
                    struct ir_basicblock *basicblock,
                    struct td_ifelsestmt *if_else_stmt) {
  struct ir_stmt *cond_stmt = ir_alloc_stmt(irb->func, basicblock);
  struct ir_op *cond = build_ir_for_expr(irb, &cond_stmt, &if_else_stmt->cond);

  // basic block for if body
  struct ir_basicblock *if_basicblock = ir_alloc_basicblock(irb->func);
  struct ir_basicblock *after_if_bb =
      build_ir_for_stmt(irb, if_basicblock, if_else_stmt->body);

  // basic block for else body
  struct ir_basicblock *else_basicblock = ir_alloc_basicblock(irb->func);
  struct ir_basicblock *after_else_bb =
      build_ir_for_stmt(irb, else_basicblock, if_else_stmt->else_body);

  struct ir_basicblock *after_if_else_basicblock =
      ir_alloc_basicblock(irb->func);

  struct ir_basicblock *pre_if_basicblock = cond->stmt->basicblock;

  ir_make_basicblock_split(irb->func, pre_if_basicblock, if_basicblock,
                           else_basicblock);

  struct ir_stmt *br_cond_stmt = ir_alloc_stmt(irb->func, pre_if_basicblock);
  struct ir_op *br_cond = ir_alloc_op(irb->func, br_cond_stmt);
  br_cond->ty = IR_OP_TY_BR_COND;
  br_cond->var_ty = IR_VAR_TY_NONE;
  br_cond->br_cond.cond = cond;

  struct ir_stmt *br_after_if_stmt = ir_alloc_stmt(irb->func, after_if_bb);
  struct ir_op *br_after_if = ir_alloc_op(irb->func, br_after_if_stmt);
  br_after_if->ty = IR_OP_TY_BR;
  br_after_if->var_ty = IR_VAR_TY_NONE;
  ir_make_basicblock_merge(irb->func, after_if_bb, after_if_else_basicblock);

  struct ir_stmt *br_after_else_stmt = ir_alloc_stmt(irb->func, after_else_bb);
  struct ir_op *br_after_else = ir_alloc_op(irb->func, br_after_else_stmt);
  br_after_else->ty = IR_OP_TY_BR;
  br_after_else->var_ty = IR_VAR_TY_NONE;
  ir_make_basicblock_merge(irb->func, after_else_bb, after_if_else_basicblock);

  return after_if_else_basicblock;
}

static struct ir_basicblock *
build_ir_for_switch(struct ir_func_builder *irb,
                    struct ir_basicblock *basicblock,
                    struct td_switchstmt *switch_stmt) {
  struct ir_jump new_loop = {.ty = IR_JUMP_TY_NEW_LOOP};
  vector_push_back(irb->jumps, &new_loop);

  struct ir_case new_switch = {.ty = IR_CASE_TY_NEW_SWITCH};
  vector_push_back(irb->switch_cases, &new_switch);

  struct ir_stmt *ctrl_stmt = ir_alloc_stmt(irb->func, basicblock);
  struct ir_op *ctrl_op =
      build_ir_for_expr(irb, &ctrl_stmt, &switch_stmt->ctrl_expr);

  struct ir_stmt *switch_op_stmt =
      ir_alloc_stmt(irb->func, ctrl_stmt->basicblock);
  struct ir_op *switch_op = ir_alloc_op(irb->func, switch_op_stmt);
  switch_op->ty = IR_OP_TY_BR_SWITCH;
  switch_op->var_ty = IR_VAR_TY_NONE;
  switch_op->br_switch = (struct ir_op_br_switch){.value = ctrl_op};

  struct ir_basicblock *body_bb = ir_alloc_basicblock(irb->func);
  struct ir_basicblock *end_bb =
      build_ir_for_stmt(irb, body_bb, switch_stmt->body);

  struct ir_basicblock *after_body_bb = ir_alloc_basicblock(irb->func);
  ir_make_basicblock_merge(irb->func, end_bb, after_body_bb);
  struct ir_stmt *br_stmt = ir_alloc_stmt(irb->func, end_bb);
  struct ir_op *br = ir_alloc_op(irb->func, br_stmt);
  br->ty = IR_OP_TY_BR;
  br->var_ty = IR_VAR_TY_NONE;

  struct ir_basicblock *default_block = NULL;

  struct vector *cases = vector_create(sizeof(struct ir_split_case));

  while (!vector_empty(irb->switch_cases)) {
    struct ir_case *switch_case = vector_pop(irb->switch_cases);

    switch (switch_case->ty) {
    case IR_CASE_TY_NEW_SWITCH:
      goto jumps;
    case IR_CASE_TY_CASE: {
      vector_push_back(cases, &switch_case->split_case);
      break;
    }
    case IR_CASE_TY_DEFAULT:
      default_block = switch_case->split_case.target;
      break;
    }
  }

jumps:

  if (!default_block) {
    default_block = after_body_bb;
  }

  ir_make_basicblock_switch(irb->func, basicblock, vector_length(cases),
                            vector_head(cases), default_block);

  struct vector *continues = vector_create(sizeof(struct ir_jump));

  while (!vector_empty(irb->jumps)) {
    struct ir_jump *jump = vector_pop(irb->jumps);

    switch (jump->ty) {
    case IR_JUMP_TY_NEW_LOOP:
      // end
      goto end;
    case IR_JUMP_TY_BREAK: {
      ir_make_basicblock_merge(irb->func, jump->basicblock, after_body_bb);
      struct ir_stmt *break_br_stmt =
          ir_alloc_stmt(irb->func, jump->basicblock);
      struct ir_op *break_br = ir_alloc_op(irb->func, break_br_stmt);
      break_br->ty = IR_OP_TY_BR;
      break_br->var_ty = IR_VAR_TY_NONE;
      break;
    }
    case IR_JUMP_TY_CONTINUE:
      vector_push_back(continues, jump);
      break;
    }
  }

end:
  // propogate the `continue`s to the next level up
  vector_extend(irb->jumps, vector_head(continues), vector_length(continues));

  vector_free(&continues);
  vector_free(&cases);

  return after_body_bb;
}

static struct ir_basicblock *
build_ir_for_selectstmt(struct ir_func_builder *irb,
                        struct ir_basicblock *basicblock,
                        struct td_selectstmt *select_stmt) {
  switch (select_stmt->ty) {
  case TD_SELECTSTMT_TY_IF: {
    return build_ir_for_if(irb, basicblock, &select_stmt->if_stmt);
  }
  case TD_SELECTSTMT_TY_IF_ELSE:
    return build_ir_for_ifelse(irb, basicblock, &select_stmt->if_else_stmt);
  case TD_SELECTSTMT_TY_SWITCH:
    return build_ir_for_switch(irb, basicblock, &select_stmt->switch_stmt);
  }
}

static void build_ir_for_declaration(struct ir_func_builder *irb,
                                     struct ir_stmt **stmt,
                                     struct td_declaration *declaration);

static void
build_ir_for_declorexpr(struct ir_func_builder *irb, struct ir_stmt **stmt,
                        struct td_declaration_or_expr *decl_or_expr) {
  switch (decl_or_expr->ty) {
  case TD_DECLARATION_OR_EXPR_TY_DECL:
    build_ir_for_declaration(irb, stmt, &decl_or_expr->decl);
    break;
  case TD_DECLARATION_OR_EXPR_TY_EXPR:
    build_ir_for_expr(irb, stmt, &decl_or_expr->expr);
    break;
  }
}

struct ir_loop {
  // for CONTINUE
  struct ir_basicblock *entry;
  // for BREAK
  struct ir_basicblock *exit;
};

static struct ir_loop build_ir_for_whilestmt(struct ir_func_builder *irb,
                                             struct ir_basicblock *basicblock,
                                             struct td_whilestmt *while_stmt) {

  struct ir_basicblock *before_cond_basicblock = basicblock;
  struct ir_basicblock *cond_basicblock = ir_alloc_basicblock(irb->func);

  ir_make_basicblock_merge(irb->func, before_cond_basicblock, cond_basicblock);
  struct ir_stmt *pre_cond_stmt =
      ir_alloc_stmt(irb->func, before_cond_basicblock);
  struct ir_op *pre_cond_br = ir_alloc_op(irb->func, pre_cond_stmt);
  pre_cond_br->ty = IR_OP_TY_BR;
  pre_cond_br->var_ty = IR_VAR_TY_NONE;

  struct ir_stmt *cond_stmt = ir_alloc_stmt(irb->func, cond_basicblock);
  struct ir_op *cond = build_ir_for_expr(irb, &cond_stmt, &while_stmt->cond);
  struct ir_stmt *cond_br_stmt =
      ir_alloc_stmt(irb->func, cond_stmt->basicblock);
  struct ir_op *cond_br = ir_alloc_op(irb->func, cond_br_stmt);
  cond_br->ty = IR_OP_TY_BR_COND;
  cond_br->var_ty = IR_VAR_TY_NONE;
  cond_br->br_cond.cond = cond;

  struct ir_basicblock *cond_stmt_basicblock = cond_stmt->basicblock;

  struct ir_basicblock *body_basicblock = ir_alloc_basicblock(irb->func);
  struct ir_basicblock *body_stmt_basicblock =
      build_ir_for_stmt(irb, body_basicblock, while_stmt->body);
  struct ir_basicblock *after_body_basicblock = ir_alloc_basicblock(irb->func);

  ir_make_basicblock_split(irb->func, cond_stmt_basicblock, body_basicblock,
                           after_body_basicblock);

  ir_make_basicblock_merge(irb->func, body_stmt_basicblock, cond_basicblock);
  struct ir_stmt *br_stmt = ir_alloc_stmt(irb->func, body_stmt_basicblock);
  struct ir_op *br = ir_alloc_op(irb->func, br_stmt);
  br->ty = IR_OP_TY_BR;
  br->var_ty = IR_VAR_TY_NONE;

  return (struct ir_loop){.entry = cond_basicblock,
                          .exit = after_body_basicblock};
}

static struct ir_loop
build_ir_for_dowhilestmt(struct ir_func_builder *irb,
                         struct ir_basicblock *basicblock,
                         struct td_dowhilestmt *do_while_stmt) {
  struct ir_basicblock *before_body_basicblock = basicblock;
  struct ir_stmt *pre_cond_stmt =
      ir_alloc_stmt(irb->func, before_body_basicblock);
  struct ir_op *pre_body_br = ir_alloc_op(irb->func, pre_cond_stmt);
  pre_body_br->ty = IR_OP_TY_BR;
  pre_body_br->var_ty = IR_VAR_TY_NONE;

  struct ir_basicblock *body_basicblock = ir_alloc_basicblock(irb->func);

  ir_make_basicblock_merge(irb->func, before_body_basicblock, body_basicblock);

  struct ir_basicblock *body_stmt_basicblock =
      build_ir_for_stmt(irb, body_basicblock, do_while_stmt->body);

  struct ir_basicblock *cond_basicblock = ir_alloc_basicblock(irb->func);
  struct ir_stmt *cond_stmt = ir_alloc_stmt(irb->func, cond_basicblock);
  struct ir_op *cond = build_ir_for_expr(irb, &cond_stmt, &do_while_stmt->cond);

  struct ir_stmt *cond_br_stmt =
      ir_alloc_stmt(irb->func, cond_stmt->basicblock);
  struct ir_op *cond_br = ir_alloc_op(irb->func, cond_br_stmt);
  cond_br->ty = IR_OP_TY_BR_COND;
  cond_br->var_ty = IR_VAR_TY_NONE;
  cond_br->br_cond.cond = cond;

  ir_make_basicblock_merge(irb->func, body_stmt_basicblock, cond_basicblock);
  struct ir_stmt *br_stmt = ir_alloc_stmt(irb->func, body_stmt_basicblock);
  struct ir_op *br = ir_alloc_op(irb->func, br_stmt);
  br->ty = IR_OP_TY_BR;
  br->var_ty = IR_VAR_TY_NONE;

  struct ir_basicblock *cond_stmt_basicblock = cond_stmt->basicblock;

  struct ir_basicblock *after_cond_basicblock = ir_alloc_basicblock(irb->func);
  ir_make_basicblock_split(irb->func, cond_stmt_basicblock, body_basicblock,
                           after_cond_basicblock);

  return (struct ir_loop){.entry = cond_basicblock,
                          .exit = after_cond_basicblock};
}

static struct ir_loop build_ir_for_forstmt(struct ir_func_builder *irb,
                                           struct ir_basicblock *basicblock,
                                           struct td_forstmt *for_stmt) {

  struct ir_basicblock *before_cond_basicblock = basicblock;
  struct ir_basicblock *before_body_basicblock = basicblock;
  struct ir_basicblock *after_cond_basicblock = basicblock;

  if (for_stmt->init) {
    struct ir_stmt *init_stmt =
        ir_alloc_stmt(irb->func, before_cond_basicblock);
    build_ir_for_declorexpr(irb, &init_stmt, for_stmt->init);

    before_cond_basicblock = init_stmt->basicblock;
    before_body_basicblock = init_stmt->basicblock;
    after_cond_basicblock = init_stmt->basicblock;
  }

  if (for_stmt->cond) {
    struct ir_basicblock *cond_basicblock = ir_alloc_basicblock(irb->func);
    ir_make_basicblock_merge(irb->func, before_cond_basicblock,
                             cond_basicblock);

    struct ir_stmt *to_cond_stmt =
        ir_alloc_stmt(irb->func, before_cond_basicblock);
    struct ir_op *to_cond_br = ir_alloc_op(irb->func, to_cond_stmt);
    to_cond_br->ty = IR_OP_TY_BR;
    to_cond_br->var_ty = IR_VAR_TY_NONE;

    struct ir_stmt *cond_stmt = ir_alloc_stmt(irb->func, cond_basicblock);
    struct ir_op *cond = build_ir_for_expr(irb, &cond_stmt, for_stmt->cond);

    struct ir_stmt *cond_br_stmt =
        ir_alloc_stmt(irb->func, cond_stmt->basicblock);
    struct ir_op *cond_br = ir_alloc_op(irb->func, cond_br_stmt);
    cond_br->ty = IR_OP_TY_BR_COND;
    cond_br->var_ty = IR_VAR_TY_NONE;
    cond_br->br_cond.cond = cond;

    before_body_basicblock = cond_basicblock;
    after_cond_basicblock = cond_stmt->basicblock;
  } else {
    struct ir_stmt *to_body_stmt =
        ir_alloc_stmt(irb->func, before_body_basicblock);
    struct ir_op *to_body_br = ir_alloc_op(irb->func, to_body_stmt);
    to_body_br->ty = IR_OP_TY_BR;
    to_body_br->var_ty = IR_VAR_TY_NONE;
  }

  struct ir_basicblock *body_basicblock = ir_alloc_basicblock(irb->func);
  ir_make_basicblock_merge(irb->func, after_cond_basicblock, body_basicblock);

  if (!for_stmt->cond) {
    before_body_basicblock = body_basicblock;
  }

  struct ir_basicblock *body_stmt_basicblock =
      build_ir_for_stmt(irb, body_basicblock, for_stmt->body);

  struct ir_basicblock *end_body_basicblock = body_stmt_basicblock;

  if (for_stmt->iter) {
    struct ir_basicblock *iter_basicblock = ir_alloc_basicblock(irb->func);
    ir_make_basicblock_merge(irb->func, body_stmt_basicblock, iter_basicblock);

    struct ir_stmt *to_iter_stmt =
        ir_alloc_stmt(irb->func, body_stmt_basicblock);
    struct ir_op *to_iter_br = ir_alloc_op(irb->func, to_iter_stmt);
    to_iter_br->ty = IR_OP_TY_BR;
    to_iter_br->var_ty = IR_VAR_TY_NONE;

    struct ir_stmt *iter_stmt = ir_alloc_stmt(irb->func, iter_basicblock);
    build_ir_for_expr(irb, &iter_stmt, for_stmt->iter);

    end_body_basicblock = iter_stmt->basicblock;
  }

  struct ir_stmt *end_stmt = ir_alloc_stmt(irb->func, end_body_basicblock);
  struct ir_op *end_br = ir_alloc_op(irb->func, end_stmt);
  end_br->ty = IR_OP_TY_BR;
  end_br->var_ty = IR_VAR_TY_NONE;
  ir_make_basicblock_merge(irb->func, end_body_basicblock,
                           before_body_basicblock);

  struct ir_basicblock *after_body_basicblock = ir_alloc_basicblock(irb->func);

  if (for_stmt->cond) {
    ir_make_basicblock_split(irb->func, after_cond_basicblock, body_basicblock,
                             after_body_basicblock);
  }

  return (struct ir_loop){.entry = end_body_basicblock,
                          .exit = after_body_basicblock};
}

static struct ir_basicblock *
build_ir_for_iterstmt(struct ir_func_builder *irb,
                      struct ir_basicblock *basicblock,
                      struct td_iterstmt *iter_stmt) {
  struct ir_jump new_loop = {.ty = IR_JUMP_TY_NEW_LOOP};
  vector_push_back(irb->jumps, &new_loop);

  struct ir_loop loop;
  switch (iter_stmt->ty) {
  case TD_ITERSTMT_TY_WHILE:
    loop = build_ir_for_whilestmt(irb, basicblock, &iter_stmt->while_stmt);
    break;
  case TD_ITERSTMT_TY_DO_WHILE:
    loop = build_ir_for_dowhilestmt(irb, basicblock, &iter_stmt->do_while_stmt);
    break;
  case TD_ITERSTMT_TY_FOR:
    loop = build_ir_for_forstmt(irb, basicblock, &iter_stmt->for_stmt);
    break;
  }

  for (size_t i = vector_length(irb->jumps); i; i--) {
    struct ir_jump *jump = vector_pop(irb->jumps);

    switch (jump->ty) {
    case IR_JUMP_TY_NEW_LOOP:
      return loop.exit;
    case IR_JUMP_TY_BREAK:
      ir_make_basicblock_merge(irb->func, jump->basicblock, loop.exit);
      break;
    case IR_JUMP_TY_CONTINUE:
      ir_make_basicblock_merge(irb->func, jump->basicblock, loop.entry);
      break;
    }

    struct ir_stmt *br_stmt = ir_alloc_stmt(irb->func, jump->basicblock);
    struct ir_op *br = ir_alloc_op(irb->func, br_stmt);
    br->ty = IR_OP_TY_BR;
    br->var_ty = IR_VAR_TY_NONE;
  }

  BUG("should've found IR_JUMP_TY_NEW_LOOP in jump vector");
}

static struct ir_basicblock *build_ir_for_goto(struct ir_func_builder *irb,
                                               struct ir_stmt **stmt,
                                               struct td_gotostmt *goto_stmt) {
  struct ir_basicblock *before_goto_basicblock = (*stmt)->basicblock;

  struct ir_stmt *br_stmt = ir_alloc_stmt(irb->func, before_goto_basicblock);
  struct ir_op *br = ir_alloc_op(irb->func, br_stmt);

  br->ty = IR_OP_TY_BR;
  br->var_ty = IR_VAR_TY_NONE;

  // put the label we target into metadata
  // copy it out to ignore const warnings
  size_t label_len = goto_stmt->label.len;
  br->metadata = aralloc(irb->arena, label_len + 1);
  memcpy(br->metadata, goto_stmt->label.str, label_len + 1);

  struct ir_basicblock *after_goto_basicblock = ir_alloc_basicblock(irb->func);
  return after_goto_basicblock;
}

/* Return stmt be null when this is used to add implicit returns not in code
 * (e.g at end of method) */
static struct ir_basicblock *
build_ir_for_ret(struct ir_func_builder *irb, struct ir_stmt **stmt,
                 struct td_returnstmt *return_stmt) {
  build_ensure_deferred(irb, stmt);

  struct ir_op *expr_op;
  if (return_stmt && return_stmt->expr) {
    expr_op = build_ir_for_expr(irb, stmt, return_stmt->expr);
    expr_op = store_load_if_needed(irb, stmt, expr_op);
  } else {
    expr_op = NULL;
  }

  *stmt = ir_alloc_stmt(irb->func, (*stmt)->basicblock);

  struct ir_op *op = ir_alloc_op(irb->func, *stmt);
  op->ty = IR_OP_TY_RET;
  op->var_ty =
      return_stmt && return_stmt->expr
          ? ir_var_ty_for_td_var_ty(irb->unit, &return_stmt->expr->var_ty)
          : IR_VAR_TY_NONE;
  op->ret.value = expr_op;

  op->stmt->basicblock->ty = IR_BASICBLOCK_TY_RET;

  struct ir_basicblock *after_ret_basicblock = ir_alloc_basicblock(irb->func);

  return after_ret_basicblock;
}

static struct ir_basicblock *build_ir_for_break(struct ir_func_builder *irb,
                                                struct ir_stmt **stmt) {
  build_ensure_deferred(irb, stmt);

  struct ir_jump jump = {.ty = IR_JUMP_TY_BREAK,
                         .basicblock = (*stmt)->basicblock};
  vector_push_back(irb->jumps, &jump);

  struct ir_basicblock *after_break_basicblock = ir_alloc_basicblock(irb->func);
  return after_break_basicblock;
}

static struct ir_basicblock *build_ir_for_continue(struct ir_func_builder *irb,
                                                   struct ir_stmt **stmt) {
  build_ensure_deferred(irb, stmt);

  struct ir_jump jump = {.ty = IR_JUMP_TY_CONTINUE,
                         .basicblock = (*stmt)->basicblock};
  vector_push_back(irb->jumps, &jump);

  struct ir_basicblock *after_continue_basicblock =
      ir_alloc_basicblock(irb->func);
  return after_continue_basicblock;
}

static struct ir_basicblock *
build_ir_for_jumpstmt(struct ir_func_builder *irb,
                      struct ir_basicblock *basicblock,
                      struct td_jumpstmt *jump_stmt) {
  struct ir_stmt *stmt = ir_alloc_stmt(irb->func, basicblock);

  switch (jump_stmt->ty) {
  case TD_JUMPSTMT_TY_RETURN:
    return build_ir_for_ret(irb, &stmt, &jump_stmt->return_stmt);
  case TD_JUMPSTMT_TY_GOTO:
    return build_ir_for_goto(irb, &stmt, &jump_stmt->goto_stmt);
  case TD_JUMPSTMT_TY_BREAK:
    return build_ir_for_break(irb, &stmt);
  case TD_JUMPSTMT_TY_CONTINUE:
    return build_ir_for_continue(irb, &stmt);
  }
}

// describes a fully flattened init list
// init lists in functions then build `expr` to `ir_op`s, while global ones turn
// it into `ir_var`s
struct ir_build_init {
  size_t offset;
  bool is_bitfield;
  struct ir_bitfield bitfield;
  struct td_expr *expr;
};

struct ir_build_init_list_layout {
  size_t num_inits;
  struct ir_build_init *inits;
};

static struct ir_build_init_list_layout
build_init_list_layout(struct ir_unit *iru, struct typechk *tchk,
                       const struct td_init_list *init_list);

struct init_range {
  size_t offset;
  size_t size;
};

static int sort_ranges_by_offset(const void *l, const void *r) {
  return (ssize_t)((const struct init_range *)l)->offset -
         (ssize_t)((const struct init_range *)r)->offset;
}

static void build_ir_zero_range(struct ir_func_builder *irb,
                                struct ir_stmt *stmt,
                                struct ir_op *insert_before,
                                struct ir_op *address, size_t byte_size) {
  if (!byte_size) {
    return;
  }

  struct ir_op *mem_set;
  if (insert_before) {
    mem_set = ir_insert_before_op(irb->func, insert_before, IR_OP_TY_MEM_SET,
                                  IR_VAR_TY_NONE);
  } else {
    mem_set = ir_alloc_op(irb->func, stmt);
    mem_set->ty = IR_OP_TY_MEM_SET;
    mem_set->var_ty = IR_VAR_TY_NONE;
  }

  mem_set->mem_set =
      (struct ir_op_mem_set){.addr = address, .length = byte_size, .value = 0};
}

static void build_ir_for_init_list(struct ir_func_builder *irb,
                                   struct ir_stmt **stmt, struct ir_op *address,
                                   struct td_init_list *init_list) {
  if (td_var_ty_is_scalar_ty(&init_list->var_ty)) {
    DEBUG_ASSERT(init_list->num_inits == 1, "expected 1 init");

    struct td_init_list_init *init = &init_list->inits[0];

    DEBUG_ASSERT(!init->designator_list, "scalar should not have designator");
    DEBUG_ASSERT(init->init->ty == TD_INIT_TY_EXPR,
                 "scalar should have expr init");

    // BUG: this needs to write an op to var refs for phi gen

    struct ir_op *value = build_ir_for_expr(irb, stmt, &init->init->expr);

    if (address) {
      struct ir_op *store =
          ir_append_op(irb->func, *stmt, IR_OP_TY_STORE, IR_VAR_TY_NONE);

      store->store = (struct ir_op_store){
          .ty = IR_OP_STORE_TY_ADDR, .addr = address, .value = value};
    }

    return;
  }

  struct ir_build_init_list_layout layout =
      build_init_list_layout(irb->unit, irb->tchk, init_list);

  struct vector *init_ranges = vector_create(sizeof(struct init_range));

  // add a "fake range" to cover the start of the struct
  struct init_range start_range = {.offset = 0, .size = 0};
  vector_push_back(init_ranges, &start_range);

  struct ir_op *first_init = NULL;

  for (size_t i = 0; i < layout.num_inits; i++) {
    struct ir_build_init *init = &layout.inits[i];

    struct ir_op *value = build_ir_for_expr(irb, stmt, init->expr);

    struct ir_op *init_address = address;

    if (init->offset) {
      init_address = ir_alloc_op(irb->func, *stmt);
      init_address->ty = IR_OP_TY_ADDR_OFFSET;
      init_address->var_ty = IR_VAR_TY_POINTER;
      init_address->addr_offset =
          (struct ir_op_addr_offset){.base = address, .offset = init->offset};
    }

    struct ir_op *store = ir_alloc_op(irb->func, *stmt);
    if (init->is_bitfield) {
      store->ty = IR_OP_TY_STORE_BITFIELD;
      store->var_ty = IR_VAR_TY_NONE;
      store->store_bitfield =
          (struct ir_op_store_bitfield){.ty = IR_OP_STORE_TY_ADDR,
                                        .addr = init_address,
                                        .value = value,
                                        .bitfield = init->bitfield};
    } else {
      store->ty = IR_OP_TY_STORE;
      store->var_ty = IR_VAR_TY_NONE;
      store->store = (struct ir_op_store){
          .ty = IR_OP_STORE_TY_ADDR, .addr = init_address, .value = value};
    }

    if (!first_init) {
      first_init = store;
    }

    struct ir_var_ty var_ty =
        ir_var_ty_for_td_var_ty(irb->unit, &init->expr->var_ty);
    struct ir_var_ty_info info = ir_var_ty_info(irb->unit, &var_ty);

    struct init_range range = {.offset = init->offset, .size = info.size};

    vector_push_back(init_ranges, &range);
  }

  vector_sort(init_ranges, sort_ranges_by_offset);

  struct ir_var_ty var_ty =
      ir_var_ty_for_td_var_ty(irb->unit, &init_list->var_ty);
  struct ir_var_ty_info info = ir_var_ty_info(irb->unit, &var_ty);

  // add a "fake range" to cover the end of the struct
  struct init_range end_range = {.offset = info.size, .size = 0};
  vector_push_back(init_ranges, &end_range);

  // TODO: logic to determine if all fields are set can be improved
  // size_t num_offsets = vector_length(init_ranges);

  // bool needs_zero = false;
  // size_t head = 0;
  // size_t end = info.size;
  // for (size_t i = 0; i < num_offsets + 1; i++) {
  //   size_t new_head;
  //   size_t offset;

  //   if (i < num_offsets) {
  //     struct init_range *init_range = vector_get(init_ranges, i);
  //     new_head = init_range->offset + init_range->size;
  //     offset = init_range->offset;
  //   } else {
  //     new_head = head;
  //     offset = end;
  //   }

  //   if (i != 0) {
  //     ssize_t gap = (ssize_t)offset - (ssize_t)head;
  //     DEBUG_ASSERT(gap >= 0, "bad math");

  //     needs_zero = true;
  //     break;
  //   }

  //   head = new_head;
  // }

  // if (needs_zero) {

  // FIXME: we always zero because:
  //   * the code above doesn't handle arbitrary order inits (from designated
  //   initializers)
  //   * opts_promote works better if it can see whole thing is zeroed
  build_ir_zero_range(irb, *stmt, first_init, address, info.size);
  // }

  vector_free(&init_ranges);
}

static struct ir_op *build_ir_for_init(struct ir_func_builder *irb,
                                       struct ir_stmt **stmt,
                                       struct ir_op *start_address,
                                       struct td_init *init) {
  switch (init->ty) {
  case TD_INIT_TY_EXPR:
    // FIXME: special case compound expr so it gets the local to write into
    // this logic is BROKEN if a cast is needed (e.g `struct foo a = { .field =
    // (int){1} }`;
    if (init->expr.ty == TD_EXPR_TY_COMPOUND_LITERAL) {
      enum build_compoundliteral_mode mode =
          td_var_ty_is_scalar_ty(&init->expr.var_ty)
              ? BUILD_COMPOUNDLITERAL_MODE_LOAD
              : BUILD_COMPOUNDLITERAL_MODE_ADDR;

      struct ir_op *value = build_ir_for_compoundliteral(
          irb, stmt, start_address, mode, &init->expr);

      // null signifies build_ir_for_var should not insert a STORE
      // so if build_ir_for_compoundliteral did the writing (non scalar) return
      // null
      return mode == BUILD_COMPOUNDLITERAL_MODE_ADDR ? NULL : value;
    } else {
      return build_ir_for_expr(irb, stmt, &init->expr);
    }
  case TD_INIT_TY_INIT_LIST:
    DEBUG_ASSERT(start_address,
                 "start_address required when building with init list");
    build_ir_for_init_list(irb, stmt, start_address, &init->init_list);
    return NULL;
  }
}

static struct ir_var_value
build_ir_for_var_value(struct ir_var_builder *irb, struct td_init *init,
                       const struct td_var_ty *var_ty);

static void
build_ir_for_global_var(struct ir_var_builder *irb, struct ir_func *func,
                        struct var_refs *var_refs,
                        enum td_storage_class_specifier storage_class,
                        enum td_function_specifier_flags func_specifiers,
                        const struct td_var_declaration *decl) {
  // `extern struct c` is allowed for an incomplete type
  // so we need to handle that
  struct ir_var_ty var_ty =
      ir_var_ty_for_decl_td_var_ty(irb->unit, &decl->var_ty);

  ustr_t name = decl->var.identifier;
  const char *symbol_name;
  if (storage_class == TD_STORAGE_CLASS_SPECIFIER_STATIC &&
      var_ty.ty != IR_VAR_TY_TY_FUNC) {
    symbol_name = mangle_static_name(irb, func, name);
  } else {
    symbol_name = aralloc_strndup(irb->arena, name.str, name.len);
  }

  struct var_key key = {.name = name,
                        .scope = decl->var_ty.ty == TD_VAR_TY_TY_FUNC
                                     ? SCOPE_GLOBAL
                                     : decl->var.scope};

  enum ir_glb_ty ty;
  if (decl->var_ty.ty == TD_VAR_TY_TY_FUNC) {
    ty = IR_GLB_TY_FUNC;
  } else {
    ty = IR_GLB_TY_DATA;
  }

  struct var_ref *ref = var_refs_get(var_refs, &key);

  if (ref) {
    DEBUG_ASSERT(ref->glb, "ref but has no glb");
  }

  enum ir_linkage linkage;
  bool is_func = decl->var_ty.ty == TD_VAR_TY_TY_FUNC;
  bool is_extern = storage_class == TD_STORAGE_CLASS_SPECIFIER_EXTERN;
  bool is_static = storage_class == TD_STORAGE_CLASS_SPECIFIER_STATIC;
  bool is_inline = func_specifiers & TD_FUNCTION_SPECIFIER_FLAG_INLINE;
  bool is_file_scope = key.scope == SCOPE_GLOBAL;
  bool is_unspecified_storage =
      storage_class == TD_STORAGE_CLASS_SPECIFIER_NONE;

  if ((is_func && !is_static && !is_inline) || is_extern ||
      (is_file_scope && !is_inline && !is_static)) {
    linkage = IR_LINKAGE_EXTERNAL;
  } else if (is_file_scope && is_static) {
    linkage = IR_LINKAGE_INTERNAL;
  } else {
    linkage = IR_LINKAGE_NONE;
  }

  enum ir_glb_def_ty def_ty;
  if (decl->init || !is_file_scope ||
      (ref && ref->glb->def_ty == IR_GLB_DEF_TY_DEFINED)) {
    def_ty = IR_GLB_DEF_TY_DEFINED;
  } else if (is_file_scope && !is_func &&
             (is_unspecified_storage || is_static)) {
    def_ty = IR_GLB_DEF_TY_TENTATIVE;
  } else {
    def_ty = IR_GLB_DEF_TY_UNDEFINED;
  }

  if (ref && linkage == IR_LINKAGE_EXTERNAL &&
      ref->glb->linkage == IR_LINKAGE_INTERNAL) {
    // extern but prev was static, stays static
    linkage = IR_LINKAGE_INTERNAL;
  }

  if (!ref) {
    ref = var_refs_add(var_refs, &key, VAR_REF_TY_GLB);
  }

  if (!ref->glb) {
    ref->glb = ir_add_global(irb->unit, ty, &var_ty, def_ty, symbol_name);
  }

  struct ir_glb *glb = ref->glb;

  if (decl->var_ty.attrs.weak) {
    glb->flags |= IR_GLB_FLAG_WEAK;
  }

  glb->def_ty = def_ty;
  glb->linkage = linkage;

  if (def_ty == IR_GLB_DEF_TY_TENTATIVE) {
    // already defined, and this is tentative, so do nothing
    return;
  }

  if (is_func) {
    return;
  }

  struct ir_var_value value;
  if (decl->init) {
    value = build_ir_for_var_value(irb, decl->init, &decl->var_ty);
  } else {
    value = (struct ir_var_value){.ty = IR_VAR_VALUE_TY_ZERO, .var_ty = var_ty};
  }

  if (!glb->var) {
    glb->var = aralloc(irb->arena, sizeof(*glb->var));
  }

  *glb->var = (struct ir_var){.unit = irb->unit,
                              .ty = IR_VAR_TY_DATA,
                              .var_ty = var_ty,
                              .value = value};
}

static void
build_ir_for_global_declaration(struct ir_var_builder *irb,
                                struct ir_func *func, struct var_refs *var_refs,
                                struct td_declaration *declaration) {
  for (size_t i = 0; i < declaration->num_var_declarations; i++) {
    if (declaration->storage_class_specifier ==
        TD_STORAGE_CLASS_SPECIFIER_TYPEDEF) {
      continue;
    }

    build_ir_for_global_var(irb, func, var_refs,
                            declaration->storage_class_specifier,
                            declaration->function_specifier_flags,
                            &declaration->var_declarations[i]);
  }
}

struct ir_var_def {
  struct ir_lcl *lcl;
};

static struct ir_var_def build_ir_var(struct ir_func_builder *irb,
                                      struct ir_stmt **stmt, struct td_var *var,
                                      const struct ir_var_ty *var_ty) {
  struct ir_lcl *lcl;

  if ((irb->flags & IR_BUILD_FLAG_SPILL_ALL) ||
      ir_var_ty_is_aggregate(var_ty) || var_ty->ty == IR_VAR_TY_TY_ARRAY) {
    // this is a new var, so we can safely create a new ref
    struct var_key key = get_var_key(var, (*stmt)->basicblock);
    struct var_ref *ref = var_refs_add(irb->var_refs, &key, VAR_REF_TY_LCL);
    ref->lcl = ir_add_local(irb->func, var_ty);

    lcl = ref->lcl;
  } else {
    lcl = NULL;
  }

  return (struct ir_var_def){.lcl = lcl};
}

static void build_ir_for_auto_var(struct ir_func_builder *irb,
                                  struct ir_stmt **stmt,
                                  struct td_var_declaration *decl) {
  struct ir_var_ty var_ty = ir_var_ty_for_td_var_ty(irb->unit, &decl->var_ty);
  struct ir_var_def def = build_ir_var(irb, stmt, &decl->var, &var_ty);
  struct ir_lcl *lcl = def.lcl;

  struct ir_op *assignment = NULL;
  if (decl->init) {
    struct ir_op *address = NULL;

    if (lcl) {
      address = ir_alloc_op(irb->func, *stmt);
      address->ty = IR_OP_TY_ADDR;
      address->var_ty = IR_VAR_TY_POINTER;
      address->addr = (struct ir_op_addr){.ty = IR_OP_ADDR_TY_LCL, .lcl = lcl};
    }

    assignment = build_ir_for_init(irb, stmt, address, decl->init);
  } else if (!lcl) {
    assignment = ir_alloc_op(irb->func, *stmt);
    assignment->ty = IR_OP_TY_UNDF;
    assignment->var_ty = ir_var_ty_for_td_var_ty(irb->unit, &decl->var_ty);
  }

  if (lcl && assignment) {
    if (lcl->var_ty.ty == IR_VAR_TY_TY_ARRAY &&
        assignment->ty == IR_OP_TY_ADDR &&
        assignment->addr.ty == IR_OP_ADDR_TY_GLB) {
      // `const char[] foo = "string literal"`
      // so need to load
      struct ir_glb *glb = assignment->addr.glb;
      assignment->ty = IR_OP_TY_LOAD;
      assignment->var_ty = lcl->var_ty;
      assignment->load =
          (struct ir_op_load){.ty = IR_OP_LOAD_TY_GLB, .glb = glb};
    }

    struct ir_op *str = ir_alloc_op(irb->func, *stmt);
    str->ty = IR_OP_TY_STORE;
    str->var_ty = IR_VAR_TY_NONE;
    str->store = (struct ir_op_store){
        .ty = IR_OP_STORE_TY_LCL, .lcl = lcl, .value = assignment};
  } else if (assignment) {
    var_assg(irb, *stmt, assignment, &decl->var);
  }
}

// this is called for decl lists WITHIN a function (i.e default is local
// storage)
static void build_ir_for_declaration(struct ir_func_builder *irb,
                                     struct ir_stmt **stmt,
                                     struct td_declaration *declaration) {
  if (declaration->storage_class_specifier ==
      TD_STORAGE_CLASS_SPECIFIER_TYPEDEF) {
    return;
  }

  for (size_t i = 0; i < declaration->num_var_declarations; i++) {
    struct td_var_declaration *decl = &declaration->var_declarations[i];

    if (decl->var_ty.ty != TD_VAR_TY_TY_FUNC &&
        (declaration->storage_class_specifier ==
             TD_STORAGE_CLASS_SPECIFIER_NONE ||
         declaration->storage_class_specifier ==
             TD_STORAGE_CLASS_SPECIFIER_AUTO ||
         declaration->storage_class_specifier ==
             TD_STORAGE_CLASS_SPECIFIER_REGISTER)) {
      build_ir_for_auto_var(irb, stmt, decl);
    } else {
      struct ir_var_builder builder = {
          .arena = irb->arena,
          .tchk = irb->tchk,
          .unit = irb->unit,
          .global_var_refs = irb->global_var_refs,
      };

      build_ir_for_global_var(&builder, irb->func, irb->var_refs,
                              declaration->storage_class_specifier,
                              declaration->function_specifier_flags, decl);
    }
  }
}

static struct ir_basicblock *
build_ir_for_labeledstmt(struct ir_func_builder *irb,
                         struct ir_basicblock *basicblock,
                         struct td_labeledstmt *labeled_stmt) {
  struct ir_basicblock *next_bb = ir_alloc_basicblock(irb->func);
  ir_make_basicblock_merge(irb->func, basicblock, next_bb);

  struct ir_stmt *br_stmt = ir_alloc_stmt(irb->func, basicblock);
  struct ir_op *br_op = ir_alloc_op(irb->func, br_stmt);
  br_op->ty = IR_OP_TY_BR;
  br_op->var_ty = IR_VAR_TY_NONE;

  switch (labeled_stmt->ty) {
  case TD_LABELEDSTMT_TY_LABEL: {
    add_label(irb, labeled_stmt->label, next_bb);
    break;
  }
  case TD_LABELEDSTMT_TY_CASE: {
    struct ir_case switch_case = {
        .ty = IR_CASE_TY_CASE,
        .split_case = {.target = next_bb, .value = labeled_stmt->cnst}};
    vector_push_back(irb->switch_cases, &switch_case);
    break;
  }
  case TD_LABELEDSTMT_TY_DEFAULT: {
    struct ir_case switch_case = {.ty = IR_CASE_TY_DEFAULT,
                                  .split_case = {.target = next_bb}};
    vector_push_back(irb->switch_cases, &switch_case);
    break;
  }
  }

  return build_ir_for_stmt(irb, next_bb, labeled_stmt->stmt);
}

static struct ir_basicblock *build_ir_for_stmt(struct ir_func_builder *irb,
                                               struct ir_basicblock *basicblock,
                                               struct td_stmt *stmt) {

  DEBUG_ASSERT(basicblock, "bb cannot be null");

  switch (stmt->ty) {
  case TD_STMT_TY_DECLARATION: {
    struct ir_stmt *ir_stmt = ir_alloc_stmt(irb->func, basicblock);
    build_ir_for_declaration(irb, &ir_stmt, &stmt->declaration);
    return ir_stmt->basicblock;
  }
  case TD_STMT_TY_EXPR: {
    // TODO: ternaries
    struct ir_stmt *ir_stmt = ir_alloc_stmt(irb->func, basicblock);
    build_ir_for_expr(irb, &ir_stmt, &stmt->expr);
    return ir_stmt->basicblock;
  }
  case TD_STMT_TY_DEFER: {
    struct ir_defer defer = {.ty = IR_DEFER_TY_DEFER,
                             .deferstmt = &stmt->deferstmt};

    vector_push_back(irb->defers, &defer);
    return basicblock;
  }
  case TD_STMT_TY_JUMP: {
    return build_ir_for_jumpstmt(irb, basicblock, &stmt->jump);
  }
  case TD_STMT_TY_COMPOUND: {
    return build_ir_for_compoundstmt(irb, basicblock, &stmt->compound);
  }
  case TD_STMT_TY_SELECT: {
    return build_ir_for_selectstmt(irb, basicblock, &stmt->select);
  }
  case TD_STMT_TY_ITER: {
    return build_ir_for_iterstmt(irb, basicblock, &stmt->iter);
  }
  case TD_STMT_TY_LABELED: {
    return build_ir_for_labeledstmt(irb, basicblock, &stmt->labeled);
  }
  case TD_STMT_TY_NULL: {
    return basicblock;
  }
  }
}

struct ir_build_phi_build {
  struct ir_phi_entry *entry;
  struct ir_basicblock *pred;
};

static void gen_var_phis(struct ir_func_builder *irb,
                         struct ir_op **basicblock_ops_for_var,
                         struct vector *preds, struct td_var *var,
                         struct ir_var_ty *var_ty) {
  size_t head = 0;
  while (vector_length(preds) - head) {
    struct ir_build_phi_build *build = vector_get(preds, head++);

    struct ir_basicblock *basicblock = build->pred;

    struct ir_op *op;

    struct var_key key = get_var_key(var, basicblock);
    // FIXME: broken
    // struct var_ref *ref = var_refs_get_for_basicblock(irb->var_refs, &key);
    TODO("phi gen");
    struct var_ref *ref;

    if (ref) {
      // DEBUG_ASSERT(ref->ty == VAR_REF_TY_SSA,
      //              "non-ssa ref ty makes no sense for phi");

      op = ref->op;
    } else {
      op = basicblock_ops_for_var[basicblock->id];
    }

    if (op) {
      *build->entry =
          (struct ir_phi_entry){.basicblock = basicblock, .value = op};

      basicblock_ops_for_var[basicblock->id] = op;
      continue;
    }

    DEBUG_ASSERT(basicblock->pred, "can't insert a phi in first bb");

    // var is not in this bb, so gen phi
    struct ir_op *phi = ir_insert_phi(irb->func, basicblock, *var_ty);

    phi->phi = (struct ir_op_phi){
        .num_values = basicblock->num_preds,
        .values = aralloc(irb->arena,
                          sizeof(*phi->phi.values) * basicblock->num_preds)};
    *build->entry =
        (struct ir_phi_entry){.basicblock = basicblock, .value = phi};

    basicblock_ops_for_var[basicblock->id] = phi;

    ref = var_refs_add(irb->var_refs, &key, VAR_REF_TY_SSA);
    ref->op = phi;

    for (size_t i = 0; i < basicblock->num_preds; i++) {
      struct ir_basicblock *pred = basicblock->preds[i];

      struct ir_build_phi_build pred_build = {.entry = &phi->phi.values[i],
                                              .pred = pred};

      vector_push_back(preds, &pred_build);
    }
  }
}

static void find_phi_exprs(struct ir_func_builder *irb, struct ir_op *phi) {
  DEBUG_ASSERT(phi->ty == IR_OP_TY_PHI, "non-phi in `find_phi_exprs`");

  // walk predecessor basic blocks (splitting into seperate walks each time we
  // have multiple predecessors) until we
  // * A) find a write
  // * B) re-reach current bb
  // * or C) reach end (first bb)

  struct ir_basicblock *basicblock = phi->stmt->basicblock;

  struct ir_op **basicblock_ops_for_var =
      aralloc(irb->arena, sizeof(struct ir_op *) * irb->func->basicblock_count);
  memset(basicblock_ops_for_var, 0,
         sizeof(struct ir_op *) * irb->func->basicblock_count);
  basicblock_ops_for_var[basicblock->id] = phi;

  phi->phi = (struct ir_op_phi){
      .num_values = basicblock->num_preds,
      .values = aralloc(irb->arena,
                        sizeof(*phi->phi.values) * basicblock->num_preds)};

  struct vector *phi_builds = vector_create(sizeof(struct ir_build_phi_build));

  for (size_t i = 0; i < basicblock->num_preds; i++) {
    struct ir_build_phi_build build = {.entry = &phi->phi.values[i],
                                       .pred = basicblock->preds[i]};

    vector_push_back(phi_builds, &build);
  }

  gen_var_phis(irb, basicblock_ops_for_var, phi_builds, phi->metadata,
               &phi->var_ty);

  vector_free(&phi_builds);
}

struct validate_metadata {
  struct ir_func_builder *irb;
  struct ir_op *consumer;
};

static void validate_op_tys_callback(struct ir_op **op,
                                     UNUSED enum ir_op_use_ty use_ty,
                                     void *cb_metadata) {
  struct validate_metadata *metadata = cb_metadata;
  struct ir_op *consumer = metadata->consumer;

  struct ir_var_ty res_ty = (*op)->var_ty;

  // TODO: validate cast types (make sure they are valid)
  switch (consumer->ty) {
  case IR_OP_TY_BINARY_OP:
    res_ty = consumer->var_ty;
    break;
  case IR_OP_TY_CALL:
    res_ty = *consumer->call.func_ty.func.ret_ty;
    break;
  case IR_OP_TY_CAST_OP:
    res_ty = consumer->var_ty;
    break;
  case IR_OP_TY_UNARY_OP:
    if (consumer->unary_op.ty == IR_OP_UNARY_OP_TY_LOGICAL_NOT &&
        ir_var_ty_is_integral(&consumer->var_ty) &&
        ir_var_ty_is_integral(&res_ty)) {
      // logical not on other-sized int is fine
      res_ty = consumer->var_ty;
    }
    break;
  case IR_OP_TY_ADDR:
    res_ty = IR_VAR_TY_POINTER;
    break;
  case IR_OP_TY_VA_ARG:
    res_ty = (*op)->va_arg.arg_ty;
    break;
  case IR_OP_TY_LOAD:
  case IR_OP_TY_LOAD_BITFIELD:
    // loads happen on opaque pointers so we can't check types
    return;
  default:
    break;
  }

  if (ir_op_produces_value(consumer)) {
    if (ir_var_ty_needs_cast_op(metadata->irb, &res_ty, &consumer->var_ty)) {
      BUG("op %zu uses op %zu with different type!", consumer->id, (*op)->id);
    }
  }
}

static struct ir_func *build_ir_for_function(struct ir_unit *unit,
                                             struct typechk *tchk,
                                             struct arena_allocator *arena,
                                             struct td_funcdef *def,
                                             struct var_refs *global_var_refs,
                                             enum ir_build_flags flags) {
  ustr_t ident = def->var_declaration.var.identifier;

  struct var_refs *var_refs = var_refs_create(unit->arena);
  struct ir_func b = {
      .unit = unit,
      .func_ty =
          ir_var_ty_for_td_var_ty(unit, &def->var_declaration.var_ty).func,
      .name = aralloc_strndup(unit->arena, ident.str, ident.len),
      .arena = arena,
      .flags = IR_FUNC_FLAG_NONE,
      .first = NULL,
      .last = NULL,
      .op_count = 0,
      .lcl_count = 0,
      .total_locals_size = 0};

  struct ir_func *f = aralloc(arena, sizeof(b));
  *f = b;

  struct ir_func_builder *builder = aralloc(arena, sizeof(b));
  *builder = (struct ir_func_builder){
      .flags = flags,
      .arena = arena,
      .unit = unit,
      .func = f,
      .tchk = tchk,
      .var_writes =
          hashtbl_create(sizeof(struct td_var), sizeof(struct vector *),
                         hash_td_var, eq_td_var),
      .jumps = vector_create(sizeof(struct ir_jump)),
      .defers = vector_create_in_arena(sizeof(struct ir_defer), arena),
      .switch_cases = vector_create(sizeof(struct ir_case)),
      .var_refs = var_refs,
      .global_var_refs = global_var_refs};

  // needs at least one initial basic block
  ir_alloc_basicblock(builder->func);
  struct ir_basicblock *basicblock = builder->func->first;

  // params live in the first stmt normally reserved for phis (as they have
  // similar function)
  struct ir_stmt *param_stmt = basicblock->first
                                   ? ir_insert_before_stmt(f, basicblock->first)
                                   : ir_alloc_stmt(f, basicblock);
  param_stmt->flags |= IR_STMT_FLAG_PARAM;

  struct ir_stmt *after_params = ir_insert_after_stmt(f, param_stmt);

  // first statement is a bunch of magic MOV commands that explain to the rest
  // of the IR that these are params this is encoded as MOV NULL with the
  // IR_OP_FLAG_PARAM flag
  struct td_ty_func func_ty = def->var_declaration.var_ty.func;

  for (size_t i = 0; i < func_ty.num_params; i++) {
    const struct td_ty_param *param = &func_ty.params[i];

    if (param->var_ty.ty == TD_VAR_TY_TY_VARIADIC) {
      continue;
    }

    // TODO: the whole decl code needs reworking
    struct td_var var = {
        .scope = SCOPE_PARAMS,
        .identifier = param->identifier,
    };

    struct var_key key = get_var_key(&var, basicblock);

    struct ir_var_ty param_var_ty =
        ir_var_ty_for_td_var_ty(builder->unit, &param->var_ty);

    if (param_var_ty.ty == IR_VAR_TY_TY_STRUCT ||
        param_var_ty.ty == IR_VAR_TY_TY_UNION) {
      // add a local, and let codegen magically fill it with the param
      struct ir_lcl *lcl = ir_add_local(builder->func, &param_var_ty);
      lcl->flags |= IR_LCL_FLAG_PARAM;

      struct ir_op *addr = ir_alloc_op(builder->func, param_stmt);
      addr->ty = IR_OP_TY_ADDR;
      addr->var_ty = IR_VAR_TY_POINTER;
      addr->flags |= IR_OP_FLAG_PARAM;
      addr->addr = (struct ir_op_addr){.ty = IR_OP_ADDR_TY_LCL, .lcl = lcl};

      if (var.identifier.len) {
        struct var_ref *ref =
            var_refs_add(builder->var_refs, &key, VAR_REF_TY_LCL);

        ref->lcl = lcl;
      }
    } else {
      if (param_var_ty.ty == IR_VAR_TY_TY_ARRAY) {
        // arrays/aggregates are actually pointers
        param_var_ty = IR_VAR_TY_POINTER;
      }

      struct ir_op *mov = ir_alloc_op(builder->func, param_stmt);
      mov->ty = IR_OP_TY_MOV;
      mov->var_ty = param_var_ty;
      mov->flags |= IR_OP_FLAG_PARAM;
      mov->mov.value = NULL;

      if (var.identifier.len) {
        build_ir_var(builder, &param_stmt, &var, &param_var_ty);
        var_assg(builder, after_params, mov, &var);
      }
    }
  }

  basicblock = build_ir_for_stmt(builder, basicblock, &def->body);

  struct ir_basicblock *last_bb = basicblock;

  // now we have generated the IR we first need to fix up labels
  basicblock = builder->func->first;
  while (basicblock) {
    struct ir_stmt *stmt = basicblock->first;
    while (stmt) {
      struct ir_op *op = stmt->first;
      while (op) {
        if (op->ty == IR_OP_TY_BR && op->metadata) {
          // any BR with metadata is a label
          const char *name = op->metadata;

          struct ir_label *label = builder->labels;
          while (label) {
            if (strcmp(name, label->name) == 0) {
              ir_make_basicblock_merge(builder->func, basicblock,
                                       label->basicblock);
            }

            label = label->succ;
          }
        }

        op = op->succ;
      }

      stmt = stmt->succ;
    }

    basicblock = basicblock->succ;
  }

  // we may generate empty basicblocks or statements, prune them here
  ir_prune_basicblocks(builder->func);

  // may not end in a return, but needs to to be well-formed IR
  if (!last_bb || last_bb->id == DETACHED_BASICBLOCK ||
      (last_bb->last && last_bb->last->last &&
       ir_op_is_branch(last_bb->last->last->ty) &&
       last_bb->last->last->ty != IR_OP_TY_RET)) {
    // add extra bb if there is no last bb, or if there is one
    debug("adding bb to create ret");
    last_bb = ir_alloc_basicblock(builder->func);
  }

  struct ir_stmt *last_stmt = last_bb->last;
  if (!last_stmt) {
    debug("adding bb to create stmt");
    last_stmt = ir_alloc_stmt(builder->func, last_bb);
  }

  struct ir_op *last_op = last_stmt->last;

  if (!last_op || last_op->ty != IR_OP_TY_RET) {
    struct ir_op *return_value = NULL;

    if (strcmp(builder->func->name, "main") == 0) {
      debug("adding implicit return 0 to bb %zu", last_bb->id);

      struct ir_op *cnst = ir_alloc_op(builder->func, last_stmt);
      cnst->ty = IR_OP_TY_CNST;
      cnst->var_ty = (struct ir_var_ty){
          .ty = IR_VAR_TY_TY_PRIMITIVE,
          .primitive = IR_VAR_PRIMITIVE_TY_I32,
      };
      cnst->cnst = (struct ir_op_cnst){.ty = IR_OP_CNST_TY_INT, .int_value = 0};

      return_value = cnst;
    }

    basicblock = build_ir_for_ret(builder, &last_stmt, NULL);
    DEBUG_ASSERT(last_stmt->last->ty == IR_OP_TY_RET,
                 "expected ret after call to build ret");
    last_stmt->last->ret.value = return_value;
  }

  // prune again, as inserting the ret can introduce an extraneous empty bb
  ir_prune_basicblocks(builder->func);

  // now we fix up phis
  basicblock = builder->func->first;
  while (basicblock) {
    struct ir_stmt *stmt = basicblock->first;
    while (stmt) {
      struct ir_op *op = stmt->first;
      while (op) {
        if (op->ty == IR_OP_TY_PHI && op->metadata) {
          find_phi_exprs(builder, op);
        }

        op = op->succ;
      }

      stmt = stmt->succ;
    }

    basicblock = basicblock->succ;
  }

  ir_simplify_phis(builder->func);

  basicblock = builder->func->first;
  while (basicblock) {
    struct ir_stmt *stmt = basicblock->first;
    while (stmt) {
      struct ir_op *op = stmt->first;
      while (op) {
        struct validate_metadata metadata = {.irb = builder, .consumer = op};

        ir_walk_op_uses(op, validate_op_tys_callback, &metadata);

        op = op->succ;
      }

      stmt = stmt->succ;
    }

    basicblock = basicblock->succ;
  }

  vector_free(&builder->jumps);
  vector_free(&builder->switch_cases);
  hashtbl_free(&builder->var_writes);
  var_refs_free(&builder->var_refs);

  return builder->func;
}

// static struct ir_var_value build_ir_for_zero_var(struct ir_unit *iru,
//                                                  struct td_var_ty *var_ty)
//                                                  {
//   switch (var_ty->ty) {
//   case TD_VAR_TY_TY_UNKNOWN:
//   case TD_VAR_TY_TY_VOID:
//   case TD_VAR_TY_TY_VARIADIC:
//   case TD_VAR_TY_TY_FUNC:
//     bug("no sense");
//   case TD_VAR_TY_TY_WELL_KNOWN:
//   case TD_VAR_TY_TY_POINTER:
//   case TD_VAR_TY_TY_ARRAY:
//   case TD_VAR_TY_TY_INCOMPLETE_AGGREGATE:
//   case TD_VAR_TY_TY_AGGREGATE:
//     return (struct ir_var_value){.var_ty = var_ty_for_td_var_ty(iru,
//     var_ty)};
//   }
// }

static size_t get_member_index_offset(struct ir_unit *iru, struct typechk *tchk,
                                      const struct td_var_ty *var_ty,
                                      size_t member_index,
                                      struct td_var_ty *member_ty,
                                      bool *is_bitfield,
                                      struct ir_bitfield *bitfield) {
  *is_bitfield = false;

  if (var_ty->ty == TD_VAR_TY_TY_ARRAY) {
    *member_ty = td_var_ty_get_underlying(tchk, var_ty);
    struct ir_var_ty el_ty = ir_var_ty_for_td_var_ty(iru, member_ty);
    struct ir_var_ty_info info = ir_var_ty_info(iru, &el_ty);

    return info.size * member_index;
  } else {
    DEBUG_ASSERT(var_ty->ty == TD_VAR_TY_TY_AGGREGATE ||
                     var_ty->ty == TD_VAR_TY_TY_INCOMPLETE_AGGREGATE,
                 "bad type");

    struct td_struct_field *struct_field =
        &var_ty->aggregate.fields[member_index];
    ustr_t member_name = struct_field->identifier;

    if (!member_name.str) {
      // anonymous field
      // get info for first field of sub ty

      struct ir_var_ty ir_aggregate = ir_var_ty_for_td_var_ty(iru, var_ty);
      struct ir_var_ty_info info = ir_var_ty_info(iru, &ir_aggregate);

      *member_ty = struct_field->var_ty;

      // offsets are null for a union
      return info.offsets ? info.offsets[member_index] : 0;
    }

    struct ir_var_ty ir_member_ty;
    size_t member_offset;
    size_t idx;
    get_member_info(iru, var_ty, member_name, &ir_member_ty, &idx,
                    &member_offset, is_bitfield, bitfield, member_ty);

    return member_offset;
  }
}

static size_t get_designator_offset(struct ir_unit *iru,
                                    const struct td_var_ty *var_ty,
                                    struct td_designator_list *designator_list,
                                    size_t *member_index, bool *is_bitfield,
                                    struct ir_bitfield *bitfield,
                                    struct td_var_ty *member_ty) {
  DEBUG_ASSERT(designator_list->num_designators,
               "not defined for 0 designators");

  size_t offset = 0;

  struct td_var_ty cur_var_ty = *var_ty;
  for (size_t i = 0; i < designator_list->num_designators; i++) {
    struct td_designator *designator = &designator_list->designators[i];

    switch (designator->ty) {
    case TD_DESIGNATOR_TY_FIELD: {
      ustr_t member_name = designator->field;
      struct ir_var_ty ir_member_ty;
      size_t member_offset;
      get_member_info(iru, &cur_var_ty, member_name, &ir_member_ty,
                      member_index, &member_offset, is_bitfield, bitfield,
                      member_ty);

      offset += member_offset;
      break;
    }
    case TD_DESIGNATOR_TY_INDEX: {
      *member_ty = designator->var_ty;
      struct ir_var_ty el_var_ty = ir_var_ty_for_td_var_ty(iru, member_ty);
      struct ir_var_ty_info info = ir_var_ty_info(iru, &el_var_ty);

      offset += info.size * designator->index;
      *member_index = designator->index;
      break;
    }
    }

    cur_var_ty = designator->var_ty;
  }

  return offset;
}

enum init_list_layout_ty {
  INIT_LIST_LAYOUT_TY_STRUCT,
  INIT_LIST_LAYOUT_TY_UNION,
  INIT_LIST_LAYOUT_TY_ARRAY,
};

static void build_init_list_layout_entry(struct ir_unit *iru,
                                         struct typechk *tchk,
                                         const struct td_init_list *init_list,
                                         const struct td_var_ty *var_ty,
                                         size_t offset, struct vector *inits) {

  enum init_list_layout_ty ty;
  struct td_var_ty el_ty;
  size_t el_size;
  switch (var_ty->ty) {
  case TD_VAR_TY_TY_AGGREGATE:
    ty = var_ty->aggregate.ty == TD_TY_AGGREGATE_TY_STRUCT
             ? INIT_LIST_LAYOUT_TY_STRUCT
             : INIT_LIST_LAYOUT_TY_UNION;
    break;
  case TD_VAR_TY_TY_ARRAY:
    ty = INIT_LIST_LAYOUT_TY_ARRAY;
    el_ty = *var_ty->array.underlying;
    struct ir_var_ty ir_el_ty = ir_var_ty_for_td_var_ty(iru, &el_ty);
    el_size = ir_var_ty_info(iru, &ir_el_ty).size;
    break;
  default:
    BUG("bad type for init list (%s)", tchk_type_name(tchk, var_ty));
  }

  size_t num_elements = init_list->num_inits;

  size_t member_idx = 0;
  for (size_t i = 0; i < num_elements; i++) {
    struct td_init_list_init *init = &init_list->inits[i];

    bool is_bitfield = false;
    struct ir_bitfield bitfield;

    size_t init_offset = offset;
    struct td_var_ty member_ty;
    if (init->designator_list && init->designator_list->num_designators) {
      init_offset += get_designator_offset(
          iru, &init_list->var_ty, init_list->inits[i].designator_list,
          &member_idx, &is_bitfield, &bitfield, &member_ty);
    } else {
      switch (ty) {
      case INIT_LIST_LAYOUT_TY_STRUCT:
      case INIT_LIST_LAYOUT_TY_UNION:
        init_offset += get_member_index_offset(
            iru, tchk, var_ty, member_idx, &member_ty, &is_bitfield, &bitfield);
        break;
      case INIT_LIST_LAYOUT_TY_ARRAY:
        member_ty = el_ty;
        init_offset += member_idx * el_size;
      }
    }

    member_idx++;

    switch (init->init->ty) {
    case TD_INIT_TY_EXPR: {
      if (init->init->expr.ty == TD_EXPR_TY_COMPOUND_LITERAL &&
          !td_var_ty_is_scalar_ty(&member_ty)) {
        // again broken if cast needed
        build_init_list_layout_entry(
            iru, tchk, &init->init->expr.compound_literal.init_list, &member_ty,
            init_offset, inits);
      } else {
        struct ir_build_init build_init = {
            .is_bitfield = is_bitfield,
            .bitfield = bitfield,
            .offset = init_offset,
            .expr = &init->init->expr,
        };

        vector_push_back(inits, &build_init);
      }
      break;
    }
    case TD_INIT_TY_INIT_LIST:
      build_init_list_layout_entry(iru, tchk, &init->init->init_list,
                                   &member_ty, init_offset, inits);
      break;
    }
  }
}

static struct ir_build_init_list_layout
build_init_list_layout(struct ir_unit *iru, struct typechk *tchk,
                       const struct td_init_list *init_list) {
  struct vector *inits = vector_create(sizeof(struct ir_build_init));

  build_init_list_layout_entry(iru, tchk, init_list, &init_list->var_ty, 0,
                               inits);

  struct ir_build_init_list_layout layout = {
      .num_inits = vector_length(inits),
      .inits = aralloc(iru->arena, vector_byte_size(inits))};

  vector_copy_to(inits, layout.inits);
  vector_free(&inits);

  return layout;
}

static struct ir_var_value
build_ir_for_var_value_expr(struct ir_var_builder *irb,
                            const struct td_expr *expr,
                            const struct td_var_ty *var_ty);

static struct ir_var_value
build_ir_for_var_value_init_list(struct ir_var_builder *irb,
                                 const struct td_init_list *init_list,
                                 const struct td_var_ty *var_ty);

static struct ir_var_value
build_ir_for_var_value_unary_op(struct ir_var_builder *irb,
                                const struct td_expr *expr,
                                const struct td_var_ty *var_ty);

static struct ir_var_value build_ir_for_var_value_addr(
    struct ir_var_builder *irb, const struct td_expr *addr,
    const struct td_expr *offset, const struct td_var_ty *var_ty);

static struct ir_var_value
build_ir_for_compound_literal_addr(struct ir_var_builder *irb,
                                   const struct td_expr *addr,
                                   const struct td_expr *offset) {
  struct ir_var_ty glb_var_ty =
      ir_var_ty_for_td_var_ty(irb->unit, &addr->compound_literal.var_ty);
  struct ir_glb *glb = ir_add_global(irb->unit, IR_GLB_TY_DATA, &glb_var_ty,
                                     IR_GLB_DEF_TY_DEFINED, NULL);

  struct ir_var_value var_value = build_ir_for_var_value_addr(
      irb, addr, offset, &addr->compound_literal.var_ty);

  glb->var = aralloc(irb->arena, sizeof(*glb->var));
  *glb->var = (struct ir_var){.unit = irb->unit,
                              .ty = IR_VAR_TY_DATA,
                              .var_ty = glb->var_ty,
                              .value = var_value};

  return (struct ir_var_value){.ty = IR_VAR_VALUE_TY_ADDR,
                               .var_ty = IR_VAR_TY_POINTER,
                               .addr = {.glb = glb, .offset = 0}};
}

static struct ir_var_value build_ir_for_var_value_addr(
    struct ir_var_builder *irb, const struct td_expr *addr,
    const struct td_expr *offset, const struct td_var_ty *var_ty) {
  switch (addr->ty) {
  case TD_EXPR_TY_UNARY_OP: {
    switch (addr->unary_op.ty) {
    case TD_UNARY_OP_TY_ADDRESSOF:
      // allow `&(((Foo *)0)->bar)`, common for offsetof
      if (addr->unary_op.expr->ty == TD_EXPR_TY_POINTERACCESS) {
        struct td_pointeraccess *access = &addr->unary_op.expr->pointer_access;
        if (access->lhs->ty == TD_EXPR_TY_CNST &&
            access->lhs->cnst.ty == TD_CNST_TY_NUM &&
            ap_val_iszero(access->lhs->cnst.num_value)) {

          struct ir_var_ty member_ty;
          bool is_bitfield;
          struct ir_bitfield bitfield;
          size_t offset_of = get_member_address_offset(
              irb->unit, access->lhs->var_ty.pointer.underlying, access->member,
              &member_ty, &is_bitfield, &bitfield, NULL);

          DEBUG_ASSERT(!is_bitfield, "addr of bitfield");

          return (struct ir_var_value){
              .ty = IR_VAR_VALUE_TY_INT,
              .var_ty = ir_var_ty_for_td_var_ty(irb->unit, var_ty),
              .int_value = offset_of};
        }
      } else if (addr->unary_op.expr->ty == TD_EXPR_TY_COMPOUND_LITERAL) {
        return build_ir_for_compound_literal_addr(irb, addr->unary_op.expr,
                                                  offset);
      }

      return build_ir_for_var_value_addr(irb, addr->unary_op.expr, offset,
                                         var_ty);
    case TD_UNARY_OP_TY_CAST:
      return build_ir_for_var_value_unary_op(irb, addr, var_ty);
    default:
      BUG("non var addr of global (ty %u line %zu)", addr->ty,
          addr->span.start.line);
    }
  }

  case TD_EXPR_TY_CNST: {
    // occurs from e.g `static int *p = &(int){10}`;
    struct ir_var_ty ir_var_ty = ir_var_ty_for_td_var_ty(irb->unit, var_ty);
    struct ir_glb *glb = ir_add_global(irb->unit, IR_GLB_TY_DATA, &ir_var_ty,
                                       IR_GLB_DEF_TY_DEFINED, NULL);

    // FIXME: the whole global code can be neatened because typechk does more
    // stuff now e.g this _should_ always be a TD_EXPR_TY_CNST
    size_t offset_cnst = 0;
    if (offset) {
      struct ir_var_value offset_value =
          build_ir_for_var_value_expr(irb, offset, var_ty);

      if (offset_value.ty != IR_VAR_VALUE_TY_INT) {
        TODO("non-int global values offset");
      }

      struct td_var_ty underlying_td_var_ty =
          td_var_ty_get_underlying(irb->tchk, var_ty);
      struct ir_var_ty underlying_var_ty =
          ir_var_ty_for_td_var_ty(irb->unit, &underlying_td_var_ty);
      struct ir_var_ty_info info =
          ir_var_ty_info(irb->unit, &underlying_var_ty);

      offset_cnst = offset_value.int_value * info.size;
    }

    glb->var = aralloc(irb->arena, sizeof(*glb->var));
    *glb->var = (struct ir_var){
        .unit = irb->unit,
        .ty = IR_VAR_TY_DATA,
        .var_ty = ir_var_ty,
        .value = build_ir_for_var_value_expr(irb, addr, var_ty)};

    return (struct ir_var_value){.ty = IR_VAR_VALUE_TY_ADDR,
                                 .var_ty =
                                     ir_var_ty_for_td_var_ty(irb->unit, var_ty),
                                 .addr = {.glb = glb, .offset = offset_cnst}};
  }

  case TD_EXPR_TY_POINTERACCESS: {
    struct ir_var_ty pointer_ty;
    bool pointer_is_bitfield;
    struct ir_bitfield pointer_bitfield;

    struct td_var_ty underlying =
        td_var_ty_get_underlying(irb->tchk, &addr->pointer_access.lhs->var_ty);

    size_t field_offset = get_member_address_offset(
        irb->unit, &underlying, addr->pointer_access.member, &pointer_ty,
        &pointer_is_bitfield, &pointer_bitfield, NULL);

    struct ir_var_value base_addr = build_ir_for_var_value_addr(
        irb, addr->pointer_access.lhs, NULL, var_ty);

    base_addr.addr.offset += field_offset;
    return base_addr;
  }

  case TD_EXPR_TY_MEMBERACCESS: {
    struct ir_var_ty member_ty;
    bool member_is_bitfield;
    struct ir_bitfield member_bitfield;

    size_t field_offset = get_member_address_offset(
        irb->unit, &addr->member_access.lhs->var_ty, addr->member_access.member,
        &member_ty, &member_is_bitfield, &member_bitfield, NULL);

    struct ir_var_value base_addr =
        build_ir_for_var_value_addr(irb, addr->member_access.lhs, NULL, var_ty);

    base_addr.addr.offset += field_offset;
    return base_addr;
  }

  case TD_EXPR_TY_BINARY_OP: {
    struct td_var_ty underlying =
        td_var_ty_get_underlying(irb->tchk, &addr->binary_op.lhs->var_ty);
    struct ir_var_ty el_ty = ir_var_ty_for_td_var_ty(irb->unit, &underlying);

    struct ir_var_ty_info info = ir_var_ty_info(irb->unit, &el_ty);

    struct ir_var_value base_addr =
        build_ir_for_var_value_addr(irb, addr->binary_op.lhs, NULL, var_ty);

    DEBUG_ASSERT(addr->binary_op.rhs->ty == TD_EXPR_TY_CNST,
                 "expected cnst rhs");
    struct td_cnst cnst = addr->binary_op.rhs->cnst;
    DEBUG_ASSERT(cnst.ty == TD_CNST_TY_NUM &&
                     cnst.num_value.ty == AP_VAL_TY_INT,
                 "expected integer ty");

    base_addr.addr.offset += info.size * ap_int_as_ull(cnst.num_value.ap_int);
    return base_addr;
  }

  case TD_EXPR_TY_ARRAYACCESS: {
    struct td_var_ty underlying =
        td_var_ty_get_underlying(irb->tchk, &addr->array_access.lhs->var_ty);
    struct ir_var_ty el_ty = ir_var_ty_for_td_var_ty(irb->unit, &underlying);

    struct ir_var_ty_info info = ir_var_ty_info(irb->unit, &el_ty);

    struct ir_var_value base_addr =
        build_ir_for_var_value_addr(irb, addr->array_access.lhs, NULL, var_ty);

    DEBUG_ASSERT(addr->array_access.rhs->ty == TD_EXPR_TY_CNST,
                 "expected cnst rhs (got %u)", addr->array_access.rhs->ty);
    struct td_cnst cnst = addr->array_access.rhs->cnst;
    DEBUG_ASSERT(cnst.ty == TD_CNST_TY_NUM &&
                     cnst.num_value.ty == AP_VAL_TY_INT,
                 "expected integer ty");

    base_addr.addr.offset += info.size * ap_int_as_ull(cnst.num_value.ap_int);
    return base_addr;
  }

  case TD_EXPR_TY_COMPOUND_LITERAL: {
    if (var_ty->ty == TD_VAR_TY_TY_POINTER &&
        addr->compound_literal.var_ty.ty == TD_VAR_TY_TY_ARRAY) {
      // decay, take address
      return build_ir_for_compound_literal_addr(irb, addr, offset);
    }

    return build_ir_for_var_value_init_list(
        irb, &addr->compound_literal.init_list, &addr->compound_literal.var_ty);
  }

  case TD_EXPR_TY_VAR: {
    const struct td_var *var = &addr->var;

    struct var_key key = get_var_key(var, NULL);
    struct var_ref *ref = var_refs_get(irb->global_var_refs, &key);

    DEBUG_ASSERT(ref, "var '%.*s' (scope=%d) did not exist",
                 USTR_SPEC(key.name), key.scope);
    DEBUG_ASSERT(ref->ty == VAR_REF_TY_GLB, "wasn't global");

    size_t offset_cnst = 0;
    if (offset) {
      struct ir_var_value offset_value =
          build_ir_for_var_value_expr(irb, offset, var_ty);

      if (offset_value.ty != IR_VAR_VALUE_TY_INT) {
        TODO("non-int global values offset");
      }

      struct td_var_ty underlying_td_var_ty =
          td_var_ty_get_underlying(irb->tchk, var_ty);
      struct ir_var_ty underlying_var_ty =
          ir_var_ty_for_td_var_ty(irb->unit, &underlying_td_var_ty);
      struct ir_var_ty_info info =
          ir_var_ty_info(irb->unit, &underlying_var_ty);

      offset_cnst = offset_value.int_value * info.size;
    }

    return (struct ir_var_value){
        .ty = IR_VAR_VALUE_TY_ADDR,
        .var_ty = ir_var_ty_for_td_var_ty(irb->unit, var_ty),
        .addr = {.glb = ref->glb, .offset = offset_cnst}};
  }
  default:
    BUG("non var addr of global (ty %u)", addr->ty);
  }
}

static struct ir_var_value
build_ir_for_var_value_binary_op(struct ir_var_builder *irb,
                                 const struct td_expr *expr,
                                 const struct td_var_ty *var_ty) {
  const struct td_expr *lhs = expr->binary_op.lhs;
  const struct td_expr *rhs = expr->binary_op.rhs;
  enum td_binary_op_ty ty = expr->binary_op.ty;

  // FIXME: whole thing needs reworking, won't work for non-ptr adds
  switch (ty) {
  case TD_BINARY_OP_TY_ADD:
    return build_ir_for_var_value_addr(irb, lhs, rhs, var_ty);

  default:
    TODO("other binary op tys in global vars");
  }
}

static struct ir_var_value
build_ir_for_var_value_unary_op(struct ir_var_builder *irb,
                                const struct td_expr *expr,
                                const struct td_var_ty *var_ty) {
  switch (expr->unary_op.ty) {
  case TD_UNARY_OP_TY_CAST: {
    const struct td_var_ty *from = &expr->unary_op.expr->var_ty;
    const struct td_var_ty *to = &expr->unary_op.cast.var_ty;

    DEBUG_ASSERT(td_var_ty_eq(irb->tchk, var_ty, to),
                 "expr ty didn't equal cast ty");

    struct ir_var_value value = build_ir_for_var_value_expr(
        irb, expr->unary_op.expr, &expr->unary_op.cast.var_ty);

    if (from->ty == TD_VAR_TY_TY_POINTER || from->ty == TD_VAR_TY_TY_FUNC ||
        from->ty == TD_VAR_TY_TY_ARRAY) {
      if (to->ty == TD_VAR_TY_TY_POINTER || to->ty == TD_VAR_TY_TY_FUNC ||
          to->ty == TD_VAR_TY_TY_ARRAY) {
        // nop
        value.var_ty = ir_var_ty_for_td_var_ty(irb->unit, var_ty);
        return value;
      } else if (to->ty == TD_VAR_TY_TY_WELL_KNOWN) {
        DEBUG_ASSERT(td_var_ty_is_integral_ty(to),
                     "non integral cast from ptr-like");

        // struct ir_var_ty integral = ir_var_ty_for_td_var_ty(irb->unit, to);
        // struct ir_var_ty_info integral_info =
        //     ir_var_ty_info(irb->unit, &integral);
        // struct ir_var_ty_info ptr_info =
        //     ir_var_ty_info(irb->unit, &IR_VAR_TY_POINTER);
        // if (integral_info.size != ptr_info.size) {
        //   TODO("should have been rejected by typechk, pointer (sz=%zu) -> non
        //   "
        //        "pointer sized int (sz=%zu) in constant expr (line %zu)",
        //        ptr_info.size, integral_info.size, expr->span.start.line);
        // }

        value.var_ty = ir_var_ty_for_td_var_ty(irb->unit, var_ty);
        return value;
      }

      TODO("unsupported cast in const expr");
    } else if (to->ty == TD_VAR_TY_TY_POINTER || to->ty == TD_VAR_TY_TY_FUNC ||
               to->ty == TD_VAR_TY_TY_ARRAY) {
      if (from->ty == TD_VAR_TY_TY_WELL_KNOWN) {
        value.var_ty = ir_var_ty_for_td_var_ty(irb->unit, var_ty);
        return value;
      }

      TODO("unsupported cast in const expr");
    } else if (from->ty == TD_VAR_TY_TY_WELL_KNOWN &&
               to->ty == TD_VAR_TY_TY_WELL_KNOWN) {
      enum well_known_ty fwk = from->well_known;
      enum well_known_ty twk = to->well_known;

      if (WKT_IS_INTEGRAL(fwk) && WKT_IS_FP(twk)) {
        long double flt_value;

#define VALUE                                                                  \
  (WKT_IS_SIGNED(fwk) ? (signed long long)value.int_value                      \
                      : (unsigned long long)value.int_value)
        switch (twk) {
        case WELL_KNOWN_TY_HALF:
          TODO("constant cast to half");
        case WELL_KNOWN_TY_FLOAT:
          flt_value = (long double)(float)VALUE;
          break;
        case WELL_KNOWN_TY_DOUBLE:
          flt_value = (long double)(double)VALUE;
          break;
        case WELL_KNOWN_TY_LONG_DOUBLE:
          flt_value = (long double)VALUE;
          break;
        default:
          unreachable();
        }

        return (struct ir_var_value){
            .ty = IR_VAR_VALUE_TY_FLT,
            .var_ty = ir_var_ty_for_td_var_ty(irb->unit, var_ty),
            .flt_value = flt_value};
      } else if (WKT_IS_INTEGRAL(fwk) && WKT_IS_INTEGRAL(twk) &&
                 value.ty == IR_VAR_VALUE_TY_INT) {
        return (struct ir_var_value){
            .ty = IR_VAR_VALUE_TY_INT,
            .var_ty = ir_var_ty_for_td_var_ty(irb->unit, var_ty),
            .int_value = value.int_value};
      }
    }

    TODO("unsupported cast in const expr");
  }
  case TD_UNARY_OP_TY_ADDRESSOF: {
    return build_ir_for_var_value_addr(irb, expr, NULL, var_ty);
  }
  default:
    TODO("other unary ops in globals");
  }
}

static struct ir_var_value
build_ir_for_var_value_var(struct ir_var_builder *irb,
                           const struct td_expr *expr,
                           const struct td_var_ty *var_ty) {
  // a var can only ever be used as an init when it is an address or constant
  // (enum)

  switch (expr->var.ty) {
  case TD_VAR_VAR_TY_ENUMERATOR:
    // FIXME: i think this is wrong for `int *p = ENUM_VALUE`
    return (struct ir_var_value){.ty = IR_VAR_VALUE_TY_INT,
                                 .var_ty =
                                     ir_var_ty_for_td_var_ty(irb->unit, var_ty),
                                 .int_value = expr->var.enumerator};
  case TD_VAR_VAR_TY_VAR:
    return build_ir_for_var_value_addr(irb, expr, NULL, var_ty);
  }
}

static struct ir_var_value
build_ir_for_var_value_expr(struct ir_var_builder *irb,
                            const struct td_expr *expr,
                            const struct td_var_ty *var_ty) {
  switch (expr->ty) {
  // TODO: some of this is no longer needed as typechk can resolve
  // sizeof/alignof/etc
  case TD_EXPR_TY_COMPOUND_LITERAL:
    if (expr->compound_literal.var_ty.ty == TD_VAR_TY_TY_ARRAY) {
      return build_ir_for_var_value_addr(irb, expr, 0, var_ty);
    }
    BUG("non array compound literal?");
  case TD_EXPR_TY_VAR:
    return build_ir_for_var_value_var(irb, expr, var_ty);
  case TD_EXPR_TY_UNARY_OP:
    return build_ir_for_var_value_unary_op(irb, expr, var_ty);
  case TD_EXPR_TY_BINARY_OP:
    return build_ir_for_var_value_binary_op(irb, expr, var_ty);
  case TD_EXPR_TY_CNST: {
    const struct td_cnst *cnst = &expr->cnst;
    switch (cnst->ty) {
    case TD_CNST_TY_STRING:
      if (var_ty->ty == TD_VAR_TY_TY_ARRAY) {
        struct ir_var_ty ir_var_ty = ir_var_ty_for_td_var_ty(irb->unit, var_ty);

        struct ir_var_ty char_ty;
        bool is_data;
        struct ir_var_value str = {.ty = IR_VAR_VALUE_TY_STR,
                                   .var_ty = ir_var_ty,
                                   .str_value =
                                       build_ir_str(cnst, &char_ty, &is_data)};

        return str;
      } else {
        switch (cnst->str_value.ty) {
        case TD_CNST_STR_TY_ASCII: {
          struct ir_glb *glb =
              build_str_literal(irb->unit, &expr->var_ty, cnst);

          if (var_ty->ty == TD_VAR_TY_TY_POINTER) {
            return (struct ir_var_value){
                .ty = IR_VAR_VALUE_TY_ADDR,
                .var_ty = ir_var_ty_for_td_var_ty(irb->unit, var_ty),
                .addr = {.glb = glb, .offset = 0}};
          } else {
            // FIXME: this leads to duplicates in the IR (as a glb was
            // constructed in build_str_literal)
            return glb->var->value;
          }
        }
        case TD_CNST_STR_TY_WIDE: {
          TODO("wide str globals");
          break;
        }
        }
      }
      break;
    case TD_CNST_TY_NUM:
      switch (cnst->num_value.ty) {
      case AP_VAL_TY_INT:
        return (struct ir_var_value){
            .ty = IR_VAR_VALUE_TY_INT,
            .var_ty = ir_var_ty_for_td_var_ty(irb->unit, var_ty),
            .int_value = ap_int_as_ull(cnst->num_value.ap_int)};
      case AP_VAL_TY_FLOAT:
        return (struct ir_var_value){
            .ty = IR_VAR_VALUE_TY_FLT,
            .var_ty = ir_var_ty_for_td_var_ty(irb->unit, var_ty),
            .flt_value = ap_float_as_ld(cnst->num_value.ap_float)};
      case AP_VAL_TY_INVALID:
        BUG("invalid ap val should not reach ir gen");
      }
    }
  }
  default:
    TODO("other expr tys");
  }
}

static struct ir_var_value
build_ir_for_var_value_init_list(struct ir_var_builder *irb,
                                 const struct td_init_list *init_list,
                                 const struct td_var_ty *var_ty) {

  struct ir_build_init_list_layout layout =
      build_init_list_layout(irb->unit, irb->tchk, init_list);

  struct ir_var_value_list value_list = {
      .num_values = layout.num_inits,
      .values =
          aralloc(irb->arena, sizeof(*value_list.values) * layout.num_inits),
      .offsets =
          aralloc(irb->arena, sizeof(*value_list.offsets) * layout.num_inits),
  };

  for (size_t i = 0; i < layout.num_inits; i++) {
    struct ir_build_init *build_init = &layout.inits[i];

    if (build_init->is_bitfield) {
      TODO("bitfield init for globals");
    }

    value_list.values[i] = build_ir_for_var_value_expr(
        irb, build_init->expr, &build_init->expr->var_ty);
    value_list.offsets[i] = build_init->offset;
  }

  return (struct ir_var_value){.ty = IR_VAR_VALUE_TY_VALUE_LIST,
                               .var_ty =
                                   ir_var_ty_for_td_var_ty(irb->unit, var_ty),
                               .value_list = value_list};
}

static struct ir_var_value
build_ir_for_var_value(struct ir_var_builder *irb, struct td_init *init,
                       const struct td_var_ty *var_ty) {
  switch (init->ty) {
  case TD_INIT_TY_EXPR:
    return build_ir_for_var_value_expr(irb, &init->expr, &init->expr.var_ty);
  case TD_INIT_TY_INIT_LIST:
    return build_ir_for_var_value_init_list(irb, &init->init_list, var_ty);
  }
}

// static void spill_phis_to_locals(struct ir_func_builder *irb) {
//   struct hashtbl_iter *iter = hashtbl_iter(irb->var_writes);
//   struct hashtbl_entry entry;

//   while (hashtbl_iter_next(iter, &entry)) {
//     const struct td_var *var = entry.key;
//     struct vector *writes = *(struct vector **)entry.data;

//     struct var_key key = {
//         .name = var->identifier, .scope = var->scope, .basicblock = NULL};

//     struct var_ref *ref = var_refs_get(irb->var_refs, &key);

//     struct ir_lcl *lcl;
//     switch (ref->ty) {
//     case VAR_REF_TY_SSA:
//       lcl = ir_add_local(irb->func, &ref->op->var_ty);
//       break;
//     case VAR_REF_TY_LCL:
//       lcl = ref->lcl;
//       break;
//     case VAR_REF_TY_GLB:
//       unreachable();
//     }

//     size_t num_writes = vector_length(writes);

//   }
// }

struct ir_unit *
build_ir_for_translationunit(const struct target *target, struct typechk *tchk,
                             struct arena_allocator *arena,
                             struct td_translationunit *translation_unit,
                             enum ir_build_flags flags) {

  struct ir_unit *iru = aralloc(arena, sizeof(*iru));
  *iru = (struct ir_unit){.arena = arena,
                          .target = target,
                          .first_global = NULL,
                          .last_global = NULL,
                          .glb_count = 0};

  struct var_refs *global_var_refs = var_refs_create(iru->arena);
  // funcs do not necessarily have a seperate decl so we do it for defs too

  for (size_t i = 0; i < translation_unit->num_external_declarations; i++) {
    struct td_external_declaration *external_declaration =
        &translation_unit->external_declarations[i];

    switch (external_declaration->ty) {
    case TD_EXTERNAL_DECLARATION_TY_DECLARATION: {
      struct ir_var_builder builder = {
          .arena = iru->arena,
          .tchk = tchk,
          .unit = iru,
          .global_var_refs = global_var_refs,
      };

      build_ir_for_global_declaration(&builder, NULL, global_var_refs,
                                      &external_declaration->declaration);
      break;
    }
    case TD_EXTERNAL_DECLARATION_TY_FUNC_DEF: {
      struct ir_var_builder var_builder = {
          .arena = iru->arena,
          .tchk = tchk,
          .unit = iru,
          .global_var_refs = global_var_refs,
      };

      struct td_funcdef *def = &external_declaration->func_def;

      build_ir_for_global_var(
          &var_builder, NULL, global_var_refs, def->storage_class_specifier,
          def->function_specifier_flags, &def->var_declaration);

      struct ir_func *func =
          build_ir_for_function(iru, tchk, arena, def, global_var_refs, flags);

      struct var_key key = {.name = def->var_declaration.var.identifier,
                            .scope = SCOPE_GLOBAL};

      struct var_ref *ref = var_refs_get(global_var_refs, &key);
      ref->glb->def_ty = IR_GLB_DEF_TY_DEFINED;
      ref->glb->func = func;

      break;
    }
    }
  }

  // finally, we need to convert tentative definitions to real ones
  struct ir_glb *glb = iru->first_global;
  while (glb) {
    if (glb->def_ty == IR_GLB_DEF_TY_TENTATIVE) {
      DEBUG_ASSERT(glb->ty == IR_GLB_TY_DATA, "tentative func makes no sense");

      glb->def_ty = IR_GLB_DEF_TY_DEFINED;
      glb->var = aralloc(iru->arena, sizeof(*glb->var));
      *glb->var = (struct ir_var){.unit = iru,
                                  .ty = IR_VAR_TY_DATA,
                                  .var_ty = glb->var_ty,
                                  .value = {.ty = IR_VAR_VALUE_TY_ZERO}};
    }

    glb = glb->succ;
  }

  var_refs_free(&global_var_refs);

  return iru;
}
