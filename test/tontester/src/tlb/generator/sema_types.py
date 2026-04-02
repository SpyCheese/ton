"""Resolved IR types produced by semantic analysis.

All identifiers are resolved, nat vs type expressions are separated into
distinct union types, and deserialization plans are computed.
"""

from dataclasses import dataclass, field
from enum import Enum, auto

from .ast_nodes import CompareOp


class SemaError(Exception):
    pass


class ParamKind(Enum):
    NAT = auto()
    TYPE = auto()


@dataclass(frozen=True)
class NatParamDef:
    name: str


@dataclass(frozen=True)
class TypeParamDef:
    name: str
    type_level_param: TypeLevelParam


type ParamDef = TypeParamDef | NatParamDef


@dataclass(frozen=True)
class TypeVarBinding:
    """Sentinel for a type variable name binding (the bare X in class foo[X])."""

    pass


@dataclass(frozen=True)
class TypeLevelParam:
    """Sentinel for a type-level parameter position.

    Used as a scope binding key so that NatTypeArg(position) can resolve
    to the correct Python variable name. One per position on the type.
    For TYPE params, type_var is a binding key for the bare type variable name.
    """

    position: int
    kind: ParamKind
    is_output: bool
    type_var: TypeVarBinding = field(default_factory=TypeVarBinding)


@dataclass
class NatLiteral:
    value: int


@dataclass
class NatParamRef:
    param: NatParamDef


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

    param: TypeLevelParam


type ResolvedNatExpr = (
    NatLiteral | NatParamRef | NatFieldValue | NatAdd | NatSub | NatMul | NatGetBit | NatTypeArg
)


@dataclass
class TypeParamRef:
    param: TypeParamDef


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
class CellRefType:
    """^T — value in a referenced cell."""

    inner: ResolvedTypeExpr


@dataclass
class AnonymousRecordType:
    """[field1:T1 field2:T2 ...] — inline anonymous record."""

    type: ResolvedType


type ResolvedTypeExpr = TypeParamRef | TypeApply | TupleType | CellRefType | AnonymousRecordType

type ResolvedExpr = ResolvedNatExpr | ResolvedTypeExpr


def _nat_references_params(expr: ResolvedNatExpr) -> bool:
    """Check whether a nat expression references any constructor parameters."""
    match expr:
        case NatParamRef() | NatFieldValue():
            return True
        case (
            NatAdd(left=left, right=right)
            | NatSub(left=left, right=right)
            | NatMul(left=left, right=right)
        ):
            return _nat_references_params(left) or _nat_references_params(right)
        case NatGetBit(value=value, bit=bit):
            return _nat_references_params(value) or _nat_references_params(bit)
        case _:
            return False


def references_type_params(expr: ResolvedTypeExpr) -> bool:
    """Check whether a type expression references any constructor parameters (type or nat)."""
    match expr:
        case TypeParamRef():
            return True
        case TypeApply(arguments=arguments):
            for a in arguments:
                if isinstance(
                    a, TypeParamRef | TypeApply | TupleType | CellRefType | AnonymousRecordType
                ):
                    if references_type_params(a):
                        return True
                elif _nat_references_params(a):
                    return True
            return False
        case CellRefType(inner=inner):
            return references_type_params(inner)
        case TupleType(element=element):
            return references_type_params(element)
        case AnonymousRecordType():
            return any(
                references_type_params(f.type_expr) for f in expr.type.constructors[0].fields
            )


@dataclass(eq=False)
class ResolvedField:
    name: str | None
    type_expr: ResolvedTypeExpr
    is_nat_valued: bool = False
    condition: ResolvedNatExpr | None = None


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

    target_param: TypeParamDef
    position: int  # type arg position


@dataclass
class BindOutputParam:
    target_param: NatParamDef
    extraction: OutputExtraction


@dataclass
class SolveConstraint:
    target_param: NatParamDef
    value: ResolvedNatExpr


@dataclass
class CheckConstraint:
    op: CompareOp
    left: ResolvedNatExpr
    right: ResolvedNatExpr


type DeserStep = ReadField | BindParam | BindOutputParam | SolveConstraint | CheckConstraint


@dataclass
class ResolvedConstraint:
    """A resolved constraint expression (from { expr } in the source).

    For equality constraints with a negated variable, negated_param is set
    and the constraint can be solved during deserialization.
    """

    op: CompareOp
    left: ResolvedNatExpr
    right: ResolvedNatExpr
    negated_param: NatParamDef | None = None


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
    result_param_exprs: dict[int, ResolvedExpr] = field(default_factory=dict)
    source_order: list[FieldOrConstraint] = field(default_factory=list)
    deser_steps: list[DeserStep] = field(default_factory=list)
    output_values: list[ResolvedNatExpr] = field(default_factory=list)


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


@dataclass
class InferenceInfo:
    """Per-Type-param: can output params propagate through this param?"""

    is_capable: bool = False
    constructor_field: dict[ResolvedConstructor, ResolvedField] = field(default_factory=dict)


@dataclass(eq=False)
class ResolvedType:
    name: str
    type_idx: int
    arity: int = 0
    type_level_params: list[TypeLevelParam] = field(default_factory=list)
    constructors: list[ResolvedConstructor] = field(default_factory=list)

    produces_nat: bool = False
    is_builtin: bool = False
    is_special: bool = False  # all constructors have ! prefix (exotic cell type)
    is_enum: bool = False
    is_typedef: bool = False
    typedef_target: ResolvedTypeExpr | None = None

    match_tree: MatchTree | None = None
    inference: list[InferenceInfo] = field(default_factory=list)
