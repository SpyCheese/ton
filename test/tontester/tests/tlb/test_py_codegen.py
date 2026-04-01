"""Tests for generated Python code from TL-B schemas.

Generated files live in tests/tlb/generated/ and are produced by generate_tl.py.
"""

import pytest
from generated.basic import (
    BoolType,
    MixedType,
    MsgType,
    TickTockType,
    UnitType,
    bool_false,
    bool_true,
    mixed,
    msg_a,
    msg_b,
    msg_c,
    tick_tock,
    unit,
)
from generated.cellref import (
    DoubleRefType,
    NestedRefType,
    Ref_Ref_uint8,
    Ref_TickTock,
    Ref_uint8,
    Ref_uint32,
    Ref_uint64,
    RefToRecordType,
    SimpleRefType,
    double_ref,
    nested_ref,
    ref_to_record,
    simple_ref,
)
from generated.cellref import (
    bool_false as cr_bool_false,
)
from generated.cellref import (
    bool_true as cr_bool_true,
)
from generated.cellref import (
    tick_tock as cr_tick_tock,
)
from generated.collisions import (
    BarType,
    ClassType,
    FooType,
    IntType,
    PassType,
    bar,
    class_1,
    foo,
    foo_1,
    int_1,
    pass_1,
    return_1,
)
from generated.cond_tuple import (
    FixedBitsType,
    MultiOptType,
    OptionalType,
    VarBitsType,
    fixed_bits,
    multi_opt,
    optional,
    var_bits,
)
from generated.cond_tuple import bit as ct_bit
from generated.generics import (
    EitherType,
    MaybeType,
    PairType,
    WrapperType,
    just,
    left,
    nothing,
    pair,
    right,
    wrapper,
)
from generated.nat_types import (
    Hash32Type,
    HashWidthType,
    LeqTestType,
    LtTestType,
    MultiNatType,
    hash32,
    hash_width,
    leq_test,
    lt_test,
    multi_nat,
)
from pytoniq_core import Builder
from tlb.object import UintTypeConstructor

# ── Bool ──────────────────────────────────────────────────────────────


class TestBool:
    def test_serialize_false_is_one_zero_bit(self):
        cell = bool_false().serialize()
        cs = cell.begin_parse()
        assert cs.remaining_bits == 1
        assert cs.load_bit() == 0

    def test_serialize_true_is_one_one_bit(self):
        cell = bool_true().serialize()
        cs = cell.begin_parse()
        assert cs.remaining_bits == 1
        assert cs.load_bit() == 1

    def test_roundtrip_false(self):
        result = BoolType().load_from(bool_false().serialize().begin_parse())
        assert isinstance(result, bool_false)

    def test_roundtrip_true(self):
        result = BoolType().load_from(bool_true().serialize().begin_parse())
        assert isinstance(result, bool_true)

    def test_deserialize_from_explicit_bits(self):
        b = Builder()
        _ = b.store_bit(0)
        assert isinstance(BoolType().load_from(b.end_cell().begin_parse()), bool_false)
        b = Builder()
        _ = b.store_bit(1)
        assert isinstance(BoolType().load_from(b.end_cell().begin_parse()), bool_true)

    def test_wrong_tag_fails(self):
        with pytest.raises(AssertionError):
            _ = bool_false.load_from(bool_true().serialize().begin_parse())
        with pytest.raises(AssertionError):
            _ = bool_true.load_from(bool_false().serialize().begin_parse())


# ── Unit ──────────────────────────────────────────────────────────────


class TestUnit:
    def test_serialize_is_zero_bits(self):
        assert unit().serialize().begin_parse().remaining_bits == 0

    def test_roundtrip(self):
        assert isinstance(UnitType().load_from(unit().serialize().begin_parse()), unit)


# ── TickTock ──────────────────────────────────────────────────────────


class TestTickTock:
    def test_roundtrip(self):
        obj = tick_tock(tick=bool_true(), tock=bool_false())
        result = TickTockType().load_from(obj.serialize().begin_parse())
        assert isinstance(result.tick, bool_true)
        assert isinstance(result.tock, bool_false)

    def test_explicit_bits(self):
        """tick_tock(true, false) = bits 1 0."""
        b = Builder()
        _ = b.store_bit(1)
        _ = b.store_bit(0)
        result = TickTockType().load_from(b.end_cell().begin_parse())
        assert isinstance(result.tick, bool_true)
        assert isinstance(result.tock, bool_false)

    def test_bit_count(self):
        cell = tick_tock(tick=bool_false(), tock=bool_false()).serialize()
        assert cell.begin_parse().remaining_bits == 2


# ── Mixed fields ─────────────────────────────────────────────────────


class TestMixed:
    def test_roundtrip(self):
        obj = mixed(a=100, b=-42, c=2**63, d=-100000)
        result = MixedType().load_from(obj.serialize().begin_parse())
        assert result.a == 100
        assert result.b == -42
        assert result.c == 2**63
        assert result.d == -100000

    def test_bit_count(self):
        """32 + 8 + 64 + 32 = 136 bits."""
        assert mixed(a=0, b=0, c=0, d=0).serialize().begin_parse().remaining_bits == 136

    def test_empty_cell_fails(self):
        with pytest.raises(Exception):
            _ = MixedType().load_from(Builder().end_cell().begin_parse())


# ── Three-way dispatch ────────────────────────────────────────────────


class TestMsg:
    def test_roundtrip_a(self):
        result = MsgType().load_from(msg_a(x=42).serialize().begin_parse())
        assert isinstance(result, msg_a)
        assert result.x == 42

    def test_roundtrip_b(self):
        result = MsgType().load_from(msg_b(y=-1).serialize().begin_parse())
        assert isinstance(result, msg_b)
        assert result.y == -1

    def test_roundtrip_c(self):
        result = MsgType().load_from(msg_c(z=999).serialize().begin_parse())
        assert isinstance(result, msg_c)
        assert result.z == 999

    def test_tag_bits(self):
        cs = msg_a(x=0).serialize().begin_parse()
        assert (cs.load_bit(), cs.load_bit()) == (0, 0)
        cs = msg_b(y=0).serialize().begin_parse()
        assert (cs.load_bit(), cs.load_bit()) == (0, 1)
        cs = msg_c(z=0).serialize().begin_parse()
        assert cs.load_bit() == 1


# ── Nat builtins ─────────────────────────────────────────────────────


class TestNatBuiltins:
    def test_hash32_roundtrip(self):
        assert Hash32Type().load_from(hash32(n=42).serialize().begin_parse()).n == 42

    def test_hash32_bit_count(self):
        assert hash32(n=0).serialize().begin_parse().remaining_bits == 32

    def test_hash_width_roundtrip(self):
        assert HashWidthType().load_from(hash_width(n=31).serialize().begin_parse()).n == 31

    def test_hash_width_bit_count(self):
        assert hash_width(n=0).serialize().begin_parse().remaining_bits == 5

    def test_leq_roundtrip(self):
        assert LeqTestType().load_from(leq_test(n=100).serialize().begin_parse()).n == 100

    def test_leq_bit_count(self):
        assert leq_test(n=0).serialize().begin_parse().remaining_bits == (100).bit_length()

    def test_lt_roundtrip(self):
        assert LtTestType().load_from(lt_test(n=15).serialize().begin_parse()).n == 15

    def test_lt_bit_count(self):
        assert lt_test(n=0).serialize().begin_parse().remaining_bits == (15).bit_length()

    def test_multi_nat_roundtrip(self):
        r = MultiNatType().load_from(multi_nat(a=7, b=5, c=42).serialize().begin_parse())
        assert (r.a, r.b, r.c) == (7, 5, 42)


# ── Name collisions ──────────────────────────────────────────────────


class TestNameCollisions:
    def test_python_keyword_constructor_names(self):
        """Constructors named 'pass' and 'return' get suffixed."""
        result = PassType().load_from(pass_1().serialize().begin_parse())
        assert isinstance(result, pass_1)
        result = PassType().load_from(return_1().serialize().begin_parse())
        assert isinstance(result, return_1)

    def test_python_keyword_field_names(self):
        """Fields named 'for' and 'import' get suffixed."""
        obj = class_1(for_1=42, import_1=-1)
        result = ClassType().load_from(obj.serialize().begin_parse())
        assert result.for_1 == 42
        assert result.import_1 == -1

    def test_builtin_type_name(self):
        """Type named 'Int' (collides with Python int) works."""
        obj = int_1(value=123)
        result = IntType().load_from(obj.serialize().begin_parse())
        assert result.value == 123

    def test_constructor_name_collision_across_types(self):
        """'foo' constructor in Foo type vs 'foo' constructor in Bar type get different names."""
        # foo (in Foo) has x:uint32 with tag $0
        obj_foo = foo(x=42)
        result = FooType().load_from(obj_foo.serialize().begin_parse())
        assert isinstance(result, foo)
        assert result.x == 42

        # foo_1 (in Bar, originally named 'foo') has y:uint64, no tag
        obj_bar = foo_1(y=999)
        result = BarType().load_from(obj_bar.serialize().begin_parse())
        assert isinstance(result, foo_1)
        assert result.y == 999

    def test_bar_in_foo(self):
        """bar constructor in Foo type."""
        obj = bar(x=77)
        result = FooType().load_from(obj.serialize().begin_parse())
        assert isinstance(result, bar)
        assert result.x == 77


# ── Cell references ───────────────────────────────────────────────────


class TestCellRef:
    def test_simple_ref_roundtrip(self):
        """^uint32: value stored in a referenced cell, accessed via .ref."""
        obj = simple_ref(x=Ref_uint32(42))
        result = SimpleRefType().load_from(obj.serialize().begin_parse())
        assert result.x.ref == 42

    def test_simple_ref_lazy(self):
        """Ref wrapper is lazy — doesn't parse until .ref is accessed."""
        obj = simple_ref(x=Ref_uint32(999))
        result = SimpleRefType().load_from(obj.serialize().begin_parse())
        # Cell is stored, not yet parsed
        assert result.x._cell is not None  # pyright: ignore[reportPrivateUsage]
        val = result.x.ref
        assert val == 999
        # After access, cell is released
        assert result.x._cell is None  # pyright: ignore[reportPrivateUsage]

    def test_simple_ref_structure(self):
        """simple_ref serializes as 0 data bits + 1 ref."""
        cell = simple_ref(x=Ref_uint32(999)).serialize()
        cs = cell.begin_parse()
        assert cs.remaining_bits == 0
        assert cs.remaining_refs == 1

    def test_nested_ref_roundtrip(self):
        """Mix of inline and ref fields."""
        obj = nested_ref(a=10, b=Ref_uint64(20), c=-5)
        result = NestedRefType().load_from(obj.serialize().begin_parse())
        assert result.a == 10
        assert result.b.ref == 20
        assert result.c == -5

    def test_nested_ref_structure(self):
        """a:uint32 (32 bits) + b:^uint64 (1 ref) + c:int8 (8 bits) = 40 bits + 1 ref."""
        cell = nested_ref(a=0, b=Ref_uint64(0), c=0).serialize()
        cs = cell.begin_parse()
        assert cs.remaining_bits == 40  # 32 + 8
        assert cs.remaining_refs == 1

    def test_ref_to_user_type(self):
        """^TickTock: user-defined type in a referenced cell."""
        tt = cr_tick_tock(tick=cr_bool_true(), tock=cr_bool_false())
        obj = ref_to_record(inner=Ref_TickTock(tt))
        result = RefToRecordType().load_from(obj.serialize().begin_parse())
        inner = result.inner.ref
        assert isinstance(inner, cr_tick_tock)
        assert isinstance(inner.tick, cr_bool_true)
        assert isinstance(inner.tock, cr_bool_false)

    def test_ref_to_user_type_structure(self):
        """ref_to_record: 0 data bits + 1 ref (containing 2 bits for TickTock)."""
        tt = cr_tick_tock(tick=cr_bool_false(), tock=cr_bool_false())
        cell = ref_to_record(inner=Ref_TickTock(tt)).serialize()
        cs = cell.begin_parse()
        assert cs.remaining_bits == 0
        assert cs.remaining_refs == 1
        ref_cs = cs.load_ref().begin_parse()
        assert ref_cs.remaining_bits == 2

    def test_double_ref_roundtrip(self):
        """^^uint8: two levels of cell references, access via .ref.ref."""
        inner = Ref_uint8(42)
        obj = double_ref(x=Ref_Ref_uint8(inner))
        result = DoubleRefType().load_from(obj.serialize().begin_parse())
        assert result.x.ref.ref == 42

    def test_double_ref_structure(self):
        """^^uint8: outer cell has 0 bits + 1 ref, inner ref has 0 bits + 1 ref, innermost has 8 bits."""
        inner = Ref_uint8(255)
        cell = double_ref(x=Ref_Ref_uint8(inner)).serialize()
        cs = cell.begin_parse()
        assert cs.remaining_bits == 0
        assert cs.remaining_refs == 1
        inner1 = cs.load_ref().begin_parse()
        assert inner1.remaining_bits == 0
        assert inner1.remaining_refs == 1
        inner2 = inner1.load_ref().begin_parse()
        assert inner2.remaining_bits == 8
        assert inner2.load_uint(8) == 255

    def test_special_cell_error(self):
        """Accessing .ref on a special cell raises TlbModelError for non-special types."""
        # Normal cell works fine
        obj = simple_ref(x=Ref_uint32(42))
        result = SimpleRefType().load_from(obj.serialize().begin_parse())
        assert result.x.ref == 42


# ── Conditional fields ────────────────────────────────────────────────


class TestConditional:
    def test_optional_present(self):
        obj = optional(has_x=1, x=42)
        result = OptionalType().load_from(obj.serialize().begin_parse())
        assert result.has_x == 1
        assert result.x == 42

    def test_optional_absent(self):
        obj = optional(has_x=0, x=None)
        result = OptionalType().load_from(obj.serialize().begin_parse())
        assert result.has_x == 0
        assert result.x is None

    def test_optional_present_bit_count(self):
        """has_x (1 bit) + x (32 bits) = 33 bits when present."""
        cell = optional(has_x=1, x=0).serialize()
        assert cell.begin_parse().remaining_bits == 33

    def test_optional_absent_bit_count(self):
        """has_x (1 bit) only when absent."""
        cell = optional(has_x=0, x=None).serialize()
        assert cell.begin_parse().remaining_bits == 1

    def test_multi_opt_all_present(self):
        obj = multi_opt(flags=0b11, a=10, b=-5)
        result = MultiOptType().load_from(obj.serialize().begin_parse())
        assert result.flags == 0b11
        assert result.a == 10
        assert result.b == -5

    def test_multi_opt_none_present(self):
        obj = multi_opt(flags=0, a=None, b=None)
        result = MultiOptType().load_from(obj.serialize().begin_parse())
        assert result.a is None
        assert result.b is None

    def test_multi_opt_first_only(self):
        obj = multi_opt(flags=0b01, a=99, b=None)
        result = MultiOptType().load_from(obj.serialize().begin_parse())
        assert result.a == 99
        assert result.b is None


# ── Tuple fields ──────────────────────────────────────────────────────


class TestTuple:
    def test_fixed_bits_roundtrip(self):
        bits = [ct_bit(1), ct_bit(0), ct_bit(1)]
        obj = fixed_bits(s=bits)
        result = FixedBitsType().load_from(obj.serialize().begin_parse())
        assert len(result.s) == 3

    def test_fixed_bits_count(self):
        """3 * Bit = 3 * (## 1) = 3 bits, but each Bit also has its own (## 1) field."""
        bits = [ct_bit(0), ct_bit(0), ct_bit(0)]
        cell = fixed_bits(s=bits).serialize()
        assert cell.begin_parse().remaining_bits == 3

    def test_var_bits_roundtrip(self):
        bits = [ct_bit(1), ct_bit(1), ct_bit(0), ct_bit(1)]
        obj = var_bits(n=4, s=bits)
        result = VarBitsType().load_from(obj.serialize().begin_parse())
        assert result.n == 4
        assert len(result.s) == 4

    def test_var_bits_empty(self):
        obj = var_bits(n=0, s=[])
        result = VarBitsType().load_from(obj.serialize().begin_parse())
        assert result.n == 0
        assert result.s == []


# ── Generic types ─────────────────────────────────────────────────────


class TestMaybe:
    def test_nothing_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        obj = nothing()
        cell = obj.serialize()
        result = MaybeType().load_from(cell.begin_parse(), uint32_ti)
        assert isinstance(result, nothing)

    def test_just_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        obj = just(uint32_ti, value=42)
        cell = obj.serialize()
        result = MaybeType().load_from(cell.begin_parse(), uint32_ti)
        assert isinstance(result, just)
        assert result.value == 42

    def test_nothing_bit_count(self):
        assert nothing().serialize().begin_parse().remaining_bits == 1

    def test_just_bit_count(self):
        uint32_ti = UintTypeConstructor(32)
        assert just(uint32_ti, value=0).serialize().begin_parse().remaining_bits == 33


class TestEither:
    def test_left_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        int8_ti = UintTypeConstructor(8)
        obj = left(uint32_ti, value=42)
        result = EitherType().load_from(obj.serialize().begin_parse(), uint32_ti, int8_ti)
        assert isinstance(result, left)
        assert result.value == 42

    def test_right_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        int8_ti = UintTypeConstructor(8)
        obj = right(int8_ti, value=99)
        result = EitherType().load_from(obj.serialize().begin_parse(), uint32_ti, int8_ti)
        assert isinstance(result, right)
        assert result.value == 99


class TestPair:
    def test_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        int8_ti = UintTypeConstructor(8)
        obj = pair(uint32_ti, int8_ti, first=42, second=7)
        result = PairType().load_from(obj.serialize().begin_parse(), uint32_ti, int8_ti)
        assert isinstance(result, pair)
        assert result.first == 42
        assert result.second == 7


class TestWrapper:
    def test_wrapper_just(self):
        """Wrapper T = { inner: Maybe T }. Test with T=uint32, inner=just."""
        uint32_ti = UintTypeConstructor(32)
        inner = just(uint32_ti, value=42)
        obj = wrapper(uint32_ti, inner=inner)
        result = WrapperType().load_from(obj.serialize().begin_parse(), uint32_ti)
        assert isinstance(result, wrapper)
        assert isinstance(result.inner, just)
        assert result.inner.value == 42

    def test_wrapper_nothing(self):
        """Wrapper T with inner=nothing."""
        uint32_ti = UintTypeConstructor(32)
        inner = nothing()
        obj = wrapper(uint32_ti, inner=inner)
        result = WrapperType().load_from(obj.serialize().begin_parse(), uint32_ti)
        assert isinstance(result, wrapper)
        assert isinstance(result.inner, nothing)
