from tlb.generator.identity_key import IdentityKey
from tlb.generator.py import generate_python
from tlb.generator.sema import analyze_text
from tlb.generator.sema.types import Module

_TEST_MODULE = Module("<test>")


def _gen(text: str, *, py_module: str = "generated.test"):
    analyzed = analyze_text(text, current_module=_TEST_MODULE)
    code, manifest = generate_python(
        analyzed.types, current_module=analyzed.module, py_module=py_module
    )
    return analyzed, code, manifest


class TestPyManifest:
    def test_records_py_module(self):
        _, _, manifest = _gen("foo$_ = Foo;", py_module="my.module")
        assert manifest.py_module == "my.module"

    def test_captures_type_name(self):
        analyzed, _, manifest = _gen("foo$_ x:uint32 = Foo;")
        foo = next(t for t in analyzed.types if t.name == "Foo")
        assert manifest.type_names[IdentityKey(foo)] == "Foo"

    def test_captures_constructor_names(self):
        analyzed, _, manifest = _gen(
            "bool_false$0 = Bool; bool_true$1 = Bool;"
        )
        bool_t = next(t for t in analyzed.types if t.name == "Bool")
        names = sorted(manifest.constructor_names[IdentityKey(c)] for c in bool_t.constructors)
        assert names == ["bool_false", "bool_true"]

    def test_unnamed_sole_constructor_only_constructor_in_manifest(self):
        # _ x:uint32 = Foo; — has no constructor name; codegen uses the type
        # name for the constructor and skips the type binding.
        analyzed, _, manifest = _gen("_ x:uint32 = Foo;")
        foo = next(t for t in analyzed.types if t.name == "Foo")
        assert IdentityKey(foo) not in manifest.type_names
        assert manifest.constructor_names[IdentityKey(foo.constructors[0])] == "Foo"

    def test_collision_with_reserved_gets_suffixed(self):
        # 'Cell' is in NameScope's _RESERVED — a user type named Cell would be
        # remapped. (This is a contrived case since 'Cell' is also a builtin
        # alias, but it exercises that the manifest reflects the actual chosen
        # name, not the source identifier.)
        analyzed, _, manifest = _gen("foo$_ x:uint32 = Slice;")
        slice_t = next(t for t in analyzed.types if t.name == "Slice")
        # 'Slice' is reserved — manifest must record whatever NameScope picked.
        bound = manifest.type_names[IdentityKey(slice_t)]
        assert bound != "Slice"
        assert bound.startswith("Slice")

    def test_foreign_type_imported(self):
        block = analyze_text(
            "currency$_ amount:uint64 = Currency;",
            current_module=Module("block"),
        )
        _, block_manifest = generate_python(block.types, current_module=block.module, py_module="block.generated")
        consumer = analyze_text(
            "//@import block.tlb\nwallet$_ bal:Currency = Wallet;",
            current_module=Module("user"),
            imports={"block.tlb": block},
        )
        code, _ = generate_python(
            consumer.types,
            current_module=consumer.module,
            py_module="user.generated",
            foreign_manifests={block.module: block_manifest},
        )
        # Sole-cons named foreign type: import both type alias and constructor.
        assert "from block.generated import" in code
        assert "Currency" in code
        assert "currency" in code
        # No alias when names don't collide.
        assert "as Currency" not in code
        assert "as currency" not in code

    def test_foreign_multi_cons_imports_type_and_typeinfo(self):
        block = analyze_text(
            "bool_false$0 = Bool; bool_true$1 = Bool;",
            current_module=Module("block"),
        )
        _, block_manifest = generate_python(block.types, current_module=block.module, py_module="block.generated")
        consumer = analyze_text(
            "//@import block.tlb\nflag$_ b:Bool = Flag;",
            current_module=Module("user"),
            imports={"block.tlb": block},
        )
        code, _ = generate_python(
            consumer.types,
            current_module=consumer.module,
            py_module="user.generated",
            foreign_manifests={block.module: block_manifest},
        )
        # Multi-cons foreign: imports `Bool` and `BoolType`, but not constructors.
        assert "from block.generated import Bool, BoolType" in code
        assert "bool_false" not in code
        assert "bool_true" not in code

    def test_foreign_constructor_collision_aliased(self):
        block = analyze_text(
            "foo$_ x:uint32 = Block;",
            current_module=Module("block"),
        )
        _, block_manifest = generate_python(block.types, current_module=block.module, py_module="block.generated")
        # Consumer has its own `foo` constructor for an unrelated local type;
        # importing `Block` triggers binding `foo` from the foreign module too.
        consumer = analyze_text(
            (
                "//@import block.tlb\n"
                "foo$_ y:uint16 = Local;\n"
                "user$_ b:Block = User;"
            ),
            current_module=Module("user"),
            imports={"block.tlb": block},
        )
        code, _ = generate_python(
            consumer.types,
            current_module=consumer.module,
            py_module="user.generated",
            foreign_manifests={block.module: block_manifest},
        )
        # Local `foo` keeps the bare name; foreign `foo` is aliased.
        assert "foo as foo_1" in code or "foo as foo_2" in code

    def test_foreign_generated_code_compiles(self):
        block = analyze_text(
            "currency$_ amount:uint64 = Currency;",
            current_module=Module("block"),
        )
        _, block_manifest = generate_python(block.types, current_module=block.module, py_module="block.generated")
        consumer = analyze_text(
            "//@import block.tlb\nwallet$_ bal:Currency = Wallet;",
            current_module=Module("user"),
            imports={"block.tlb": block},
        )
        code, _ = generate_python(
            consumer.types,
            current_module=consumer.module,
            py_module="user.generated",
            foreign_manifests={block.module: block_manifest},
        )
        # Smoke test: the generated source must be syntactically valid Python.
        _ = compile(code, "<consumer>", "exec")

    def test_unused_foreign_module_no_import(self):
        block = analyze_text("foo$_ x:uint32 = Foo;", current_module=Module("block"))
        _, block_manifest = generate_python(block.types, current_module=block.module, py_module="block.generated")
        # Consumer imports block but never references any of its types.
        consumer = analyze_text(
            "//@import block.tlb\nlocal$_ x:uint32 = Local;",
            current_module=Module("user"),
            imports={"block.tlb": block},
        )
        code, _ = generate_python(
            consumer.types,
            current_module=consumer.module,
            py_module="user.generated",
            foreign_manifests={block.module: block_manifest},
        )
        assert "from block.generated" not in code

    def test_separate_modules_get_separate_manifests(self):
        block = analyze_text(
            "currency$_ amount:uint64 = Currency;",
            current_module=Module("block"),
        )
        other = analyze_text("foo$_ x:uint32 = Foo;", current_module=Module("other"))
        _, block_manifest = generate_python(block.types, current_module=block.module, py_module="block.generated")
        _, other_manifest = generate_python(other.types, current_module=other.module, py_module="other.generated")
        currency = next(t for t in block.types if t.name == "Currency")
        foo = next(t for t in other.types if t.name == "Foo")
        # Each manifest contains only its own module's bindings.
        assert IdentityKey(currency) in block_manifest.type_names
        assert IdentityKey(foo) not in block_manifest.type_names
        assert IdentityKey(foo) in other_manifest.type_names
        assert IdentityKey(currency) not in other_manifest.type_names
        assert block_manifest.py_module == "block.generated"
        assert other_manifest.py_module == "other.generated"
