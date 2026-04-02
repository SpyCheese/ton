"""Tests for serialize/deserialize validation assertions."""

import pytest
from bitarray import bitarray
from generated.validation import (
    ConstrainedType,
    EqFieldType,
    FixedBitsType,
    NatFieldType,
    OrderedType,
    ValParentType,
    ValType,
    VarBitsFieldType,
    constrained,
    eq_field,
    fixed_bits,
    nat_field,
    ordered,
    val_n,
    val_parent,
    val_zero,
    var_bits_field,
)
from pytoniq_core import Builder
from tlb.object import TlbModelError


class TestSerializeNatParamNonNegative:
    """serialize_to asserts nat params >= 0."""

    def test_valid_nat_param(self):
        obj = val_n(n=4, x=10)
        _ = obj.serialize()

    def test_negative_nat_param(self):
        obj = val_n(n=-1, x=0)
        with pytest.raises(AssertionError):
            _ = obj.serialize()


class TestSerializeFieldConstraints:
    """serialize_to re-checks field constraints."""

    def test_constrained_valid(self):
        obj = constrained(flags=1, x=42)
        result = ConstrainedType().load_from(obj.serialize().begin_parse())
        assert result.flags == 1
        assert result.x == 42

    def test_constrained_zero(self):
        obj = constrained(flags=0, x=99)
        result = ConstrainedType().load_from(obj.serialize().begin_parse())
        assert result.flags == 0

    def test_constrained_too_high(self):
        """flags > 1 fails assertion during serialization."""
        obj = constrained(flags=2, x=0)
        with pytest.raises(AssertionError):
            _ = obj.serialize()

    def test_ordered_valid(self):
        obj = ordered(a=3, b=5)
        result = OrderedType().load_from(obj.serialize().begin_parse())
        assert result.a == 3 and result.b == 5

    def test_ordered_equal(self):
        obj = ordered(a=10, b=10)
        result = OrderedType().load_from(obj.serialize().begin_parse())
        assert result.a == result.b == 10

    def test_ordered_wrong(self):
        """b < a fails assertion during serialization."""
        obj = ordered(a=5, b=3)
        with pytest.raises(AssertionError):
            _ = obj.serialize()


class TestSerializeSubtypeConsistency:
    """serialize_to asserts sub-type nat params match parent expectations."""

    def test_parent_with_matching_val_n(self):
        inner = val_n(n=4, x=10)
        obj = val_parent(n=4, inner=inner)
        result = ValParentType().load_from(obj.serialize().begin_parse())
        assert result.n == 4
        assert isinstance(result.inner, val_n)
        assert result.inner.x == 10

    def test_parent_with_val_zero(self):
        obj = val_parent(n=0, inner=val_zero())
        result = ValParentType().load_from(obj.serialize().begin_parse())
        assert result.n == 0
        assert isinstance(result.inner, val_zero)

    def test_parent_with_wrong_val_zero(self):
        """val_zero is Val 0, but parent claims n=5 — assertion catches mismatch."""
        obj = val_parent(n=5, inner=val_zero())
        with pytest.raises(AssertionError):
            _ = obj.serialize()

    def test_parent_with_wrong_val_n(self):
        """val_n(n=3) is Val 3, but parent claims n=5 — assertion catches mismatch."""
        inner = val_n(n=3, x=5)
        obj = val_parent(n=5, inner=inner)
        with pytest.raises(AssertionError):
            _ = obj.serialize()

    def test_mutated_inner_caught(self):
        """Deserialize correctly, then mutate inner, serialization catches it."""
        obj = val_parent(n=4, inner=val_n(n=4, x=10))
        result = ValParentType().load_from(obj.serialize().begin_parse())
        result.inner = val_zero()
        with pytest.raises(AssertionError):
            _ = result.serialize()


class TestDeserializeNatParamNonNegative:
    """load_from asserts nat type-level params >= 0."""

    def test_valid_type_arg(self):
        obj = val_n(n=4, x=10)
        result = ValType().load_from(obj.serialize().begin_parse(), 4)
        assert isinstance(result, val_n)

    def test_negative_type_arg_type_info(self):
        """Negative nat type arg to TypeInfo.load_from triggers assertion."""
        b = Builder()
        _ = b.store_uint(1, 1)  # tag for val_n
        with pytest.raises(AssertionError):
            _ = ValType().load_from(b.end_cell().begin_parse(), -1)

    def test_negative_type_arg_constructor(self):
        """Negative nat type arg to constructor load_from triggers assertion."""
        b = Builder()
        _ = b.store_uint(1, 1)  # tag for val_n
        with pytest.raises(AssertionError):
            _ = val_n.load_from(b.end_cell().begin_parse(), -1)


class TestNatFieldAsOutputParam:
    """m:# = NatField ~m — explicit field exposed as output type-level param."""

    def test_roundtrip(self):
        obj = nat_field(m=42)
        result = NatFieldType().load_from(obj.serialize().begin_parse())
        assert result.m == 42

    def test_get_output(self):
        obj = nat_field(m=7)
        assert obj.get_output(0) == 7


class TestEqFieldConstraint:
    """{m:#} n:# { m = n } = EqField m — implicit param constrained to equal a field."""

    def test_roundtrip(self):
        obj = eq_field(m=42, n=42)
        result = EqFieldType().load_from(obj.serialize().begin_parse(), 42)
        assert result.m == 42 and result.n == 42

    def test_serialize_mismatch(self):
        """m != n fails assertion during serialization."""
        obj = eq_field(m=5, n=10)
        with pytest.raises(AssertionError):
            _ = obj.serialize()

    def test_deserialize_mismatch(self):
        """Stream has n=10 but type arg m=5 — constraint fails on load."""
        b = Builder()
        _ = b.store_uint(10, 32)
        with pytest.raises(TlbModelError, match="constraint failed"):
            _ = EqFieldType().load_from(b.end_cell().begin_parse(), 5)


class TestVarWidthBits:
    """Variable-width bits field: {n:#} s:(bits n) = VarBitsField n."""

    def test_roundtrip(self):
        obj = var_bits_field(n=4, s=bitarray("1010"))
        result = VarBitsFieldType().load_from(obj.serialize().begin_parse(), 4)
        assert result.s == bitarray("1010")

    def test_zero_width(self):
        obj = var_bits_field(n=0, s=bitarray())
        result = VarBitsFieldType().load_from(obj.serialize().begin_parse(), 0)
        assert result.s == bitarray()

    def test_wrong_length_on_serialize(self):
        """bits n with n=4 rejects a 2-bit value on serialize."""
        obj = var_bits_field(n=4, s=bitarray("10"))
        with pytest.raises(AssertionError):
            _ = obj.serialize()

    def test_constant_width_roundtrip(self):
        obj = fixed_bits(s=bitarray("10101010"))
        result = FixedBitsType().load_from(obj.serialize().begin_parse())
        assert result.s == bitarray("10101010")

    def test_constant_width_wrong_length(self):
        """Constant-width bits8 rejects wrong length on serialize."""
        obj = fixed_bits(s=bitarray("1"))
        with pytest.raises(AssertionError):
            _ = obj.serialize()


class TestDeserializeConstraints:
    """load_from raises TlbModelError on constraint violations."""

    def test_constrained_invalid_on_load(self):
        """flags=2 in the stream violates { flags <= 1 }."""
        b = Builder()
        _ = b.store_uint(2, 8)
        _ = b.store_uint(0, 32)
        with pytest.raises(TlbModelError, match="constraint failed"):
            _ = ConstrainedType().load_from(b.end_cell().begin_parse())

    def test_ordered_invalid_on_load(self):
        """a=10, b=5 in the stream violates { b >= a }."""
        b = Builder()
        _ = b.store_uint(10, 32)
        _ = b.store_uint(5, 32)
        with pytest.raises(TlbModelError, match="constraint failed"):
            _ = OrderedType().load_from(b.end_cell().begin_parse())

    def test_wrong_type_arg_for_val_zero(self):
        """val_zero expects type_arg_0 == 0, passing 5 triggers TlbModelError."""
        obj = val_zero()
        with pytest.raises(TlbModelError, match="constraint failed"):
            _ = val_zero.load_from(obj.serialize().begin_parse(), 5)
