"""Type-aware code emission strategies for Python codegen.

Each TypeStrategy knows how to emit store/load code for a particular
type expression. Uses PyContext for import tracking and name scoping.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from typing import final, override

from .name_scope import NameScope
from .py_context import PyContext
from .sema_builtins import (
    Any_type,
    Bits_type,
    Cell_type,
    Int_type,
    Nat_type,
    NatLeq_type,
    NatLess_type,
    NatWidth_type,
    UInt_type,
)
from .sema_types import (
    CellRefType,
    CondType,
    NatAdd,
    NatFieldValue,
    NatGetBit,
    NatLiteral,
    NatMul,
    NatParamRef,
    NatSub,
    NatTypeArg,
    ParamDef,
    ResolvedExpr,
    ResolvedNatExpr,
    ResolvedTypeExpr,
    TupleType,
    TypeApply,
    TypeParamRef,
)
from .source_builder import SourceBuilder


class TypeStrategy(ABC):
    """Knows how to emit store/load code for a resolved type expression."""

    @abstractmethod
    def py_type(self) -> str:
        """Python type annotation string."""
        ...

    @abstractmethod
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        """Emit statement(s) to store `value` into `builder`."""
        ...

    @abstractmethod
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        """Emit statement(s) to load from `cs` into variable `target`."""
        ...

    @abstractmethod
    def descriptor(self) -> str:
        """A unique string identifying this strategy for dedup purposes."""
        ...


@final
class UintStrategy(TypeStrategy):
    def __init__(self, width: str) -> None:
        self.width = width

    @override
    def py_type(self) -> str:
        return "int"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"_ = {builder}.store_uint({value}, {self.width})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target} = {cs}.load_uint({self.width})")

    @override
    def descriptor(self) -> str:
        return f"uint{self.width}"


@final
class IntStrategy(TypeStrategy):
    def __init__(self, width: str) -> None:
        self.width = width

    @override
    def py_type(self) -> str:
        return "int"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"_ = {builder}.store_int({value}, {self.width})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target} = {cs}.load_int({self.width})")

    @override
    def descriptor(self) -> str:
        return f"int{self.width}"


@final
class BitsStrategy(TypeStrategy):
    def __init__(self, width: str, ctx: PyContext) -> None:
        self.width = width
        ctx.use("bitarray")

    @override
    def py_type(self) -> str:
        return "bitarray"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"_ = {builder}.store_bits({value})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target} = {cs}.load_bits({self.width})")

    @override
    def descriptor(self) -> str:
        return f"bits{self.width}"


@final
class UserTypeStrategy(TypeStrategy):
    def __init__(
        self,
        type_name: str,
        ti_args: list[str] | None = None,
        type_var_args: list[str] | None = None,
    ) -> None:
        self.type_name = type_name
        self.ti_args = ti_args or []
        self.type_var_args = type_var_args or []

    @override
    def py_type(self) -> str:
        if self.type_var_args:
            return f"{self.type_name}[{', '.join(self.type_var_args)}]"
        return self.type_name

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"{value}.serialize_to({builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        type_info_name = f"{self.type_name}Type"
        if self.type_var_args:
            type_info_name = f"{type_info_name}[{', '.join(self.type_var_args)}]"
        if self.ti_args:
            args = ", ".join([cs] + self.ti_args)
            sb.line(f"{target} = {type_info_name}().load_from({args})")
        else:
            sb.line(f"{target} = {type_info_name}().load_from({cs})")

    @override
    def descriptor(self) -> str:
        return self.type_name


@final
class CellRefStrategy(TypeStrategy):
    """^Type: uses a lazy ref wrapper class.

    Each distinct (inner_type, is_special) pair gets one wrapper class,
    shared across all fields that use it.
    """

    def __init__(self, inner: TypeStrategy, ctx: PyContext, is_special: bool = False) -> None:
        self.inner = inner
        self.ctx = ctx

        inner_type = inner.py_type()
        # Use the strategy's descriptor for dedup — e.g. "uint32" not just "int"
        inner_key = inner.descriptor()

        def emit_store_body(sb: SourceBuilder) -> None:
            inner.emit_store("value", "builder", sb)

        def emit_load_body(sb: SourceBuilder) -> None:
            inner.emit_load("_result", "cs", sb)
            sb.line("return _result")

        self.wrapper_name = ctx.get_or_create_ref_wrapper(
            inner_py_type=inner_type,
            inner_class_name=inner_key,
            is_special=is_special,
            store_code_fn=emit_store_body,
            load_code_fn=emit_load_body,
        )

    @override
    def py_type(self) -> str:
        return self.wrapper_name

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"_ = {builder}.store_ref({value}.serialize_ref())")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target} = {self.wrapper_name}({cs}.load_ref())")

    @override
    def descriptor(self) -> str:
        return self.wrapper_name


@final
class CellTypeStrategy(TypeStrategy):
    def __init__(self, ctx: PyContext) -> None:
        ctx.use("Cell")

    @override
    def py_type(self) -> str:
        return "Cell"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"_ = {builder}.store_ref({value})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target} = {cs}.load_ref()")

    @override
    def descriptor(self) -> str:
        return "Cell"


@final
class SliceTypeStrategy(TypeStrategy):
    def __init__(self, ctx: PyContext) -> None:
        ctx.use("Slice")

    @override
    def py_type(self) -> str:
        return "Slice"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"_ = {builder}.store_slice({value})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target} = {cs}")

    @override
    def descriptor(self) -> str:
        return "Slice"


@final
class CondStrategy(TypeStrategy):
    """flag?Type: conditional field, present when selector is nonzero.

    Stores both a 'self.' prefixed selector (for serialize_to) and a bare
    selector (for load_from) since the same field is an attribute vs local.
    """

    def __init__(
        self, selector_self: str, selector_local: str, inner: TypeStrategy, ctx: PyContext
    ) -> None:
        self.selector_self = selector_self
        self.selector_local = selector_local
        self.inner = inner
        self.ctx = ctx

    @override
    def py_type(self) -> str:
        return f"{self.inner.py_type()} | None"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"if {self.selector_self}:")
        with sb.block():
            sb.line(f"assert {value} is not None")
            self.inner.emit_store(value, builder, sb)

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"if {self.selector_local}:")
        with sb.block():
            self.inner.emit_load(target, cs, sb)
        sb.line("else:")
        with sb.block():
            sb.line(f"{target} = None")

    @override
    def descriptor(self) -> str:
        return f"cond_{self.selector_local}_{self.inner.descriptor()}"


@final
class TupleStrategy(TypeStrategy):
    """n * Type: fixed or variable-length sequence of values.

    Like CondStrategy, stores both self-prefixed and local count expressions.
    """

    def __init__(
        self, count_self: str, count_local: str, element: TypeStrategy, ctx: PyContext
    ) -> None:
        self.count_self = count_self
        self.count_local = count_local
        self.element = element
        self.ctx = ctx

    @override
    def py_type(self) -> str:
        return f"list[{self.element.py_type()}]"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        idx = self.ctx.tmp("_i")
        sb.line(f"for {idx} in range({self.count_self}):")
        with sb.block():
            self.element.emit_store(f"{value}[{idx}]", builder, sb)

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target}: list[{self.element.py_type()}] = []")
        sb.line(f"for _ in range({self.count_local}):")
        with sb.block():
            elem_tmp = self.ctx.tmp("_elem")
            self.element.emit_load(elem_tmp, cs, sb)
            sb.line(f"{target}.append({elem_tmp})")

    @override
    def descriptor(self) -> str:
        return f"tuple_{self.count_local}_{self.element.descriptor()}"


@final
class TypeParamStrategy(TypeStrategy):
    """Field whose type is a generic type parameter (e.g. value:X where {X:Type}).

    Delegates serialization to a runtime TypeInfo passed as an argument.
    type_var is the generic type variable name (e.g. "X").
    ti_var is the Python variable holding the TypeInfo (e.g. "self._tX" or "_tX").
    """

    def __init__(self, param: ParamDef, type_var: str, ti_var: str) -> None:
        self.param = param
        self.type_var = type_var
        self.ti_var = ti_var

    @override
    def py_type(self) -> str:
        return self.type_var

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"self.{self.ti_var}.serialize_value({value}, {builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target} = {self.ti_var}.load_from({cs})")

    @override
    def descriptor(self) -> str:
        return f"typeparam_{self.param.name}"


@final
class TodoStrategy(TypeStrategy):
    def __init__(self, desc: str = "unsupported") -> None:
        self.desc = desc

    @override
    def py_type(self) -> str:
        return "object"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"raise NotImplementedError('{self.desc}')")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target} = None  # TODO: {self.desc}")

    @override
    def descriptor(self) -> str:
        return f"todo_{self.desc}"


def _as_nat(expr: ResolvedExpr) -> ResolvedNatExpr:
    """Assert that a ResolvedExpr is a nat expression and narrow the type."""
    assert isinstance(
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
    return expr


def strategy_for(
    type_expr: ResolvedTypeExpr,
    ctx: PyContext,
    scope: NameScope | None = None,
    type_param_vars: dict[ParamDef, str] | None = None,
) -> TypeStrategy:
    """Create a TypeStrategy for a resolved type expression."""
    if isinstance(type_expr, TypeApply):
        t = type_expr.type
        if t is UInt_type:
            assert len(type_expr.arguments) == 1
            return UintStrategy(nat_to_python(_as_nat(type_expr.arguments[0])))
        if t is Int_type:
            assert len(type_expr.arguments) == 1
            return IntStrategy(nat_to_python(_as_nat(type_expr.arguments[0])))
        if t is Bits_type:
            assert len(type_expr.arguments) == 1
            return BitsStrategy(nat_to_python(_as_nat(type_expr.arguments[0])), ctx)
        if t is Nat_type:
            return UintStrategy("32")
        if t is NatWidth_type:
            assert len(type_expr.arguments) == 1
            return UintStrategy(nat_to_python(_as_nat(type_expr.arguments[0])))
        if t is NatLeq_type:
            assert len(type_expr.arguments) == 1
            b = nat_to_python(_as_nat(type_expr.arguments[0]))
            return UintStrategy(f"({b}).bit_length()")
        if t is NatLess_type:
            assert len(type_expr.arguments) == 1
            b = nat_to_python(_as_nat(type_expr.arguments[0]))
            return UintStrategy(f"({b} - 1).bit_length()")
        if t.is_builtin and t.arity == 0 and t.produces_nat:
            name = t.name
            if name.startswith("uint"):
                return UintStrategy(str(int(name[4:])))
            if name.startswith("int"):
                return IntStrategy(str(int(name[3:])))
            if name.startswith("bits"):
                return BitsStrategy(str(int(name[4:])), ctx)
        if t is Cell_type:
            return CellTypeStrategy(ctx)
        if t is Any_type:
            return SliceTypeStrategy(ctx)
        if not t.is_builtin:
            ti_args: list[str] = []
            type_var_args: list[str] = []
            for arg in type_expr.arguments:
                if isinstance(arg, TypeParamRef):
                    if type_param_vars is not None and arg.param in type_param_vars:
                        ti_args.append(type_param_vars[arg.param])
                        type_var_args.append(arg.param.name)
            return UserTypeStrategy(ctx.scope.lookup(t), ti_args, type_var_args)

    if isinstance(type_expr, TypeParamRef):
        if type_param_vars is not None and type_expr.param in type_param_vars:
            ti_var = type_param_vars[type_expr.param]
            return TypeParamStrategy(type_expr.param, type_expr.param.name, ti_var)
        return TodoStrategy(f"unbound type param {type_expr.param.name}")

    if isinstance(type_expr, CellRefType):
        inner = strategy_for(type_expr.inner, ctx, scope, type_param_vars)
        return CellRefStrategy(inner, ctx)

    if isinstance(type_expr, CondType):
        sel_local = nat_to_python(type_expr.selector, scope)
        sel_self = nat_to_python(type_expr.selector, scope, prefix="self.")
        inner = strategy_for(type_expr.inner, ctx, scope, type_param_vars)
        return CondStrategy(sel_self, sel_local, inner, ctx)

    if isinstance(type_expr, TupleType):
        cnt_local = nat_to_python(type_expr.count, scope)
        cnt_self = nat_to_python(type_expr.count, scope, prefix="self.")
        element = strategy_for(type_expr.element, ctx, scope, type_param_vars)
        return TupleStrategy(cnt_self, cnt_local, element, ctx)

    return TodoStrategy()


class NatEmitter:
    """Converts resolved nat expressions to Python expression strings.

    Handles scope-aware name lookup and self-prefix for different contexts.
    """

    scope: NameScope | None
    prefix: str

    def __init__(self, scope: NameScope | None = None, prefix: str = "") -> None:
        self.scope = scope
        self.prefix = prefix

    def emit(self, expr: ResolvedNatExpr) -> str:
        if isinstance(expr, NatLiteral):
            return str(expr.value)
        if isinstance(expr, NatParamRef):
            name = self.scope.lookup(expr.param) if self.scope is not None else expr.param.name
            return f"{self.prefix}{name}"
        if isinstance(expr, NatFieldValue):
            name = (
                self.scope.lookup(expr.field)
                if self.scope is not None
                else (expr.field.name or "_field")
            )
            return f"{self.prefix}{name}"
        if isinstance(expr, NatAdd):
            return f"({self.emit(expr.left)} + {self.emit(expr.right)})"
        if isinstance(expr, NatSub):
            return f"({self.emit(expr.left)} - {self.emit(expr.right)})"
        if isinstance(expr, NatMul):
            return f"({self.emit(expr.left)} * {self.emit(expr.right)})"
        if isinstance(expr, NatGetBit):
            return f"(({self.emit(expr.value)} >> {self.emit(expr.bit)}) & 1)"
        # NatTypeArg
        return f"_type_arg_{expr.position}"


def nat_to_python(expr: ResolvedNatExpr, scope: NameScope | None = None, prefix: str = "") -> str:
    """Convenience wrapper around NatEmitter."""
    return NatEmitter(scope, prefix).emit(expr)
