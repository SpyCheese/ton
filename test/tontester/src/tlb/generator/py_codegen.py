"""Python code generator for TL-B schemas.

Generates Python dataclasses with serialize/deserialize methods from
the resolved sema IR. Uses the runtime support library in tlb.object.
"""

from .ast_nodes import CompareOp
from .identity_key import IdentityKey
from .name_scope import NameScope
from .py_context import PyContext
from .py_emit import NatExpr, StrategyBuilder, TypeStrategy
from .sema_types import (
    BindOutputParam,
    BindParam,
    CheckConstraint,
    InferenceStep,
    MatchBit,
    MatchConstraint,
    MatchConstructor,
    MatchFail,
    MatchTag,
    MatchTree,
    NatParamDef,
    NatTypeArg,
    ParamDef,
    ParamKind,
    ReadField,
    TypeParamDef,
    ResolvedConstructor,
    ResolvedField,
    ResolvedType,
    SolveConstraint,
    TypeLevelParam,
    TypeParamRef,
)
from .source_builder import SourceBuilder

_COMPARE_OP_STR: dict[CompareOp, str] = {
    CompareOp.EQ: "==",
    CompareOp.LT: "<",
    CompareOp.LE: "<=",
    CompareOp.GT: ">",
    CompareOp.GE: ">=",
}


def generate_python(types: list[ResolvedType]) -> str:
    """Generate Python source code for a list of resolved types."""
    ctx = PyContext()

    for t in types:
        if t.is_builtin or not t.constructors:
            continue
        type_name = t.name or "Anon"
        _ = ctx.scope.bind(t, type_name)
        for c in t.constructors:
            _ = ctx.scope.bind(c, c.name or f"{type_name}_cons")

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
    for wrapper_code in ctx.ref_wrapper_code:
        sb.line(wrapper_code)
    sb.blank()
    return sb.build()


class TypeGenerator:
    ctx: PyContext
    t: ResolvedType
    scope: NameScope
    type_vars: list[str]
    cons_generators: list[ConstructorGenerator]

    def __init__(self, ctx: PyContext, t: ResolvedType) -> None:
        self.ctx = ctx
        self.t = t

        self.bind_names()

    def bind_names(self) -> None:
        """Pre-bind type-level and constructor field names (must happen before codegen)."""
        self.scope = self.ctx.scope.child()
        self.ctx.set_type_scope(self.t, self.scope)
        self.type_vars = []
        for tlp in self.t.type_level_params:
            if tlp.is_output:
                continue
            if tlp.kind == ParamKind.TYPE:
                nice_name = f"T{tlp.position}"
                for c in self.t.constructors:
                    expr = c.result_param_exprs.get(tlp.position)
                    if isinstance(expr, TypeParamRef):
                        nice_name = expr.param.name
                        break
                type_var = self.scope.bind(tlp.type_var, nice_name)
                _ = self.scope.bind(tlp, f"_t{type_var}")
                self.type_vars.append(type_var)
            else:
                _ = self.scope.bind(tlp, f"_type_arg_{tlp.position}")

        self.cons_generators = []
        for c in self.t.constructors:
            cg = ConstructorGenerator(self.ctx, c, self.scope)
            cg.bind_names()
            self.cons_generators.append(cg)

    def generate(self, sb: SourceBuilder) -> None:
        type_name = self.ctx.scope.lookup(self.t)

        for cg in self.cons_generators:
            cg.generate(sb)
            sb.blank()
            sb.blank()

        generic_suffix = f"[{', '.join(self.type_vars)}]" if self.type_vars else ""

        def _cons_type(c: ResolvedConstructor) -> str:
            name = self.ctx.scope.lookup(c)
            cg = self.ctx.get_constructor(c)
            if cg.type_params:
                return f"{name}[{', '.join(cg.type_var_name(p) for p in cg.type_params)}]"
            return name

        if len(self.t.constructors) > 1:
            cons_names = " | ".join(_cons_type(c) for c in self.t.constructors)
            sb.line(f"type {type_name}{generic_suffix} = {cons_names}")
        else:
            sb.line(f"type {type_name}{generic_suffix} = {_cons_type(self.t.constructors[0])}")
        sb.blank()
        sb.blank()

        self._generate_type_info(sb)
        sb.blank()
        sb.blank()

    @staticmethod
    def _tree_needs_probe(tree: MatchTree) -> bool:
        """Check if a match tree reads bits from the stream (needs probe = cs.copy())."""
        if isinstance(tree, MatchBit | MatchTag):
            return True
        if isinstance(tree, MatchConstraint):
            return TypeGenerator._tree_needs_probe(tree.if_true) or TypeGenerator._tree_needs_probe(
                tree.if_false
            )
        return False

    def _generate_type_info(
        self,
        sb: SourceBuilder,
    ) -> None:
        self.ctx.use("final", "Builder", "Slice", "override")
        type_name = self.ctx.scope.lookup(self.t)
        info_name = f"{type_name}Type"

        generic_suffix = f"[{', '.join(self.type_vars)}]" if self.type_vars else ""
        entry_nat_count = sum(
            1 for tlp in self.t.type_level_params if not tlp.is_output and tlp.kind == ParamKind.NAT
        )
        has_args = bool(self.type_vars) or entry_nat_count > 0

        protocol_args: list[str] = []
        for _ in range(entry_nat_count):
            protocol_args.append("int")
        for v in self.type_vars:
            protocol_args.append(f"TypeInfo[{v}]")
        protocol_args_str = ", ".join(protocol_args)

        if has_args:
            self.ctx.use("InstantiableTypeInfo", "TypeInfo")
            sb.line("@final")
            sb.line(
                f"class {info_name}{generic_suffix}"
                + f"(InstantiableTypeInfo[{type_name}{generic_suffix}, {protocol_args_str}]):"
            )
        else:
            self.ctx.use("TypeInfo")
            sb.line("@final")
            sb.line(f"class {info_name}(TypeInfo[{type_name}]):")

        with sb.block():
            sb.line("@override")
            sb.line(
                f"def serialize_value(self, value: {type_name}{generic_suffix}, builder: Builder) -> None:"
            )
            with sb.block():
                sb.line("value.serialize_to(builder)")
            sb.blank()

            entry_params = [tlp for tlp in self.t.type_level_params if not tlp.is_output]
            params = ["cs: Slice"]
            type_var_idx = 0
            for tlp in entry_params:
                name = self.scope.lookup(tlp)
                if tlp.kind == ParamKind.NAT:
                    params.append(f"{name}: int")
                else:
                    params.append(f"{name}: TypeInfo[{self.type_vars[type_var_idx]}]")
                    type_var_idx += 1
            params_str = ", ".join(params)

            sb.line("@override")
            sb.line(f"def load_from(self, {params_str}) -> {type_name}{generic_suffix}:")
            with sb.block():
                assert self.t.match_tree is not None
                probe_name = "probe"
                if self._tree_needs_probe(self.t.match_tree):
                    probe_name = self.scope.reserve("probe")
                    sb.line(f"{probe_name} = cs.copy()")
                MatchTreeGenerator(self.ctx, self.scope, entry_params, probe_name).generate(
                    self.t.match_tree, sb
                )

            if self.t.is_special:
                self.ctx.use("Cell", "TlbModelError")
                sb.blank()
                deser_params = ["cell: Cell"] + params[1:]
                deser_params_str = ", ".join(deser_params)
                sb.line("@override")
                sb.line(
                    f"def deserialize(self, {deser_params_str}) -> {type_name}{generic_suffix}:"
                )
                with sb.block():
                    sb.line("cs = cell.begin_parse()")
                    sb.line("if not cs.is_special():")
                    with sb.block():
                        sb.line(
                            "raise TlbModelError("
                            + f"'expected special cell for {type_name}, got ordinary cell')"
                        )
                    if entry_params:
                        arg_names = [self.scope.lookup(tlp) for tlp in entry_params]
                        load_args = ", ".join(["cs"] + arg_names)
                        sb.line(f"result = self.load_from({load_args})")
                    else:
                        sb.line("result = self.load_from(cs)")
                    sb.line("TlbModelError.raise_if_not_empty(cs)")
                    sb.line("return result")


class ConstructorGenerator:
    ctx: PyContext
    c: ResolvedConstructor
    type_scope: NameScope
    scope: NameScope
    params: list[ParamDef]
    type_params: list[TypeParamDef]
    strategies: dict[IdentityKey[ResolvedField], TypeStrategy]
    cls_name: str

    def __init__(self, ctx: PyContext, c: ResolvedConstructor, type_scope: NameScope) -> None:
        self.ctx = ctx
        self.c = c
        self.type_scope = type_scope
        self.params = []
        self.type_params = []
        self.strategies = {}

        self.bind_names()

    def bind_names(self) -> None:
        """Pre-bind all field and param names in this constructor's scope."""
        self.scope = self.type_scope.child()
        self.ctx.register_constructor(self.c, self)

        for p in self.c.params:
            match p:
                case TypeParamDef():
                    _ = self.scope.bind_field(p, f"_t{p.name}")
                case NatParamDef():
                    _ = self.scope.bind_field(p, p.name)

        for f in self.c.fields:
            _ = self.scope.bind_field(f, f.name or "field")

        self.cls_name = self.ctx.scope.lookup(self.c)

    def type_var_name(self, p: TypeParamDef) -> str:
        """Get the scope-bound type variable name for a TypeParamDef."""
        return self.type_scope.lookup(p.type_level_param.type_var)

    def generate(self, sb: SourceBuilder) -> None:
        self.ctx.use("final", "dataclass", "TLBRecord", "Builder", "Slice", "override")

        builder = StrategyBuilder(self.ctx, self.scope)
        for f in self.c.fields:
            self.strategies[IdentityKey(f)] = builder.build(f.type_expr)

        self.params = [
            p for p in self.c.params if isinstance(p, NatParamDef) or p in builder.used_type_params
        ]
        self.type_params = [p for p in self.params if isinstance(p, TypeParamDef)]

        if self.type_params:
            self.ctx.use("TypeInfo")
            generic_vars = ", ".join(self.type_var_name(p) for p in self.type_params)
            sb.line("@final")
            sb.line("@dataclass")
            sb.line(f"class {self.cls_name}[{generic_vars}](TLBRecord):")
        else:
            sb.line("@final")
            sb.line("@dataclass")
            sb.line(f"class {self.cls_name}(TLBRecord):")

        with sb.block():
            for p in self.c.params:
                match p:
                    case TypeParamDef():
                        if p in self.type_params:
                            sb.line(f"{self.scope.lookup(p)}: TypeInfo[{self.type_var_name(p)}]")
                    case NatParamDef():
                        sb.line(f"{self.scope.lookup(p)}: int")
            for f in self.c.fields:
                py_type = self.strategies[IdentityKey(f)].py_type()
                if f.condition is not None:
                    py_type = f"{py_type} | None"
                sb.line(f"{self.scope.lookup(f)}: {py_type}")

            sb.blank()
            self._generate_serialize_to(sb)
            sb.blank()
            self._generate_load_from(sb)
            if self.c.output_values:
                sb.blank()
                self._generate_get_output(sb)

    def _generate_serialize_to(
        self,
        sb: SourceBuilder,
    ) -> None:
        sb.line("@override")
        sb.line("def serialize_to(self, builder: Builder) -> None:")
        with sb.block():
            if not self.c.fields and self.c.tag_len == 0:
                sb.line("pass")
                return
            if self.c.tag_bits:
                tag_val = int(self.c.tag_bits, 2)
                sb.line(f"_ = builder.store_uint({tag_val}, {self.c.tag_len})")
            for f in self.c.fields:
                name = self.scope.lookup(f)
                strat = self.strategies[IdentityKey(f)]
                if f.condition is not None:
                    sel = NatExpr(f.condition, self.scope).self_
                    sb.line(f"if {sel}:")
                    with sb.block():
                        sb.line(f"assert self.{name} is not None")
                        strat.emit_store(f"self.{name}", "builder", sb)
                else:
                    strat.emit_store(f"self.{name}", "builder", sb)

    def _generate_load_from(self, sb: SourceBuilder) -> None:
        cs_used = self.c.tag_len > 0 or any(
            self.strategies[IdentityKey(s.field)].load_uses_cs()
            for s in self.c.deser_steps
            if isinstance(s, ReadField)
        )
        cs_name = "cs" if cs_used else "_cs"
        params = [f"{cs_name}: Slice"]

        for tlp in self.c.parent_type.type_level_params:
            if tlp.is_output:
                continue
            name = self.type_scope.lookup(tlp)
            if tlp.kind == ParamKind.NAT:
                params.append(f"{name}: int")
            else:
                expr = self.c.result_param_exprs.get(tlp.position)
                assert isinstance(expr, TypeParamRef)
                if expr.param in self.type_params:
                    type_var = self.type_scope.lookup(tlp.type_var)
                    params.append(f"{name}: TypeInfo[{type_var}]")

        params_str = ", ".join(params)
        if self.type_params:
            generic_vars = ", ".join(self.type_var_name(p) for p in self.type_params)
            sb.line("@classmethod")
            sb.line(f"def load_from(cls, {params_str}) -> {self.cls_name}[{generic_vars}]:")
        else:
            sb.line("@classmethod")
            sb.line(f"def load_from(cls, {params_str}) -> {self.cls_name}:")

        with sb.block():
            if self.c.tag_bits:
                self.ctx.use("TlbModelError")
                tag_val = int(self.c.tag_bits, 2)
                sb.line(f"if cs.load_uint({self.c.tag_len}) != {tag_val}:")
                with sb.block():
                    sb.line("raise TlbModelError('tag mismatch')")
            ctor_args: list[str] = []
            for p in self.c.params:
                match p:
                    case TypeParamDef():
                        if p in self.type_params:
                            ctor_args.append(self.scope.lookup(p))
                    case NatParamDef():
                        ctor_args.append(self.scope.lookup_local(p))
            for step in self.c.deser_steps:
                self._emit_deser_step(step, ctor_args, sb)
            sb.line(f"return cls({', '.join(ctor_args)})")

    def _emit_deser_step(
        self,
        step: ReadField | BindParam | BindOutputParam | SolveConstraint | CheckConstraint,
        ctor_args: list[str],
        sb: SourceBuilder,
    ) -> None:
        match step:
            case ReadField(field=f):
                strat = self.strategies[IdentityKey(f)]
                field_name = self.scope.lookup(f)
                var_name = self.scope.lookup_local(f)
                if f.condition is not None:
                    sel = NatExpr(f.condition, self.scope).local
                    sb.line(f"if {sel}:")
                    with sb.block():
                        strat.emit_load(var_name, "cs", sb)
                    sb.line("else:")
                    with sb.block():
                        sb.line(f"{var_name} = None")
                else:
                    strat.emit_load(var_name, "cs", sb)
                ctor_args.append(f"{field_name}={var_name}")

            case SolveConstraint(target_param=target_param, value=value):
                var_name = self.scope.lookup_local(target_param)
                value_expr = NatExpr(value, self.scope).local
                sb.line(f"{var_name} = {value_expr}")

                if not isinstance(value, NatTypeArg):
                    self.ctx.use("TlbModelError")
                    sb.line(f"if {var_name} < 0:")
                    with sb.block():
                        sb.line(
                            "raise TlbModelError("
                            + f"f'nat parameter {target_param.name} is negative: {{{var_name}}}')"
                        )

            case BindOutputParam(target_param=target_param, extraction=extraction):
                self.ctx.use("TlbModelError")
                var_name = self.scope.lookup_local(target_param)
                source_name = self.scope.lookup_local(extraction.source_field)
                expr = source_name
                for inf_step in extraction.chain:
                    expr = self._emit_inference_access(inf_step, expr, sb)
                expr = f"{expr}.get_output({extraction.final_output_idx})"
                sb.line(f"{var_name} = {expr}")
                sb.line(f"if {var_name} < 0:")
                with sb.block():
                    sb.line(
                        "raise TlbModelError("
                        + f"f'nat parameter {target_param.name} is negative: {{{var_name}}}')"
                    )

            case CheckConstraint(op=op, left=left, right=right):
                self.ctx.use("TlbModelError")
                left = NatExpr(left, self.scope).local
                right = NatExpr(right, self.scope).local
                op_str = _COMPARE_OP_STR[op]
                sb.line(f"if not ({left} {op_str} {right}):")
                with sb.block():
                    sb.line(
                        "raise TlbModelError("
                        + f"f'constraint failed: {{{left}}} {op_str} {{{right}}}')"
                    )

            case BindParam(target_param=target_param, position=position):
                if target_param not in self.type_params:
                    return
                tlp = self.c.parent_type.type_level_params[position]
                type_name = self.scope.lookup_local(tlp)
                var_name = self.scope.lookup_local(target_param)
                if type_name != var_name:
                    sb.line(f"{var_name} = {type_name}")

    def _emit_inference_access(
        self, inf_step: InferenceStep, expr: str, sb: SourceBuilder
    ) -> str:
        """Emit code to access the inference field on a (possibly multi-constructor) type.

        Returns the expression for the accessed field value.
        """
        cons_fields = inf_step.type.inference[inf_step.param_idx].constructor_field
        field_names: set[str] = set()
        for inf_cons, inf_field in cons_fields.items():
            inf_scope = self.ctx.get_constructor(inf_cons).scope
            field_names.add(inf_scope.lookup(inf_field))
        if len(field_names) == 1:
            return f"{expr}.{next(iter(field_names))}"
        tmp = self.ctx.tmp("_inf")
        items = list(cons_fields.items())
        for i, (inf_cons, inf_field) in enumerate(items):
            cons_name = self.ctx.scope.lookup(inf_cons)
            inf_scope = self.ctx.get_constructor(inf_cons).scope
            field_name = inf_scope.lookup(inf_field)
            if i == 0:
                sb.line(f"if isinstance({expr}, {cons_name}):")
            elif i < len(items) - 1:
                sb.line(f"elif isinstance({expr}, {cons_name}):")
            else:
                sb.line("else:")
            with sb.block():
                sb.line(f"{tmp} = {expr}.{field_name}")
        return tmp

    def _generate_get_output(self, sb: SourceBuilder) -> None:
        """Generate get_output() override for constructors with output params."""
        sb.line("@override")
        sb.line("def get_output(self, idx: int) -> int:")
        with sb.block():
            for i, val in enumerate(self.c.output_values):
                expr = NatExpr(val, self.scope).self_
                if i == 0:
                    sb.line(f"if idx == {i}:")
                else:
                    sb.line(f"elif idx == {i}:")
                with sb.block():
                    sb.line(f"return {expr}")
            sb.line("raise ValueError(f'no output param at index {idx}')")


class MatchTreeGenerator:
    ctx: PyContext
    scope: NameScope
    entry_params: list[TypeLevelParam]
    probe_name: str

    def __init__(
        self,
        ctx: PyContext,
        scope: NameScope,
        entry_params: list[TypeLevelParam],
        probe_name: str = "probe",
    ) -> None:
        self.ctx = ctx
        self.scope = scope
        self.entry_params = entry_params
        self.probe_name = probe_name

    def _check_constraint_to_python(self, cc: CheckConstraint) -> str:
        left = NatExpr(cc.left, self.scope).local
        right = NatExpr(cc.right, self.scope).local
        return f"{left} {_COMPARE_OP_STR.get(cc.op, '==')} {right}"

    def _constructor_load_args(self, cons: ResolvedConstructor) -> list[str]:
        """Map type-level entry params to a constructor's load_from args."""
        cons_type_params = set(self.ctx.get_constructor(cons).type_params)
        result: list[str] = []
        for tlp in self.entry_params:
            if tlp.kind == ParamKind.NAT:
                result.append(self.scope.lookup(tlp))
            else:
                expr = cons.result_param_exprs.get(tlp.position)
                if isinstance(expr, TypeParamRef) and expr.param in cons_type_params:
                    result.append(self.scope.lookup(tlp))
        return result

    def generate(self, tree: MatchTree, sb: SourceBuilder) -> None:
        match tree:
            case MatchConstructor(constructor=cons):
                cons_name = self.ctx.scope.lookup(cons)
                cons_load_args = self._constructor_load_args(cons)
                cons_cg = self.ctx.get_constructor(cons)
                if cons_cg.type_params:
                    cons_name = f"{cons_name}[{', '.join(cons_cg.type_var_name(p) for p in cons_cg.type_params)}]"
                if cons_load_args:
                    args = ", ".join(["cs"] + cons_load_args)
                    sb.line(f"return {cons_name}.load_from({args})")
                else:
                    sb.line(f"return {cons_name}.load_from(cs)")
            case MatchBit(zero=zero, one=one):
                sb.line(f"if {self.probe_name}.load_bit() == 0:")
                with sb.block():
                    self.generate(zero, sb)
                sb.line("else:")
                with sb.block():
                    self.generate(one, sb)
            case MatchTag(bits=bits, child=child):
                self.ctx.use("TlbModelError")
                tag_val = int(bits, 2)
                sb.line(f"if {self.probe_name}.load_uint({len(bits)}) != {tag_val}:")
                with sb.block():
                    sb.line("raise TlbModelError('tag mismatch')")
                self.generate(child, sb)
            case MatchConstraint(condition=condition, if_true=if_true, if_false=if_false):
                cond = self._check_constraint_to_python(condition)
                sb.line(f"if {cond}:")
                with sb.block():
                    self.generate(if_true, sb)
                sb.line("else:")
                with sb.block():
                    self.generate(if_false, sb)
            case MatchFail():
                self.ctx.use("TlbModelError")
                sb.line("raise TlbModelError('no matching constructor')")
