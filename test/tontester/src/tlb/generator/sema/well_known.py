"""Detection of well-known TL-B type patterns."""

from .types import NatLiteral, ParamKind, ResolvedType, TypeApply, TypeParamRef, WellKnownType


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
    elif _is_bit(rt):
        rt.well_known = WellKnownType.BIT
    elif _is_hashmap_e(rt):
        rt.well_known = WellKnownType.HASHMAP_E
    elif _is_hashmap(rt):
        rt.well_known = WellKnownType.HASHMAP
    elif _is_var_uinteger(rt):
        rt.well_known = WellKnownType.VAR_UINTEGER
    elif _is_var_integer(rt):
        rt.well_known = WellKnownType.VAR_INTEGER


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


def _is_bit(rt: ResolvedType) -> bool:
    """bit$_ (## 1) = Bit;"""
    if rt.name != "Bit":
        return False
    if not rt.is_typedef:
        return False
    target = rt.typedef_target
    if not isinstance(target, TypeApply):
        return False
    if target.type.name != "##" or len(target.arguments) != 1:
        return False
    arg = target.arguments[0]
    return isinstance(arg, NatLiteral) and arg.value == 1


def _is_hashmap_e(rt: ResolvedType) -> bool:
    """hme_empty$0 {n:#} {X:Type} = HashmapE n X;
    hme_root$1 {n:#} {X:Type} root:^(Hashmap n X) = HashmapE n X;"""
    if rt.name != "HashmapE":
        return False
    if rt.arity != 2 or len(rt.constructors) != 2:
        return False
    tlps = rt.type_level_params
    if len(tlps) != 2 or tlps[0].kind != ParamKind.NAT or tlps[1].kind != ParamKind.TYPE:
        return False
    cons = sorted(rt.constructors, key=lambda c: c.tag_bits)
    empty, root = cons[0], cons[1]
    if empty.name != "hme_empty" or root.name != "hme_root":
        return False
    if empty.tag_bits != "0" or root.tag_bits != "1":
        return False
    if len(empty.fields) != 0 or len(root.fields) != 1:
        return False
    return True


def _is_hashmap(rt: ResolvedType) -> bool:
    """hm_edge#_ {n:#} {X:Type} {l:#} {m:#} label:(HmLabel ~l n) {n = (~m) + l} node:(HashmapNode m X) = Hashmap n X;"""
    if rt.name != "Hashmap":
        return False
    if rt.arity != 2 or len(rt.constructors) != 1:
        return False
    tlps = rt.type_level_params
    if len(tlps) != 2 or tlps[0].kind != ParamKind.NAT or tlps[1].kind != ParamKind.TYPE:
        return False
    return rt.constructors[0].name == "hm_edge"


def _is_var_uinteger(rt: ResolvedType) -> bool:
    """var_uint$_ {n:#} len:(#< n) value:(uint (len * 8)) = VarUInteger n;"""
    if rt.name != "VarUInteger":
        return False
    if rt.arity != 1 or len(rt.constructors) != 1:
        return False
    if len(rt.type_level_params) != 1 or rt.type_level_params[0].kind != ParamKind.NAT:
        return False
    return rt.constructors[0].name == "var_uint"


def _is_var_integer(rt: ResolvedType) -> bool:
    """var_int$_ {n:#} len:(#< n) value:(int (len * 8)) = VarInteger n;"""
    if rt.name != "VarInteger":
        return False
    if rt.arity != 1 or len(rt.constructors) != 1:
        return False
    if len(rt.type_level_params) != 1 or rt.type_level_params[0].kind != ParamKind.NAT:
        return False
    return rt.constructors[0].name == "var_int"
