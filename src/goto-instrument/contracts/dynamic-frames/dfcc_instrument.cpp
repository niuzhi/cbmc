/*******************************************************************\

Module: Dynamic frame condition checking

Author: Remi Delmas, delmarsd@amazon.com

\*******************************************************************/

// TODO apply loop contracts transformations as part of function instrumentation

#include "dfcc_instrument.h"

#include <util/format_expr.h>
#include <util/fresh_symbol.h>
#include <util/namespace.h>
#include <util/pointer_expr.h>
#include <util/pointer_predicates.h>
#include <util/prefix.h>
#include <util/suffix.h>

#include <goto-programs/goto_model.h>
#include <goto-programs/remove_skip.h>

#include <ansi-c/c_expr.h>
#include <ansi-c/c_object_factory_parameters.h>
#include <goto-instrument/contracts/utils.h>
#include <goto-instrument/generate_function_bodies.h>
#include <goto-instrument/unwind.h>
#include <goto-instrument/unwindset.h>
#include <langapi/language_util.h>

#include "dfcc_cfg_info.h"
#include "dfcc_contract_clauses_codegen.h"
#include "dfcc_instrument_loop.h"
#include "dfcc_is_cprover_symbol.h"
#include "dfcc_is_freeable.h"
#include "dfcc_is_fresh.h"
#include "dfcc_library.h"
#include "dfcc_obeys_contract.h"
#include "dfcc_pointer_in_range.h"
#include "dfcc_spec_functions.h"
#include "dfcc_utils.h"

#include <memory>

std::set<irep_idt> dfcc_instrumentt::function_cache;

dfcc_instrumentt::dfcc_instrumentt(
  goto_modelt &goto_model,
  message_handlert &message_handler,
  dfcc_libraryt &library,
  dfcc_spec_functionst &spec_functions,
  dfcc_contract_clauses_codegent &contract_clauses_codegen)
  : goto_model(goto_model),
    message_handler(message_handler),
    log(message_handler),
    library(library),
    spec_functions(spec_functions),
    contract_clauses_codegen(contract_clauses_codegen),
    instrument_loop(
      goto_model,
      message_handler,
      library,
      spec_functions,
      contract_clauses_codegen),
    ns(goto_model.symbol_table)
{
  // these come from different assert.h implementation on different systems
  // and eventually become ASSERT instructions and must not be instrumented
  internal_symbols.insert("__assert_fail");
  internal_symbols.insert("_assert");
  internal_symbols.insert("__assert_c99");
  internal_symbols.insert("_wassert");
  internal_symbols.insert("__assert_rtn");
  internal_symbols.insert("__assert");
  internal_symbols.insert("__assert_func");

  /// These builtins are translated to no-ops and must not be instrumented
  internal_symbols.insert("__builtin_prefetch");
  internal_symbols.insert("__builtin_unreachable");

  /// These builtins are valist management functions
  /// and interpreted natively in src/goto-symex/symex_builtin_functions.cpp
  /// and must not be instrumented
  internal_symbols.insert(ID_gcc_builtin_va_arg);
  internal_symbols.insert("__builtin_va_copy");
  internal_symbols.insert("__builtin_va_start");
  internal_symbols.insert("__va_start");
  internal_symbols.insert("__builtin_va_end");

  /// These builtins get translated to CPROVER equivalents
  /// and must not be instrumented
  internal_symbols.insert("__builtin_isgreater");
  internal_symbols.insert("__builtin_isgreaterequal");
  internal_symbols.insert("__builtin_isless");
  internal_symbols.insert("__builtin_islessequal");
  internal_symbols.insert("__builtin_islessgreater");
  internal_symbols.insert("__builtin_isunordered");
}

void dfcc_instrumentt::get_instrumented_functions(
  std::set<irep_idt> &dest) const
{
  dest.insert(
    dfcc_instrumentt::function_cache.begin(),
    dfcc_instrumentt::function_cache.end());
}

std::size_t dfcc_instrumentt::get_max_assigns_clause_size() const
{
  return instrument_loop.get_max_assigns_clause_size();
}

/*
  A word on built-ins:

  C compiler builtins are declared in
  - src/ansi-c/clang_builtin_headers*.h
  - src/ansi-c/gcc_builtin_headers*.h
  - src/ansi-c/windows_builtin_headers.h

  Some gcc builtins are compiled down to goto instructions
  and inlined in place during type-checking:
  - src/ansi-c/c_typecheck_gcc_polymorphic_builtins.cpp
  - src/ansi-c/c_typecheck_expr.cpp, method do_special_functions
  so they essentially disappear from the model.

  Builtins are also handled in:
  - src/goto-programs/builtin_functions.cpp
  - src/goto-symex/symex_builtin_functions.cpp

  Some compiler builtins have implementations defined as C functions in the
  cprover library, and these should be instrumented just like other functions.

  Last, some compiler built-ins might have just a declaration but
  no conversion or library implementation.
  They might then persist in the program as functions which return a nondet
  value or transformed into side_effect_nondet_exprt during conversion
  If they survive as functions we should be able to add an extra parameter
  to these functions even if they have no body.

  The CPROVER built-ins are declared here:
  - src/ansi-c/cprover_builtin_headers.h
  - src/ansi-c/cprover_library.h
  - src/ansi-c/library/cprover_contracts.c
  and should not be instrumented.

  The case of __CPROVER_deallocate is special: it is a wrapper for an assignment
  to the __CPROVER_deallocated_object global. We do not want to
  instrument this function, but we still want to check that its parameters
  are allowed for deallocation by the write set.

  There is also a number of CPROVER global static symbols that are used to
  support memory safety property instrumentation, and assignments to these
  statics should always be allowed (i.e not instrumented):
  - __CPROVER_alloca_object,
  - __CPROVER_dead_object,
  - __CPROVER_deallocated,
  - __CPROVER_malloc_is_new_array,
  - __CPROVER_max_malloc_size,
  - __CPROVER_memory_leak,
  - __CPROVER_new_object,
  - __CPROVER_next_thread_id,
  - __CPROVER_next_thread_key,
  - __CPROVER_pipe_count,
  - __CPROVER_rounding_mode,
  - __CPROVER_thread_id,
  - __CPROVER_thread_key_dtors,
  - __CPROVER_thread_keys,
  - __CPROVER_threads_exited,
  -  ... (and more of them)

  /// These have a library implementation and must be instrumented
  static std::set<irep_idt> alloca_builtins = {"alloca", "__builtin_alloca"};

  /// These built-ins have CPROVER library implementation, can be instrumented
  static std::set<std::string> builtins_with_cprover_impl = {
    "__builtin_ia32_sfence",
    "__builtin_ia32_lfence",
    "__builtin_ia32_mfence",
    "__builtin_ffs",
    "__builtin_ffsl",
    "__builtin_ffsll",
    "__builtin_ia32_vec_ext_v4hi",
    "__builtin_ia32_vec_ext_v8hi",
    "__builtin_ia32_vec_ext_v4si",
    "__builtin_ia32_vec_ext_v2di",
    "__builtin_ia32_vec_ext_v16qi",
    "__builtin_ia32_vec_ext_v4sf",
    "__builtin_ia32_psubsw128",
    "__builtin_ia32_psubusw128",
    "__builtin_ia32_paddsw",
    "__builtin_ia32_psubsw",
    "__builtin_ia32_vec_init_v4hi",
    "__builtin_flt_rounds",
    "__builtin_fabs",
    "__builtin_fabsl",
    "__builtin_fabsf",
    "__builtin_inff",
    "__builtin_inf",
    "__builtin_infl",
    "__builtin_isinf",
    "__builtin_isinff",
    "__builtin_isinf",
    "__builtin_isnan",
    "__builtin_isnanf",
    "__builtin_huge_valf",
    "__builtin_huge_val",
    "__builtin_huge_vall",
    "__builtin_nan",
    "__builtin_nanf",
    "__builtin_abs",
    "__builtin_labs",
    "__builtin_llabs",
    "__builtin_alloca",
    "__builtin___strcpy_chk",
    "__builtin___strcat_chk",
    "__builtin___strncat_chk",
    "__builtin___strncpy_chk",
    "__builtin___memcpy_chk",
    "__builtin_memset",
    "__builtin___memset_chk",
    "__builtin___memmove_chk"};
*/

/// True iff the symbol must not be instrumented
bool dfcc_instrumentt::is_internal_symbol(const irep_idt &id) const
{
  return internal_symbols.find(id) != internal_symbols.end();
}

bool dfcc_instrumentt::do_not_instrument(const irep_idt &id) const
{
  return !has_prefix(id2string(id), CPROVER_PREFIX "file_local") &&
         (dfcc_is_cprover_function_symbol(id) || is_internal_symbol(id));
}

void dfcc_instrumentt::instrument_harness_function(
  const irep_idt &function_id,
  const loop_contract_configt &loop_contract_config,
  std::set<irep_idt> &function_pointer_contracts)
{
  // never instrument a function twice
  bool inserted = dfcc_instrumentt::function_cache.insert(function_id).second;
  if(!inserted)
    return;

  auto found = goto_model.goto_functions.function_map.find(function_id);
  PRECONDITION_WITH_DIAGNOSTICS(
    found != goto_model.goto_functions.function_map.end(),
    "Function '" + id2string(function_id) + "' must exist in the model.");

  const null_pointer_exprt null_expr(
    to_pointer_type(library.dfcc_type[dfcc_typet::WRITE_SET_PTR]));

  // create a local write set symbol
  const auto &function_symbol =
    dfcc_utilst::get_function_symbol(goto_model.symbol_table, function_id);
  const auto write_set = dfcc_utilst::create_symbol(
    goto_model.symbol_table,
    library.dfcc_type[dfcc_typet::WRITE_SET_PTR],
    function_id,
    "__write_set_to_check",
    function_symbol.location);

  std::set<symbol_exprt> local_statics = get_local_statics(function_id);

  goto_functiont &goto_function = found->second;

  instrument_goto_function(
    function_id,
    goto_function,
    write_set,
    local_statics,
    loop_contract_config,
    function_pointer_contracts);

  auto &body = goto_function.body;

  // add write set definitions at the top of the function
  // DECL write_set_harness
  // ASSIGN write_set_harness := NULL
  auto first_instr = body.instructions.begin();

  body.insert_before(
    first_instr,
    goto_programt::make_decl(write_set, first_instr->source_location()));
  body.update();

  body.insert_before(
    first_instr,
    goto_programt::make_assignment(
      write_set, null_expr, first_instr->source_location()));

  goto_model.goto_functions.update();
}

std::set<symbol_exprt>
dfcc_instrumentt::get_local_statics(const irep_idt &function_id)
{
  std::set<symbol_exprt> local_statics;
  for(const auto &sym_pair : goto_model.symbol_table)
  {
    const auto &sym = sym_pair.second;
    if(sym.is_static_lifetime)
    {
      const auto &loc = sym.location;
      if(!loc.get_function().empty() && loc.get_function() == function_id)
      {
        local_statics.insert(sym.symbol_expr());
      }
    }
  }
  return local_statics;
}

void dfcc_instrumentt::instrument_function(
  const irep_idt &function_id,
  const loop_contract_configt &loop_contract_config,
  std::set<irep_idt> &function_pointer_contracts)
{
  // never instrument a function twice
  bool inserted = dfcc_instrumentt::function_cache.insert(function_id).second;
  if(!inserted)
    return;

  auto found = goto_model.goto_functions.function_map.find(function_id);
  PRECONDITION_WITH_DIAGNOSTICS(
    found != goto_model.goto_functions.function_map.end(),
    "Function '" + id2string(function_id) + "' must exist in the model.");

  const auto &write_set = dfcc_utilst::add_parameter(
                            goto_model,
                            function_id,
                            "__write_set_to_check",
                            library.dfcc_type[dfcc_typet::WRITE_SET_PTR])
                            .symbol_expr();

  std::set<symbol_exprt> local_statics = get_local_statics(function_id);

  goto_functiont &goto_function = found->second;

  instrument_goto_function(
    function_id,
    goto_function,
    write_set,
    local_statics,
    loop_contract_config,
    function_pointer_contracts);
}

void dfcc_instrumentt::instrument_wrapped_function(
  const irep_idt &wrapped_function_id,
  const irep_idt &initial_function_id,
  const loop_contract_configt &loop_contract_config,
  std::set<irep_idt> &function_pointer_contracts)
{
  // never instrument a function twice
  bool inserted =
    dfcc_instrumentt::function_cache.insert(wrapped_function_id).second;
  if(!inserted)
    return;

  auto found = goto_model.goto_functions.function_map.find(wrapped_function_id);
  PRECONDITION_WITH_DIAGNOSTICS(
    found != goto_model.goto_functions.function_map.end(),
    "Function '" + id2string(wrapped_function_id) +
      "' must exist in the model.");

  const auto &write_set = dfcc_utilst::add_parameter(
                            goto_model,
                            wrapped_function_id,
                            "__write_set_to_check",
                            library.dfcc_type[dfcc_typet::WRITE_SET_PTR])
                            .symbol_expr();

  std::set<symbol_exprt> local_statics = get_local_statics(initial_function_id);

  goto_functiont &goto_function = found->second;

  instrument_goto_function(
    wrapped_function_id,
    goto_function,
    write_set,
    local_statics,
    loop_contract_config,
    function_pointer_contracts);
}

void dfcc_instrumentt::instrument_goto_program(
  const irep_idt &function_id,
  goto_programt &goto_program,
  const exprt &write_set,
  std::set<irep_idt> &function_pointer_contracts)
{
  // create a dummy goto function with empty parameters
  goto_functiont goto_function;
  goto_function.body.copy_from(goto_program);

  // build control flow graph information
  dfcc_cfg_infot cfg_info(
    function_id,
    goto_function,
    write_set,
    loop_contract_configt{false},
    goto_model.symbol_table,
    message_handler,
    library);

  // instrument instructions
  goto_programt &body = goto_function.body;
  instrument_instructions(
    function_id,
    body,
    body.instructions.begin(),
    body.instructions.end(),
    cfg_info,
    function_pointer_contracts);

  // cleanup
  goto_program.clear();
  goto_program.copy_from(goto_function.body);
  remove_skip(goto_program);
  goto_model.goto_functions.update();
}

void dfcc_instrumentt::instrument_goto_function(
  const irep_idt &function_id,
  goto_functiont &goto_function,
  const exprt &write_set,
  const std::set<symbol_exprt> &local_statics,
  const loop_contract_configt &loop_contract_config,
  std::set<irep_idt> &function_pointer_contracts)
{
  if(!goto_function.body_available())
  {
    // generate a default body `assert(false);assume(false);`
    std::string options = "assert-false-assume-false";
    c_object_factory_parameterst object_factory_params;
    auto generate_function_bodies = generate_function_bodies_factory(
      options, object_factory_params, goto_model.symbol_table, message_handler);
    generate_function_bodies->generate_function_body(
      goto_function, goto_model.symbol_table, function_id);
    return;
  }

  auto &body = goto_function.body;

  // build control flow graph information
  dfcc_cfg_infot cfg_info(
    function_id,
    goto_function,
    write_set,
    loop_contract_config,
    goto_model.symbol_table,
    message_handler,
    library);

  // instrument instructions
  instrument_instructions(
    function_id,
    body,
    body.instructions.begin(),
    body.instructions.end(),
    cfg_info,
    function_pointer_contracts);

  // recalculate numbers, etc.
  goto_model.goto_functions.update();

  if(loop_contract_config.apply_loop_contracts)
  {
    apply_loop_contracts(
      function_id,
      goto_function,
      cfg_info,
      loop_contract_config,
      local_statics,
      function_pointer_contracts);
  }

  // insert add/remove instructions for local statics in the top level write set
  auto begin = body.instructions.begin();
  auto end = std::prev(body.instructions.end());

  // automatically add/remove local statics from the top level write set
  for(const auto &local_static : local_statics)
  {
    insert_add_decl_call(function_id, write_set, local_static, begin, body);
    insert_record_dead_call(function_id, write_set, local_static, end, body);
  }

  const code_typet &code_type = to_code_type(
    dfcc_utilst::get_function_symbol(goto_model.symbol_table, function_id)
      .type);
  const auto &top_level_tracked = cfg_info.get_top_level_tracked();

  // automatically add/remove function parameters that must be tracked in the
  // function write set (they must be explicitly tracked if they are assigned
  // in the body of a loop)
  for(const auto &param : code_type.parameters())
  {
    const irep_idt &param_id = param.get_identifier();
    if(top_level_tracked.find(param_id) != top_level_tracked.end())
    {
      symbol_exprt param_symbol{param.get_identifier(), param.type()};
      insert_add_decl_call(function_id, write_set, param_symbol, begin, body);
      insert_record_dead_call(function_id, write_set, param_symbol, end, body);
    }
  }

  remove_skip(body);

  // recalculate numbers, etc.
  goto_model.goto_functions.update();
}

void dfcc_instrumentt::instrument_instructions(
  const irep_idt &function_id,
  goto_programt &goto_program,
  goto_programt::targett first_instruction,
  const goto_programt::targett &last_instruction,
  dfcc_cfg_infot &cfg_info,
  std::set<irep_idt> &function_pointer_contracts)
{
  // rewrite pointer_in_range calls
  dfcc_pointer_in_ranget pointer_in_range(library, message_handler);
  pointer_in_range.rewrite_calls(
    goto_program, first_instruction, last_instruction, cfg_info);

  // rewrite is_fresh calls
  dfcc_is_fresht is_fresh(library, message_handler);
  is_fresh.rewrite_calls(
    goto_program, first_instruction, last_instruction, cfg_info);

  // rewrite is_freeable/was_freed calls
  dfcc_is_freeablet is_freeable(library, message_handler);
  is_freeable.rewrite_calls(
    goto_program, first_instruction, last_instruction, cfg_info);

  // rewrite obeys_contract calls
  dfcc_obeys_contractt obeys_contract(library, message_handler);
  obeys_contract.rewrite_calls(
    goto_program,
    first_instruction,
    last_instruction,
    cfg_info,
    function_pointer_contracts);

  const namespacet ns(goto_model.symbol_table);
  auto &target = first_instruction;

  // excluding the last
  while(target != last_instruction)
  {
    if(target->is_decl())
    {
      instrument_decl(function_id, target, goto_program, cfg_info);
    }
    if(target->is_dead())
    {
      instrument_dead(function_id, target, goto_program, cfg_info);
    }
    else if(target->is_assign())
    {
      instrument_assign(function_id, target, goto_program, cfg_info);
    }
    else if(target->is_function_call())
    {
      instrument_function_call(function_id, target, goto_program, cfg_info);
    }
    else if(target->is_other())
    {
      instrument_other(function_id, target, goto_program, cfg_info);
    }
    // else do nothing
    target++;
  }
}

void dfcc_instrumentt::insert_add_decl_call(
  const irep_idt &function_id,
  const exprt &write_set,
  const symbol_exprt &symbol_expr,
  goto_programt::targett &target,
  goto_programt &goto_program)
{
  goto_programt payload;
  const auto &target_location = target->source_location();
  auto goto_instruction = payload.add(goto_programt::make_incomplete_goto(
    dfcc_utilst::make_null_check_expr(write_set), target_location));

  payload.add(goto_programt::make_function_call(
    library.write_set_add_decl_call(
      write_set, address_of_exprt(symbol_expr), target_location),
    target_location));

  auto label_instruction =
    payload.add(goto_programt::make_skip(target_location));
  goto_instruction->complete_goto(label_instruction);

  insert_before_swap_and_advance(goto_program, target, payload);
}

/// \details
/// ```c
/// DECL x;
/// ----
/// insert_add_decl_call(...);
/// ```
void dfcc_instrumentt::instrument_decl(
  const irep_idt &function_id,
  goto_programt::targett &target,
  goto_programt &goto_program,
  dfcc_cfg_infot &cfg_info)
{
  if(!cfg_info.must_track_decl_or_dead(target))
    return;
  const auto &decl_symbol = target->decl_symbol();
  auto &write_set = cfg_info.get_write_set(target);

  target++;
  insert_add_decl_call(
    function_id, write_set, decl_symbol, target, goto_program);
  target--;
}

void dfcc_instrumentt::insert_record_dead_call(
  const irep_idt &function_id,
  const exprt &write_set,
  const symbol_exprt &symbol_expr,
  goto_programt::targett &target,
  goto_programt &goto_program)
{
  goto_programt payload;
  const auto &target_location = target->source_location();
  auto goto_instruction = payload.add(goto_programt::make_incomplete_goto(
    dfcc_utilst::make_null_check_expr(write_set), target_location));

  payload.add(goto_programt::make_function_call(
    library.write_set_record_dead_call(
      write_set, address_of_exprt(symbol_expr), target_location),
    target_location));

  auto label_instruction =
    payload.add(goto_programt::make_skip(target_location));

  goto_instruction->complete_goto(label_instruction);

  insert_before_swap_and_advance(goto_program, target, payload);
}

/// \details
/// ```c
/// insert_record_dead_call(...);
/// ----
/// DEAD x;
/// ```
void dfcc_instrumentt::instrument_dead(
  const irep_idt &function_id,
  goto_programt::targett &target,
  goto_programt &goto_program,
  dfcc_cfg_infot &cfg_info)
{
  if(!cfg_info.must_track_decl_or_dead(target))
    return;

  const auto &decl_symbol = target->dead_symbol();
  auto &write_set = cfg_info.get_write_set(target);
  insert_record_dead_call(
    function_id, write_set, decl_symbol, target, goto_program);
}

void dfcc_instrumentt::instrument_lhs(
  const irep_idt &function_id,
  goto_programt::targett &target,
  const exprt &lhs,
  goto_programt &goto_program,
  dfcc_cfg_infot &cfg_info)
{
  const irep_idt &mode =
    dfcc_utilst::get_function_symbol(goto_model.symbol_table, function_id).mode;

  goto_programt payload;

  const auto &lhs_source_location = target->source_location();
  auto &write_set = cfg_info.get_write_set(target);
  auto goto_instruction = payload.add(goto_programt::make_incomplete_goto(
    dfcc_utilst::make_null_check_expr(write_set), lhs_source_location));

  source_locationt check_source_location(target->source_location());
  check_source_location.set_property_class("assigns");
  check_source_location.set_comment(
    "Check that " + from_expr_using_mode(ns, mode, lhs) + " is assignable");

  if(cfg_info.must_check_lhs(target))
  {
    // ```
    // IF !write_set GOTO skip_target;
    // DECL check_assign: bool;
    // CALL check_assign = check_assignment(write_set, &lhs, sizeof(lhs));
    // ASSERT(check_assign);
    // DEAD check_assign;
    // skip_target: SKIP;
    // ----
    // ASSIGN lhs := rhs;
    // ```

    const auto check_var = dfcc_utilst::create_symbol(
      goto_model.symbol_table,
      bool_typet(),
      function_id,
      "__check_lhs_assignment",
      lhs_source_location);

    payload.add(goto_programt::make_decl(check_var, lhs_source_location));

    payload.add(goto_programt::make_function_call(
      library.write_set_check_assignment_call(
        check_var,
        write_set,
        typecast_exprt::conditional_cast(
          address_of_exprt(lhs), pointer_type(empty_typet{})),
        dfcc_utilst::make_sizeof_expr(lhs, ns),
        lhs_source_location),
      lhs_source_location));

    payload.add(
      goto_programt::make_assertion(check_var, check_source_location));
    payload.add(goto_programt::make_dead(check_var, check_source_location));
  }
  else
  {
    // ```
    // IF !write_set GOTO skip_target;
    // ASSERT(true);
    // skip_target: SKIP;
    // ----
    // ASSIGN lhs := rhs;
    // ```
    payload.add(
      goto_programt::make_assertion(true_exprt(), check_source_location));
  }

  auto label_instruction =
    payload.add(goto_programt::make_skip(lhs_source_location));
  goto_instruction->complete_goto(label_instruction);

  insert_before_swap_and_advance(goto_program, target, payload);
}

/// Checks if lhs is the `dead_object`, and if the rhs
/// is an `if_exprt(nondet, ptr, dead_object)` expression.
/// Returns `ptr` if the pattern was matched, nullptr otherwise.
std::optional<exprt>
dfcc_instrumentt::is_dead_object_update(const exprt &lhs, const exprt &rhs)
{
  if(
    lhs.id() == ID_symbol &&
    to_symbol_expr(lhs).get_identifier() == CPROVER_PREFIX "dead_object" &&
    rhs.id() == ID_if)
  {
    // only handle `if_exprt(nondet, ptr, dead_object)`
    auto &if_expr = to_if_expr(rhs);
    if(
      if_expr.cond().id() == ID_side_effect &&
      to_side_effect_expr(if_expr.cond()).get_statement() == ID_nondet &&
      if_expr.false_case() == lhs)
    {
      return if_expr.true_case();
    }
  }

  return {};
}

void dfcc_instrumentt::instrument_assign(
  const irep_idt &function_id,
  goto_programt::targett &target,
  goto_programt &goto_program,
  dfcc_cfg_infot &cfg_info)
{
  const auto &lhs = target->assign_lhs();
  const auto &rhs = target->assign_rhs();
  const auto &target_location = target->source_location();
  auto &write_set = cfg_info.get_write_set(target);

  // check the lhs
  instrument_lhs(function_id, target, lhs, goto_program, cfg_info);

  // handle dead_object updates (created by __builtin_alloca for instance)
  // Remark: we do not really need to track this deallocation since the default
  // CBMC checks are already able to detect writes to DEAD objects
  const auto dead_ptr = is_dead_object_update(lhs, rhs);
  if(dead_ptr.has_value())
  {
    // ```
    // ASSIGN dead_object := if_exprt(nondet, ptr, dead_object);
    // ----
    // IF !write_set GOTO skip_target;
    // CALL record_deallocated(write_set, ptr);
    // skip_target: SKIP;
    // ```

    // step over the instruction
    target++;

    goto_programt payload;

    auto goto_instruction = payload.add(goto_programt::make_incomplete_goto(
      dfcc_utilst::make_null_check_expr(write_set), target_location));

    payload.add(goto_programt::make_function_call(
      library.write_set_record_dead_call(
        write_set, dead_ptr.value(), target_location),
      target_location));

    auto label_instruction =
      payload.add(goto_programt::make_skip(target_location));
    goto_instruction->complete_goto(label_instruction);

    insert_before_swap_and_advance(goto_program, target, payload);

    // step back
    target--;
  }

  // is the rhs expression a side_effect("allocate") expression ?
  if(rhs.id() == ID_side_effect && rhs.get(ID_statement) == ID_allocate)
  {
    // ```
    // CALL lhs := side_effect(statement = ID_allocate, args = {size, clear});
    // ----
    // IF !write_set GOTO skip_target;
    // CALL add_allocated(write_set, lhs);
    // skip_target: SKIP;
    // ```

    // step over the instruction
    target++;

    goto_programt payload;
    auto goto_instruction = payload.add(goto_programt::make_incomplete_goto(
      dfcc_utilst::make_null_check_expr(write_set), target_location));

    payload.add(goto_programt::make_function_call(
      library.write_set_add_allocated_call(write_set, lhs, target_location),
      target_location));

    auto label_instruction =
      payload.add(goto_programt::make_skip(target_location));
    goto_instruction->complete_goto(label_instruction);

    insert_before_swap_and_advance(goto_program, target, payload);

    // step back
    target--;
  }
}

void dfcc_instrumentt::instrument_fptr_call_instruction_dynamic_lookup(
  const exprt &write_set,
  goto_programt::targett &target,
  goto_programt &goto_program)
{
  // Insert a dynamic lookup in __instrumented_functions_map
  // and pass the write set only to functions that are known to be able
  // to accept it.
  //
  // ```
  // IF __instrumented_functions_map[__CPROVER_POINTER_OBJECT(fptr)] != 1
  //   GOTO no_inst;
  // CALL [lhs] = fptr(params, write_set);
  // GOTO end;
  // no_inst:
  // CALL [lhs] = fptr(params);
  // end:
  // SKIP;
  // ---
  // SKIP // [lhs] = fptr(params) turned into SKIP
  // ```
  const auto &target_location = target->source_location();
  const auto &callf = target->call_function();
  auto object_id = pointer_object(
    (callf.id() == ID_dereference) ? to_dereference_expr(callf).pointer()
                                   : address_of_exprt(callf));
  auto index_expr = index_exprt(
    library.get_instrumented_functions_map_symbol().symbol_expr(), object_id);
  auto cond = notequal_exprt(index_expr, from_integer(1, unsigned_char_type()));
  goto_programt payload;
  auto goto_no_inst =
    payload.add(goto_programt::make_incomplete_goto(cond, target_location));
  code_function_callt call_inst(
    target->call_lhs(), target->call_function(), target->call_arguments());
  call_inst.arguments().push_back(write_set);
  payload.add(goto_programt::make_function_call(call_inst, target_location));
  auto goto_end_inst = payload.add(
    goto_programt::make_incomplete_goto(true_exprt(), target_location));
  auto no_inst_label = payload.add(goto_programt::make_skip(target_location));
  goto_no_inst->complete_goto(no_inst_label);
  code_function_callt call_no_inst(
    target->call_lhs(), target->call_function(), target->call_arguments());
  payload.add(goto_programt::make_function_call(call_no_inst, target_location));
  auto end_label = payload.add(goto_programt::make_skip(target_location));
  goto_end_inst->complete_goto(end_label);
  // erase the original call
  target->turn_into_skip();
  insert_before_swap_and_advance(goto_program, target, payload);
}

void dfcc_instrumentt::instrument_call_instruction(
  const exprt &write_set,
  goto_programt::targett &target,
  goto_programt &goto_program)
{
  if(target->is_function_call())
  {
    if(target->call_function().id() == ID_symbol)
    {
      // this is a function call
      if(!do_not_instrument(
           to_symbol_expr(target->call_function()).get_identifier()))
      {
        // pass write set argument only if function is known to be instrumented
        target->call_arguments().push_back(write_set);
      }
    }
    else
    {
      // This is a function pointer call. We insert a dynamic lookup into the
      // map of instrumented functions to determine if the target function is
      // able to accept a write set parameter.
      instrument_fptr_call_instruction_dynamic_lookup(
        write_set, target, goto_program);
    }
  }
}

void dfcc_instrumentt::instrument_deallocate_call(
  const irep_idt &function_id,
  const exprt &write_set,
  goto_programt::targett &target,
  goto_programt &goto_program)
{
  INVARIANT(target->is_function_call(), "target must be a function call");
  INVARIANT(
    target->call_function().id() == ID_symbol &&
      (id2string(to_symbol_expr(target->call_function()).get_identifier()) ==
       CPROVER_PREFIX "deallocate"),
    "target must be a call to" CPROVER_PREFIX "deallocate");

  auto &target_location = target->source_location();
  // ```
  // IF !write_set GOTO skip_target;
  // DECL check_deallocate: bool;
  // CALL check_deallocate := check_deallocate(write_set, ptr);
  // ASSERT(check_deallocate);
  // DEAD check_deallocate;
  // CALL record_deallocated(write_set, lhs);
  // skip_target: SKIP;
  // ----
  // CALL __CPROVER_deallocate(ptr);
  // ```
  goto_programt payload;

  auto goto_instruction = payload.add(goto_programt::make_incomplete_goto(
    dfcc_utilst::make_null_check_expr(write_set), target_location));

  const auto check_var = dfcc_utilst::create_symbol(
    goto_model.symbol_table,
    bool_typet(),
    function_id,
    "__check_deallocate",
    target_location);

  payload.add(goto_programt::make_decl(check_var, target_location));

  const auto &ptr = target->call_arguments().at(0);

  payload.add(goto_programt::make_function_call(
    library.write_set_check_deallocate_call(
      check_var, write_set, ptr, target_location),
    target_location));

  // add property class on assertion source_location
  const irep_idt &mode =
    dfcc_utilst::get_function_symbol(goto_model.symbol_table, function_id).mode;
  source_locationt check_location(target_location);
  check_location.set_property_class("frees");
  std::string comment =
    "Check that " + from_expr_using_mode(ns, mode, ptr) + " is freeable";
  check_location.set_comment(comment);

  payload.add(goto_programt::make_assertion(check_var, check_location));
  payload.add(goto_programt::make_dead(check_var, target_location));

  payload.add(goto_programt::make_function_call(
    library.write_set_record_deallocated_call(write_set, ptr, target_location),
    target_location));

  auto label_instruction =
    payload.add(goto_programt::make_skip(target_location));
  goto_instruction->complete_goto(label_instruction);

  insert_before_swap_and_advance(goto_program, target, payload);
}

void dfcc_instrumentt::instrument_function_call(
  const irep_idt &function_id,
  goto_programt::targett &target,
  goto_programt &goto_program,
  dfcc_cfg_infot &cfg_info)
{
  INVARIANT(
    target->is_function_call(),
    "the target must be a function call instruction");

  auto &write_set = cfg_info.get_write_set(target);

  // Instrument the lhs if any.
  if(target->call_lhs().is_not_nil())
  {
    instrument_lhs(
      function_id, target, target->call_lhs(), goto_program, cfg_info);
  }

  const auto &call_function = target->call_function();
  if(
    call_function.id() == ID_symbol &&
    (id2string(to_symbol_expr(call_function).get_identifier()) == CPROVER_PREFIX
     "deallocate"))
  {
    instrument_deallocate_call(function_id, write_set, target, goto_program);
  }
  else
  {
    // instrument as a normal function/function pointer call
    instrument_call_instruction(write_set, target, goto_program);
  }
}

void dfcc_instrumentt::instrument_other(
  const irep_idt &function_id,
  goto_programt::targett &target,
  goto_programt &goto_program,
  dfcc_cfg_infot &cfg_info)
{
  const auto &target_location = target->source_location();
  auto &statement = target->get_other().get_statement();
  auto &write_set = cfg_info.get_write_set(target);
  const irep_idt &mode =
    dfcc_utilst::get_function_symbol(goto_model.symbol_table, function_id).mode;

  if(statement == ID_array_set || statement == ID_array_copy)
  {
    const bool is_array_set = statement == ID_array_set;
    // ```
    // IF !write_set GOTO skip_target;
    // DECL check_array_set: bool;
    // CALL check_array_set = check_array_set(write_set, dest);
    // ASSERT(check_array_set);
    // DEAD check_array_set;
    // skip_target: SKIP;
    // ----
    // OTHER {statement = array_set, args = {dest, value}};
    // ```
    goto_programt payload;

    auto goto_instruction = payload.add(goto_programt::make_incomplete_goto(
      dfcc_utilst::make_null_check_expr(write_set), target_location));

    const auto check_var = dfcc_utilst::create_symbol(
      goto_model.symbol_table,
      bool_typet(),
      function_id,
      is_array_set ? "__check_array_set" : "__check_array_copy",
      target_location);

    payload.add(goto_programt::make_decl(check_var, target_location));

    const auto &dest = target->get_other().operands().at(0);

    payload.add(goto_programt::make_function_call(
      is_array_set ? library.write_set_check_array_set_call(
                       check_var, write_set, dest, target_location)
                   : library.write_set_check_array_copy_call(
                       check_var, write_set, dest, target_location),
      target_location));

    // add property class on assertion source_location
    source_locationt check_location(target_location);
    check_location.set_property_class("assigns");

    std::string fun_name = is_array_set ? "array_set" : "array_copy";

    std::string comment = "Check that " + fun_name + "(" +
                          from_expr_using_mode(ns, mode, dest) +
                          ", ...) is allowed by the assigns clause";
    check_location.set_comment(comment);

    payload.add(goto_programt::make_assertion(check_var, check_location));
    payload.add(goto_programt::make_dead(check_var, target_location));

    auto label_instruction =
      payload.add(goto_programt::make_skip(target_location));
    goto_instruction->complete_goto(label_instruction);

    insert_before_swap_and_advance(goto_program, target, payload);
  }
  else if(statement == ID_array_replace)
  {
    // ```
    // IF !write_set GOTO skip_target;
    // DECL check_array_replace: bool;
    // CALL check_array_replace = check_array_replace(write_set, dest);
    // ASSERT(check_array_replace);
    // DEAD check_array_replace;
    // skip_target: SKIP;
    // ----
    // OTHER {statement = array_replace, args = {dest, src}};
    // ```
    goto_programt payload;

    auto goto_instruction = payload.add(goto_programt::make_incomplete_goto(
      dfcc_utilst::make_null_check_expr(write_set), target_location));

    const auto check_var = dfcc_utilst::create_symbol(
      goto_model.symbol_table,
      bool_typet(),
      function_id,
      "__check_array_replace",
      target_location);

    payload.add(goto_programt::make_decl(check_var, target_location));

    const auto &dest = target->get_other().operands().at(0);
    const auto &src = target->get_other().operands().at(1);

    payload.add(goto_programt::make_function_call(
      library.write_set_check_array_replace_call(
        check_var, write_set, dest, src, target_location),
      target_location));

    // add property class on assertion source_location
    source_locationt check_location(target_location);
    check_location.set_property_class("assigns");
    std::string comment = "Check that array_replace(" +
                          from_expr_using_mode(ns, mode, dest) +
                          ", ...) is allowed by the assigns clause";
    check_location.set_comment(comment);

    payload.add(goto_programt::make_assertion(check_var, check_location));
    payload.add(goto_programt::make_dead(check_var, target_location));

    auto label_instruction =
      payload.add(goto_programt::make_skip(target_location));
    goto_instruction->complete_goto(label_instruction);

    insert_before_swap_and_advance(goto_program, target, payload);
  }
  else if(statement == ID_havoc_object)
  {
    // insert before instruction
    // ```
    // IF !write_set GOTO skip_target;
    // DECL check_havoc_object: bool;
    // CALL check_havoc_object = check_havoc_object(write_set, ptr);
    // ASSERT(check_havoc_object);
    // DEAD check_havoc_object;
    // ```
    goto_programt payload;

    auto goto_instruction = payload.add(goto_programt::make_incomplete_goto(
      dfcc_utilst::make_null_check_expr(write_set), target_location));

    const auto check_var = dfcc_utilst::create_symbol(
      goto_model.symbol_table,
      bool_typet(),
      function_id,
      "__check_havoc_object",
      target_location);

    payload.add(goto_programt::make_decl(check_var, target_location));

    const auto &ptr = target->get_other().operands().at(0);

    payload.add(goto_programt::make_function_call(
      library.write_set_check_havoc_object_call(
        check_var, write_set, ptr, target_location),
      target_location));

    // add property class on assertion source_location
    source_locationt check_location(target_location);
    check_location.set_property_class("assigns");
    std::string comment = "Check that havoc_object(" +
                          from_expr_using_mode(ns, mode, ptr) +
                          ") is allowed by the assigns clause";
    check_location.set_comment(comment);

    payload.add(goto_programt::make_assertion(check_var, check_location));
    payload.add(goto_programt::make_dead(check_var, target_location));

    auto label_instruction =
      payload.add(goto_programt::make_skip(target_location));
    goto_instruction->complete_goto(label_instruction);

    insert_before_swap_and_advance(goto_program, target, payload);
  }
  else if(statement == ID_expression)
  {
    // When in Rome do like the Romans (cf src/pointer_analysis/value_set.cpp)
    // can be ignored, we don't expect side effects here
  }
  else
  {
    // Other cases not presently handled
    // * ID_array_equal
    // * ID_decl         track new symbol ?
    // * ID_cpp_delete
    // * ID_printf       track as side effect on stdout ?
    // * code_inputt     track as nondet ?
    // * code_outputt    track as side effect on stdout ?
    // * ID_nondet       track as nondet ?
    // * ID_asm          track as side effect depending on the instruction ?
    // * ID_user_specified_predicate
    //                   should be pure ?
    // * ID_user_specified_parameter_predicates
    //                   should be pure ?
    // * ID_user_specified_return_predicates
    //                   should be pure ?
    // * ID_fence
    //                   bail out ?
    log.warning().source_location = target_location;
    log.warning() << "dfcc_instrument::instrument_other: statement type '"
                  << statement << "' is not supported, analysis may be unsound"
                  << messaget::eom;
  }
}

void dfcc_instrumentt::apply_loop_contracts(
  const irep_idt &function_id,
  goto_functiont &goto_function,
  dfcc_cfg_infot &cfg_info,
  const loop_contract_configt &loop_contract_config,
  const std::set<symbol_exprt> &local_statics,
  std::set<irep_idt> &function_pointer_contracts)
{
  PRECONDITION(loop_contract_config.apply_loop_contracts);
  cfg_info.get_loops_toposorted();

  std::list<std::string> to_unwind;

  // Apply loop contract transformations in topological order
  for(const auto &loop_id : cfg_info.get_loops_toposorted())
  {
    const auto &loop = cfg_info.get_loop_info(loop_id);
    if(loop.invariant.is_nil() && loop.decreases.empty())
    {
      // skip loops that do not have contracts
      log.warning() << "loop " << function_id << "." << loop.cbmc_loop_id
                    << " does not have a contract, skipping instrumentation"
                    << messaget::eom;
      continue;
    }

    instrument_loop(
      loop_id,
      function_id,
      goto_function,
      cfg_info,
      local_statics,
      function_pointer_contracts);
    to_unwind.push_back(
      id2string(function_id) + "." + std::to_string(loop.cbmc_loop_id) + ":2");
  }

  // If required, unwind all transformed loops to yield base and step cases
  if(loop_contract_config.unwind_transformed_loops)
  {
    unwindsett unwindset{goto_model};
    unwindset.parse_unwindset(to_unwind, log.get_message_handler());
    goto_unwindt goto_unwind;
    goto_unwind(goto_model, unwindset, goto_unwindt::unwind_strategyt::ASSUME);
  }

  remove_skip(goto_function.body);
}
