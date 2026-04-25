"""Python code generator for TL-B schemas.

Generates Python dataclasses with serialize/deserialize methods from
the resolved sema IR. Uses the runtime support library in tlb.object.
"""

from collections.abc import Mapping

from ..identity_key import IdentityKey
from ..sema.types import Module, NatLiteral, ResolvedConstructor, ResolvedType
from ..simplify_config import SimplifyConfig
from .context import PyContext
from .manifest import PyManifest
from .source_builder import SourceBuilder
from .type_generator import TypeGenerator


def _anonymous_constructor_name(type_name: str, c: ResolvedConstructor) -> str:
    literals = [v for v in c.nat_param_values if isinstance(v, NatLiteral)]
    if len(c.parent_type.type_level_params) == 1 and len(literals) == 1:
        return f"{type_name}_{literals[0].value}"
    return f"{type_name}_cons"


def generate_python(
    types: list[ResolvedType],
    *,
    current_module: Module,
    py_module: str,
    simplify: SimplifyConfig | None = None,
    foreign_manifests: Mapping[Module, PyManifest] | None = None,
) -> tuple[str, PyManifest]:
    """Generate Python source code for a list of resolved types.

    `current_module` is the sema Module the types belong to; it is what
    `t.origin_module` is compared against to decide whether `t` is local or
    foreign. `py_module` is the dotted Python import path the produced file
    will live at (e.g. "block.generated"). `foreign_manifests` maps each
    imported sema Module to its previously captured PyManifest. References
    to types/constructors from those modules are bound and imported on
    demand during codegen.
    """
    config = simplify or SimplifyConfig.none()
    ctx = PyContext(
        current_module=current_module,
        simplify=config,
        foreign_manifests=foreign_manifests,
    )
    manifest = PyManifest(py_module=py_module)

    for t in types:
        if t.is_builtin or not t.constructors:
            continue
        type_name = t.name or "Anon"
        if t.has_unnamed_sole_constructor:
            # Unnamed sole constructor: use the type name directly, don't bind type
            cons = t.constructors[0]
            cname = ctx.scope.bind(cons, type_name)
            manifest.constructor_names[IdentityKey(cons)] = cname
        else:
            tname = ctx.scope.bind(t, type_name)
            manifest.type_names[IdentityKey(t)] = tname
            for c in t.constructors:
                cname = ctx.scope.bind(
                    c, c.name or _anonymous_constructor_name(type_name, c)
                )
                manifest.constructor_names[IdentityKey(c)] = cname

    # Pre-bind all field names so cross-type inference chain lookups work.
    type_generators: list[TypeGenerator] = []
    for t in types:
        if t.is_builtin or not t.constructors:
            continue
        tg = TypeGenerator(ctx, t)
        type_generators.append(tg)

    # Capture field bindings into the manifest now that all constructors are
    # registered (their child scopes hold the field bindings).
    for t in types:
        if t.is_builtin or not t.constructors:
            continue
        for c in t.constructors:
            cons_scope = ctx.get_constructor(c).scope
            for f in c.fields:
                if cons_scope.is_bound(f):
                    manifest.field_names[IdentityKey(f)] = cons_scope.lookup(f)

    body = SourceBuilder()
    for tg in type_generators:
        tg.generate(body)

    sb = SourceBuilder()
    ctx.emit_imports(sb)
    sb.blank()
    sb.line(body.build().rstrip())
    sb.blank()
    return sb.build(), manifest
