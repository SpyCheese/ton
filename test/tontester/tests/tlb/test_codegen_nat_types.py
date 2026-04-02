"""Tests for generated Python code from TL-B nat_types schema."""

import pytest
from generated.nat_types import (
    Hash32Type,
    HashWidthType,
    LeqTestType,
    LtNestedType,
    LtParamType,
    LtTestType,
    MultiNatType,
    ZeroWidthType,
    hash32,
    hash_width,
    just,
    leq_test,
    lt_nested,
    lt_param,
    lt_test,
    multi_nat,
    nothing,
    zero_width,
)
from tlb.object import TlbModelError, UintTypeConstructor

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

    def test_zero_width_roundtrip(self):
        r = ZeroWidthType().load_from(zero_width(n=0).serialize().begin_parse())
        assert r.n == 0

    def test_zero_width_bit_count(self):
        assert zero_width(n=0).serialize().begin_parse().remaining_bits == 0

    def test_lt_param_roundtrip(self):
        obj = lt_param(4, x=3)
        result = LtParamType().load_from(obj.serialize().begin_parse(), 4)
        assert result.x == 3
        assert result.n == 4

    def test_lt_param_zero_rejected(self):
        """#< n with n=0 is rejected at runtime by the implicit constraint."""
        with pytest.raises(TlbModelError, match="constraint failed"):
            _ = LtParamType().load_from(lt_param(1, x=0).serialize().begin_parse(), 0)

    def test_lt_nested_roundtrip(self):
        """#< n nested inside Maybe — round-trips with just value."""
        ti = UintTypeConstructor((4 - 1).bit_length())
        obj = lt_nested(4, x=just(ti, value=3))
        result = LtNestedType().load_from(obj.serialize().begin_parse(), 4)
        assert isinstance(result.x, just)
        assert result.x.value == 3

    def test_lt_nested_nothing(self):
        """#< n nested inside Maybe — round-trips with nothing."""
        obj = lt_nested(4, x=nothing())
        result = LtNestedType().load_from(obj.serialize().begin_parse(), 4)
        assert isinstance(result.x, nothing)

    def test_lt_nested_zero_rejected(self):
        """#< n nested inside Maybe — n=0 rejected before reading."""
        with pytest.raises(TlbModelError, match="constraint failed"):
            _ = LtNestedType().load_from(lt_nested(1, x=nothing()).serialize().begin_parse(), 0)
