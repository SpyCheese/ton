"""Shared context for Python code generation.

Owns the import tracker and name scope.
Both codegen and strategy modules depend on this, avoiding circular imports.
"""

from typing import Protocol

from ..identity_key import IdentityKey
from ..sema.types import ResolvedConstructor, ResolvedType, TypeParamDef
from ..simplify_config import SimplifyConfig
from .name_scope import NameScope
from .source_builder import SourceBuilder


class ConstructorInfo(Protocol):
    """Protocol for constructor codegen data needed by cross-type lookups."""

    scope: NameScope
    type_params: list[TypeParamDef]

    def type_var_name(self, p: TypeParamDef) -> str: ...


class PyContext:
    """Shared state for a single file generation run."""

    scope: NameScope
    simplify: SimplifyConfig
    used_imports: set[str]
    _next_tmp: int
    _type_scopes: dict[IdentityKey[ResolvedType], NameScope]
    _constructors: dict[IdentityKey[ResolvedConstructor], ConstructorInfo]

    def __init__(self, simplify: SimplifyConfig | None = None) -> None:
        self.scope = NameScope()
        self.simplify = simplify or SimplifyConfig.none()
        self.used_imports = set()
        self._next_tmp = 0
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

    _IMPORTS: dict[str, list[str]] = {
        "dataclasses": ["dataclass"],
        "typing": ["Literal", "final", "override"],
        "bitarray": ["bitarray"],
        "pytoniq_core": ["Builder", "Cell", "Slice"],
        "tlb.object": [
            "AnyType",
            "BitsTypeConstructor",
            "BoolTypeInfo",
            "BoundedUintTypeConstructor",
            "CellRefType",
            "FalseTypeInfo",
            "GenericTLBType",
            "InstantiableTypeInfo",
            "IntTypeConstructor",
            "MaybeTypeConstructor",
            "Ref",
            "SelfTypeInfoTag",
            "TLBRecord",
            "TLBType",
            "TlbModelError",
            "TrueTypeInfo",
            "TupleTypeConstructor",
            "TypeInfo",
            "UintTypeConstructor",
            "UnaryTypeInfo",
            "UnitTypeInfo",
            "VarIntTypeConstructor",
            "VarUIntTypeConstructor",
        ],
        "tlb.hashmap": ["HashmapDict", "UnitAug"],
    }

    def emit_imports(self, sb: SourceBuilder) -> None:
        for module, names in self._IMPORTS.items():
            used = [n for n in names if n in self.used_imports]
            if used:
                sb.line(f"from {module} import {', '.join(used)}")
