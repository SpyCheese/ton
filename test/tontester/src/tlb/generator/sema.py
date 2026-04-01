"""Top-level semantic analysis orchestrator.

Phases:
1. Register types (names, arities, param kinds)
2. Resolve constructors (expressions, fields)
3. Compute tags (CRC32 auto-tags)
4. Build match trees (bit dispatch + constraint disambiguation)
5. Classify inference capability
6. Compute deserialization plans
7. Classify types (enum, typedef)
"""

from __future__ import annotations

from .ast_nodes import Constructor, Schema
from .lexer import Lexer
from .parser import Parser
from .sema_deser import build_deser_plan, classify_inference
from .sema_match import build_match_tree
from .sema_resolve import TypeRegistry, check_type_arities, register_types, resolve_constructors
from .sema_tags import assign_tags
from .sema_types import ResolvedType


def analyze(schema: Schema) -> tuple[TypeRegistry, list[ResolvedType]]:
    """Run semantic analysis on a parsed schema.

    Returns the type registry and a list of user-defined resolved types.
    """
    registry = TypeRegistry()

    # Phase 1: Register all types
    register_types(schema, registry)

    # Phase 2: Resolve constructors
    resolve_constructors(schema, registry)

    user_types = registry.all_user_types()

    # Phase 2b: Check type application arities (must happen after all types are registered)
    check_type_arities(user_types)

    # Phase 3: Compute tags
    ast_by_type: dict[str, list[Constructor]] = {}
    for c in schema.constructors:
        ast_by_type.setdefault(c.result_type, []).append(c)
    for rt in user_types:
        ast_constructors = ast_by_type.get(rt.name, [])
        if ast_constructors:
            assign_tags(rt, ast_constructors)

    # Phase 4: Build match trees (bit dispatch + constraint disambiguation)
    for rt in user_types:
        rt.match_tree = build_match_tree(rt)

    # Phase 5: Classify inference
    for rt in user_types:
        rt.inference = classify_inference(rt)

    # Phase 6: Compute deser plans
    for rt in user_types:
        for rc in rt.constructors:
            _ = build_deser_plan(rc)

    # Phase 7: Classify types
    for rt in user_types:
        _classify_type(rt)

    return registry, user_types


def analyze_text(text: str) -> tuple[TypeRegistry, list[ResolvedType]]:
    """Convenience: lex, parse, and analyze a TL-B schema string."""
    tokens = Lexer(text).tokenize()
    schema = Parser(tokens).parse()
    return analyze(schema)


def _classify_type(rt: ResolvedType) -> None:
    """Set is_enum, is_typedef, is_special flags."""
    if rt.constructors:
        rt.is_enum = all(len(c.fields) == 0 for c in rt.constructors)
        rt.is_special = rt.constructors[0].is_special  # all agree (checked in sema_resolve)

    if len(rt.constructors) == 1:
        c = rt.constructors[0]
        if (
            len(c.fields) == 1
            and len(c.params) == 0
            and c.fields[0].name is None
            and c.tag_len == 0
        ):
            rt.is_typedef = True
            rt.typedef_target = c.fields[0].type_expr
