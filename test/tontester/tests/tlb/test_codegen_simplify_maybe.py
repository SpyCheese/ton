"""Tests for Maybe simplification (--simplify=maybe)."""

from generated.simplify_maybe import (
    EnumFieldsType,
    MaybeRefType,
    MixedType,
    NestedMaybeType,
    SimpleMaybeType,
    enum_fields,
    just,
    maybe_ref,
    mixed,
    nested_maybe,
    nothing,
    simple_maybe,
)
from tlb.object import MaybeTypeInfo, Ref, UintTypeConstructor


class TestSimpleMaybe:
    """Maybe uint32 → int | None."""

    def test_just_roundtrip(self):
        obj = simple_maybe(x=42)
        result = SimpleMaybeType().load_from(obj.serialize().begin_parse())
        assert result.x == 42

    def test_nothing_roundtrip(self):
        obj = simple_maybe(x=None)
        result = SimpleMaybeType().load_from(obj.serialize().begin_parse())
        assert result.x is None

    def test_just_bit_count(self):
        cell = simple_maybe(x=0).serialize()
        assert cell.begin_parse().remaining_bits == 33  # 1 tag + 32 value

    def test_nothing_bit_count(self):
        cell = simple_maybe(x=None).serialize()
        assert cell.begin_parse().remaining_bits == 1  # just the 0 tag bit


class TestMixed:
    """Maybe in a record with other fields."""

    def test_just_roundtrip(self):
        obj = mixed(a=10, b=20, c=-5)
        result = MixedType().load_from(obj.serialize().begin_parse())
        assert result.a == 10
        assert result.b == 20
        assert result.c == -5

    def test_nothing_roundtrip(self):
        obj = mixed(a=10, b=None, c=-5)
        result = MixedType().load_from(obj.serialize().begin_parse())
        assert result.a == 10
        assert result.b is None
        assert result.c == -5


class TestNestedMaybe:
    """Maybe (Maybe uint32) — outer NOT simplified, uses full Maybe type.

    Inner Maybe uint32 is simplified to int | None, so the outer
    Maybe's type arg X = int | None. Outer stays as nothing | just[int | None].
    """

    def test_just_some_roundtrip(self):
        inner_ti = MaybeTypeInfo(UintTypeConstructor(32))
        obj = nested_maybe(x=just(inner_ti, value=42))
        result = NestedMaybeType().load_from(obj.serialize().begin_parse())
        assert isinstance(result.x, just)
        assert result.x.value == 42

    def test_just_none_roundtrip(self):
        inner_ti = MaybeTypeInfo(UintTypeConstructor(32))
        obj = nested_maybe(x=just(inner_ti, value=None))
        result = NestedMaybeType().load_from(obj.serialize().begin_parse())
        assert isinstance(result.x, just)
        assert result.x.value is None

    def test_nothing_roundtrip(self):
        obj = nested_maybe(x=nothing())
        result = NestedMaybeType().load_from(obj.serialize().begin_parse())
        assert isinstance(result.x, nothing)


class TestEnumLiterals:
    """Bool → bool, Unit → None, True → Literal[True]."""

    def test_all_fields_roundtrip(self):
        obj = enum_fields(a=True, b=None, c=True, x=42)
        result = EnumFieldsType().load_from(obj.serialize().begin_parse())
        assert result.a is True
        assert result.b is None
        assert result.c is True
        assert result.x == 42

    def test_bool_false_roundtrip(self):
        obj = enum_fields(a=False, b=None, c=True, x=99)
        result = EnumFieldsType().load_from(obj.serialize().begin_parse())
        assert result.a is False
        assert result.b is None
        assert result.c is True
        assert result.x == 99

    def test_serialized_bits(self):
        """a:Bool(1) + b:Unit(0) + c:True(1) + x:uint32(32) = 34 bits."""
        obj = enum_fields(a=True, b=None, c=True, x=0x12345678)
        cs = obj.serialize().begin_parse()
        assert cs.remaining_bits == 34
        assert cs.load_uint(1) == 1  # a = True
        # b = Unit, 0 bits
        assert cs.load_uint(1) == 1  # c = True (tag $1)
        assert cs.load_uint(32) == 0x12345678  # x

    def test_bool_false_serialized_bits(self):
        obj = enum_fields(a=False, b=None, c=True, x=0)
        cs = obj.serialize().begin_parse()
        assert cs.load_uint(1) == 0  # a = False
        assert cs.load_uint(1) == 1  # c = True
        assert cs.load_uint(32) == 0


class TestMaybeRef:
    """Maybe ^uint32 → Ref[int] | None."""

    def test_just_roundtrip(self):
        obj = maybe_ref(x=Ref(UintTypeConstructor(32), 99))
        result = MaybeRefType().load_from(obj.serialize().begin_parse())
        assert result.x is not None
        assert result.x.ref == 99

    def test_nothing_roundtrip(self):
        obj = maybe_ref(x=None)
        result = MaybeRefType().load_from(obj.serialize().begin_parse())
        assert result.x is None
