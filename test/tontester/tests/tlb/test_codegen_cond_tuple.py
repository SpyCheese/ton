"""Tests for generated Python code from TL-B cond_tuple schema."""

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
