"""UserTypeStrategy: emit store/load for user-defined types."""

from typing import final, override

from ...sema.types import (
    AnonymousRecordType,
    CellRefType,
    NatAdd,
    NatFieldValue,
    NatGetBit,
    NatLiteral,
    NatMul,
    NatParamRef,
    NatSub,
    ParamKind,
    TupleType,
    TypeApply,
    TypeParamRef,
    is_nat,
)
from ..context import PyContext
from ..name_scope import NameScope
from ..nat_expr import NatExpr
from ..source_builder import SourceBuilder
from ._base import StrategyBuilderProtocol, TypeStrategy


@final
class UserTypeStrategy(TypeStrategy):
    """User-defined type, possibly generic. Self-contained — computes all
    rendering from the TypeApply and scope."""

    def __init__(
        self,
        type_expr: TypeApply,
        ctx: PyContext,
        scope: NameScope,
        builder: StrategyBuilderProtocol,
    ) -> None:
        self._type_expr = type_expr
        self._type_name = ctx.scope.lookup(type_expr.type)
        self._ctx = ctx

        self._ti_args: list[str] = []
        self._ti_args_self: list[str] = []
        self._type_var_args: list[str] = []
        self._nat_assertions: list[tuple[int, str]] = []
        self._type_assertions: list[tuple[int, str]] = []

        t = type_expr.type
        for tlp, arg in zip(t.type_level_params, type_expr.arguments, strict=True):
            if tlp.is_output:
                continue
            if tlp.kind == ParamKind.NAT:
                assert is_nat(arg)
                self._ti_args.append(NatExpr(arg, scope).local)
                self._ti_args_self.append(NatExpr(arg, scope).self_)
                if (
                    isinstance(
                        arg,
                        NatLiteral
                        | NatParamRef
                        | NatFieldValue
                        | NatAdd
                        | NatSub
                        | NatMul
                        | NatGetBit,
                    )
                    and not arg.references_type_arg
                ):
                    self._nat_assertions.append((tlp.position, NatExpr(arg, scope).self_))
            else:
                assert isinstance(
                    arg, TypeParamRef | TypeApply | TupleType | CellRefType | AnonymousRecordType
                )
                arg_strategy = builder.build(arg)
                self._ti_args.append(arg_strategy.type_info_expr())
                self._ti_args_self.append(arg_strategy.type_info_expr_self())
                self._type_var_args.append(arg_strategy.py_type())
                self._type_assertions.append((tlp.position, arg_strategy.type_info_expr_self()))

    def _info_name(self) -> str:
        info_name = f"{self._type_name}Type"
        if self._type_var_args:
            info_name = f"{info_name}[{', '.join(self._type_var_args)}]"
        return info_name

    @override
    def py_type(self) -> str:
        if self._type_var_args:
            return f"{self._type_name}[{', '.join(self._type_var_args)}]"
        return self._type_name

    @override
    def type_info_expr(self) -> str:
        info = self._info_name()
        if self._ti_args:
            return f"{info}.instantiate({', '.join(self._ti_args)})"
        return f"{info}()"

    @override
    def type_info_expr_self(self) -> str:
        info = self._info_name()
        if self._ti_args_self:
            return f"{info}.instantiate({', '.join(self._ti_args_self)})"
        return f"{info}()"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"{value}.serialize_to({builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        info = self._info_name()
        if self._ti_args:
            args = ", ".join([cs] + self._ti_args)
            sb.line(f"{target} = {info}().load_from({args})")
        else:
            sb.line(f"{target} = {info}().load_from({cs})")

    @override
    def emit_get_output(self, field_expr: str, position: int) -> str:
        tlp = self._type_expr.type.type_level_params[position]
        assert tlp.kind == ParamKind.NAT and tlp.is_output
        return f"{field_expr}.get_output({position})"

    @override
    def emit_serialize_assertions(self, field_name: str, sb: SourceBuilder) -> bool:
        emitted = False
        for pos, expected in self._nat_assertions:
            sb.line(f"assert {field_name}.get_output({pos}) == {expected}")
            emitted = True
        for pos, ti_expr in self._type_assertions:
            sb.line(f"{field_name}.check_type({pos}, {ti_expr})")
            emitted = True
        return emitted
