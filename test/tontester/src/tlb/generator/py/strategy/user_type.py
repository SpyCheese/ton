"""UserTypeStrategy: emit store/load for user-defined types."""

from typing import final, override

from ..source_builder import SourceBuilder
from ._base import TypeStrategy


@final
class UserTypeStrategy(TypeStrategy):
    """User-defined type, possibly generic.

    ti_args: runtime expressions for each type/nat arg (TypeInfo exprs or int exprs).
    type_var_args: Python type annotations for type args only (for generic subscription).
    """

    def __init__(
        self,
        type_name: str,
        ti_args: list[str] | None = None,
        ti_args_self: list[str] | None = None,
        type_var_args: list[str] | None = None,
    ) -> None:
        self.type_name = type_name
        self.ti_args = ti_args or []
        self.ti_args_self = ti_args_self or self.ti_args
        self.type_var_args = type_var_args or []

    @override
    def py_type(self) -> str:
        if self.type_var_args:
            return f"{self.type_name}[{', '.join(self.type_var_args)}]"
        return self.type_name

    @override
    def type_info_expr(self) -> str:
        info_name = f"{self.type_name}Type"
        if self.type_var_args:
            info_name = f"{info_name}[{', '.join(self.type_var_args)}]"
        if self.ti_args:
            return f"{info_name}.instantiate({', '.join(self.ti_args)})"
        return f"{info_name}()"

    @override
    def type_info_expr_self(self) -> str:
        info_name = f"{self.type_name}Type"
        if self.type_var_args:
            info_name = f"{info_name}[{', '.join(self.type_var_args)}]"
        if self.ti_args_self:
            return f"{info_name}.instantiate({', '.join(self.ti_args_self)})"
        return f"{info_name}()"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"{value}.serialize_to({builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        info_name = f"{self.type_name}Type"
        if self.type_var_args:
            info_name = f"{info_name}[{', '.join(self.type_var_args)}]"
        if self.ti_args:
            args = ", ".join([cs] + self.ti_args)
            sb.line(f"{target} = {info_name}().load_from({args})")
        else:
            sb.line(f"{target} = {info_name}().load_from({cs})")

    @override
    def descriptor(self) -> str:
        if not self.ti_args:
            return self.type_name
        parts: list[str] = []
        for a in self.ti_args:
            sanitized = a
            for ch in "()[], .":
                sanitized = sanitized.replace(ch, "_")
            parts.append(sanitized)
        return f"{self.type_name}_{'_'.join(parts)}"
