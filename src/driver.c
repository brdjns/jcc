#include "driver.h"

#include "aarch64.h"
#include "alloc.h"
#include "args.h"
#include "compiler.h"
#include "fs.h"
#include "hashtbl.h"
#include "io.h"
#include "log.h"
#include "lsp/lsp.h"
#include "preproc.h"
#include "profile.h"
#include "program.h"
#include "rv32i.h"
#include "target.h"
#include "util.h"
#include "x64.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool target_needs_linking(const struct compile_args *args,
                                 const struct target *target) {
  if (args->preproc_only || args->lex_only || args->parse_only ||
      args->syntax_only || args->build_asm_file || args->build_object_file) {
    return false;
  }

  return target->link_objects != NULL;
}

static const struct target *get_target(enum compile_target target) {
  const struct target *target_val;

  switch (target) {
  case COMPILE_TARGET_MACOS_X86_64:
    target_val = &X64_MACOS_TARGET;
    break;
  case COMPILE_TARGET_LINUX_X86_64:
    target_val = &X64_LINUX_TARGET;
    break;
  case COMPILE_TARGET_LINUX_ARM64:
    target_val = &AARCH64_LINUX_TARGET;
    break;
  case COMPILE_TARGET_MACOS_ARM64:
    target_val = &AARCH64_MACOS_TARGET;
    break;
  case COMPILE_TARGET_LINUX_RV32I:
    target_val = &RV32I_LINUX_TARGET;
    break;
  case COMPILE_TARGET_EEP:
    BUG("redo eep");
    // return &EEP_TARGET;
  }

  if (target_val->target_id == TARGET_ID_NOT_SUPPORTED) {
    fprintf(stderr, "jcc was not built with support for target '%s'\n",
            string_target(target));
    return NULL;
  }

  return target_val;
}

static bool validate_fixed_timestamp(const char *str) {
  size_t len = strlen(str);

  if (len >= 19) {
    return true;
  }

  err("'-tm fixed_timestamp' must be at least 19 chars (for symmetry "
      "with "
      "`asctime`)");
  return false;
}

static bool get_target_for_args(enum compile_arch arch,
                                enum compile_target *target) {
  switch (arch) {
  case COMPILE_ARCH_NATIVE:
#if OS_APPLE && ARCH_AARCH64
    info("Compiling for native platform - assuming macOS ARM64...\n");
    *target = COMPILE_TARGET_MACOS_ARM64;
    return true;
#elif OS_APPLE && ARCH_X86_64
    info("Compiling for native platform - assuming macOS x64...\n");
    *target = COMPILE_TARGET_MACOS_X86_64;
    return true;
#elif OS_LINUX && ARCH_AARCH64
    info("Compiling for native platform - assuming Linux ARM64...\n");
    *target = COMPILE_TARGET_LINUX_ARM64;
    return true;
#elif OS_LINUX && ARCH_X86_64
    info("Compiling for native platform - assuming Linux x64...\n");
    *target = COMPILE_TARGET_LINUX_X86_64;
    return true;
#else
    err("Could not determine native platform (OS_NAME=" OS_NAME
        ", ARCH_NAME=" ARCH_NAME ")");
    return false;
#endif

  case COMPILE_ARCH_X86_64:
#if OS_APPLE
    *target = COMPILE_TARGET_MACOS_X86_64;
    info("Compiling for '%s'...\n", string_target(*target));
    return true;
#elif OS_LINUX
    *target = COMPILE_TARGET_LINUX_X86_64;
    info("Compiling for '%s'...\n", string_target(*target));
    return true;
#else
    err("Could not determine native platform for x86_64 (OS_NAME=" OS_NAME ")");
    return false;
#endif
  case COMPILE_ARCH_ARM64:
#if OS_APPLE
    *target = COMPILE_TARGET_MACOS_ARM64;
    info("Compiling for '%s'...\n", string_target(*target));
    return true;
#elif OS_LINUX
    *target = COMPILE_TARGET_LINUX_ARM64;
    info("Compiling for '%s'...\n", string_target(*target));
    return true;
    break;
#else
    err("Could not determine native platform for arm64 (OS_NAME=" OS_NAME ")");
    return false;
#endif
  case COMPILE_ARCH_RV32I:
#if OS_LINUX
    *target = COMPILE_TARGET_LINUX_RV32I;
    info("Compiling for '%s'...\n", string_target(*target));
    return true;
#else
    err("Could not determine native platform for rv32i (OS_NAME=" OS_NAME ")");
    return false;
#endif
  case COMPILE_ARCH_EEP:
    *target = COMPILE_TARGET_EEP;
    info("Compiling for '%s'...\n", string_target(*target));
    return true;
  }
}

static const char *get_default_isysroot(struct fs *fs,
                                        struct arena_allocator *arena,
                                        enum compile_target target) {
  // requires target to have been resolved
  switch (target) {
  case COMPILE_TARGET_MACOS_ARM64:
  case COMPILE_TARGET_MACOS_X86_64: {
    const char *env = getenv("SDKROOT");
    if (env && env[0]) {
      return env;
    }

#if OS_APPLE
    struct fs_file sdk_path;
    if (!fs_read_proc(fs, MK_USTR("xcrun --sdk macosx --show-sdk-path"),
                      &sdk_path)) {
      BUG("xcrun call failed!");
    }

    char *path = aralloc_strndup(arena, sdk_path.data, sdk_path.len);

    if (sdk_path.len && sdk_path.data[sdk_path.len - 1] == '\n') {
      // strip newline
      path[sdk_path.len - 1] = '\0';
    }

    return path;
#else
    warn("no isysroot found!");
#endif
  }
  case COMPILE_TARGET_LINUX_RV32I:
    return "/opt/riscv/riscv64-unknown-elf/include";
  default:
    return "";
  }
}

static void print_ver(FILE *file, const char *location) {
  fprintf(file,
          "jcc version %s\n"
          "John Kelly <johnharrykelly@gmail.com>\n"
          "location:  %s\n"
          "OS_NAME:   %s\n"
          "ARCH_NAME: %s\n",
          JCC_VERSION, location, OS_NAME, ARCH_NAME);

#ifndef NDEBUG
  fprintf(file, "SANITIZERS: ");
  bool any = false;
#if MSAN
  if (any) {
    fprintf(file, "|");
  }
  fprintf(file, "memory");
  any = true;
#endif
#if ASAN
  if (any) {
    fprintf(file, "|");
  }
  fprintf(file, "address");
  any = true;
#endif
#if LSAN
  if (any) {
    fprintf(file, "|");
  }
  fprintf(file, "leak");
  any = true;
#endif
#if HWASAN
  if (any) {
    fprintf(file, "|");
  }
  fprintf(file, "hwaddress");
  any = true;
#endif
#if TSAN
  if (any) {
    fprintf(file, "|");
  }
  fprintf(file, "thread");
  any = true;
#endif
#if UBSAN
  if (any) {
    fprintf(file, "|");
  }
  fprintf(file, "undefined");
  any = true;
#endif

  if (!any) {
    fprintf(file, "none");
  }

  fprintf(file, "\n");

#endif

#ifdef JCC_DEFAULT_TARGET
#define MKSTR_INNER(x) #x
#define MKSTR(x) MKSTR_INNER(x)
  fprintf(file, "JCC_DEFAULT_TARGET: %s\n", MKSTR(JCC_DEFAULT_TARGET));
#endif
}

static enum parse_args_result
try_get_compile_args(int argc, char **argv, struct parsed_args *args,
                     struct arena_allocator *arena, struct fs **fs,
                     struct compile_args *compile_args, size_t *num_sources,
                     const char ***sources) {
  enum parse_args_result result = parse_args(argc, argv, args);

  // zero init to make freeing easier later
  *compile_args = (struct compile_args){0};

  if (result != PARSE_ARGS_RESULT_SUCCESS) {
    return result;
  }

  if (args->version || args->verbose) {
    print_ver(args->version ? stdout : stderr, argv[0]);

    if (args->version) {
      return PARSE_ARGS_RESULT_HELP;
    }
  }

  enum fs_flags fs_flags = FS_FLAG_NONE;
  switch (args->driver) {
  case JCC_DRIVER_COMPILER:
  case JCC_DRIVER_INTERP:
    fs_flags |= FS_FLAG_ASSUME_CONSTANT;
    break;
  case JCC_DRIVER_LSP:
    break;
  }

  fs_create(arena, fs_flags, fs);

  struct hashtbl *log_symbols = NULL;

  if (args->log_symbols.num_values) {
    log_symbols = hashtbl_create_str_keyed(0);
    for (size_t i = 0; i < args->log_symbols.num_values; i++) {
      const char *sym = args->log_symbols.values[i];

      hashtbl_insert(log_symbols, &sym, NULL);
    }
  }

  struct compile_file output;
  if (!args->output) {
    output = (struct compile_file){
        .ty = COMPILE_FILE_TY_NONE,
    };
  } else if (!strcmp(args->output, "-")) {
    output = (struct compile_file){
        .ty = COMPILE_FILE_TY_STDOUT,
    };
  } else {
    output =
        (struct compile_file){.ty = COMPILE_FILE_TY_PATH, .path = args->output};
  }

  if (!args->target) {
    if (!get_target_for_args(args->arch, &args->target)) {
      return PARSE_ARGS_RESULT_FAIL;
    }
  } else {
    if (args->arch) {
      err("Cannot provide '-arch' and '-target'");
      return PARSE_ARGS_RESULT_FAIL;
    }

    compile_args->target = args->target;
  }

  size_t num_sys_include_paths = args->sys_include_paths.num_values + 2;
  const char **sys_include_paths =
      aralloc(arena, sizeof(*sys_include_paths) * num_sys_include_paths);

  if (!args->isys_root) {
    args->isys_root = get_default_isysroot(*fs, arena, args->target);
  }

  const char *target = string_target(args->target);

  sys_include_paths[0] = path_combine(arena, args->isys_root, "/usr/include");
  sys_include_paths[1] = path_combine(arena, sys_include_paths[0], target);
  // TODO: support `=` prefix for `isystem`
  for (size_t i = 0; i < args->sys_include_paths.num_values; i++) {
    sys_include_paths[2 + i] = args->sys_include_paths.values[i];
  }

  // is having two seperate structs for args really sensible?
  // the original reason is that e.g `parsed_args` has an `arch` and a `target`
  // whereas `compile_args` only has `target`, but it is a hassle
  *compile_args = (struct compile_args){
      // don't print diagnostics in LSP context (it will consume them itself)
      .print_diagnostics = args->driver == JCC_DRIVER_COMPILER,

      .warnings_as_errors = args->warnings_as_error,
      .no_warnings = args->no_warnings,

      .preproc_only = args->preprocess,
      .lex_only = args->lex_only,
      .parse_only = args->parse_only,
      .syntax_only = args->syntax_only || args->driver == JCC_DRIVER_LSP,
      .build_asm_file = args->assembly,
      .build_object_file = args->object,
      .codegen_flags = args->codegen_flags,
      .target = args->target,

      .log_symbols = log_symbols,

      .diagnostics_sink = args->diagnostics_sink,

      .verbose = args->verbose,

      .c_standard = args->c_standard,
      .log_flags = args->log_level,
      .opts_level = args->opts,

      .fixed_timestamp = args->timestamp,
      .sys_include_paths = sys_include_paths,
      .num_sys_include_paths = num_sys_include_paths,

      .sys_root = args->isys_root,

      .include_paths = args->include_paths.values,
      .num_include_paths = args->include_paths.num_values,

      .use_graphcol_regalloc = args->use_graphcol_regalloc,

      .output = output,

      .num_defines = args->define_macros.num_values,
      .defines = aralloc(arena, sizeof(compile_args->defines[0]) *
                                    args->define_macros.num_values)};

  for (size_t i = 0; i < args->define_macros.num_values; i++) {
    const char *def_macro = args->define_macros.values[i];

    const char *val_str = strchr(def_macro, '=');

    ustr_t name;
    ustr_t value;
    if (val_str) {
      name = (ustr_t){.str = def_macro, .len = (size_t)(val_str - def_macro)};
      value = MK_USTR(val_str + 1);
    } else {
      name = MK_USTR(def_macro);
      value = MK_USTR("1");
    }

    compile_args->defines[i] =
        (struct preproc_define_macro){.name = name, .value = value};
  }

  *num_sources = args->num_values;
  *sources = args->values;

  if (args->log_level & COMPILE_LOG_FLAGS_ARGS) {
    debug_print_parsed_args(stderr, args);
  }

  if (compile_args->fixed_timestamp &&
      !validate_fixed_timestamp(compile_args->fixed_timestamp)) {
    return PARSE_ARGS_RESULT_FAIL;
  }

  switch (args->driver) {
  case JCC_DRIVER_INTERP:
    if (args->num_values != 1) {
      err("interpretor only supports single-file currently");
      return PARSE_ARGS_RESULT_FAIL;
    }
    break;
  case JCC_DRIVER_COMPILER:
    if (!args->num_values) {
      err("No sources provided");
      return PARSE_ARGS_RESULT_FAIL;
    }
    break;
  case JCC_DRIVER_LSP:
    if (args->num_values) {
      err("Cannot provide sources in lsp mode");
      return PARSE_ARGS_RESULT_FAIL;
    }
    break;
  }

  return PARSE_ARGS_RESULT_SUCCESS;
}

// static void signal_handle(UNUSED int signal) {
//   debug_print_stack_trace(); // NOLINT(bugprone-signal-handler)
// }

void jcc_init(void) {
  // signal(SIGTRAP, signal_handle);
  // signal(SIGABRT, signal_handle);
  // signal(SIGSEGV, signal_handle);

  // we want to use the user's locale i think?
  // TODO: remove this
  const char *locale;
  if (!(locale = setlocale(LC_ALL, "C.utf8"))) {
    locale = setlocale(LC_ALL, "");
  }

#if SAN && OS_APPLE
  // sanitizer running causes spurious 'malloc: nano zone abandoned due to
  // inability to reserve vm space.' messages unless `MallocNanoZone=0` can be
  // resolved by https://github.com/google/sanitizers/issues/1666
  const char *val = getenv("MallocNanoZone");
  if (!val || strcmp(val, "0") != 0) {
    warn("With sanitisers enabled on macOS, buggy warning messages can appear. "
         "Set `MallocNanoZone=0` to fix (or run via `jcc.sh` which does this "
         "automatically)");
  }
#endif
}

// FIXME: in clang you can do `-x c foo.c -x object foo`
// but our args are not positional
static bool try_get_language_for_file(struct path_components components,
                                      enum compile_language *language) {
  if (!components.ext[0] || !strcmp(components.ext, "o")) {
    // assume no extension or `.o` is object file
    *language = COMPILE_LANGUAGE_OBJECT;
    return true;
  }

  if (!strcmp(components.ext, "a")) {
    // intermediate (already preprocessed) file
    *language = COMPILE_LANGUAGE_SHAREDLIB;
    return true;
  }

  if (!strcmp(components.ext, "i")) {
    // intermediate (already preprocessed) file
    *language = COMPILE_LANGUAGE_CPP_OUTPUT;
    return true;
  }

  if (!strcmp(components.ext, "h")) {
    *language = COMPILE_LANGUAGE_C_HEADER;
    return true;
  }

  if (!strcmp(components.ext, "c")) {
    *language = COMPILE_LANGUAGE_C;
    return true;
  }

  return false;
}

static int jcc_driver_lsp(struct arena_allocator *arena, struct fs *fs,
                          struct parsed_args args,
                          struct compile_args compile_args,
                          const struct target *target);

#if JCC_INTERP

static int jcc_driver_interp(UNUSED struct arena_allocator *arena,
                             struct fs *fs, UNUSED struct parsed_args args,
                             struct compile_args compile_args,
                             const struct target *target, const char *source);

#endif

static int jcc_driver_compiler(struct arena_allocator *arena, struct fs *fs,
                               struct parsed_args args,
                               struct compile_args compile_args,
                               const struct target *target, size_t num_sources,
                               const char **sources);

int jcc_main(int argc, char **argv) {
  int exc = 0;

  profiler_init();

  struct arena_allocator *arena = NULL;
  arena_allocator_create("main", &arena);

  struct fs *fs;
  struct parsed_args args;
  struct compile_args compile_args;
  size_t num_sources;
  const char **sources;
  enum parse_args_result parse_result = try_get_compile_args(
      argc, argv, &args, arena, &fs, &compile_args, &num_sources, &sources);

  switch (parse_result) {
  case PARSE_ARGS_RESULT_SUCCESS:
    break;
  case PARSE_ARGS_RESULT_HELP:
    exc = 0;
    goto exit;
  case PARSE_ARGS_RESULT_FAIL:
    exc = 1;
    goto exit;
  }

  const struct target *target = get_target(compile_args.target);
  if (!target) {
    exc = 1;
    goto exit;
  }

  switch (args.driver) {
  case JCC_DRIVER_COMPILER:
    exc = jcc_driver_compiler(arena, fs, args, compile_args, target,
                              num_sources, sources);
    break;
  case JCC_DRIVER_INTERP:
#if JCC_INTERP
    exc = jcc_driver_interp(arena, fs, args, compile_args, target, sources[0]);
#else
    err("jcc was not built with interpreter support");
    exc = 1;
#endif
    break;
  case JCC_DRIVER_LSP:
    exc = jcc_driver_lsp(arena, fs, args, compile_args, target);
    break;
  }

exit:
  free_args(&args);
  arena_allocator_free(&arena);
  return exc;
}

static int jcc_driver_lsp(struct arena_allocator *arena, struct fs *fs,
                          struct parsed_args args,
                          struct compile_args compile_args,
                          const struct target *target) {
  return lsp_run(arena, fs, args, compile_args, target);
}

#if JCC_INTERP

static int jcc_driver_interp(UNUSED struct arena_allocator *arena,
                             struct fs *fs, UNUSED struct parsed_args args,
                             struct compile_args compile_args,
                             const struct target *target, const char *source) {
  // FIXME: does not contain full logic supported by compiler

  disable_log();
  struct compiler *compiler;

  struct fs_file file;
  if (!fs_read_path(fs, MK_USTR(source), &file)) {
    return 1;
  }

  struct compiler_create_args comp_args = {
      .program = (struct program){.text = file.data},
      .fs = fs,
      .target = target,
      .args = compile_args,
      .working_dir = source,
      .mode = COMPILE_PREPROC_MODE_PREPROC,
      .output =
          (struct compile_file){.ty = COMPILE_FILE_TY_PATH, .path = NULL}};

  if (compiler_create(&comp_args, &compiler) !=
      COMPILER_CREATE_RESULT_SUCCESS) {
    err("failed to create compiler");
    return 1;
  }

  struct interp_result result = interp(compiler);
  if (result.compile_result != COMPILE_RESULT_SUCCESS) {
    // temp disabled because doesnt respect `-fdiagnostics-sink`
    // err("compilation failed!");

    free_compiler(&compiler);
    return 1;
  }

  free_compiler(&compiler);

  return result.exc;
}

#endif

static int jcc_driver_compiler(struct arena_allocator *arena, struct fs *fs,
                               struct parsed_args args,
                               struct compile_args compile_args,
                               const struct target *target, size_t num_sources,
                               const char **sources) {
  int exc = 1;

  const char **objects = nonnull_malloc(sizeof(*objects) * num_sources);

  info("beginning compilation stage...");
  for (size_t i = 0; i < num_sources; i++) {
    const char *source_path = sources[i];

    info("compiling source file \"%s\"", source_path);

    struct path_components components = path_components(arena, source_path);

    enum compile_preproc_mode mode = COMPILE_PREPROC_MODE_PREPROC;

    if (!strcmp(source_path, "-")) {
      // stdin, fine
      info("reading source file from stdin\n");
    }

    enum compile_language language = args.language;
    if (language == COMPILE_LANGUAGE_NONE &&
        !try_get_language_for_file(components, &language)) {
      err("unrecognised file type \"%s\"", components.ext);
      exc = -1;
      goto exit;
    }

    switch (language) {
    case COMPILE_LANGUAGE_NONE:
      unreachable();
    case COMPILE_LANGUAGE_C:
      break;
    case COMPILE_LANGUAGE_C_HEADER:
      warn("compiling header file '%s', is this intentional?", source_path);
      break;
    case COMPILE_LANGUAGE_CPP_OUTPUT:
      mode = COMPILE_PREPROC_MODE_NO_PREPROC;
      break;
    case COMPILE_LANGUAGE_OBJECT:
    case COMPILE_LANGUAGE_SHAREDLIB:
      info("linking object file '%s", source_path);
      objects[i] = source_path;
      continue;
    }

    PROFILE_BEGIN(compile);

    PROFILE_BEGIN(source_read);

    struct fs_file source;
    bool success;
    if (!strcmp(source_path, "-")) {
      success = fs_read_stdin(fs, &source);
    } else {
      success = fs_read_path(fs, MK_USTR(source_path), &source);
    }

    PROFILE_END(source_read);

    if (!success) {
      err("source file \"%s\" could not be read!", source_path);
      exc = COMPILE_RESULT_BAD_FILE;
      goto exit;
    }

    struct compile_file file;

    // this will output `-.o` or `-.s` if read from stdin, which is weird, but
    // matches clang?

    if (compile_args.preproc_only &&
        compile_args.output.ty == COMPILE_FILE_TY_NONE) {
      file = (struct compile_file){
          .ty = COMPILE_FILE_TY_STDOUT,
      };
      info("preprocessing source file '%s' into stdout", source_path);
    } else if (compile_args.build_asm_file) {
      if (compile_args.output.ty == COMPILE_FILE_TY_NONE) {
        file = (struct compile_file){
            .ty = COMPILE_FILE_TY_PATH,
            .path = path_replace_ext(arena, source_path, "s")};

        info("compiling source file '%s' into assembly file '%s'", source_path,
             file.path);
      } else {
        file = compile_args.output;
      }
    } else if (target_needs_linking(&compile_args, target) ||
               compile_args.output.ty == COMPILE_FILE_TY_NONE) {
      const char *path;
      FILE *tmp = fs_tmpfile(&path);

      file = (struct compile_file){
          .ty = COMPILE_FILE_TY_FILE, .file = tmp, .path = path};
    } else {
      file = compile_args.output;
      info("compiling source file '%s' into object file '%s'", source_path,
           file.path);
    }

    if (file.ty == COMPILE_FILE_TY_PATH || file.ty == COMPILE_FILE_TY_FILE) {
      objects[i] = file.path;
    }

    // TODO: make program contain length to to allow null chars
    struct program program = {.text = source.data};

    disable_log();
    struct compiler *compiler;

    PROFILE_BEGIN(create_compiler);

    struct compiler_create_args comp_args = {.program = program,
                                             .fs = fs,
                                             .target = target,
                                             .args = compile_args,
                                             .working_dir = source_path,
                                             .mode = mode,
                                             .output = file};

    if (compiler_create(&comp_args, &compiler) !=
        COMPILER_CREATE_RESULT_SUCCESS) {
      err("failed to create compiler");
      exc = -1;
      goto exit;
    }

    PROFILE_END(create_compiler);

    if (compile(compiler) != COMPILE_RESULT_SUCCESS) {
      // temp disabled because doesnt respect `-fdiagnostics-sink`
      // err("compilation failed!");

      free_compiler(&compiler);
      exc = -1;
      goto exit;
    }

    PROFILE_END(compile);

    // this can be non-trivially slow and maybe isn't worth doing
    PROFILE_BEGIN(free_compiler);
    free_compiler(&compiler);
    PROFILE_END(free_compiler);
  }

  if (target_needs_linking(&compile_args, target)) {
    const char *output;
    switch (compile_args.output.ty) {
    case COMPILE_FILE_TY_NONE:
      output = "a.out";
      break;
    case COMPILE_FILE_TY_PATH:
    case COMPILE_FILE_TY_FILE:
      output = compile_args.output.path;
      break;
    case COMPILE_FILE_TY_STDOUT:
      BUG("linking to stdout/stderr not supported");
    }

    struct link_args link_args = {.fs = fs,
                                  .args = &compile_args,
                                  .linker_args = args.linker_args.values,
                                  .num_linker_args =
                                      args.linker_args.num_values,
                                  .objects = (const char *const *)objects,
                                  .num_objects = num_sources,
                                  .output = output};

    PROFILE_BEGIN(link);

    if (target->link_objects(&link_args) != LINK_RESULT_SUCCESS) {
      err("link failed");
      exc = -1;
      goto exit;
    }

    PROFILE_END(link);
  } else {
    if (num_sources > 1) {
      TODO("multiple objects, but target does not support linking");
    }
  }

  info("Compilation succeeded!");

  exc = 0;

exit:
  if (args.profile) {
    profiler_print_text(stderr);
  }

  if (args.profile_json) {
    FILE *file;
    if (!strcmp(args.profile_json, "-")) {
      file = stdout;
    } else {
      file = fopen(args.profile_json, "w");
    }
    invariant_assert(file, "open file failed");
    profiler_print_json(file);
    fclose(file);
  }

  if (objects) {
    free(objects);
  }

  if (compile_args.log_symbols) {
    hashtbl_free(&compile_args.log_symbols);
  }

  return exc;
}
