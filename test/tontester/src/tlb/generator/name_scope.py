"""Name scope for Python code generation.

Tracks registered names, avoids collisions with Python keywords, builtins,
and previously registered names. Maps sema objects (ResolvedType,
ResolvedConstructor, ParamDef, ResolvedField) to unique Python identifiers.
"""

from __future__ import annotations

import keyword

from .sema_types import (
    ParamDef,
    ResolvedConstructor,
    ResolvedField,
    ResolvedType,
)

# Python keywords and soft keywords that can't be used as identifiers
_RESERVED: frozenset[str] = (
    frozenset(keyword.kwlist)
    | frozenset(keyword.softkwlist)
    | frozenset(
        {
            # builtins we don't want to shadow
            "int",
            "str",
            "bool",
            "list",
            "dict",
            "set",
            "tuple",
            "type",
            "None",
            "True",
            "False",
            "print",
            "len",
            "range",
            "isinstance",
            "super",
            "object",
            "Exception",
            "ValueError",
            "TypeError",
            # names from our runtime library
            "Builder",
            "Cell",
            "Slice",
            "TypeInfo",
            "TLBRecord",
            "TlbModelError",
        }
    )
)

# Sema objects we can bind names to
type Bindable = ResolvedType | ResolvedConstructor | ResolvedField | ParamDef


class NameScope:
    """Manages name bindings for a scope in generated code.

    Each scope tracks which names are taken and provides collision-free
    name generation. Names are bound to sema objects so codegen can look
    up the generated Python name for any sema construct.
    """

    _used: set[str]
    _bindings: dict[int, str]  # id(sema_object) -> python name
    _parent: NameScope | None

    def __init__(self, parent: NameScope | None = None) -> None:
        self._used = set()
        self._bindings = {}
        self._parent = parent

    def _is_taken(self, name: str) -> bool:
        if name in _RESERVED:
            return True
        if name in self._used:
            return True
        if self._parent is not None:
            return self._parent._is_taken(name)
        return False

    def _make_unique(self, preferred: str) -> str:
        """Return preferred name or a suffixed variant if taken."""
        candidate = preferred
        suffix = 1
        while self._is_taken(candidate):
            candidate = f"{preferred}_{suffix}"
            suffix += 1
        return candidate

    def bind(self, obj: Bindable, preferred: str) -> str:
        """Register a name for a sema object. Returns the actual name used."""
        name = self._make_unique(preferred)
        self._used.add(name)
        self._bindings[id(obj)] = name
        return name

    def reserve(self, name: str) -> str:
        """Reserve a name without binding to an object."""
        actual = self._make_unique(name)
        self._used.add(actual)
        return actual

    def lookup(self, obj: Bindable) -> str:
        """Get the Python name for a sema object. Must have been bound."""
        key = id(obj)
        if key in self._bindings:
            return self._bindings[key]
        if self._parent is not None:
            return self._parent.lookup(obj)
        raise KeyError(f"no binding for {obj!r}")

    def child(self) -> NameScope:
        """Create a child scope that inherits this scope's names."""
        return NameScope(parent=self)
