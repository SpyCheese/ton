"""Deserialization plan generation and inference capability classification."""

from __future__ import annotations

from .ast_nodes import CompareOp
from .sema_types import (
    AnonymousRecordType,
    BindOutputParam,
    BindParam,
    CellRefType,
    CheckConstraint,
    CondType,
    DeserStep,
    InferenceInfo,
    InferenceStep,
    NatAdd,
    NatFieldValue,
    NatGetBit,
    NatLiteral,
    NatMul,
    NatParamRef,
    NatSub,
    NatTypeArg,
    OutputExtraction,
    ParamDef,
    ParamKind,
    ReadField,
    ResolvedConstraint,
    ResolvedConstructor,
    ResolvedExpr,
    ResolvedField,
    ResolvedNatExpr,
    ResolvedType,
    ResolvedTypeExpr,
    SemaError,
    SolveConstraint,
    TupleType,
    TypeApply,
    TypeParamRef,
)

# ── Inference capability classification ──────────────────────────────


def classify_inference(resolved_type: ResolvedType) -> list[InferenceInfo]:
    """For each Type parameter, check if output params can propagate through it.

    A Type param is inference-capable if EVERY constructor has an explicit field
    whose type is directly TypeParamRef to the corresponding implicit param.
    """
    result: list[InferenceInfo] = []

    for i, kind in enumerate(resolved_type.param_kinds):
        if kind != ParamKind.TYPE:
            continue

        info = InferenceInfo()
        if not resolved_type.constructors:
            result.append(info)
            continue

        all_have = True
        for constructor in resolved_type.constructors:
            param = _param_for_type_position(constructor, i)
            if param is None:
                all_have = False
                break
            field = _field_exposing_param(constructor, param)
            if field is None:
                all_have = False
                break
            info.constructor_field[constructor] = field

        info.is_capable = all_have
        result.append(info)

    return result


def _param_for_type_position(constructor: ResolvedConstructor, position: int) -> ParamDef | None:
    """Find the implicit Type param at the given type-parameter position."""
    type_idx = 0
    for i, kind in enumerate(constructor.parent_type.param_kinds):
        if i == position:
            # Find the type_idx-th Type param in constructor
            count = 0
            for p in constructor.params:
                if p.kind == ParamKind.TYPE:
                    if count == type_idx:
                        return p
                    count += 1
            return None
        if kind == ParamKind.TYPE:
            type_idx += 1
    return None


def _field_exposing_param(
    constructor: ResolvedConstructor, param: ParamDef
) -> ResolvedField | None:
    """Find a field whose type is directly TypeParamRef(param)."""
    for field in constructor.fields:
        if isinstance(field.type_expr, TypeParamRef) and field.type_expr.param is param:
            return field
    return None


# ── Deserialization plan generation ──────────────────────────────────


def build_deser_plan(constructor: ResolvedConstructor) -> list[DeserStep]:
    """Generate ordered deserialization steps for a constructor.

    First binds all params from type args (entry constraints), then processes
    fields and constraints in source order (left-to-right).
    """
    steps: list[DeserStep] = []
    known_params: set[ParamDef] = set()

    # Bind params from type args (non-output result param positions)
    _emit_entry_bindings(constructor, known_params, steps)

    # Process fields and constraints in source order
    for item in constructor.source_order:
        if isinstance(item, ResolvedField):
            steps.append(ReadField(field=item))
            _emit_bindings(item, constructor, known_params, steps)
        else:
            _process_constraint(item, known_params, steps)

    # Verify all params are bound
    unbound = set(constructor.params) - known_params
    if unbound:
        names = ", ".join(sorted(p.name for p in unbound))
        raise SemaError(
            f"constructor '{constructor.name}' of type '{constructor.parent_type.name}': "
            + f"parameter(s) {names} cannot be computed during deserialization"
        )

    constructor.deser_steps = steps
    _validate_deser_plan(constructor)
    return steps


def _emit_entry_bindings(
    constructor: ResolvedConstructor,
    known_params: set[ParamDef],
    steps: list[DeserStep],
) -> None:
    """Bind implicit params from type arguments at the start of deserialization.

    For each non-output result param position:
    - If the expression is a bare param ref (NatParamRef/TypeParamRef): bind directly
    - If it's a constant (NatLiteral): emit a CheckConstraint
    - If it's a complex expression: solve for the unknown param(s)
    """
    for position, expr in constructor.result_param_exprs.items():
        if isinstance(expr, NatParamRef):
            # Trivial: param = type_arg
            steps.append(
                SolveConstraint(
                    target_param=expr.param,
                    value=NatTypeArg(position=position),
                )
            )
            known_params.add(expr.param)
        elif isinstance(expr, TypeParamRef):
            # Type param: bind directly
            steps.append(BindParam(target_param=expr.param, position=position))
            known_params.add(expr.param)
        elif isinstance(expr, NatLiteral):
            # Constant: check that type arg matches
            steps.append(
                CheckConstraint(
                    op=CompareOp.EQ,
                    left=NatTypeArg(position=position),
                    right=expr,
                )
            )
        elif isinstance(expr, TypeApply):
            raise SemaError(
                f"constructor '{constructor.name}' of type '{constructor.parent_type.name}': "
                + f"result param at position {position} is a type application; "
                + "Type-kinded result params must be bare type parameter references"
            )
        else:
            # Complex nat expression like (n + 1): solve for unknown params
            _solve_entry_expr(expr, position, constructor, known_params, steps)


def _solve_entry_expr(
    expr: ResolvedExpr,
    position: int,
    constructor: ResolvedConstructor,
    known_params: set[ParamDef],
    steps: list[DeserStep],
) -> None:
    """Solve a complex result param expression for unknown params.

    E.g. position 0 has expr (n + 1): solve NatTypeArg(0) = n + 1 → n = NatTypeArg(0) - 1.
    """
    if not _is_resolved_nat(expr):
        raise SemaError(
            f"constructor '{constructor.name}': non-nat expression at result position {position}"
        )
    nat_expr = _as_nat(expr)

    target = _find_unknown_nat_param(nat_expr, known_params)
    if target is None:
        # No unknown — emit as check
        steps.append(
            CheckConstraint(
                op=CompareOp.EQ,
                left=NatTypeArg(position=position),
                right=nat_expr,
            )
        )
        return

    solved = _isolate_param(nat_expr, NatTypeArg(position=position), target)
    if solved is not None:
        steps.append(SolveConstraint(target_param=target, value=solved))
        known_params.add(target)
    else:
        raise SemaError(
            f"constructor '{constructor.name}' of type '{constructor.parent_type.name}': "
            + f"cannot solve result param expression at position {position} for '{target.name}'"
        )


def _find_unknown_nat_param(expr: ResolvedNatExpr, known: set[ParamDef]) -> ParamDef | None:
    """Find a NatParamRef in a nat expression that isn't in the known set."""
    if isinstance(expr, NatParamRef) and expr.param not in known:
        return expr.param
    if isinstance(expr, NatAdd | NatSub | NatMul):
        return _find_unknown_nat_param(expr.left, known) or _find_unknown_nat_param(
            expr.right, known
        )
    if isinstance(expr, NatGetBit):
        return _find_unknown_nat_param(expr.value, known) or _find_unknown_nat_param(
            expr.bit, known
        )
    return None


def _is_resolved_nat(expr: ResolvedExpr) -> bool:
    return isinstance(
        expr,
        NatLiteral
        | NatParamRef
        | NatFieldValue
        | NatAdd
        | NatSub
        | NatMul
        | NatGetBit
        | NatTypeArg,
    )


def _as_nat(expr: ResolvedExpr) -> ResolvedNatExpr:
    assert _is_resolved_nat(expr)
    assert not isinstance(
        expr, TypeParamRef | TypeApply | TupleType | CondType | CellRefType | AnonymousRecordType
    )
    return expr


def _emit_bindings(
    field: ResolvedField,
    constructor: ResolvedConstructor,
    known_params: set[ParamDef],
    steps: list[DeserStep],
) -> None:
    """After reading a field, emit BindOutputParam for extractable output params."""
    _scan_for_outputs(field, field.type_expr, constructor, known_params, steps, [])


def _scan_for_outputs(
    source_field: ResolvedField,
    type_expr: ResolvedTypeExpr,
    constructor: ResolvedConstructor,
    known_params: set[ParamDef],
    steps: list[DeserStep],
    chain: list[InferenceStep],
) -> None:
    """Recursively scan a type expression for output param bindings."""
    if not isinstance(type_expr, TypeApply):
        return

    applied_type = type_expr.type

    # Direct output bindings: if the type has output params and an argument
    # is NatParamRef to an unknown param, bind it
    if applied_type.output_param_positions:
        for arg_idx, arg in enumerate(type_expr.arguments):
            if not isinstance(arg, NatParamRef):
                continue
            if arg.param in known_params:
                continue

            output_idx = _output_index_for_arg(applied_type, arg_idx)
            if output_idx is not None:
                extraction = OutputExtraction(
                    source_field=source_field,
                    chain=list(chain),
                    final_output_idx=output_idx,
                )
                steps.append(BindOutputParam(target_param=arg.param, extraction=extraction))
                known_params.add(arg.param)

    # Inference through generic type params: if an argument is itself a TypeApply
    # and the param position is inference-capable, recurse into it
    for arg_idx, arg in enumerate(type_expr.arguments):
        if not isinstance(arg, TypeApply):
            continue
        inf_idx = _inference_index_for_param(applied_type, arg_idx)
        if inf_idx is not None and inf_idx < len(applied_type.inference):
            if applied_type.inference[inf_idx].is_capable:
                new_chain = chain + [InferenceStep(type=applied_type, param_idx=arg_idx)]
                _scan_for_outputs(source_field, arg, constructor, known_params, steps, new_chain)


def _output_index_for_arg(resolved_type: ResolvedType, arg_position: int) -> int | None:
    """Map an argument position to an output param index (if it's an output)."""
    for idx, pos in enumerate(resolved_type.output_param_positions):
        if pos == arg_position:
            return idx
    return None


def _inference_index_for_param(resolved_type: ResolvedType, arg_position: int) -> int | None:
    """Map an argument position to an inference info index (for Type params only)."""
    if arg_position >= len(resolved_type.param_kinds):
        return None
    if resolved_type.param_kinds[arg_position] != ParamKind.TYPE:
        return None
    # Count how many Type params come before this position
    count = 0
    for i in range(arg_position):
        if resolved_type.param_kinds[i] == ParamKind.TYPE:
            count += 1
    return count


# ── Constraint processing ────────────────────────────────────────────


def _process_constraint(
    constraint: ResolvedConstraint,
    known_params: set[ParamDef],
    steps: list[DeserStep],
) -> None:
    """Process a resolved constraint: either solve for a ~variable or emit a check."""
    if constraint.negated_param is not None and constraint.negated_param not in known_params:
        # Solve for the negated variable
        solved = _solve_for_negated(constraint)
        if solved is not None:
            steps.append(SolveConstraint(target_param=constraint.negated_param, value=solved))
            known_params.add(constraint.negated_param)
            return

    # Emit a check constraint
    steps.append(CheckConstraint(op=constraint.op, left=constraint.left, right=constraint.right))


def _solve_for_negated(constraint: ResolvedConstraint) -> ResolvedNatExpr | None:
    """Solve an equality constraint for its negated variable.

    Handles patterns like:
        n = (~m) + l      → m = n - l
        n = l + (~m)      → m = n - l
        n = (~m) + l + 1  → m = n - l - 1
        (~m) = n          → m = n
    """
    assert constraint.negated_param is not None

    if constraint.op != constraint.op.EQ:
        return None  # Can only solve equalities

    target = constraint.negated_param

    # Determine which side has the target and solve
    left_has = _expr_references_param(constraint.left, target)
    right_has = _expr_references_param(constraint.right, target)

    if left_has and not right_has:
        return _isolate_param(constraint.left, constraint.right, target)
    elif right_has and not left_has:
        return _isolate_param(constraint.right, constraint.left, target)

    return None


def _expr_references_param(expr: ResolvedNatExpr, param: ParamDef) -> bool:
    """Check if a resolved nat expression references a specific param."""
    if isinstance(expr, NatParamRef):
        return expr.param is param
    if isinstance(expr, NatAdd | NatSub | NatMul):
        return _expr_references_param(expr.left, param) or _expr_references_param(expr.right, param)
    if isinstance(expr, NatGetBit):
        return _expr_references_param(expr.value, param) or _expr_references_param(expr.bit, param)
    return False


def _isolate_param(
    side_with_target: ResolvedNatExpr,
    other_side: ResolvedNatExpr,
    target: ParamDef,
) -> ResolvedNatExpr | None:
    """Isolate target from: side_with_target = other_side.

    Returns an expression for target's value.
    """
    # Direct: (~m) = expr → m = expr
    if isinstance(side_with_target, NatParamRef) and side_with_target.param is target:
        return other_side

    # (~m) + rest = expr → m = expr - rest
    if isinstance(side_with_target, NatAdd):
        if _expr_references_param(side_with_target.left, target):
            rest = side_with_target.right
            return _isolate_param(
                side_with_target.left, NatSub(left=other_side, right=rest), target
            )
        if _expr_references_param(side_with_target.right, target):
            rest = side_with_target.left
            return _isolate_param(
                side_with_target.right, NatSub(left=other_side, right=rest), target
            )

    return None


# ── Deser plan validation (assert — catches bugs, not user errors) ────


def _validate_deser_plan(constructor: ResolvedConstructor) -> None:
    """Assert that every expression in the deser plan only references
    values that are known at the point of evaluation."""
    known_params: set[ParamDef] = set()
    known_fields: set[int] = set()  # field ids

    for step in constructor.deser_steps:
        if isinstance(step, SolveConstraint):
            _assert_nat_deps_met(step.value, known_params, known_fields, constructor)
            known_params.add(step.target_param)
        elif isinstance(step, BindParam):
            known_params.add(step.target_param)
        elif isinstance(step, CheckConstraint):
            _assert_nat_deps_met(step.left, known_params, known_fields, constructor)
            _assert_nat_deps_met(step.right, known_params, known_fields, constructor)
        elif isinstance(step, ReadField):
            known_fields.add(id(step.field))
        else:
            # BindOutputParam — source field must have been read
            assert id(step.extraction.source_field) in known_fields, (
                f"BindOutputParam references unread field '{step.extraction.source_field.name}'"
            )
            known_params.add(step.target_param)

    # All output values must be computable from known params
    for ov in constructor.output_values:
        _assert_nat_deps_met(ov, known_params, known_fields, constructor)


def _assert_nat_deps_met(
    expr: ResolvedNatExpr,
    known_params: set[ParamDef],
    known_fields: set[int],
    constructor: ResolvedConstructor,
) -> None:
    """Assert all param/field references in a nat expression are known."""
    if isinstance(expr, NatParamRef):
        assert expr.param in known_params, (
            f"constructor '{constructor.name}': expression references unbound param '{expr.param.name}'"
        )
    elif isinstance(expr, NatFieldValue):
        assert id(expr.field) in known_fields, (
            f"constructor '{constructor.name}': expression references unread field '{expr.field.name}'"
        )
    elif isinstance(expr, NatAdd | NatSub | NatMul):
        _assert_nat_deps_met(expr.left, known_params, known_fields, constructor)
        _assert_nat_deps_met(expr.right, known_params, known_fields, constructor)
    elif isinstance(expr, NatGetBit):
        _assert_nat_deps_met(expr.value, known_params, known_fields, constructor)
        _assert_nat_deps_met(expr.bit, known_params, known_fields, constructor)
    # NatLiteral, NatTypeArg — no deps
