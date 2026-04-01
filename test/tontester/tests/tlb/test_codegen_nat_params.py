"""Tests for generated code with nat parameters."""

from generated.nat_params import (
    FixedArrayType,
    MaybeTupleType,
    ThreeIntsType,
    fixed_array,
    just,
    maybe_tuple,
    nothing,
    three_ints,
)
from tlb.object import UintTypeConstructor


class TestFixedArray:
    """FixedArray n X: type with nat param n and type param X."""

    def test_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        obj = fixed_array[int](3, uint32_ti, items=[10, 20, 30])
        result = FixedArrayType[int]().load_from(obj.serialize().begin_parse(), 3, uint32_ti)
        assert isinstance(result, fixed_array)
        assert result.items == [10, 20, 30]

    def test_empty(self):
        uint32_ti = UintTypeConstructor(32)
        obj = fixed_array[int](0, uint32_ti, items=[])
        result = FixedArrayType[int]().load_from(obj.serialize().begin_parse(), 0, uint32_ti)
        assert result.items == []

    def test_single_element(self):
        uint8_ti = UintTypeConstructor(8)
        obj = fixed_array[int](1, uint8_ti, items=[255])
        result = FixedArrayType[int]().load_from(obj.serialize().begin_parse(), 1, uint8_ti)
        assert result.items == [255]

    def test_bit_count(self):
        uint32_ti = UintTypeConstructor(32)
        obj = fixed_array[int](3, uint32_ti, items=[0, 0, 0])
        assert obj.serialize().begin_parse().remaining_bits == 96  # 3 * 32


class TestThreeInts:
    """ThreeInts: concrete instantiation FixedArray 3 uint32."""

    def test_roundtrip(self):
        obj = three_ints(items=fixed_array[int](3, UintTypeConstructor(32), items=[1, 2, 3]))
        result = ThreeIntsType().load_from(obj.serialize().begin_parse())
        assert isinstance(result, three_ints)
        assert isinstance(result.items, fixed_array)
        assert result.items.items == [1, 2, 3]

    def test_bit_count(self):
        obj = three_ints(items=fixed_array[int](3, UintTypeConstructor(32), items=[0, 0, 0]))
        assert obj.serialize().begin_parse().remaining_bits == 96


class TestMaybeTuple:
    """Maybe (3 * uint8): tuple as a generic arg, exercises TupleTypeConstructor."""

    def test_just_roundtrip(self):
        from tlb.object import TupleTypeConstructor

        uint8_ti = UintTypeConstructor(8)
        tuple_ti = TupleTypeConstructor(3, uint8_ti)
        inner = just[list[int]](tuple_ti, value=[10, 20, 30])
        obj = maybe_tuple(inner=inner)
        result = MaybeTupleType().load_from(obj.serialize().begin_parse())
        assert isinstance(result, maybe_tuple)
        assert isinstance(result.inner, just)
        assert result.inner.value == [10, 20, 30]

    def test_nothing_roundtrip(self):
        obj = maybe_tuple(inner=nothing())
        result = MaybeTupleType().load_from(obj.serialize().begin_parse())
        assert isinstance(result, maybe_tuple)
        assert isinstance(result.inner, nothing)
