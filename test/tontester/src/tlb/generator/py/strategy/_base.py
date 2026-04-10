"""TypeStrategy ABC."""

from abc import ABC, abstractmethod
from typing import Protocol

from ...sema.types import ResolvedTypeExpr
from ..source_builder import SourceBuilder


class StrategyBuilderProtocol(Protocol):
    def build(self, type_expr: ResolvedTypeExpr) -> TypeStrategy: ...


class TypeStrategy(ABC):
    """Knows how to emit store/load code for a resolved type expression."""

    @abstractmethod
    def py_type(self) -> str:
        """Python type annotation string."""
        ...

    @abstractmethod
    def type_info_expr(self) -> str:
        """Python expression evaluating to a TypeInfo for this type.

        Used when this type appears as a generic argument, e.g. the T in
        Maybe T needs to produce a TypeInfo[T] expression at runtime.
        """
        ...

    def type_info_expr_self(self) -> str:
        """Like type_info_expr but renders nat params with self. prefix.

        Used in serialize_to assertions. Default delegates to type_info_expr
        which works when there are no nat param references in the expression.
        """
        return self.type_info_expr()

    @abstractmethod
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        """Emit statement(s) to store `value` into `builder`."""
        ...

    @abstractmethod
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        """Emit statement(s) to load from `cs` into variable `target`."""
        ...

    def load_uses_cs(self) -> bool:
        """Whether emit_load actually reads from the cs slice."""
        return True

    @property
    def is_nullable(self) -> bool:
        """Whether this type can already represent None (e.g., Maybe, generic type params)."""
        return False

    def emit_conditional_assert(self, value: str, sb: SourceBuilder) -> None:
        """Emit assertion that `value` is not None inside a conditional field guard.

        Called when a conditional field (flag?Type) is present. Most strategies
        emit `assert value is not None`. Nullable strategies (Maybe, type params)
        override: Maybe does nothing, type params delegate to TypeInfo.assert_nonnull.
        """
        sb.line(f"assert {value} is not None")

    def emit_get_output(self, _field_expr: str, _position: int) -> str:
        """Return a Python expression for the output param at the given TLP position."""
        assert False, f"{type(self).__name__} does not support emit_get_output"

    def emit_serialize_assertions(self, _field_name: str, _sb: SourceBuilder) -> bool:
        """Emit assertions verifying sub-type consistency during serialize_to.

        field_name is the self.X accessor for this field. Returns True if
        any assertions were emitted. Default: no assertions (for primitives).
        """
        return False
