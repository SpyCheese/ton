"""Detection of well-known TL-B type patterns."""

from .types import ParamKind, ResolvedType, TypeApply, TypeParamRef, WellKnownType


def classify_well_known(rt: ResolvedType) -> None:
    """Detect well-known type patterns and set rt.well_known."""
    if _is_maybe(rt):
        rt.well_known = WellKnownType.MAYBE
    elif _is_unit(rt):
        rt.well_known = WellKnownType.UNIT
    elif _is_bool(rt):
        rt.well_known = WellKnownType.BOOL
    elif _is_bool_true(rt):
        rt.well_known = WellKnownType.BOOL_TRUE
    elif _is_bool_false(rt):
        rt.well_known = WellKnownType.BOOL_FALSE
    elif _is_unary(rt):
        rt.well_known = WellKnownType.UNARY


def _is_maybe(rt: ResolvedType) -> bool:
    """nothing$0 {X:Type} = Maybe X; just$1 {X:Type} value:X = Maybe X;"""
    if rt.name != "Maybe":
        return False
    if rt.arity != 1 or len(rt.constructors) != 2:
        return False
    if len(rt.type_level_params) != 1 or rt.type_level_params[0].kind != ParamKind.TYPE:
        return False
    cons = sorted(rt.constructors, key=lambda c: c.tag_bits)
    nothing, just = cons[0], cons[1]
    if nothing.name != "nothing" or just.name != "just":
        return False
    if nothing.tag_bits != "0" or just.tag_bits != "1":
        return False
    if len(nothing.fields) != 0 or len(just.fields) != 1:
        return False
    if not isinstance(just.fields[0].type_expr, TypeParamRef):
        return False
    return True


def _no_params_no_fields(rt: ResolvedType) -> bool:
    return rt.arity == 0 and all(len(c.fields) == 0 and len(c.params) == 0 for c in rt.constructors)


def _is_unit(rt: ResolvedType) -> bool:
    """_ = Unit; (single constructor, no tag, no fields)"""
    if rt.name != "Unit":
        return False
    if len(rt.constructors) != 1:
        return False
    c = rt.constructors[0]
    return c.tag_len == 0 and _no_params_no_fields(rt)


def _is_bool(rt: ResolvedType) -> bool:
    """bool_false$0 = Bool; bool_true$1 = Bool;"""
    if rt.name != "Bool":
        return False
    if len(rt.constructors) != 2 or not _no_params_no_fields(rt):
        return False
    cons = sorted(rt.constructors, key=lambda c: c.tag_bits)
    return (
        cons[0].name == "bool_false"
        and cons[0].tag_bits == "0"
        and cons[1].name == "bool_true"
        and cons[1].tag_bits == "1"
    )


def _is_bool_true(rt: ResolvedType) -> bool:
    """true$1 = True; (or bool_true$1 = BoolTrue;)"""
    if rt.name not in ("True", "BoolTrue"):
        return False
    if len(rt.constructors) != 1 or not _no_params_no_fields(rt):
        return False
    return rt.constructors[0].tag_bits == "1"


def _is_bool_false(rt: ResolvedType) -> bool:
    """bool_false$0 = BoolFalse;"""
    if rt.name != "BoolFalse":
        return False
    if len(rt.constructors) != 1 or not _no_params_no_fields(rt):
        return False
    return rt.constructors[0].tag_bits == "0"


def _is_unary(rt: ResolvedType) -> bool:
    """unary_zero$0 = Unary ~0; unary_succ$1 {n:#} x:(Unary ~n) = Unary ~(n + 1);"""
    if rt.name != "Unary":
        return False
    if len(rt.constructors) != 2 or rt.arity != 1:
        return False
    tlp = rt.type_level_params[0]
    if tlp.kind != ParamKind.NAT or not tlp.is_output:
        return False
    cons = sorted(rt.constructors, key=lambda c: c.tag_bits)
    zero, succ = cons[0], cons[1]
    if zero.name != "unary_zero" or succ.name != "unary_succ":
        return False
    if zero.tag_bits != "0" or succ.tag_bits != "1":
        return False
    if len(zero.fields) != 0:
        return False
    if len(succ.fields) != 1 or len(succ.params) != 1:
        return False
    field_type = succ.fields[0].type_expr
    if not isinstance(field_type, TypeApply) or field_type.type is not rt:
        return False
    return True
