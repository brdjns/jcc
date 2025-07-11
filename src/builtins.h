#ifndef BUILTINS_H
#define BUILTINS_H

#include "typechk.h"

#include <stddef.h>

// `BUILTIN_FN` the macro for each builtin function
// `BUILTIN_TY` the macro for each builtin type

enum builtin_type_spec_ty {
  BUILTIN_TYPE_SPEC_TY_ANY,
  BUILTIN_TYPE_SPEC_TY_SIZE_T,
  BUILTIN_TYPE_SPEC_TY_BUILTIN,
  BUILTIN_TYPE_SPEC_TY_VA_LIST,
  BUILTIN_TYPE_SPEC_TY_TD_VAR_TY,
};

struct builtin_type_spec {
  enum builtin_type_spec_ty ty;

  union {
    const struct td_var_ty *td_var_ty;
  };
};

struct builtin_fn_spec {
  struct builtin_type_spec ret;
  struct builtin_type_spec params[16];
  size_t num_params;
};

#define BUILTIN_TYPE_SPEC_TD_VAR_TY(td)                                        \
  ((struct builtin_type_spec){.ty = BUILTIN_TYPE_SPEC_TY_TD_VAR_TY,            \
                              .td_var_ty = (td)})

#define BUILTIN_TYPE_SPEC_VA_LIST                                              \
  ((struct builtin_type_spec){.ty = BUILTIN_TYPE_SPEC_TY_VA_LIST})

#define BUILTIN_TYPE_SPEC_ANY                                                  \
  ((struct builtin_type_spec){.ty = BUILTIN_TYPE_SPEC_TY_ANY})

#define BUILTIN_TYPE_SPEC_SIZE_T                                               \
  ((struct builtin_type_spec){.ty = BUILTIN_TYPE_SPEC_TY_SIZE_T})

#define ARR_LIT(...) __VA_ARGS__
#define NO_ARGS {0}

#define BUILTINS_LIST                                                          \
                                                                               \
  BUILTIN_TY(va_list)                                                          \
                                                                               \
  /********** FUNCS **********/                                                \
  BUILTIN_FN(va_start, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID), 1,        \
             ARR_LIT({BUILTIN_TYPE_SPEC_VA_LIST}))                             \
  BUILTIN_FN(va_arg, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID), 1,          \
             ARR_LIT({BUILTIN_TYPE_SPEC_VA_LIST}))                             \
  BUILTIN_FN(va_copy, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID), 2,         \
             ARR_LIT({BUILTIN_TYPE_SPEC_VA_LIST, BUILTIN_TYPE_SPEC_VA_LIST}))  \
  BUILTIN_FN(va_end, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID), 1,          \
             ARR_LIT({BUILTIN_TYPE_SPEC_VA_LIST}))                             \
                                                                               \
  BUILTIN_FN(popcount, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID), 1,        \
             ARR_LIT({BUILTIN_TYPE_SPEC_TD_VAR_TY(                             \
                 &TD_VAR_TY_WELL_KNOWN_SIGNED_INT)}))                          \
  BUILTIN_FN(popcountl, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID), 1,       \
             ARR_LIT({BUILTIN_TYPE_SPEC_TD_VAR_TY(                             \
                 &TD_VAR_TY_WELL_KNOWN_SIGNED_LONG)}))                         \
  BUILTIN_FN(popcountll, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID), 1,      \
             ARR_LIT({BUILTIN_TYPE_SPEC_TD_VAR_TY(                             \
                 &TD_VAR_TY_WELL_KNOWN_SIGNED_LONG_LONG)}))                    \
  BUILTIN_FN(clz, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID), 1,             \
             ARR_LIT({BUILTIN_TYPE_SPEC_TD_VAR_TY(                             \
                 &TD_VAR_TY_WELL_KNOWN_SIGNED_INT)}))                          \
  BUILTIN_FN(clzl, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID), 1,            \
             ARR_LIT({BUILTIN_TYPE_SPEC_TD_VAR_TY(                             \
                 &TD_VAR_TY_WELL_KNOWN_SIGNED_LONG)}))                         \
  BUILTIN_FN(clzll, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID), 1,           \
             ARR_LIT({BUILTIN_TYPE_SPEC_TD_VAR_TY(                             \
                 &TD_VAR_TY_WELL_KNOWN_SIGNED_LONG_LONG)}))                    \
  BUILTIN_FN(ctz, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID), 1,             \
             ARR_LIT({BUILTIN_TYPE_SPEC_TD_VAR_TY(                             \
                 &TD_VAR_TY_WELL_KNOWN_SIGNED_INT)}))                          \
  BUILTIN_FN(ctzl, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID), 1,            \
             ARR_LIT({BUILTIN_TYPE_SPEC_TD_VAR_TY(                             \
                 &TD_VAR_TY_WELL_KNOWN_SIGNED_LONG)}))                         \
  BUILTIN_FN(ctzll, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID), 1,           \
             ARR_LIT({BUILTIN_TYPE_SPEC_TD_VAR_TY(                             \
                 &TD_VAR_TY_WELL_KNOWN_SIGNED_LONG_LONG)}))                    \
  BUILTIN_FN(                                                                  \
      bswap16,                                                                 \
      BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_WELL_KNOWN_SIGNED_SHORT), 1,      \
      ARR_LIT(                                                                 \
          {BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_WELL_KNOWN_SIGNED_SHORT)}))  \
  BUILTIN_FN(bswap32,                                                          \
             BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_WELL_KNOWN_SIGNED_INT), 1, \
             ARR_LIT({BUILTIN_TYPE_SPEC_TD_VAR_TY(                             \
                 &TD_VAR_TY_WELL_KNOWN_SIGNED_INT)}))                          \
  BUILTIN_FN(                                                                  \
      bswap64,                                                                 \
      BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_WELL_KNOWN_SIGNED_LONG_LONG), 1,  \
      ARR_LIT({BUILTIN_TYPE_SPEC_TD_VAR_TY(                                    \
          &TD_VAR_TY_WELL_KNOWN_SIGNED_LONG_LONG)}))                           \
                                                                               \
  BUILTIN_FN(unreachable, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID), 0,     \
             NO_ARGS)                                                          \
                                                                               \
  BUILTIN_FN(                                                                  \
      memset, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID_POINTER), 3,         \
      ARR_LIT({BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID_POINTER),           \
               BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_WELL_KNOWN_SIGNED_INT),  \
               BUILTIN_TYPE_SPEC_SIZE_T}))                                     \
                                                                               \
  BUILTIN_FN(                                                                  \
      memcpy, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID_POINTER), 3,         \
      ARR_LIT({BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID_POINTER),           \
               BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_CONST_VOID_POINTER),     \
               BUILTIN_TYPE_SPEC_SIZE_T}))                                     \
                                                                               \
  BUILTIN_FN(                                                                  \
      memmove, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID_POINTER), 3,        \
      ARR_LIT({BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID_POINTER),           \
               BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_CONST_VOID_POINTER),     \
               BUILTIN_TYPE_SPEC_SIZE_T}))                                     \
                                                                               \
  BUILTIN_FN(                                                                  \
      memcmp, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_WELL_KNOWN_SIGNED_INT),   \
      3,                                                                       \
      ARR_LIT({BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_CONST_VOID_POINTER),     \
               BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_CONST_VOID_POINTER),     \
               BUILTIN_TYPE_SPEC_SIZE_T}))                                     \
                                                                               \
  BUILTIN_FN(                                                                  \
      error, BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_VOID), 1,                  \
      ARR_LIT({BUILTIN_TYPE_SPEC_TD_VAR_TY(&TD_VAR_TY_CONST_CHAR_POINTER)}))

#define BUILTIN_TY(...)
#define BUILTIN_FN(name, ret_ty, param_count, param_tys)                       \
  extern const struct builtin_fn_spec builtin_##name;

BUILTINS_LIST

#undef BUILTIN_FN
#undef BUILTIN_TY

#endif
