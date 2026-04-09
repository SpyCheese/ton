"""Python code generator for TL-B schemas.

Generates Python dataclasses with serialize/deserialize methods from
the resolved sema IR. Uses the runtime support library in tlb.object.
"""

from ..sema.types import NatLiteral, ResolvedConstructor, ResolvedType
from ..simplify_config import SimplifyConfig
from .context import PyContext
from .source_builder import SourceBuilder
from .type_generator import TypeGenerator


def _anonymous_constructor_name(type_name: str, c: ResolvedConstructor) -> str:
    literals = [v for v in c.nat_param_values if isinstance(v, NatLiteral)]
    if len(c.parent_type.type_level_params) == 1 and len(literals) == 1:
        return f"{type_name}_{literals[0].value}"
    return f"{type_name}_cons"


def generate_python(types: list[ResolvedType], simplify: SimplifyConfig | None = None) -> str:
    """Generate Python source code for a list of resolved types."""
    config = simplify or SimplifyConfig.none()
    ctx = PyContext(simplify=config)

    for t in types:
        if t.is_builtin or not t.constructors:
            continue
        type_name = t.name or "Anon"
        _ = ctx.scope.bind(t, type_name)
        for c in t.constructors:
            _ = ctx.scope.bind(c, c.name or _anonymous_constructor_name(type_name, c))

    # Pre-bind all field names so cross-type inference chain lookups work.
    type_generators: list[TypeGenerator] = []
    for t in types:
        if t.is_builtin or not t.constructors:
            continue
        tg = TypeGenerator(ctx, t)
        type_generators.append(tg)

    body = SourceBuilder()
    for tg in type_generators:
        tg.generate(body)

    sb = SourceBuilder()
    ctx.emit_imports(sb)
    sb.blank()
    sb.line(body.build().rstrip())
    sb.blank()
    return sb.build()
