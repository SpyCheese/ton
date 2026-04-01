"""Tests for generated Python code from TL-B collisions schema."""

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
