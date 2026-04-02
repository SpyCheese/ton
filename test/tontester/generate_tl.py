from pathlib import Path

from tlb.generator.py import generate_python
from tlb.generator.sema import analyze_text
from tlb.generator.simplify_config import SimplifyConfig

import tl


def generate_tlb_python(
    schema_path: Path, out_path: Path, simplify: SimplifyConfig | None = None
) -> None:
    """Generate Python code from a TL-B schema file."""
    text = schema_path.read_text()
    _, types = analyze_text(text)
    code = generate_python(types, simplify=simplify)
    _ = out_path.write_text(code)
    print(f"  {schema_path.name} -> {out_path.name}")


if __name__ == "__main__":
    repo_root = Path(__file__).resolve().parents[2]
    schemas_root = repo_root / "tl/generate/scheme"

    schemas = [
        schemas_root / "lite_api.tl",
        schemas_root / "ton_api.tl",
        schemas_root / "tonlib_api.tl",
    ]
    out_directory = repo_root / "test/tontester/src/tonapi"

    for schema in schemas:
        tl.generate(schema, out_directory)

    tl.generate(
        repo_root / "test/tontester/tests/tl/test_schema.tl",
        repo_root / "test/tontester/tests/tl/generated",
    )

    # TL-B codegen
    print("Generating TL-B Python:")
    tlb_schemas = repo_root / "test/tontester/tests/tlb/schemas"
    tlb_out = repo_root / "test/tontester/tests/tlb/generated"
    tlb_out.mkdir(parents=True, exist_ok=True)
    simplify_all = SimplifyConfig.all()
    for schema_file in sorted(tlb_schemas.glob("*.tlb")):
        out_file = tlb_out / (schema_file.stem + ".py")
        config = simplify_all if schema_file.stem == "simplify_maybe" else None
        generate_tlb_python(schema_file, out_file, simplify=config)

    # Generate block.tlb
    block_tlb = repo_root / "crypto/block/block.tlb"
    block_out = tlb_out / "block.py"
    generate_tlb_python(block_tlb, block_out)
