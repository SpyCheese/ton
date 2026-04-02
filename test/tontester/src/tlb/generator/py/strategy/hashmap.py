"""HashmapStrategy: simplified HashmapE/Hashmap → HashmapDict."""

from typing import final, override

from ...sema.types import (
    AnonymousRecordType,
    CellRefType,
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
class HashmapStrategy(TypeStrategy):
    """Simplified Hashmap/HashmapE → HashmapDict[V]."""

    def __init__(
        self,
        type_expr: TypeApply,
        ctx: PyContext,
        scope: NameScope,
        builder: StrategyBuilderProtocol,
        allow_empty: bool,
    ) -> None:
        self._ctx = ctx
        self._allow_empty = allow_empty
        ctx.use("HashmapDict")

        t = type_expr.type
        # First param is key_bits (nat), second is value type
        assert len(t.type_level_params) == 2
        assert t.type_level_params[0].kind == ParamKind.NAT
        assert t.type_level_params[1].kind == ParamKind.TYPE

        key_arg = type_expr.arguments[0]
        assert is_nat(key_arg)
        self._key_bits = NatExpr(key_arg, scope)
        self._key_bits_self = NatExpr(key_arg, scope)

        val_arg = type_expr.arguments[1]
        assert isinstance(
            val_arg, TypeParamRef | TypeApply | TupleType | CellRefType | AnonymousRecordType
        )
        val_strat = builder.build(val_arg, inside_generic_arg=True)
        self._val_ti = val_strat.type_info_expr()
        self._val_ti_self = val_strat.type_info_expr_self()
        self._val_py_type = val_strat.py_type()

    @override
    def py_type(self) -> str:
        return f"HashmapDict[{self._val_py_type}]"

    @override
    def type_info_expr(self) -> str:
        allow = "True" if self._allow_empty else "False"
        return f"{self.py_type()}.type_info({self._key_bits.local}, {self._val_ti}, allow_empty={allow})"

    @override
    def type_info_expr_self(self) -> str:
        return f"{self.py_type()}.type_info({self._key_bits_self.self_}, {self._val_ti_self}, allow_empty={self._allow_empty})"

    @override
    def emit_serialize_assertions(self, field_name: str, sb: SourceBuilder) -> bool:
        key_expr = self._key_bits_self.self_
        sb.line(f"assert {field_name}.key_bits == {key_expr}")
        sb.line(f"assert {field_name}.value_ti == {self._val_ti_self}")
        return True

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"{value}.serialize_to({builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        allow = "True" if self._allow_empty else "False"
        sb.line(
            f"{target} = {self.py_type()}.load_from("
            + f"{cs}, {self._key_bits.local}, {self._val_ti}, allow_empty={allow})"
        )

    @override
    def descriptor(self) -> str:
        kind = "HashmapE" if self._allow_empty else "Hashmap"
        return f"{kind}_{self._key_bits.local}_{self._val_py_type}"
