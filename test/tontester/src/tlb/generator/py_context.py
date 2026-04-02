"""Shared context for Python code generation.

Owns the import tracker, name scope, and ref wrapper registry.
Both py_codegen and py_emit depend on this, avoiding circular imports.
"""

from collections.abc import Callable
from typing import Protocol

from .identity_key import IdentityKey
from .name_scope import NameScope
from .sema_types import TypeParamDef, ResolvedConstructor, ResolvedType
from .source_builder import SourceBuilder


class ConstructorInfo(Protocol):
    """Protocol for constructor codegen data needed by cross-type lookups."""

    scope: NameScope
    type_params: list[TypeParamDef]

    def type_var_name(self, p: TypeParamDef) -> str: ...


class PyContext:
    """Shared state for a single file generation run."""

    scope: NameScope
    used_imports: set[str]
    _next_tmp: int
    ref_wrappers: dict[tuple[str, bool], str]
    ref_wrapper_code: list[str]
    _type_scopes: dict[IdentityKey[ResolvedType], NameScope]
    _constructors: dict[IdentityKey[ResolvedConstructor], ConstructorInfo]

    def __init__(self) -> None:
        self.scope = NameScope()
        self.used_imports = set()
        self._next_tmp = 0
        self.ref_wrappers = {}
        self.ref_wrapper_code = []
        self._type_scopes = {}
        self._constructors = {}

    def set_type_scope(self, t: ResolvedType, scope: NameScope) -> None:
        self._type_scopes[IdentityKey(t)] = scope

    def register_constructor(self, c: ResolvedConstructor, gen: ConstructorInfo) -> None:
        self._constructors[IdentityKey(c)] = gen

    def get_type_scope(self, t: ResolvedType) -> NameScope:
        return self._type_scopes[IdentityKey(t)]

    def get_constructor(self, c: ResolvedConstructor) -> ConstructorInfo:
        return self._constructors[IdentityKey(c)]

    def use(self, *names: str) -> None:
        """Mark imports as needed."""
        for n in names:
            self.used_imports.add(n)

    def tmp(self, prefix: str = "_tmp") -> str:
        """Generate a unique temporary variable name."""
        name = f"{prefix}_{self._next_tmp}"
        self._next_tmp += 1
        return name

    def get_or_create_ref_wrapper(
        self,
        inner_py_type: str,
        inner_class_name: str,
        is_special: bool,
        store_code_fn: Callable[[SourceBuilder], None],
        load_code_fn: Callable[[SourceBuilder], None],
    ) -> str:
        """Get (or generate) a ref wrapper class for `^inner_type`.

        Returns the wrapper class name. The wrapper is generated once and
        reused for all fields with the same inner type.
        """
        key = (inner_class_name, is_special)
        if key in self.ref_wrappers:
            return self.ref_wrappers[key]

        wrapper_name = self.scope.reserve(f"Ref_{inner_class_name}")
        self.ref_wrappers[key] = wrapper_name

        self.use("Cell", "Builder", "Slice", "TlbModelError")

        sb = SourceBuilder()
        sb.blank()
        sb.blank()
        sb.line(f"class {wrapper_name}:")
        with sb.block():
            sb.line(f'"""Lazy cell reference wrapper for ^{inner_class_name}."""')
            sb.blank()
            sb.line("_cell: Cell | None")
            sb.line(f"_value: {inner_py_type} | None")
            sb.blank()

            sb.line(f"def __init__(self, value: {inner_py_type} | Cell) -> None:")
            with sb.block():
                sb.line("if isinstance(value, Cell):")
                with sb.block():
                    sb.line("self._cell = value")
                    sb.line("self._value = None")
                sb.line("else:")
                with sb.block():
                    sb.line("self._cell = None")
                    sb.line("self._value = value")
            sb.blank()

            sb.line("@property")
            sb.line(f"def ref(self) -> {inner_py_type}:")
            with sb.block():
                sb.line("if self._value is None:")
                with sb.block():
                    sb.line("assert self._cell is not None")
                    sb.line("cs = self._cell.begin_parse()")
                    if not is_special:
                        sb.line("if cs.is_special():")
                        with sb.block():
                            sb.line(
                                "raise TlbModelError("
                                + f"'expected ordinary cell for {inner_class_name}, got special cell')"
                            )
                    else:
                        sb.line("if not cs.is_special():")
                        with sb.block():
                            sb.line(
                                "raise TlbModelError("
                                + f"'expected special cell for {inner_class_name}, got ordinary cell')"
                            )
                    sb.line(f"self._value = {wrapper_name}._load_inner(cs)")
                    sb.line("self._cell = None")
                sb.line("return self._value")
            sb.blank()

            sb.line("def serialize_ref(self) -> Cell:")
            with sb.block():
                sb.line("if self._cell is not None:")
                with sb.block():
                    sb.line("return self._cell")
                sb.line("assert self._value is not None")
                sb.line("builder = Builder()")
                sb.line(f"{wrapper_name}._store_inner(self._value, builder)")
                sb.line("return builder.end_cell()")
            sb.blank()

            sb.line("@staticmethod")
            sb.line(f"def _store_inner(value: {inner_py_type}, builder: Builder) -> None:")
            with sb.block():
                store_code_fn(sb)
            sb.blank()

            sb.line("@staticmethod")
            sb.line(f"def _load_inner(cs: Slice) -> {inner_py_type}:")
            with sb.block():
                load_code_fn(sb)

        self.ref_wrapper_code.append(sb.build().rstrip())
        return wrapper_name

    def emit_imports(self, sb: SourceBuilder) -> None:
        if "dataclass" in self.used_imports:
            sb.line("from dataclasses import dataclass")
        typing_parts = [n for n in ["final", "override"] if n in self.used_imports]
        if typing_parts:
            sb.line(f"from typing import {', '.join(typing_parts)}")
        sb.blank()
        if "bitarray" in self.used_imports:
            sb.line("from bitarray import bitarray")
        pytoniq = [n for n in ["Builder", "Cell", "Slice"] if n in self.used_imports]
        if pytoniq:
            sb.line(f"from pytoniq_core import {', '.join(pytoniq)}")
        sb.blank()
        tlb_obj = [
            n
            for n in [
                "AnyType",
                "BitsTypeConstructor",
                "InstantiableTypeInfo",
                "IntTypeConstructor",
                "Ref",
                "RefType",
                "TLBRecord",
                "TlbModelError",
                "TupleTypeConstructor",
                "TypeInfo",
                "UintTypeConstructor",
            ]
            if n in self.used_imports
        ]
        if tlb_obj:
            sb.line(f"from tlb.object import {', '.join(tlb_obj)}")
