"""Type registration and expression resolution.

Phase 1: Register all type names and determine arities/param kinds.
Phase 2: Resolve each constructor's fields and expressions.
"""

from __future__ import annotations

from .ast_nodes import (
    Add,
    Apply,
    CellRef,
    Compare,
    CompareOp,
    Conditional,
    Constraint,
    Constructor,
    ExplicitField,
    FieldDef,
    GetBit,
    Identifier,
    ImplicitParam,
    InlineRecord,
    IntConst,
    Multiply,
    NegatedIdentifier,
    Schema,
    TypeExpr,
)
from .sema_builtins import BUILTIN_TYPES_NUM, create_builtin_registry
from .sema_types import (
    AnonymousRecordType,
    CellRefType,
    CondType,
    NatAdd,
    NatFieldValue,
    NatGetBit,
    NatLiteral,
    NatMul,
    NatParamRef,
    NatSub,
    ParamDef,
    ParamKind,
    ResolvedConstraint,
    ResolvedConstructor,
    ResolvedExpr,
    ResolvedField,
    ResolvedNatExpr,
    ResolvedType,
    ResolvedTypeExpr,
    SemaError,
    TupleType,
    TypeApply,
    TypeParamRef,
)

# ── Type Registry ────────────────────────────────────────────────────


class TypeRegistry:
    _types: dict[str, ResolvedType]
    _next_idx: int

    def __init__(self) -> None:
        self._types = create_builtin_registry()
        self._next_idx = BUILTIN_TYPES_NUM + len(self._types)

    def lookup(self, name: str) -> ResolvedType | None:
        return self._types.get(name)

    def register(self, name: str) -> ResolvedType:
        if name in self._types:
            return self._types[name]
        t = ResolvedType(name=name, type_idx=self._next_idx)
        self._next_idx += 1
        self._types[name] = t
        return t

    def all_user_types(self) -> list[ResolvedType]:
        return [t for t in self._types.values() if not t.is_builtin]


# ── Phase 1: Register types and determine arities ────────────────────


def register_types(schema: Schema, registry: TypeRegistry) -> None:
    """First pass: group constructors by result type, determine arity and param kinds."""
    by_type: dict[str, list[Constructor]] = {}
    for c in schema.constructors:
        by_type.setdefault(c.result_type, []).append(c)

    for type_name, constructors in by_type.items():
        resolved_type = registry.register(type_name)
        if resolved_type.is_builtin:
            raise SemaError(f"cannot redefine built-in type '{type_name}'")

        # Determine arity from result params count (must be consistent)
        arity = len(constructors[0].result_params)
        for c in constructors[1:]:
            if len(c.result_params) != arity:
                raise SemaError(
                    f"inconsistent arity for type '{type_name}': "
                    + f"constructor '{c.name}' has {len(c.result_params)} params, "
                    + f"expected {arity}"
                )

        # Determine param kinds from implicit param declarations
        param_kinds = _determine_param_kinds(type_name, constructors, arity)
        resolved_type.arity = arity
        resolved_type.param_kinds = param_kinds

        # Determine which result positions are output (~) and check consistency
        output_positions: list[int] = []
        for i in range(arity):
            negated_count = sum(1 for c in constructors if c.result_params[i].negated)
            if negated_count == len(constructors):
                output_positions.append(i)
            elif negated_count > 0:
                raise SemaError(
                    f"type '{type_name}': result param at position {i} is negated (~) "
                    + f"in {negated_count} of {len(constructors)} constructors; "
                    + "must be all or none"
                )
        resolved_type.output_param_positions = output_positions

        # Check for duplicate constructor names within the same type
        seen_names: set[str] = set()
        for c in constructors:
            if c.name is not None:
                if c.name in seen_names:
                    raise SemaError(f"type '{type_name}': duplicate constructor name '{c.name}'")
                seen_names.add(c.name)

        # Check that all constructors agree on is_special (! prefix)
        special_count = sum(1 for c in constructors if c.is_special)
        if 0 < special_count < len(constructors):
            raise SemaError(
                f"type '{type_name}': {special_count} of {len(constructors)} constructors "
                + "are marked special (!); must be all or none"
            )


def _determine_param_kinds(
    type_name: str, constructors: list[Constructor], arity: int
) -> list[ParamKind]:
    """Determine the ParamKind for each type parameter position."""
    kinds: list[ParamKind | None] = [None] * arity

    for c in constructors:
        # Build a map from implicit param name to its kind
        implicit_kinds: dict[str, ParamKind] = {}
        for f in c.fields:
            if isinstance(f, ImplicitParam):
                implicit_kinds[f.name] = ParamKind.TYPE if f.is_type else ParamKind.NAT

        for i, rp in enumerate(c.result_params):
            if rp.negated:
                # Output params are always nat
                kind = ParamKind.NAT
            elif isinstance(rp.expr, Identifier) and rp.expr.name in implicit_kinds:
                kind = implicit_kinds[rp.expr.name]
            elif isinstance(rp.expr, IntConst):
                kind = ParamKind.NAT
            else:
                # Complex expression or unknown — assume nat
                kind = ParamKind.NAT

            if kinds[i] is None:
                kinds[i] = kind
            elif kinds[i] != kind:
                raise SemaError(f"inconsistent param kind at position {i} of type '{type_name}'")

    return [k or ParamKind.NAT for k in kinds]


# ── Phase 2: Resolve constructors ────────────────────────────────────


def resolve_constructors(schema: Schema, registry: TypeRegistry) -> None:
    """Second pass: resolve all constructor fields and expressions."""
    for c in schema.constructors:
        resolved = _resolve_constructor(c, registry)
        resolved.parent_type.constructors.append(resolved)


def check_type_arities(user_types: list[ResolvedType]) -> None:
    """Check type application arities after all types are fully registered."""
    for t in user_types:
        for c in t.constructors:
            for f in c.fields:
                _check_type_apply_arity(f.type_expr)
            for expr in c.result_param_exprs.values():
                if isinstance(
                    expr, TypeApply | TupleType | CondType | CellRefType | AnonymousRecordType
                ):
                    _check_type_apply_arity(expr)


# ── Scope for name resolution ────────────────────────────────────────

# A scope entry is either a param or a resolved field
type ScopeEntry = tuple[ParamDef, None] | tuple[None, ResolvedField]


class _Scope:
    entries: dict[str, ScopeEntry]

    def __init__(self) -> None:
        self.entries = {}

    def add_param(self, param: ParamDef) -> None:
        if param.name in self.entries:
            raise SemaError(f"duplicate parameter name '{param.name}'")
        self.entries[param.name] = (param, None)

    def add_field(self, name: str, field: ResolvedField) -> None:
        if name in self.entries:
            raise SemaError(f"duplicate field name '{name}'")
        self.entries[name] = (None, field)

    def lookup(self, name: str) -> ScopeEntry | None:
        return self.entries.get(name)

    def copy(self) -> _Scope:
        s = _Scope()
        s.entries = dict(self.entries)
        return s


# ── Constructor resolution ───────────────────────────────────────────


def _resolve_constructor(c: Constructor, registry: TypeRegistry) -> ResolvedConstructor:
    parent_type = registry.lookup(c.result_type)
    assert parent_type is not None

    scope = _Scope()

    # Process implicit params
    params: list[ParamDef] = []
    for f in c.fields:
        if isinstance(f, ImplicitParam):
            kind = ParamKind.TYPE if f.is_type else ParamKind.NAT
            p = ParamDef(name=f.name, kind=kind)
            params.append(p)
            scope.add_param(p)

    # Process explicit fields and constraints, preserving source order
    fields: list[ResolvedField] = []
    source_order: list[ResolvedField | ResolvedConstraint] = []
    for f in c.fields:
        if isinstance(f, ExplicitField):
            resolved_field = _resolve_field(f, scope, registry)
            fields.append(resolved_field)
            source_order.append(resolved_field)
            if f.name is not None:
                scope.add_field(f.name, resolved_field)
        elif isinstance(f, Constraint):
            rc = _resolve_constraint(f, scope, registry, params)
            source_order.append(rc)

    # Resolve output values from output (~) result params
    # These CAN reference fields (e.g. hml_long's ~n where n is an explicit field)
    output_values: list[ResolvedNatExpr] = []
    for i in parent_type.output_param_positions:
        rp = c.result_params[i]
        expr = _resolve_nat_expr(rp.expr, scope, registry)
        output_values.append(expr)

    # Resolve non-output result param expressions (used to bind params from type args)
    result_param_exprs: dict[int, ResolvedExpr] = {}
    for i, rp in enumerate(c.result_params):
        if i not in parent_type.output_param_positions:
            expr = _resolve_expr(rp.expr, scope, registry)
            # Non-output result params can only reference params, not fields
            _check_no_field_refs_in_expr(
                expr, f"result param at position {i} of constructor '{c.name}'"
            )
            result_param_exprs[i] = expr

    tag_bits = c.tag.bits
    tag_len = len(tag_bits)

    return ResolvedConstructor(
        name=c.name,
        tag_bits=tag_bits,
        tag_len=tag_len,
        parent_type=parent_type,
        is_special=c.is_special,
        params=params,
        fields=fields,
        result_param_exprs=result_param_exprs,
        source_order=source_order,
        output_values=output_values,
    )


def _resolve_field(f: ExplicitField, scope: _Scope, registry: TypeRegistry) -> ResolvedField:
    type_expr = _resolve_type_expr(f.type_expr, scope, registry)
    is_nat = _is_nat_valued(type_expr)
    return ResolvedField(name=f.name, type_expr=type_expr, is_nat_valued=is_nat)


def _resolve_constraint(
    c: Constraint, scope: _Scope, registry: TypeRegistry, params: list[ParamDef]
) -> ResolvedConstraint:
    """Resolve a constraint expression, detecting negated (output) variables."""
    from .ast_nodes import Compare as AstCompare

    expr = c.expr
    if not isinstance(expr, AstCompare):
        raise SemaError("constraint must be a comparison expression")

    left = _resolve_nat_expr(expr.left, scope, registry)
    right = _resolve_nat_expr(expr.right, scope, registry)

    # Detect negated param (output variable to solve for)
    negated = _find_negated_in_ast(expr, params)

    op = _convert_compare_op(expr.op)
    return ResolvedConstraint(op=op, left=left, right=right, negated_param=negated)


def _find_negated_in_ast(expr: Compare, params: list[ParamDef]) -> ParamDef | None:
    """Find a ~param in a comparison's AST subtree. Errors if there are multiple."""
    from .ast_nodes import Add as AstAdd
    from .ast_nodes import Multiply as AstMul
    from .ast_nodes import NegatedIdentifier as AstNeg

    found: list[ParamDef] = []

    def scan(e: TypeExpr) -> None:
        if isinstance(e, AstNeg):
            for p in params:
                if p.name == e.name:
                    found.append(p)
                    return
        if isinstance(e, AstAdd | AstMul):
            scan(e.left)
            scan(e.right)

    scan(expr.left)
    scan(expr.right)

    if len(found) > 1:
        names = ", ".join(p.name for p in found)
        raise SemaError(f"constraint has multiple negated params: {names}; only one allowed")

    return found[0] if found else None


def _convert_compare_op(op: CompareOp) -> CompareOp:
    return op


def _is_nat_valued(expr: ResolvedTypeExpr) -> bool:
    if isinstance(expr, TypeApply):
        return expr.type.produces_nat
    return False


# ── Validation helpers ────────────────────────────────────────────────


def _check_no_field_refs_in_expr(expr: ResolvedExpr, context: str) -> None:
    """Verify a resolved expression doesn't reference explicit fields."""
    if isinstance(expr, NatFieldValue):
        raise SemaError(f"{context}: cannot reference field '{expr.field.name}' in result param")
    if isinstance(expr, NatAdd | NatMul | NatSub):
        _check_no_field_refs_in_expr(expr.left, context)
        _check_no_field_refs_in_expr(expr.right, context)
    if isinstance(expr, NatGetBit):
        _check_no_field_refs_in_expr(expr.value, context)
        _check_no_field_refs_in_expr(expr.bit, context)


def _check_type_apply_arity(expr: ResolvedTypeExpr) -> None:
    """Check that TypeApply nodes have the correct number of arguments."""
    if isinstance(expr, TypeApply):
        expected = expr.type.arity
        actual = len(expr.arguments)
        if actual != expected:
            raise SemaError(f"type '{expr.type.name}' expects {expected} arguments, got {actual}")
        for arg in expr.arguments:
            if isinstance(
                arg, TypeApply | TupleType | CondType | CellRefType | AnonymousRecordType
            ):
                _check_type_apply_arity(arg)
    elif isinstance(expr, TupleType):
        _check_type_apply_arity(expr.element)
    elif isinstance(expr, CondType):
        _check_type_apply_arity(expr.inner)
    elif isinstance(expr, CellRefType):
        _check_type_apply_arity(expr.inner)
    elif isinstance(expr, AnonymousRecordType):
        for c in expr.type.constructors:
            for f in c.fields:
                _check_type_apply_arity(f.type_expr)


# ── Expression resolution ────────────────────────────────────────────


def _resolve_expr(expr: TypeExpr, scope: _Scope, registry: TypeRegistry) -> ResolvedExpr:
    match expr:
        case IntConst(value=v):
            return NatLiteral(v)

        case Identifier(name=name):
            return _resolve_identifier(name, scope, registry)

        case NegatedIdentifier(name=name):
            return _resolve_negated_identifier(name, scope)

        case Apply(function=func, arguments=args):
            resolved_func = _resolve_expr(func, scope, registry)
            if not isinstance(resolved_func, TypeApply):
                raise SemaError("cannot apply non-type expression")
            resolved_args: list[ResolvedExpr] = []
            for a in args:
                resolved_args.append(_resolve_expr(a, scope, registry))
            return TypeApply(
                type=resolved_func.type,
                arguments=resolved_func.arguments + resolved_args,
            )

        case Add(left=left, right=right):
            return NatAdd(
                left=_resolve_nat_expr(left, scope, registry),
                right=_resolve_nat_expr(right, scope, registry),
            )

        case Multiply(left=left, right=right):
            rl = _resolve_expr(left, scope, registry)
            rr = _resolve_expr(right, scope, registry)
            if _is_resolved_nat(rl) and _is_resolved_nat(rr):
                return NatMul(left=_as_nat(rl), right=_as_nat(rr))
            elif _is_resolved_nat(rl) and not _is_resolved_nat(rr):
                return TupleType(count=_as_nat(rl), element=_as_type(rr))
            else:
                raise SemaError("left operand of '*' must be a nat expression")

        case GetBit(value=value, bit=bit):
            return NatGetBit(
                value=_resolve_nat_expr(value, scope, registry),
                bit=_resolve_nat_expr(bit, scope, registry),
            )

        case Conditional(selector=sel, type_expr=te):
            return CondType(
                selector=_resolve_nat_expr(sel, scope, registry),
                inner=_resolve_type_expr(te, scope, registry),
            )

        case CellRef(inner=inner):
            return CellRefType(inner=_resolve_type_expr(inner, scope, registry))

        case Compare():
            raise SemaError("comparison expressions are only valid inside constraints")

        case InlineRecord(fields=field_defs):
            return _resolve_inline_record(field_defs, scope, registry)


def _resolve_identifier(name: str, scope: _Scope, registry: TypeRegistry) -> ResolvedExpr:
    entry = scope.lookup(name)
    if entry is not None:
        param, field = entry
        if param is not None:
            if param.kind == ParamKind.NAT:
                return NatParamRef(param)
            else:
                return TypeParamRef(param)
        assert field is not None
        if field.is_nat_valued:
            return NatFieldValue(field)
        else:
            raise SemaError(f"field '{name}' is not nat-valued and cannot be used in expressions")

    resolved_type = registry.lookup(name)
    if resolved_type is not None:
        return TypeApply(type=resolved_type, arguments=[])

    raise SemaError(f"undefined type or identifier '{name}'")


def _resolve_negated_identifier(name: str, scope: _Scope) -> NatParamRef:
    entry = scope.lookup(name)
    if entry is None:
        raise SemaError(f"undefined identifier '~{name}'")
    param, _ = entry
    if param is None:
        raise SemaError(f"'~{name}' must refer to an implicit parameter, not a field")
    if param.kind != ParamKind.NAT:
        raise SemaError(f"cannot negate Type parameter '{name}'")
    return NatParamRef(param)


def _resolve_nat_expr(expr: TypeExpr, scope: _Scope, registry: TypeRegistry) -> ResolvedNatExpr:
    resolved = _resolve_expr(expr, scope, registry)
    return _as_nat(resolved)


def _resolve_type_expr(expr: TypeExpr, scope: _Scope, registry: TypeRegistry) -> ResolvedTypeExpr:
    resolved = _resolve_expr(expr, scope, registry)
    return _as_type(resolved)


def _resolve_inline_record(
    field_defs: list[FieldDef], scope: _Scope, registry: TypeRegistry
) -> AnonymousRecordType:
    anon_type = ResolvedType(name="", type_idx=-1, is_builtin=False)
    fields: list[ResolvedField] = []
    params: list[ParamDef] = []

    inner_scope = scope.copy()

    for f in field_defs:
        if isinstance(f, ExplicitField):
            rf = _resolve_field(f, inner_scope, registry)
            fields.append(rf)
            if f.name is not None:
                inner_scope.add_field(f.name, rf)
        elif isinstance(f, ImplicitParam):
            kind = ParamKind.TYPE if f.is_type else ParamKind.NAT
            p = ParamDef(name=f.name, kind=kind)
            params.append(p)
            inner_scope.add_param(p)

    constructor = ResolvedConstructor(
        name=None,
        tag_bits="",
        tag_len=0,
        parent_type=anon_type,
        is_special=False,
        params=params,
        fields=fields,
    )
    anon_type.constructors.append(constructor)
    return AnonymousRecordType(type=anon_type)


# ── Helpers ───────────────────────────────────────────────────────────


def _is_resolved_nat(expr: ResolvedExpr) -> bool:
    return isinstance(expr, NatLiteral | NatParamRef | NatFieldValue | NatAdd | NatMul | NatGetBit)


def _as_nat(expr: ResolvedExpr) -> ResolvedNatExpr:
    if _is_resolved_nat(expr):
        assert not isinstance(
            expr,
            TypeParamRef | TypeApply | TupleType | CondType | CellRefType | AnonymousRecordType,
        )
        return expr
    raise SemaError("expected nat expression, got type expression")


def _as_type(expr: ResolvedExpr) -> ResolvedTypeExpr:
    if not _is_resolved_nat(expr):
        assert isinstance(
            expr,
            TypeParamRef | TypeApply | TupleType | CondType | CellRefType | AnonymousRecordType,
        )
        return expr
    raise SemaError("expected type expression, got nat expression")
