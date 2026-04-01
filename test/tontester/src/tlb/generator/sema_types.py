"""Resolved IR types produced by semantic analysis.

All identifiers are resolved, nat vs type expressions are separated into
distinct union types, and deserialization plans are computed.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum, auto

from .ast_nodes import CompareOp


class SemaError(Exception):
    pass


# ── Parameter kinds ───────────────────────────────────────────────────


class ParamKind(Enum):
    NAT = auto()
    TYPE = auto()


@dataclass(frozen=True)
class ParamDef:
    name: str
    kind: ParamKind


# ── Nat expressions (evaluate to natural numbers at runtime) ──────────


@dataclass
class NatLiteral:
    value: int


@dataclass
class NatParamRef:
    param: ParamDef


@dataclass
class NatFieldValue:
    """Runtime nat value of an explicit field (e.g. len:(#< n) → len is a nat)."""

    field: ResolvedField


@dataclass
class NatAdd:
    left: ResolvedNatExpr
    right: ResolvedNatExpr


@dataclass
class NatSub:
    """Only produced by constraint solving (e.g. m = n - l)."""

    left: ResolvedNatExpr
    right: ResolvedNatExpr


@dataclass
class NatMul:
    left: ResolvedNatExpr
    right: ResolvedNatExpr


@dataclass
class NatGetBit:
    value: ResolvedNatExpr
    bit: ResolvedNatExpr


@dataclass
class NatTypeArg:
    """Value of the nat type argument at the given result param position.

    Known from the caller at deserialization time.
    """

    position: int


type ResolvedNatExpr = (
    NatLiteral | NatParamRef | NatFieldValue | NatAdd | NatSub | NatMul | NatGetBit | NatTypeArg
)


# ── Type expressions (describe serialization format) ──────────────────


@dataclass
class TypeParamRef:
    param: ParamDef


@dataclass
class TypeApply:
    type: ResolvedType
    arguments: list[ResolvedExpr]


@dataclass
class TupleType:
    """n * T — tuple of n values of type T."""

    count: ResolvedNatExpr
    element: ResolvedTypeExpr


@dataclass
class CondType:
    """selector ? inner — conditional field."""

    selector: ResolvedNatExpr
    inner: ResolvedTypeExpr


@dataclass
class CellRefType:
    """^T — value in a referenced cell."""

    inner: ResolvedTypeExpr


@dataclass
class AnonymousRecordType:
    """[field1:T1 field2:T2 ...] — inline anonymous record."""

    type: ResolvedType


type ResolvedTypeExpr = (
    TypeParamRef | TypeApply | TupleType | CondType | CellRefType | AnonymousRecordType
)

type ResolvedExpr = ResolvedNatExpr | ResolvedTypeExpr


# ── Fields ────────────────────────────────────────────────────────────


@dataclass(eq=False)
class ResolvedField:
    name: str | None
    type_expr: ResolvedTypeExpr
    is_nat_valued: bool = False


# ── Deserialization steps ─────────────────────────────────────────────


@dataclass
class InferenceStep:
    """One step through an inference-capable generic type param."""

    type: ResolvedType
    param_idx: int


@dataclass
class OutputExtraction:
    """Full algorithm to extract a nat value from a deserialized field."""

    source_field: ResolvedField
    chain: list[InferenceStep]
    final_output_idx: int


@dataclass
class ReadField:
    field: ResolvedField


@dataclass
class BindParam:
    """Bind a Type param from a type argument (always identity assignment)."""

    target_param: ParamDef
    position: int  # type arg position


@dataclass
class BindOutputParam:
    target_param: ParamDef
    extraction: OutputExtraction


@dataclass
class SolveConstraint:
    target_param: ParamDef
    value: ResolvedNatExpr


@dataclass
class CheckConstraint:
    op: CompareOp
    left: ResolvedNatExpr
    right: ResolvedNatExpr


type DeserStep = ReadField | BindParam | BindOutputParam | SolveConstraint | CheckConstraint


# ── Constructor ───────────────────────────────────────────────────────


@dataclass
class ResolvedConstraint:
    """A resolved constraint expression (from { expr } in the source).

    For equality constraints with a negated variable, negated_param is set
    and the constraint can be solved during deserialization.
    """

    op: CompareOp
    left: ResolvedNatExpr
    right: ResolvedNatExpr
    negated_param: ParamDef | None = None


type FieldOrConstraint = ResolvedField | ResolvedConstraint


@dataclass(eq=False)
class ResolvedConstructor:
    name: str | None
    tag_bits: str
    tag_len: int
    parent_type: ResolvedType
    is_special: bool
    params: list[ParamDef]
    fields: list[ResolvedField]
    # Resolved expression for each non-output result param position.
    # Used to bind implicit params from type args at deser start.
    # Key = result param position, value = resolved expr referencing implicit params.
    result_param_exprs: dict[int, ResolvedExpr] = field(default_factory=dict)
    # Fields and constraints in source order (for left-to-right deser processing)
    source_order: list[FieldOrConstraint] = field(default_factory=list)
    deser_steps: list[DeserStep] = field(default_factory=list)
    output_values: list[ResolvedNatExpr] = field(default_factory=list)


# ── Constructor match tree ─────────────────────────────────────────────


@dataclass
class MatchTag:
    """Check/skip a known bit prefix (e.g. a 32-bit CRC tag)."""

    bits: str
    child: MatchTree


@dataclass
class MatchBit:
    """Branch on the next serialized bit."""

    zero: MatchTree
    one: MatchTree


@dataclass
class MatchConstraint:
    """Branch on a type-arg constraint."""

    condition: CheckConstraint
    if_true: MatchTree
    if_false: MatchTree


@dataclass
class MatchConstructor:
    """Leaf — matched a constructor."""

    constructor: ResolvedConstructor


@dataclass
class MatchFail:
    """No constructor matches — deserialization error."""

    pass


type MatchTree = MatchTag | MatchBit | MatchConstraint | MatchConstructor | MatchFail


# ── Inference info ────────────────────────────────────────────────────


@dataclass
class InferenceInfo:
    """Per-Type-param: can output params propagate through this param?"""

    is_capable: bool = False
    constructor_field: dict[ResolvedConstructor, ResolvedField] = field(default_factory=dict)


# ── Type ──────────────────────────────────────────────────────────────


@dataclass(eq=False)
class ResolvedType:
    name: str
    type_idx: int
    arity: int = 0
    param_kinds: list[ParamKind] = field(default_factory=list)
    constructors: list[ResolvedConstructor] = field(default_factory=list)

    produces_nat: bool = False
    is_builtin: bool = False
    is_special: bool = False  # all constructors have ! prefix (exotic cell type)
    is_enum: bool = False
    is_typedef: bool = False
    typedef_target: ResolvedTypeExpr | None = None

    match_tree: MatchTree | None = None
    output_param_positions: list[int] = field(default_factory=list)
    inference: list[InferenceInfo] = field(default_factory=list)
