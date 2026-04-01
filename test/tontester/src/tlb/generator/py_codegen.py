"""Python code generator for TL-B schemas.

Generates Python dataclasses with serialize/deserialize methods from
the resolved sema IR. Uses the runtime support library in tlb.object.
"""

from __future__ import annotations

from .py_context import PyContext
from .py_emit import TypeStrategy, nat_to_python, strategy_for
from .sema_types import (
    CheckConstraint,
    MatchBit,
    MatchConstraint,
    MatchConstructor,
    MatchTag,
    MatchTree,
    ParamDef,
    ParamKind,
    ResolvedConstructor,
    ResolvedType,
    TypeApply,
    TypeParamRef,
)
from .source_builder import SourceBuilder


def generate_python(types: list[ResolvedType]) -> str:
    """Generate Python source code for a list of resolved types."""
    ctx = PyContext()

    # Pre-bind all names
    for t in types:
        if t.is_builtin or not t.constructors:
            continue
        _ = ctx.scope.bind(t, t.name)
        for c in t.constructors:
            _ = ctx.scope.bind(c, c.name or f"_{t.name}_cons")

    # Generate body
    body = SourceBuilder()
    for t in types:
        if t.is_builtin or not t.constructors:
            continue
        TypeGenerator(ctx, t).generate(body)

    # Assemble: imports, ref wrappers, type code
    sb = SourceBuilder()
    ctx.emit_imports(sb)
    for wrapper_code in ctx.ref_wrapper_code:
        sb.line(wrapper_code)
    sb.blank()
    sb.line(body.build().rstrip())
    sb.blank()
    return sb.build()


class TypeGenerator:
    ctx: PyContext
    t: ResolvedType

    def __init__(self, ctx: PyContext, t: ResolvedType) -> None:
        self.ctx = ctx
        self.t = t

    def generate(self, sb: SourceBuilder) -> None:
        from .sema_types import ParamKind

        for c in self.t.constructors:
            ConstructorGenerator(self.ctx, c).generate(sb)
            sb.blank()
            sb.blank()

        type_name = self.ctx.scope.lookup(self.t)

        # Collect type-level generic params
        type_vars = [f"_{pk.name}" for pk in self.t.param_kinds if pk == ParamKind.TYPE]
        # Use actual param names from first constructor for consistency
        if self.t.constructors:
            type_vars = []
            for pk, p_kind in zip(
                self.t.constructors[0].params, [pk for pk in self.t.param_kinds], strict=False
            ):
                if p_kind == ParamKind.TYPE:
                    type_vars.append(pk.name)

        is_generic = len(type_vars) > 0
        generic_suffix = f"[{', '.join(type_vars)}]" if is_generic else ""

        # Type alias — each constructor gets only the type vars it actually uses
        def _cons_type(c: ResolvedConstructor) -> str:
            name = self.ctx.scope.lookup(c)
            cons_vars = _constructor_used_type_vars(c)
            if cons_vars:
                return f"{name}[{', '.join(cons_vars)}]"
            return name

        if len(self.t.constructors) > 1:
            cons_names = " | ".join(_cons_type(c) for c in self.t.constructors)
            sb.line(f"type {type_name}{generic_suffix} = {cons_names}")
        else:
            sb.line(f"type {type_name}{generic_suffix} = {_cons_type(self.t.constructors[0])}")
        sb.blank()
        sb.blank()

        self._generate_type_info(sb, type_vars)
        sb.blank()
        sb.blank()

    def _generate_type_info(self, sb: SourceBuilder, type_vars: list[str]) -> None:
        self.ctx.use("final", "TypeInfo", "Builder", "Slice", "override")
        type_name = self.ctx.scope.lookup(self.t)
        info_name = f"{type_name}Type"

        is_generic = len(type_vars) > 0
        generic_suffix = f"[{', '.join(type_vars)}]" if is_generic else ""

        # TypeInfo args: TypeInfo[T] for each type param
        ti_args = ", ".join(f"TypeInfo[{v}]" for v in type_vars)
        if is_generic:
            sb.line("@final")
            sb.line(
                f"class {info_name}{generic_suffix}(TypeInfo[{type_name}{generic_suffix}, {ti_args}]):"
            )
        else:
            sb.line("@final")
            sb.line(f"class {info_name}(TypeInfo[{type_name}]):")

        with sb.block():
            # serialize_value
            sb.line("@override")
            sb.line(
                f"def serialize_value(self, value: {type_name}{generic_suffix}, builder: Builder) -> None:"
            )
            with sb.block():
                sb.line("value.serialize_to(builder)")
            sb.blank()

            # load_from
            params = ["cs: Slice"]
            ti_field_names: list[str] = []
            for v in type_vars:
                ti_name = f"_t{v}"
                params.append(f"{ti_name}: TypeInfo[{v}]")
                ti_field_names.append(ti_name)
            params_str = ", ".join(params)

            sb.line("@override")
            sb.line(f"def load_from(self, {params_str}) -> {type_name}{generic_suffix}:")
            with sb.block():
                assert self.t.match_tree is not None
                if len(self.t.constructors) > 1:
                    sb.line("probe = cs.copy()")
                MatchTreeGenerator(self.ctx, ti_field_names).generate(self.t.match_tree, sb)


class ConstructorGenerator:
    ctx: PyContext
    c: ResolvedConstructor

    def __init__(self, ctx: PyContext, c: ResolvedConstructor) -> None:
        self.ctx = ctx
        self.c = c

    def generate(self, sb: SourceBuilder) -> None:
        self.ctx.use("final", "dataclass", "TLBRecord", "Builder", "Slice", "override")
        cls_name = self.ctx.scope.lookup(self.c)

        local = self.ctx.scope.child()

        # Collect ALL type params and their TypeInfo variable names
        from .py_emit import TypeParamStrategy
        from .sema_types import ParamKind

        all_type_params: dict[ParamDef, tuple[str, str]] = {}  # param -> (type_var, ti_field)
        type_param_vars: dict[ParamDef, str] = {}
        for p in self.c.params:
            if p.kind == ParamKind.TYPE:
                var_name = local.bind(p, f"_t{p.name}")
                all_type_params[p] = (p.name, var_name)
                type_param_vars[p] = var_name

        # Build field strategies
        field_info: list[tuple[str, TypeStrategy]] = []
        for f in self.c.fields:
            name = local.bind(f, f.name or "_field")
            strat = strategy_for(f.type_expr, self.ctx, local, type_param_vars)
            field_info.append((name, strat))

        # Determine which type params are actually used by fields
        from .py_emit import UserTypeStrategy as UTS

        used_params: set[ParamDef] = set()
        for _, strat in field_info:
            if isinstance(strat, TypeParamStrategy):
                used_params.add(strat.param)
            elif isinstance(strat, UTS):
                # Type params used as args to generic user types
                for p in all_type_params:
                    if all_type_params[p][1] in strat.ti_args:
                        used_params.add(p)
        # Only used params become class generics
        type_params: list[tuple[ParamDef, str]] = [
            (p, all_type_params[p][0]) for p in all_type_params if p in used_params
        ]
        is_generic = len(type_params) > 0

        # Class header
        if is_generic:
            self.ctx.use("TypeInfo")
            generic_vars = ", ".join(name for _, name in type_params)
            sb.line("@final")
            sb.line("@dataclass")
            sb.line(f"class {cls_name}[{generic_vars}](TLBRecord):")
        else:
            sb.line("@final")
            sb.line("@dataclass")
            sb.line(f"class {cls_name}(TLBRecord):")

        with sb.block():
            # TypeInfo fields for used generic params
            for p, type_var in type_params:
                ti_field = all_type_params[p][1]
                sb.line(f"{ti_field}: TypeInfo[{type_var}]")
            # Regular fields
            for name, strat in field_info:
                sb.line(f"{name}: {strat.py_type()}")

            sb.blank()
            self._generate_serialize_to(sb, field_info)
            sb.blank()
            self._generate_load_from(sb, cls_name, field_info, type_params, all_type_params)

    def _generate_serialize_to(
        self, sb: SourceBuilder, field_info: list[tuple[str, TypeStrategy]]
    ) -> None:
        sb.line("@override")
        sb.line("def serialize_to(self, builder: Builder) -> None:")
        with sb.block():
            if not field_info and self.c.tag_len == 0:
                sb.line("pass")
                return
            for bit in self.c.tag_bits:
                sb.line(f"_ = builder.store_bit({bit})")
            for name, strat in field_info:
                strat.emit_store(f"self.{name}", "builder", sb)

    def _generate_load_from(
        self,
        sb: SourceBuilder,
        cls_name: str,
        field_info: list[tuple[str, TypeStrategy]],
        type_params: list[tuple[ParamDef, str]],
        all_type_params: dict[ParamDef, tuple[str, str]],
    ) -> None:
        is_generic = len(type_params) > 0

        if not field_info and self.c.tag_len == 0 and not is_generic:
            sb.line("@classmethod")
            sb.line(f"def load_from(cls, _cs: Slice) -> {cls_name}:")
            with sb.block():
                sb.line("return cls()")
            return

        # Build parameter list: cs + TypeInfo args for used generic params
        params = ["cs: Slice"]
        for p, type_var in type_params:
            _, ti_field = all_type_params[p]
            params.append(f"{ti_field}: TypeInfo[{type_var}]")
        params_str = ", ".join(params)

        if is_generic:
            generic_vars = ", ".join(name for _, name in type_params)
            sb.line("@classmethod")
            sb.line(f"def load_from(cls, {params_str}) -> {cls_name}[{generic_vars}]:")
        else:
            sb.line("@classmethod")
            sb.line(f"def load_from(cls, {params_str}) -> {cls_name}:")

        with sb.block():
            for bit in self.c.tag_bits:
                sb.line(f"assert cs.load_bit() == {bit}")
            ctor_args: list[str] = []
            # Pass TypeInfo fields to constructor
            for p, _ in type_params:
                _, ti_field = all_type_params[p]
                ctor_args.append(f"{ti_field}")
            # Deserialize regular fields
            for name, strat in field_info:
                strat.emit_load(name, "cs", sb)
                ctor_args.append(f"{name}={name}")
            sb.line(f"return cls({', '.join(ctor_args)})")


class MatchTreeGenerator:
    ctx: PyContext
    ti_args: list[str]  # TypeInfo variable names to pass to constructors

    def __init__(self, ctx: PyContext, ti_args: list[str] | None = None) -> None:
        self.ctx = ctx
        self.ti_args = ti_args or []

    def generate(self, tree: MatchTree, sb: SourceBuilder) -> None:
        if isinstance(tree, MatchConstructor):
            cons = tree.constructor
            cons_name = self.ctx.scope.lookup(cons)
            cons_ti = _constructor_ti_args(cons, self.ti_args)
            # Add type annotation for generic constructors so pyright can infer
            cons_type_vars = _constructor_used_type_vars(cons)
            if cons_type_vars:
                cons_name = f"{cons_name}[{', '.join(cons_type_vars)}]"
            if cons_ti:
                args = ", ".join(["cs"] + cons_ti)
                sb.line(f"return {cons_name}.load_from({args})")
            else:
                sb.line(f"return {cons_name}.load_from(cs)")
        elif isinstance(tree, MatchBit):
            sb.line("if probe.load_bit() == 0:")
            with sb.block():
                self.generate(tree.zero, sb)
            sb.line("else:")
            with sb.block():
                self.generate(tree.one, sb)
        elif isinstance(tree, MatchTag):
            for bit in tree.bits:
                sb.line(f"_ = probe.load_bit()  # tag bit {bit}")
            self.generate(tree.child, sb)
        elif isinstance(tree, MatchConstraint):
            cond = _check_constraint_to_python(tree.condition)
            sb.line(f"if {cond}:")
            with sb.block():
                self.generate(tree.if_true, sb)
            sb.line("else:")
            with sb.block():
                self.generate(tree.if_false, sb)
        else:
            self.ctx.use("TlbModelError")
            sb.line("raise TlbModelError('no matching constructor')")


def _constructor_ti_args(cons: ResolvedConstructor, type_ti_args: list[str]) -> list[str]:
    """Map type-level TypeInfo args to a constructor's load_from args.

    Only includes TI args for type params that the constructor actually uses
    in its fields (directly or nested in type applications).
    """
    if not type_ti_args:
        return []

    used_vars = set(_constructor_used_type_vars(cons))
    if not used_vars:
        return []

    # Map type-level positions to constructor params via result_param_exprs
    parent = cons.parent_type
    result: list[str] = []
    ti_idx = 0
    for pos in range(parent.arity):
        if parent.param_kinds[pos] == ParamKind.TYPE:
            expr = cons.result_param_exprs.get(pos)
            if isinstance(expr, TypeParamRef) and expr.param.name in used_vars:
                result.append(type_ti_args[ti_idx])
            ti_idx += 1

    return result


def _constructor_used_type_vars(cons: ResolvedConstructor) -> list[str]:
    """Get the type variable names used in a constructor's fields (direct or nested)."""
    result: list[str] = []
    seen: set[str] = set()
    _collect_type_vars(cons, seen, result)
    return result


def _collect_type_vars(cons: ResolvedConstructor, seen: set[str], result: list[str]) -> None:
    """Collect type var names from all field type expressions."""
    for f in cons.fields:
        _collect_type_vars_from_expr(f.type_expr, seen, result)


def _collect_type_vars_from_expr(expr: object, seen: set[str], result: list[str]) -> None:
    """Recursively find TypeParamRef names in a type expression."""
    if isinstance(expr, TypeParamRef):
        if expr.param.name not in seen:
            result.append(expr.param.name)
            seen.add(expr.param.name)
    elif isinstance(expr, TypeApply):
        for arg in expr.arguments:
            _collect_type_vars_from_expr(arg, seen, result)


def _check_constraint_to_python(cc: CheckConstraint) -> str:
    from .ast_nodes import CompareOp

    left = nat_to_python(cc.left)
    right = nat_to_python(cc.right)
    op_map = {
        CompareOp.EQ: "==",
        CompareOp.LT: "<",
        CompareOp.LE: "<=",
        CompareOp.GT: ">",
        CompareOp.GE: ">=",
    }
    return f"{left} {op_map.get(cc.op, '==')} {right}"
