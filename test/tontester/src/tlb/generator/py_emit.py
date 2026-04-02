"""Type-aware code emission strategies for Python codegen.

Each TypeStrategy knows how to emit store/load code for a particular
type expression. Uses PyContext for import tracking and name scoping.
"""

from abc import ABC, abstractmethod
from typing import TypeIs, final, override

from .name_scope import NameScope
from .py_context import PyContext
from .sema_builtins import (
    Any_type,
    Bits_type,
    Int_type,
    Nat_type,
    NatLeq_type,
    NatLess_type,
    NatWidth_type,
    UInt_type,
)
from .sema_types import (
    AnonymousRecordType,
    CellRefType,
    NatAdd,
    NatFieldValue,
    NatGetBit,
    NatLiteral,
    NatMul,
    NatParamRef,
    NatSub,
    NatTypeArg,
    ParamDef,
    ParamKind,
    ResolvedExpr,
    ResolvedField,
    ResolvedNatExpr,
    ResolvedTypeExpr,
    TupleType,
    TypeApply,
    TypeLevelParam,
    TypeParamRef,
    references_type_params,
)
from .source_builder import SourceBuilder


class NatExpr:
    """A resolved nat expression that can be rendered in different contexts.

    Holds the sema expression and scope, renders with appropriate prefix:
    - local form (for load_from): bare variable name
    - self form (for serialize_to): self-prefixed
    """

    _expr: ResolvedNatExpr
    _scope: NameScope

    def __init__(self, expr: ResolvedNatExpr, scope: NameScope) -> None:
        self._expr = expr
        self._scope = scope

    @property
    def local(self) -> str:
        return self._render(self._expr, use_local=True)

    @property
    def self_(self) -> str:
        return self._render(self._expr, prefix="self.")

    @property
    def is_constant(self) -> bool:
        """True if this is a compile-time constant (NatLiteral)."""
        return isinstance(self._expr, NatLiteral)

    @property
    def is_zero(self) -> bool:
        """True if this is a compile-time constant 0."""
        return isinstance(self._expr, NatLiteral) and self._expr.value == 0

    def _resolve(self, obj: ParamDef | TypeLevelParam | ResolvedField, use_local: bool) -> str:
        if use_local:
            return self._scope.lookup_local(obj)
        return self._scope.lookup(obj)

    def _render(self, expr: ResolvedNatExpr, use_local: bool = False, prefix: str = "") -> str:
        match expr:
            case NatLiteral(value=value):
                return str(value)
            case NatParamRef(param=param):
                return f"{prefix}{self._resolve(param, use_local)}"
            case NatFieldValue(field=field):
                return f"{prefix}{self._resolve(field, use_local)}"
            case NatAdd(left=left, right=right):
                return f"({self._render(left, use_local, prefix)} + {self._render(right, use_local, prefix)})"
            case NatSub(left=left, right=right):
                return f"({self._render(left, use_local, prefix)} - {self._render(right, use_local, prefix)})"
            case NatMul(left=left, right=right):
                return f"({self._render(left, use_local, prefix)} * {self._render(right, use_local, prefix)})"
            case NatGetBit(value=value, bit=bit):
                return f"(({self._render(value, use_local, prefix)} >> {self._render(bit, use_local, prefix)}) & 1)"
            case NatTypeArg(param=param):
                return self._scope.lookup(param)


@final
class _DerivedNatExpr(NatExpr):
    """A NatExpr with a transformation applied, e.g. '({}).bit_length()'."""

    def __init__(self, base: NatExpr, template: str) -> None:
        super().__init__(base._expr, base._scope)
        self._base = base
        self._template = template

    @property
    @override
    def local(self) -> str:
        return self._template.format(self._base.local)

    @property
    @override
    def self_(self) -> str:
        return self._template.format(self._base.self_)

    @property
    @override
    def is_constant(self) -> bool:
        return False


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
    def __init__(self, width: NatExpr, ctx: PyContext) -> None:
        self.width = width
        self.ctx = ctx

    @override
    def py_type(self) -> str:
        return "int"

    @override
    def type_info_expr(self) -> str:
        self.ctx.use("UintTypeConstructor")
        return f"UintTypeConstructor({self.width.local})"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        if self.width.is_zero:
            sb.line("pass")
        elif self.width.is_constant:
            sb.line(f"_ = {builder}.store_uint({value}, {self.width.self_})")
        else:
            sb.line(f"if {self.width.self_} > 0:")
            with sb.block():
                sb.line(f"_ = {builder}.store_uint({value}, {self.width.self_})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        if self.width.is_zero:
            sb.line(f"{target} = 0")
        elif self.width.is_constant:
            sb.line(f"{target} = {cs}.load_uint({self.width.local})")
        else:
            sb.line(
                f"{target} = {cs}.load_uint({self.width.local}) if {self.width.local} > 0 else 0"
            )

    @override
    def descriptor(self) -> str:
        return f"uint{self.width.local}"


@final
class IntStrategy(TypeStrategy):
    def __init__(self, width: NatExpr, ctx: PyContext) -> None:
        self.width = width
        self.ctx = ctx

    @override
    def py_type(self) -> str:
        return "int"

    @override
    def type_info_expr(self) -> str:
        self.ctx.use("IntTypeConstructor")
        return f"IntTypeConstructor({self.width.local})"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        if self.width.is_zero:
            sb.line("pass")
        elif self.width.is_constant:
            sb.line(f"_ = {builder}.store_int({value}, {self.width.self_})")
        else:
            sb.line(f"if {self.width.self_} > 0:")
            with sb.block():
                sb.line(f"_ = {builder}.store_int({value}, {self.width.self_})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        if self.width.is_zero:
            sb.line(f"{target} = 0")
        elif self.width.is_constant:
            sb.line(f"{target} = {cs}.load_int({self.width.local})")
        else:
            sb.line(
                f"{target} = {cs}.load_int({self.width.local}) if {self.width.local} > 0 else 0"
            )

    @override
    def descriptor(self) -> str:
        return f"int{self.width.local}"


@final
class BitsStrategy(TypeStrategy):
    def __init__(self, width: NatExpr, ctx: PyContext) -> None:
        self.width = width
        self.ctx = ctx
        ctx.use("bitarray")

    @override
    def py_type(self) -> str:
        return "bitarray"

    @override
    def type_info_expr(self) -> str:
        self.ctx.use("BitsTypeConstructor")
        return f"BitsTypeConstructor({self.width.local})"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"_ = {builder}.store_bits({value})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target} = {cs}.load_bits({self.width.local})")

    @override
    def descriptor(self) -> str:
        return f"bits{self.width.local}"


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
    def type_info_expr(self) -> str:
        info_name = f"{self.type_name}Type"
        if self.type_var_args:
            info_name = f"{info_name}[{', '.join(self.type_var_args)}]"
        if self.ti_args:
            return f"{info_name}.instantiate({', '.join(self.ti_args)})"
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


@final
class CellRefStrategy(TypeStrategy):
    """^Type for fully concrete inner types: uses a lazy ref wrapper class.

    Each distinct (inner_type, is_special) pair gets one wrapper class,
    shared across all fields that use it. Only used when the inner type
    has no type parameters — otherwise GenericCellRefStrategy is used.
    """

    def __init__(self, inner: TypeStrategy, ctx: PyContext, is_special: bool = False) -> None:
        self.inner = inner
        self.ctx = ctx

        inner_type = inner.py_type()
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
    def type_info_expr(self) -> str:
        self.ctx.use("RefType")
        inner_ti = self.inner.type_info_expr()
        inner_py = self.inner.py_type()
        return f"RefType[{inner_py}].instantiate({inner_ti})"

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
class GenericCellRefStrategy(TypeStrategy):
    """^Type when the inner type involves type parameters.

    Uses the runtime Ref[X] and RefType[X] instead of a generated wrapper
    class, because wrapper classes are file-level and can't capture type params.
    """

    def __init__(self, inner: TypeStrategy, ctx: PyContext) -> None:
        self.inner = inner
        self.ctx = ctx
        ctx.use("Ref")

    @override
    def py_type(self) -> str:
        return f"Ref[{self.inner.py_type()}]"

    @override
    def type_info_expr(self) -> str:
        self.ctx.use("RefType")
        return f"RefType[{self.inner.py_type()}].instantiate({self.inner.type_info_expr()})"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"{value}.serialize_to({builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        self.ctx.use("Ref")
        inner_ti = self.inner.type_info_expr()
        sb.line(f"{target} = Ref({inner_ti}, {cs}.load_ref())")

    @override
    def descriptor(self) -> str:
        return f"ref_{self.inner.descriptor()}"


@final
class SliceTypeStrategy(TypeStrategy):
    def __init__(self, ctx: PyContext) -> None:
        self.ctx = ctx
        ctx.use("Slice")

    @override
    def py_type(self) -> str:
        return "Slice"

    @override
    def type_info_expr(self) -> str:
        self.ctx.use("AnyType")
        return "AnyType"

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
class TupleStrategy(TypeStrategy):
    """n * Type: fixed or variable-length sequence of values."""

    def __init__(self, count: NatExpr, element: TypeStrategy, ctx: PyContext) -> None:
        self.count = count
        self.element = element
        self.ctx = ctx

    @override
    def py_type(self) -> str:
        return f"list[{self.element.py_type()}]"

    @override
    def type_info_expr(self) -> str:
        self.ctx.use("TupleTypeConstructor")
        elem_ti = self.element.type_info_expr()
        return f"TupleTypeConstructor({self.count.local}, {elem_ti})"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        idx = self.ctx.tmp("_i")
        sb.line(f"for {idx} in range({self.count.self_}):")
        with sb.block():
            self.element.emit_store(f"{value}[{idx}]", builder, sb)

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target}: list[{self.element.py_type()}] = []")
        sb.line(f"for _ in range({self.count.local}):")
        with sb.block():
            elem_tmp = self.ctx.tmp("_elem")
            self.element.emit_load(elem_tmp, cs, sb)
            sb.line(f"{target}.append({elem_tmp})")

    @override
    def descriptor(self) -> str:
        return f"tuple_{self.count.local}_{self.element.descriptor()}"


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
    def type_info_expr(self) -> str:
        return self.ti_var

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"self.{self.ti_var}.serialize_value({value}, {builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target} = {self.ti_var}.load_from({cs})")

    @override
    def descriptor(self) -> str:
        return f"typeparam_{self.param.name}"


class StrategyBuilder:
    """Builds TypeStrategy instances for resolved type expressions.

    Holds the constructor-local context (scope, type param mappings)
    instead of threading them through every strategy_for call.
    Tracks which type params are referenced during building.
    """

    ctx: PyContext
    scope: NameScope
    used_type_params: set[ParamDef]

    def __init__(self, ctx: PyContext, scope: NameScope) -> None:
        self.ctx = ctx
        self.scope = scope
        self.used_type_params = set()

    @staticmethod
    def _to_nat(expr: ResolvedExpr) -> ResolvedNatExpr:
        assert _is_nat(expr)
        return expr

    def _nat(self, expr: ResolvedExpr) -> NatExpr:
        """Create a NatExpr from a resolved expression."""
        return NatExpr(self._to_nat(expr), self.scope)

    def _nat_derived(self, expr: ResolvedExpr, template: str) -> NatExpr:
        """Create a NatExpr with a transformation template like '({}).bit_length()'."""
        base = NatExpr(self._to_nat(expr), self.scope)
        return _DerivedNatExpr(base, template)

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
                assert param.kind == ParamKind.TYPE, f"nat param {param.name} used as type"
                ti_var = self.scope.lookup(param)
                self.used_type_params.add(param)
                return TypeParamStrategy(param, param.name, ti_var)

            case CellRefType(inner=inner_expr):
                is_concrete = not references_type_params(inner_expr)
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
            nat = self._to_nat(type_expr.arguments[0])
            if isinstance(nat, NatLiteral):
                return UintStrategy(
                    NatExpr(NatLiteral(nat.value.bit_length()), NameScope()), self.ctx
                )
            return UintStrategy(
                self._nat_derived(type_expr.arguments[0], "({}).bit_length()"), self.ctx
            )
        if t is NatLess_type:
            assert len(type_expr.arguments) == 1
            nat = self._to_nat(type_expr.arguments[0])
            if isinstance(nat, NatLiteral):
                return UintStrategy(
                    NatExpr(NatLiteral((nat.value - 1).bit_length()), NameScope()), self.ctx
                )
            return UintStrategy(
                self._nat_derived(type_expr.arguments[0], "({} - 1).bit_length()"), self.ctx
            )
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
        type_var_args: list[str] = []

        for tlp, arg in zip(t.type_level_params, type_expr.arguments, strict=True):
            if tlp.is_output:
                continue
            param_kind = tlp.kind
            if param_kind == ParamKind.NAT:
                assert _is_nat(arg)
                ti_args.append(NatExpr(arg, self.scope).local)
            else:
                assert isinstance(arg, TypeParamRef | TypeApply | TupleType | CellRefType)
                arg_strategy = self.build(arg, inside_generic_arg=True)
                ti_args.append(arg_strategy.type_info_expr())
                type_var_args.append(arg_strategy.py_type())

        return UserTypeStrategy(self.ctx.scope.lookup(t), ti_args, type_var_args)


def _is_nat(expr: ResolvedExpr) -> TypeIs[ResolvedNatExpr]:
    """Type guard: check whether a ResolvedExpr is a nat expression."""
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
