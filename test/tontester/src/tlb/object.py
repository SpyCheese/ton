from abc import ABC, abstractmethod
from typing import Protocol, cast, final, override

from bitarray import bitarray
from pytoniq_core import Builder, Cell, Slice


class TlbModelError(Exception):
    def __init__(self, message: str):
        super().__init__(message)

    @staticmethod
    def raise_if_not_empty(cs: Slice):
        if cs.remaining_bits or cs.remaining_refs:
            raise TlbModelError("Deserialization must consume all bits and references")


class TypeInfo[T, *Args](Protocol):
    def serialize_value(self, value: T, builder: Builder) -> None: ...

    def load_from(self, cs: Slice, *args: *Args) -> T: ...

    def deserialize(self, cell: Cell, *args: *Args):
        cs = cell.begin_parse()
        if cs.is_special():
            raise TlbModelError("Cell must not be special")
        result = self.load_from(cs, *args)
        TlbModelError.raise_if_not_empty(cs)
        return result


@final
class _InstantiatedGenericType[T, *Args](TypeInfo[T]):
    def __init__(self, generic: TypeInfo[T, *Args], *args: *Args):
        self._generic = generic
        self._args = args

    @override
    def serialize_value(self, value: T, builder: Builder):
        self._generic.serialize_value(value, builder)

    @override
    def load_from(self, cs: Slice) -> T:
        return self._generic.load_from(cs, *self._args)

    @override
    def deserialize(self, cell: Cell) -> T:
        return self._generic.deserialize(cell, *self._args)

    @override
    def __repr__(self):
        args_str = ", ".join(repr(arg) for arg in self._args)
        return f"{repr(self._generic)}<{args_str}>"


class InstantiableTypeInfo[T, *Args](TypeInfo[T, *Args], Protocol):
    @classmethod
    def instantiate(cls, *args: *Args) -> TypeInfo[T]:
        return _InstantiatedGenericType(cls(), *args)


class TLBRecord(ABC):
    @abstractmethod
    def serialize_to(self, builder: Builder) -> None: ...

    def serialize(self) -> Cell:
        builder = Builder()
        self.serialize_to(builder)
        return builder.end_cell()

    def get_output(self, idx: int) -> int:
        raise TlbModelError(f"type has no output param at index {idx}")


@final
class Ref[X](TLBRecord):
    def __init__(self, tx: TypeInfo[X], ref: X | Cell):
        self._tx = tx
        if isinstance(ref, Cell):
            self._value_cell = ref
            self._value = None
        else:
            self._value_cell = None
            self._value = ref

    @property
    def ref(self):
        if self._value_cell is not None:
            self._value = self._tx.deserialize(self._value_cell)
            self._value_cell = None
        return cast(X, self._value)

    @ref.setter
    def ref(self, value: X):
        self._value = value
        self._value_cell = None

    def set_cell(self, cell: Cell):
        self._value_cell = cell
        self._value = None

    @override
    def serialize_to(self, builder: Builder):
        if self._value_cell is not None:
            _ = builder.store_ref(self._value_cell)
        else:
            child = Builder()
            self._tx.serialize_value(cast(X, self._value), child)
            _ = builder.store_ref(child.end_cell())

    @override
    def __repr__(self):
        return f"Ref(_tx={self._tx}, value={self._value if self._value is not None else self._value_cell})"


@final
class RefType[X](InstantiableTypeInfo[Ref[X], TypeInfo[X]]):
    @override
    def serialize_value(cls, value: Ref[X], builder: Builder):
        return value.serialize_to(builder)

    @override
    def load_from(cls, cs: Slice, tx: TypeInfo[X]) -> Ref[X]:
        child = cs.load_ref()
        return Ref(tx, child)

    @override
    def __repr__(self):
        return "Ref"


@final
class _AnyType(TypeInfo[Slice]):
    @override
    def serialize_value(cls, value: Slice, builder: Builder):
        _ = builder.store_slice(value)

    @override
    def load_from(cls, cs: Slice):
        result = cs.copy()
        _ = cs.skip_bits(cs.remaining_bits)
        while cs.remaining_refs:
            _ = cs.load_ref()
        return result

    @override
    def __repr__(self):
        return "Any"


AnyType = _AnyType()


@final
class UintTypeConstructor(TypeInfo[int]):
    def __init__(self, n: int):
        assert n >= 0
        self._n: int = n

    @property
    def n(self):
        return self._n

    @override
    def serialize_value(self, value: int, builder: Builder):
        assert value >= 0, f"uint{self._n}: negative value {value}"
        if self._n == 0:
            assert value == 0, f"uint0: only 0 is valid, got {value}"
        else:
            _ = builder.store_uint(value, self._n)

    @override
    def load_from(self, cs: Slice):
        if self._n == 0:
            return 0
        return cs.load_uint(self._n)

    @override
    def __repr__(self):
        return f"uint{self._n}"


@final
class BoundedUintTypeConstructor(TypeInfo[int]):
    def __init__(self, bound: int, *, inclusive: bool):
        assert bound >= 0
        self._bound = bound
        self._inclusive = inclusive
        if inclusive:
            self._width = bound.bit_length()
            self._max = bound
        else:
            assert bound > 0
            self._width = (bound - 1).bit_length()
            self._max = bound - 1

    @override
    def serialize_value(self, value: int, builder: Builder):
        if value < 0 or value > self._max:
            op = "<=" if self._inclusive else "<"
            raise TlbModelError(f"value {value} out of range for #{op} {self._bound}")
        if self._width > 0:
            _ = builder.store_uint(value, self._width)

    @override
    def load_from(self, cs: Slice):
        value = cs.load_uint(self._width) if self._width > 0 else 0
        if value > self._max:
            op = "<=" if self._inclusive else "<"
            raise TlbModelError(f"value {value} out of range for #{op} {self._bound}")
        return value

    @override
    def __repr__(self):
        op = "<=" if self._inclusive else "<"
        return f"#{op}{self._bound}"


@final
class IntTypeConstructor(TypeInfo[int]):
    def __init__(self, n: int):
        assert n >= 0
        self._n: int = n

    @property
    def n(self):
        return self._n

    @override
    def serialize_value(self, value: int, builder: Builder):
        if self._n == 0:
            assert value == 0, f"int0: only 0 is valid, got {value}"
        else:
            _ = builder.store_int(value, self._n)

    @override
    def load_from(self, cs: Slice):
        if self._n == 0:
            return 0
        return cs.load_int(self._n)

    @override
    def __repr__(self):
        return f"int{self._n}"


@final
class BitsTypeConstructor(TypeInfo[bitarray]):
    def __init__(self, n: int):
        assert n >= 0
        self._n: int = n

    @property
    def n(self):
        return self._n

    @override
    def serialize_value(self, value: bitarray, builder: Builder):
        assert len(value) == self._n, f"bits{self._n}: expected {self._n} bits, got {len(value)}"
        if self._n > 0:
            _ = builder.store_bits(value)

    @override
    def load_from(self, cs: Slice) -> bitarray:
        if self._n == 0:
            return bitarray()
        return cs.load_bits(self._n)

    @override
    def __repr__(self):
        return f"bytes{self._n}"


@final
class TupleTypeConstructor[X](TypeInfo[list[X]]):
    def __init__(self, count: int, element_ti: TypeInfo[X]) -> None:
        assert count >= 0
        self._count = count
        self._element_ti = element_ti

    @override
    def serialize_value(self, value: list[X], builder: Builder) -> None:
        assert len(value) == self._count, (
            f"tuple: expected {self._count} elements, got {len(value)}"
        )
        for i in range(self._count):
            self._element_ti.serialize_value(value[i], builder)

    @override
    def load_from(self, cs: Slice) -> list[X]:
        result: list[X] = []
        for _ in range(self._count):
            result.append(self._element_ti.load_from(cs))
        return result

    @override
    def __repr__(self) -> str:
        return f"Tuple({self._count}, {self._element_ti!r})"
