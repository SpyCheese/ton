"""StrategyBuilder: builds TypeStrategy instances for resolved type expressions."""

from ...sema.builtins import (
    Any_type,
    Bits_type,
    Int_type,
    Nat_type,
    NatLeq_type,
    NatLess_type,
    NatWidth_type,
    UInt_type,
)
from ...sema.types import (
    AnonymousRecordType,
    CellRefType,
    NatLiteral,
    ParamKind,
    ResolvedExpr,
    ResolvedNatExpr,
    ResolvedTypeExpr,
    TupleType,
    TypeApply,
    TypeParamDef,
    TypeParamRef,
    is_nat,
)
from ..context import PyContext
from ..name_scope import NameScope
from ..nat_expr import NatExpr
from ._base import TypeStrategy
from .bits import BitsStrategy
from .bounded_uint import BoundedUintStrategy
from .cell_ref import CellRefStrategy, GenericCellRefStrategy
from .int import IntStrategy
from .slice import SliceTypeStrategy
from .tuple import TupleStrategy
from .type_param import TypeParamStrategy
from .uint import UintStrategy
from .user_type import UserTypeStrategy


class StrategyBuilder:
    """Builds TypeStrategy instances for resolved type expressions.

    Holds the constructor-local context (scope, type param mappings)
    instead of threading them through every strategy_for call.
    Tracks which type params are referenced during building.
    """

    ctx: PyContext
    scope: NameScope
    used_type_params: set[TypeParamDef]

    def __init__(self, ctx: PyContext, scope: NameScope) -> None:
        self.ctx = ctx
        self.scope = scope
        self.used_type_params = set()

    @staticmethod
    def _to_nat(expr: ResolvedExpr) -> ResolvedNatExpr:
        assert is_nat(expr)
        return expr

    def _nat(self, expr: ResolvedExpr) -> NatExpr:
        """Create a NatExpr from a resolved expression."""
        return NatExpr(self._to_nat(expr), self.scope)

    def build(
        self, type_expr: ResolvedTypeExpr, *, inside_generic_arg: bool = False
    ) -> TypeStrategy:
        """Create a TypeStrategy for a resolved type expression.

        inside_generic_arg: when True, ^Type uses Ref[X] from runtime instead
        of generating a wrapper class, since wrappers are file-level concrete
        classes incompatible with the generic Ref[X] type system.
        """
        match type_expr:
            case TypeApply():
                return self._build_type_apply(type_expr)

            case TypeParamRef(param=param):
                ti_var = self.scope.lookup(param)
                type_var = self.scope.lookup_generic(param.type_level_param)
                self.used_type_params.add(param)
                return TypeParamStrategy(param, type_var, ti_var)

            case CellRefType(inner=inner_expr):
                is_concrete = not inner_expr.references_type_params
                inner = self.build(inner_expr, inside_generic_arg=inside_generic_arg)
                if is_concrete and not inside_generic_arg:
                    return CellRefStrategy(inner, self.ctx)
                return GenericCellRefStrategy(inner, self.ctx)

            case TupleType(count=count_expr, element=element_expr):
                count = NatExpr(count_expr, self.scope)
                element = self.build(element_expr, inside_generic_arg=inside_generic_arg)
                return TupleStrategy(count, element, self.ctx)

            case AnonymousRecordType(type=type):
                return UserTypeStrategy(self.ctx.scope.lookup(type))

    def _build_type_apply(self, type_expr: TypeApply) -> TypeStrategy:
        t = type_expr.type
        if t is UInt_type:
            assert len(type_expr.arguments) == 1
            return UintStrategy(self._nat(type_expr.arguments[0]), self.ctx)
        if t is Int_type:
            assert len(type_expr.arguments) == 1
            return IntStrategy(self._nat(type_expr.arguments[0]), self.ctx)
        if t is Bits_type:
            assert len(type_expr.arguments) == 1
            return BitsStrategy(self._nat(type_expr.arguments[0]), self.ctx)
        if t is Nat_type:
            return UintStrategy(NatExpr(NatLiteral(32), NameScope()), self.ctx)
        if t is NatWidth_type:
            assert len(type_expr.arguments) == 1
            return UintStrategy(self._nat(type_expr.arguments[0]), self.ctx)
        if t is NatLeq_type:
            assert len(type_expr.arguments) == 1
            return BoundedUintStrategy(self._nat(type_expr.arguments[0]), True, self.ctx)
        if t is NatLess_type:
            assert len(type_expr.arguments) == 1
            return BoundedUintStrategy(self._nat(type_expr.arguments[0]), False, self.ctx)
        if t.is_builtin and t.arity == 0:
            name = t.name
            if name.startswith("uint"):
                assert t.produces_nat
                return UintStrategy(NatExpr(NatLiteral(int(name[4:])), NameScope()), self.ctx)
            if name.startswith("int"):
                assert t.produces_nat
                return IntStrategy(NatExpr(NatLiteral(int(name[3:])), NameScope()), self.ctx)
            if name.startswith("bits"):
                return BitsStrategy(NatExpr(NatLiteral(int(name[4:])), NameScope()), self.ctx)
        if t is Any_type:
            return SliceTypeStrategy(self.ctx)
        if not t.is_builtin:
            return self._build_user_type(type_expr)

        assert False, f"unhandled builtin type: {t.name}"

    def _build_user_type(self, type_expr: TypeApply) -> UserTypeStrategy:
        """Build strategy for a user-defined type, handling all arg kinds."""
        t = type_expr.type
        ti_args: list[str] = []
        ti_args_self: list[str] = []
        type_var_args: list[str] = []

        for tlp, arg in zip(t.type_level_params, type_expr.arguments, strict=True):
            if tlp.is_output:
                continue
            param_kind = tlp.kind
            if param_kind == ParamKind.NAT:
                assert is_nat(arg)
                ti_args.append(NatExpr(arg, self.scope).local)
                ti_args_self.append(NatExpr(arg, self.scope).self_)
            else:
                assert isinstance(
                    arg, TypeParamRef | TypeApply | TupleType | CellRefType | AnonymousRecordType
                )
                arg_strategy = self.build(arg, inside_generic_arg=True)
                ti_args.append(arg_strategy.type_info_expr())
                ti_args_self.append(arg_strategy.type_info_expr_self())
                type_var_args.append(arg_strategy.py_type())

        return UserTypeStrategy(self.ctx.scope.lookup(t), ti_args, ti_args_self, type_var_args)
