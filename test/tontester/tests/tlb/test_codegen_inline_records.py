"""Tests for generated code with inline records ([field:Type ...])."""

from generated.inline_records import (
    Anon_cons,
    Anon_cons_1,
    Anon_cons_2,
    GenericInlineType,
    Ref_Anon_1,
    RefInlineType,
    WithInlineType,
    generic_inline,
    ref_inline,
    with_inline,
)
from tlb.object import Ref, UintTypeConstructor


class TestWithInline:
    """Simple inline record: inner:[a:uint32 b:uint8]."""

    def test_roundtrip(self):
        inner = Anon_cons(a=42, b=7)
        obj = with_inline(inner=inner)
        result = WithInlineType().load_from(obj.serialize().begin_parse())
        assert isinstance(result, with_inline)
        assert isinstance(result.inner, Anon_cons)
        assert result.inner.a == 42
        assert result.inner.b == 7

    def test_bit_count(self):
        inner = Anon_cons(a=0, b=0)
        assert with_inline(inner=inner).serialize().begin_parse().remaining_bits == 40  # 32 + 8


class TestRefInline:
    """Cell ref to inline record: inner:^[x:uint32 y:uint32]."""

    def test_roundtrip(self):
        inner = Anon_cons_1(x=100, y=200)
        obj = ref_inline(inner=Ref_Anon_1(inner))
        result = RefInlineType().load_from(obj.serialize().begin_parse())
        assert isinstance(result, ref_inline)
        assert result.inner.ref.x == 100
        assert result.inner.ref.y == 200

    def test_structure(self):
        """Outer cell has 0 data bits + 1 ref, inner cell has 64 bits."""
        inner = Anon_cons_1(x=0, y=0)
        cell = ref_inline(inner=Ref_Anon_1(inner)).serialize()
        cs = cell.begin_parse()
        assert cs.remaining_bits == 0
        assert cs.remaining_refs == 1
        ref_cs = cs.load_ref().begin_parse()
        assert ref_cs.remaining_bits == 64


class TestGenericInline:
    """Generic inline record: {T:Type} ^([{X:Type} a:X] T)."""

    def test_roundtrip(self):
        uint32_ti = UintTypeConstructor(32)
        from generated.inline_records import Anon_2Type

        anon_ti = Anon_2Type[int].instantiate(uint32_ti)
        inner_val = Anon_cons_2[int](uint32_ti, a=42)
        obj = generic_inline[int](uint32_ti, inner=Ref(anon_ti, inner_val))
        result = GenericInlineType[int]().load_from(obj.serialize().begin_parse(), uint32_ti)
        assert isinstance(result, generic_inline)
        deref = result.inner.ref
        assert isinstance(deref, Anon_cons_2)
        assert deref.a == 42
