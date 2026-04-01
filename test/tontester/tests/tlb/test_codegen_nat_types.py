"""Tests for generated Python code from TL-B nat_types schema."""

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
