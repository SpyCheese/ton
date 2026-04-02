"""TypeParamStrategy: emit store/load for generic type parameters."""

from typing import final, override

from ...sema.types import TypeParamDef
from ..source_builder import SourceBuilder
from ._base import TypeStrategy


@final
class TypeParamStrategy(TypeStrategy):
    """Field whose type is a generic type parameter (e.g. value:X where {X:Type}).

    Delegates serialization to a runtime TypeInfo passed as an argument.
    type_var is the generic type variable name (e.g. "X").
    ti_var is the Python variable holding the TypeInfo (e.g. "self._tX" or "_tX").
    """

    def __init__(self, param: TypeParamDef, type_var: str, ti_var: str) -> None:
        self.param = param
        self.type_var = type_var
        self.ti_var = ti_var

    @override
    def py_type(self) -> str:
        return self.type_var

    @override
    def type_info_expr(self) -> str:
        return self.ti_var

    @override
    def type_info_expr_self(self) -> str:
        return f"self.{self.ti_var}"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"self.{self.ti_var}.serialize_value({value}, {builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target} = {self.ti_var}.load_from({cs})")

    @override
    def descriptor(self) -> str:
        return f"typeparam_{self.param.name}"
