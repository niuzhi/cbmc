/*******************************************************************\

Module: ANSI-C Language Type Checking

Author: Daniel Kroening, kroening@kroening.com

\*******************************************************************/

/// \file
/// ANSI-C Language Type Checking

#include "anonymous_member.h"

#include <util/namespace.h>
#include <util/std_expr.h>

static member_exprt make_member_expr(
  const exprt &struct_union,
  const struct_union_typet::componentt &component,
  const namespacet &ns)
{
  member_exprt result(
    struct_union, component.get_name(), component.type());

  if(struct_union.get_bool(ID_C_lvalue))
    result.set(ID_C_lvalue, true);

  // todo: should to typedef chains properly
  const auto &tag_type = to_struct_or_union_tag_type(struct_union.type());
  if(
    ns.follow_tag(tag_type).get_bool(ID_C_constant) ||
    struct_union.type().get_bool(ID_C_constant))
  {
    result.type().set(ID_C_constant, true);
  }

  return result;
}

exprt get_component_rec(
  const exprt &struct_union,
  const irep_idt &component_name,
  const namespacet &ns)
{
  const auto &tag_type = to_struct_or_union_tag_type(struct_union.type());

  for(const auto &comp : ns.follow_tag(tag_type).components())
  {
    const typet &type = comp.type();

    if(comp.get_name()==component_name)
    {
      return std::move(make_member_expr(struct_union, comp, ns));
    }
    else if(
      comp.get_anonymous() &&
      (type.id() == ID_struct_tag || type.id() == ID_union_tag))
    {
      const member_exprt tmp = make_member_expr(struct_union, comp, ns);
      exprt result=get_component_rec(tmp, component_name, ns);
      if(result.is_not_nil())
        return result;
    }
  }

  return nil_exprt();
}

bool has_component_rec(
  const typet &type,
  const irep_idt &component_name,
  const namespacet &ns)
{
  const auto &tag_type = to_struct_or_union_tag_type(type);

  for(const auto &comp : ns.follow_tag(tag_type).components())
  {
    if(comp.get_name()==component_name)
    {
      return true;
    }
    else if(
      comp.get_anonymous() &&
      (comp.type().id() == ID_struct_tag || comp.type().id() == ID_union_tag))
    {
      if(has_component_rec(comp.type(), component_name, ns))
        return true;
    }
  }

  return false;
}
