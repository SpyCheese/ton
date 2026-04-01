from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Protocol, cast, final, override

from bitarray import bitarray
from pytoniq_core import Builder, Cell, Slice


# ===== Support classes =====
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

    def serialize(self):
        builder = Builder()
        self.serialize_to(builder)
        return builder.end_cell()


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


# ===== Primitives =====
@final
class UintTypeConstructor(TypeInfo[int]):
    def __init__(self, n: int):
        assert n > 0
        self._n: int = n

    @property
    def n(self):
        return self._n

    @override
    def serialize_value(self, value: int, builder: Builder):
        _ = builder.store_uint(value, self._n)

    @override
    def load_from(self, cs: Slice):
        return cs.load_uint(self._n)

    @override
    def __repr__(self):
        return f"uint{self._n}"


Uint8Type: TypeInfo[int] = UintTypeConstructor(8)
Uint16Type: TypeInfo[int] = UintTypeConstructor(16)
Uint32Type: TypeInfo[int] = UintTypeConstructor(32)
Uint64Type: TypeInfo[int] = UintTypeConstructor(64)
Uint128Type: TypeInfo[int] = UintTypeConstructor(128)
Uint256Type: TypeInfo[int] = UintTypeConstructor(256)


@final
class IntTypeConstructor(TypeInfo[int]):
    def __init__(self, n: int):
        assert n > 0
        self._n: int = n

    @property
    def n(self):
        return self._n

    @override
    def serialize_value(self, value: int, builder: Builder):
        _ = builder.store_int(value, self._n)

    @override
    def load_from(self, cs: Slice):
        return cs.load_int(self._n)

    @override
    def __repr__(self):
        return f"int{self._n}"


Int8Type: TypeInfo[int] = IntTypeConstructor(8)
Int16Type: TypeInfo[int] = IntTypeConstructor(16)
Int32Type: TypeInfo[int] = IntTypeConstructor(32)
Int64Type: TypeInfo[int] = IntTypeConstructor(64)
Int128Type: TypeInfo[int] = IntTypeConstructor(128)
Int256Type: TypeInfo[int] = IntTypeConstructor(256)


@final
class BitsTypeConstructor(TypeInfo[bitarray]):
    def __init__(self, n: int):
        assert n > 0
        self._n: int = n

    @property
    def n(self):
        return self._n

    @override
    def serialize_value(self, value: bitarray, builder: Builder):
        assert len(value) == self._n
        _ = builder.store_bits(value)

    @override
    def load_from(self, cs: Slice) -> bitarray:
        return cs.load_bits(self._n)

    @override
    def __repr__(self):
        return f"bytes{self._n}"


Bits256Type: TypeInfo[bitarray] = BitsTypeConstructor(256)


# ===== Test =====
@final
@dataclass
class Just[X](TLBRecord):
    _tx: TypeInfo[X]
    value: X

    @override
    def serialize_to(self, builder: Builder):
        _ = builder.store_bit(1)
        self._tx.serialize_value(self.value, builder)


@final
@dataclass
class Nothing(TLBRecord):
    @override
    def serialize_to(self, builder: Builder):
        _ = builder.store_bit(0)


type Maybe[X] = Just[X] | Nothing


@final
class MaybeType[X](InstantiableTypeInfo[Maybe[X], TypeInfo[X]]):
    @override
    def serialize_value(cls, value: Maybe[X], builder: Builder):
        return value.serialize_to(builder)

    @override
    def load_from(cls, cs: Slice, tx: TypeInfo[X]) -> Maybe[X]:
        if cs.load_bit() == 1:
            return Just(tx, tx.load_from(cs))
        else:
            return Nothing()

    @override
    def __repr__(self):
        return "Maybe"


Maybe_int8 = MaybeType[int].instantiate(Int8Type)
Maybe_Maybe_int8 = MaybeType[Maybe[int]].instantiate(Maybe_int8)

jst = Just(Maybe_int8, Just(Int8Type, 42))
cell = jst.serialize()
print(cell)
value = Maybe_Maybe_int8.deserialize(cell)
print(value)

Ref_int8 = RefType[int].instantiate(Int8Type)
Maybe_Ref_int8 = MaybeType[Ref[int]].instantiate(Ref_int8)

maybe_ref_int = Just(Ref_int8, Ref(Int8Type, 123))
print(maybe_ref_int)
print(maybe_ref_int.serialize())

deser = Maybe_Ref_int8.deserialize(maybe_ref_int.serialize())
assert isinstance(deser, Just)
print(deser.value.ref)
print(deser)

ref_Any = RefType[Slice].instantiate(AnyType)
Maybe_ref_Any = MaybeType[Ref[Slice]].instantiate(ref_Any)

maybe_ref_any = Just(ref_Any, Ref(AnyType, Builder().store_uint(42, 32).end_cell()))
print(maybe_ref_any)
print(maybe_ref_any.serialize())
deser = Maybe_ref_Any.deserialize(maybe_ref_any.serialize())
print(deser)
