"""ConstructorGenerator: generates Python code for a resolved TL-B constructor."""

from ..identity_key import IdentityKey
from ..sema.types import (
    AnonymousRecordType,
    BindOutputParam,
    BindParam,
    CheckConstraint,
    InferenceStep,
    NatAdd,
    NatFieldValue,
    NatGetBit,
    NatLiteral,
    NatMul,
    NatParamDef,
    NatParamRef,
    NatSub,
    NatTypeArg,
    ParamDef,
    ParamKind,
    ReadField,
    ResolvedConstructor,
    ResolvedField,
    SolveConstraint,
    TypeApply,
    TypeParamDef,
    TypeParamRef,
)
from .context import PyContext
from .name_scope import NameScope
from .nat_expr import NatExpr, render_constraint
from .source_builder import SourceBuilder
from .strategy import TypeStrategy
from .strategy.builder import StrategyBuilder


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
        return self.type_scope.lookup_generic(p.type_level_param)

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
            if any(v is not None for v in self.c.nat_param_values):
                sb.blank()
                self._generate_get_output(sb)
            if self.type_params:
                sb.blank()
                self._generate_check_type(sb)

    def _generate_serialize_to(
        self,
        sb: SourceBuilder,
    ) -> None:
        sb.line("@override")
        sb.line("def serialize_to(self, builder: Builder) -> None:")
        with sb.block():
            has_assertions = self._emit_serialize_assertions(sb)
            if not self.c.fields and self.c.tag_len == 0:
                if not has_assertions:
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

    def _emit_serialize_assertions(self, sb: SourceBuilder) -> bool:
        """Emit assertions that validate internal state before serialization.

        Returns True if any assertions were emitted.
        """
        emitted = False
        for p in self.c.params:
            if isinstance(p, NatParamDef):
                sb.line(f"assert self.{self.scope.lookup(p)} >= 0")
                emitted = True
        for step in self.c.deser_steps:
            if not isinstance(step, CheckConstraint):
                continue
            if step.left.references_type_arg or step.right.references_type_arg:
                continue
            sb.line(f"assert {render_constraint(step, self.scope, use_self=True)}")
            emitted = True
        for f in self.c.fields:
            if self._emit_field_type_arg_assertions(f, sb):
                emitted = True
        return emitted

    def _emit_field_type_arg_assertions(self, f: ResolvedField, sb: SourceBuilder) -> bool:
        """Assert that a field's sub-type params match expected values."""
        if not isinstance(f.type_expr, TypeApply):
            return False
        t = f.type_expr.type
        if t.is_builtin:
            return False
        field_name = self.scope.lookup(f)
        emitted = False
        for tlp, arg in zip(t.type_level_params, f.type_expr.arguments, strict=True):
            if tlp.kind == ParamKind.NAT:
                if not isinstance(
                    arg,
                    NatLiteral | NatParamRef | NatFieldValue | NatAdd | NatSub | NatMul | NatGetBit,
                ):
                    continue
                if arg.references_type_arg:
                    continue
                expected = NatExpr(arg, self.scope).self_
                sb.line(f"assert self.{field_name}.get_output({tlp.position}) == {expected}")
                emitted = True
            else:
                if isinstance(arg, TypeParamRef | TypeApply | AnonymousRecordType):
                    arg_strategy = StrategyBuilder(self.ctx, self.scope).build(
                        arg, inside_generic_arg=True
                    )
                    ti_expr = arg_strategy.type_info_expr_self()
                    sb.line(f"self.{field_name}.check_type({tlp.position}, {ti_expr})")
                    emitted = True
        return emitted

    def _generate_load_from(self, sb: SourceBuilder) -> None:
        cs_used = self.c.tag_len > 0 or any(
            self.strategies[IdentityKey(s.field)].load_uses_cs()
            for s in self.c.deser_steps
            if isinstance(s, ReadField)
        )
        cs_name = "cs" if cs_used else "_cs"
        params = [f"{cs_name}: Slice"]
        assertions: list[str] = []

        for tlp in self.c.parent_type.type_level_params:
            if tlp.is_output:
                continue
            name = self.type_scope.lookup(tlp)
            if tlp.kind == ParamKind.NAT:
                params.append(f"{name}: int")
                assertions.append(f"assert {name} >= 0")
            else:
                expr = self.c.result_param_exprs.get(tlp.position)
                assert isinstance(expr, TypeParamRef)
                if expr.param in self.type_params:
                    type_var = self.type_scope.lookup_generic(tlp)
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
            for assertion in assertions:
                sb.line(assertion)
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
                expr = f"{expr}.get_output({extraction.result_param_position})"
                sb.line(f"{var_name} = {expr}")
                sb.line(f"if {var_name} < 0:")
                with sb.block():
                    sb.line(
                        "raise TlbModelError("
                        + f"f'nat parameter {target_param.name} is negative: {{{var_name}}}')"
                    )

            case CheckConstraint():
                self.ctx.use("TlbModelError")
                rendered = render_constraint(step, self.scope)
                sb.line(f"if not ({rendered}):")
                with sb.block():
                    sb.line(f"raise TlbModelError(f'constraint failed: {{{rendered}}}')")

            case BindParam(target_param=target_param, position=position):
                if target_param not in self.type_params:
                    return
                tlp = self.c.parent_type.type_level_params[position]
                type_name = self.scope.lookup_local(tlp)
                var_name = self.scope.lookup_local(target_param)
                if type_name != var_name:
                    sb.line(f"{var_name} = {type_name}")

    def _emit_inference_access(self, inf_step: InferenceStep, expr: str, sb: SourceBuilder) -> str:
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
        """Generate get_output() override returning nat type-level param values by TLP position."""
        sb.line("@override")
        sb.line("def get_output(self, idx: int) -> int:")
        with sb.block():
            first = True
            for i, val in enumerate(self.c.nat_param_values):
                if val is None:
                    continue
                expr = NatExpr(val, self.scope).self_
                if first:
                    sb.line(f"if idx == {i}:")
                    first = False
                else:
                    sb.line(f"elif idx == {i}:")
                with sb.block():
                    sb.line(f"return {expr}")
            sb.line("raise ValueError(f'no nat param at index {idx}')")

    def _generate_check_type(self, sb: SourceBuilder) -> None:
        """Generate check_type() override that asserts type params match by TLP position."""
        self.ctx.use("TypeInfo")
        sb.line("@override")
        sb.line("def check_type[_T](self, idx: int, ti: TypeInfo[_T]) -> None:")
        with sb.block():
            first = True
            for tlp in self.c.parent_type.type_level_params:
                if tlp.kind != ParamKind.TYPE:
                    continue
                expr = self.c.result_param_exprs.get(tlp.position)
                if not isinstance(expr, TypeParamRef):
                    continue
                if expr.param not in self.type_params:
                    continue
                ti_name = self.scope.lookup(expr.param)
                if first:
                    sb.line(f"if idx == {tlp.position}:")
                    first = False
                else:
                    sb.line(f"elif idx == {tlp.position}:")
                with sb.block():
                    sb.line(f"assert self.{ti_name} == ti")
                    sb.line("return")
            sb.line("raise ValueError(f'no type param at index {idx}')")
