"""Built-in TL-B types that exist before any user schema is processed."""

from __future__ import annotations

from .sema_types import ParamKind, ResolvedType

_next_idx = 0


def _builtin(
    name: str,
    *,
    arity: int = 0,
    param_kinds: list[ParamKind] | None = None,
    produces_nat: bool = False,
) -> ResolvedType:
    global _next_idx
    t = ResolvedType(
        name=name,
        type_idx=_next_idx,
        arity=arity,
        param_kinds=param_kinds or [],
        produces_nat=produces_nat,
        is_builtin=True,
    )
    _next_idx += 1
    return t


# Core nat types
Nat_type = _builtin("#", produces_nat=True)
NatWidth_type = _builtin("##", arity=1, param_kinds=[ParamKind.NAT], produces_nat=True)
NatLess_type = _builtin("#<", arity=1, param_kinds=[ParamKind.NAT], produces_nat=True)
NatLeq_type = _builtin("#<=", arity=1, param_kinds=[ParamKind.NAT], produces_nat=True)

# Integer/bitstring types (parameterized)
Int_type = _builtin("int", arity=1, param_kinds=[ParamKind.NAT], produces_nat=True)
UInt_type = _builtin("uint", arity=1, param_kinds=[ParamKind.NAT], produces_nat=True)
Bits_type = _builtin("bits", arity=1, param_kinds=[ParamKind.NAT])

# Special types
Any_type = _builtin("Any")
Cell_type = _builtin("Cell")

BUILTIN_TYPES_NUM = _next_idx


def create_builtin_registry() -> dict[str, ResolvedType]:
    """Create the initial type registry with all built-in types."""
    registry: dict[str, ResolvedType] = {}

    for t in [
        Nat_type,
        NatWidth_type,
        NatLess_type,
        NatLeq_type,
        Int_type,
        UInt_type,
        Bits_type,
        Any_type,
        Cell_type,
    ]:
        registry[t.name] = t

    # Fixed-width shorthands: uint1..uint256, int1..int257, bits1..bits1023
    for n in range(1, 257):
        registry[f"uint{n}"] = _builtin(f"uint{n}", produces_nat=True)
    for n in range(1, 258):
        registry[f"int{n}"] = _builtin(f"int{n}", produces_nat=True)
    for n in range(1, 1024):
        registry[f"bits{n}"] = _builtin(f"bits{n}")

    # Comparison operators (used internally for constraints)
    for name in ["=", "<", "<="]:
        registry[name] = _builtin(name, arity=2, param_kinds=[ParamKind.NAT, ParamKind.NAT])

    return registry
