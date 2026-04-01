"""Python code generator for TL-B schemas.

Generates Python dataclasses with serialize/deserialize methods from
the resolved sema IR. Uses the runtime support library in tlb.object.
"""

from .ast_nodes import CompareOp
from .name_scope import NameScope
from .py_context import PyContext
from .py_emit import NatExpr, StrategyBuilder, TypeStrategy
from .sema_types import (
    AnonymousRecordType,
    BindOutputParam,
    BindParam,
    CellRefType,
    CheckConstraint,
    MatchBit,
    MatchConstraint,
    MatchConstructor,
    MatchFail,
    MatchTag,
    MatchTree,
    ParamDef,
    ParamKind,
    ReadField,
    ResolvedConstructor,
    ResolvedNatExpr,
    ResolvedType,
    ResolvedTypeExpr,
    SolveConstraint,
    TupleType,
    TypeApply,
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

    body = SourceBuilder()
    for t in types:
        if t.is_builtin or not t.constructors:
            continue
        TypeGenerator(ctx, t).generate(body)

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

    def __init__(self, ctx: PyContext, t: ResolvedType) -> None:
        self.ctx = ctx
        self.t = t

    def generate(self, sb: SourceBuilder) -> None:
        type_name = self.ctx.scope.lookup(self.t)

        type_scope = self.ctx.scope.child()
        type_vars: list[str] = []
        entry_nat_params: list[TypeLevelParam] = []
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
                _ = type_scope.bind(tlp, f"_t{nice_name}")
                type_vars.append(nice_name)
            else:
                _ = type_scope.bind(tlp, f"_type_arg_{tlp.position}")
                entry_nat_params.append(tlp)

        for c in self.t.constructors:
            ConstructorGenerator(self.ctx, c, type_scope).generate(sb)
            sb.blank()
            sb.blank()

        generic_suffix = f"[{', '.join(type_vars)}]" if type_vars else ""

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

        self._generate_type_info(sb, type_scope, type_vars, entry_nat_params)
        sb.blank()
        sb.blank()

    def _generate_type_info(
        self,
        sb: SourceBuilder,
        type_scope: NameScope,
        type_vars: list[str],
        entry_nat_params: list[TypeLevelParam],
    ) -> None:
        self.ctx.use("final", "Builder", "Slice", "override")
        type_name = self.ctx.scope.lookup(self.t)
        info_name = f"{type_name}Type"

        generic_suffix = f"[{', '.join(type_vars)}]" if type_vars else ""
        has_args = bool(type_vars) or bool(entry_nat_params)

        protocol_args: list[str] = []
        for _ in entry_nat_params:
            protocol_args.append("int")
        for v in type_vars:
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

            params = ["cs: Slice"]
            nat_arg_names: list[str] = []
            ti_field_names: list[str] = []
            for tlp in entry_nat_params:
                name = type_scope.lookup(tlp)
                params.append(f"{name}: int")
                nat_arg_names.append(name)
            for tlp in self.t.type_level_params:
                if tlp.is_output or tlp.kind != ParamKind.TYPE:
                    continue
                ti_name = type_scope.lookup(tlp)
                type_var = type_vars[len(ti_field_names)]
                params.append(f"{ti_name}: TypeInfo[{type_var}]")
                ti_field_names.append(ti_name)
            params_str = ", ".join(params)

            sb.line("@override")
            sb.line(f"def load_from(self, {params_str}) -> {type_name}{generic_suffix}:")
            with sb.block():
                assert self.t.match_tree is not None
                probe_name = "probe"
                if _tree_needs_probe(self.t.match_tree):
                    probe_name = type_scope.reserve("probe")
                    sb.line(f"{probe_name} = cs.copy()")
                MatchTreeGenerator(
                    self.ctx, type_scope, nat_arg_names, ti_field_names, probe_name
                ).generate(self.t.match_tree, sb)

            if self.t.is_special:
                self.ctx.use("Cell", "TlbModelError")
                sb.blank()
                deser_params = ["cell: Cell"] + params[1:]  # replace cs with cell
                deser_params_str = ", ".join(deser_params)
                all_arg_names = nat_arg_names + ti_field_names
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
                    if all_arg_names:
                        load_args = ", ".join(["cs"] + all_arg_names)
                        sb.line(f"result = self.load_from({load_args})")
                    else:
                        sb.line("result = self.load_from(cs)")
                    sb.line("TlbModelError.raise_if_not_empty(cs)")
                    sb.line("return result")


class ConstructorGenerator:
    ctx: PyContext
    c: ResolvedConstructor
    type_scope: NameScope

    def __init__(self, ctx: PyContext, c: ResolvedConstructor, type_scope: NameScope) -> None:
        self.ctx = ctx
        self.c = c
        self.type_scope = type_scope
        self._used_type_params: set[ParamDef] = set()

    def generate(self, sb: SourceBuilder) -> None:
        self.ctx.use("final", "dataclass", "TLBRecord", "Builder", "Slice", "override")
        cls_name = self.ctx.scope.lookup(self.c)

        local = self.type_scope.child()

        all_type_params: dict[ParamDef, tuple[str, str]] = {}
        all_nat_params: dict[ParamDef, str] = {}
        for p in self.c.params:
            if p.kind == ParamKind.TYPE:
                var_name = local.bind_field(p, f"_t{p.name}")
                all_type_params[p] = (p.name, var_name)
            else:
                var_name = local.bind_field(p, p.name)
                all_nat_params[p] = var_name

        builder = StrategyBuilder(self.ctx, local)
        field_map: dict[int, tuple[str, TypeStrategy, ResolvedNatExpr | None]] = {}
        field_info: list[tuple[str, TypeStrategy, ResolvedNatExpr | None]] = []
        for f in self.c.fields:
            name = local.bind_field(f, f.name or "field")
            strat = builder.build(f.type_expr)
            entry = (name, strat, f.condition)
            field_map[id(f)] = entry
            field_info.append(entry)

        self._used_type_params = builder.used_type_params
        type_params: list[tuple[ParamDef, str]] = [
            (p, all_type_params[p][0]) for p in all_type_params if p in self._used_type_params
        ]
        is_generic = len(type_params) > 0

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
            for p in all_nat_params:
                sb.line(f"{all_nat_params[p]}: int")
            for p, type_var in type_params:
                ti_field = all_type_params[p][1]
                sb.line(f"{ti_field}: TypeInfo[{type_var}]")
            for name, strat, cond in field_info:
                py_type = strat.py_type()
                if cond is not None:
                    py_type = f"{py_type} | None"
                sb.line(f"{name}: {py_type}")

            sb.blank()
            self._generate_serialize_to(sb, field_info, local)
            sb.blank()
            self._generate_load_from(
                sb,
                cls_name,
                field_map,
                type_params,
                all_type_params,
                all_nat_params,
                local,
            )
            if self.c.output_values:
                sb.blank()
                self._generate_get_output(sb, local)

    def _generate_serialize_to(
        self,
        sb: SourceBuilder,
        field_info: list[tuple[str, TypeStrategy, ResolvedNatExpr | None]],
        local: NameScope,
    ) -> None:
        sb.line("@override")
        sb.line("def serialize_to(self, builder: Builder) -> None:")
        with sb.block():
            if not field_info and self.c.tag_len == 0:
                sb.line("pass")
                return
            if self.c.tag_bits:
                tag_val = int(self.c.tag_bits, 2)
                sb.line(f"_ = builder.store_uint({tag_val}, {self.c.tag_len})")
            for name, strat, cond in field_info:
                if cond is not None:
                    sel = NatExpr(cond, local).self_
                    sb.line(f"if {sel}:")
                    with sb.block():
                        sb.line(f"assert self.{name} is not None")
                        strat.emit_store(f"self.{name}", "builder", sb)
                else:
                    strat.emit_store(f"self.{name}", "builder", sb)

    def _generate_load_from(
        self,
        sb: SourceBuilder,
        cls_name: str,
        field_map: dict[int, tuple[str, TypeStrategy, ResolvedNatExpr | None]],
        type_params: list[tuple[ParamDef, str]],
        all_type_params: dict[ParamDef, tuple[str, str]],
        all_nat_params: dict[ParamDef, str],
        local: NameScope,
    ) -> None:
        is_generic = len(type_params) > 0

        cs_used = bool(self.c.fields) or self.c.tag_len > 0
        cs_name = "cs" if cs_used else "_cs"
        params = [f"{cs_name}: Slice"]
        for tlp in self.c.parent_type.type_level_params:
            if tlp.is_output:
                continue
            name = self.type_scope.lookup(tlp)
            if tlp.kind == ParamKind.NAT:
                params.append(f"{name}: int")
            else:
                for p, type_var in type_params:
                    _, ti_field = all_type_params[p]
                    if ti_field == name or f"_t{type_var}" == name:
                        params.append(f"{name}: TypeInfo[{type_var}]")
                        break
        has_params = len(params) > 1

        if (
            not self.c.fields
            and self.c.tag_len == 0
            and not has_params
            and not self.c.output_values
        ):
            sb.line("@classmethod")
            sb.line(f"def load_from(cls, _cs: Slice) -> {cls_name}:")
            with sb.block():
                sb.line("return cls()")
            return

        params_str = ", ".join(params)
        if is_generic:
            generic_vars = ", ".join(name for _, name in type_params)
            sb.line("@classmethod")
            sb.line(f"def load_from(cls, {params_str}) -> {cls_name}[{generic_vars}]:")
        else:
            sb.line("@classmethod")
            sb.line(f"def load_from(cls, {params_str}) -> {cls_name}:")

        with sb.block():
            if self.c.tag_bits:
                self.ctx.use("TlbModelError")
                tag_val = int(self.c.tag_bits, 2)
                sb.line(f"if cs.load_uint({self.c.tag_len}) != {tag_val}:")
                with sb.block():
                    sb.line("raise TlbModelError('tag mismatch')")
            ctor_args: list[str] = []
            for p in all_nat_params:
                ctor_args.append(all_nat_params[p])
            for p, _ in type_params:
                _, ti_field = all_type_params[p]
                ctor_args.append(ti_field)
            for step in self.c.deser_steps:
                self._emit_deser_step(step, field_map, ctor_args, local, sb)
            sb.line(f"return cls({', '.join(ctor_args)})")

    def _emit_deser_step(
        self,
        step: ReadField | BindParam | BindOutputParam | SolveConstraint | CheckConstraint,
        field_map: dict[int, tuple[str, TypeStrategy, ResolvedNatExpr | None]],
        ctor_args: list[str],
        local: NameScope,
        sb: SourceBuilder,
    ) -> None:
        match step:
            case ReadField():
                field_name, strat, cond = field_map[id(step.field)]
                var_name = local.lookup_local(step.field)
                if cond is not None:
                    sel = NatExpr(cond, local).local
                    sb.line(f"if {sel}:")
                    with sb.block():
                        strat.emit_load(var_name, "cs", sb)
                    sb.line("else:")
                    with sb.block():
                        sb.line(f"{var_name} = None")
                else:
                    strat.emit_load(var_name, "cs", sb)
                ctor_args.append(f"{field_name}={var_name}")
            case SolveConstraint():
                from .sema_types import NatTypeArg as NTA

                var_name = local.lookup_local(step.target_param)
                value_expr = NatExpr(step.value, local).local
                sb.line(f"{var_name} = {value_expr}")
                if not isinstance(step.value, NTA):
                    self.ctx.use("TlbModelError")
                    sb.line(f"if {var_name} < 0:")
                    with sb.block():
                        sb.line(
                            "raise TlbModelError("
                            + f"f'nat parameter {step.target_param.name} is negative: {{{var_name}}}')"
                        )
            case BindOutputParam():
                self.ctx.use("TlbModelError")
                var_name = local.lookup_local(step.target_param)
                source_name = local.lookup_local(step.extraction.source_field)
                expr = source_name
                for inf_step in step.extraction.chain:
                    field = inf_step.type.inference[inf_step.param_idx].constructor_field
                    for _, inf_field in field.items():
                        field_py_name = inf_field.name or "field"
                        expr = f"{expr}.{field_py_name}"
                        break
                expr = f"{expr}.get_output({step.extraction.final_output_idx})"
                sb.line(f"{var_name} = {expr}")
                sb.line(f"if {var_name} < 0:")
                with sb.block():
                    sb.line(
                        "raise TlbModelError("
                        + f"f'nat parameter {step.target_param.name} is negative: {{{var_name}}}')"
                    )
            case CheckConstraint():
                self.ctx.use("TlbModelError")
                left = NatExpr(step.left, local).local
                right = NatExpr(step.right, local).local
                op_str = _COMPARE_OP_STR.get(step.op, "==")
                sb.line(f"if not ({left} {op_str} {right}):")
                with sb.block():
                    sb.line(
                        "raise TlbModelError("
                        + f"f'constraint failed: {{{left}}} {op_str} {{{right}}}')"
                    )
            case BindParam():
                if step.target_param not in self._used_type_params:
                    return
                tlp = self.c.parent_type.type_level_params[step.position]
                type_name = local.lookup_local(tlp)
                var_name = local.lookup_local(step.target_param)
                if type_name != var_name:
                    sb.line(f"{var_name} = {type_name}")

    def _generate_get_output(self, sb: SourceBuilder, local: NameScope) -> None:
        """Generate get_output() override for constructors with output params."""
        sb.line("@override")
        sb.line("def get_output(self, idx: int) -> int:")
        with sb.block():
            for i, val in enumerate(self.c.output_values):
                expr = NatExpr(val, local).self_
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
    nat_args: list[str]
    ti_args: list[str]
    probe_name: str

    def __init__(
        self,
        ctx: PyContext,
        scope: NameScope,
        nat_args: list[str] | None = None,
        ti_args: list[str] | None = None,
        probe_name: str = "probe",
    ) -> None:
        self.ctx = ctx
        self.scope = scope
        self.nat_args = nat_args or []
        self.ti_args = ti_args or []
        self.probe_name = probe_name

    def generate(self, tree: MatchTree, sb: SourceBuilder) -> None:
        match tree:
            case MatchConstructor(constructor=cons):
                cons_name = self.ctx.scope.lookup(cons)
                cons_load_args = _constructor_load_args(cons, self.nat_args, self.ti_args)
                cons_type_vars = _constructor_used_type_vars(cons)
                if cons_type_vars:
                    cons_name = f"{cons_name}[{', '.join(cons_type_vars)}]"
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
                cond = _check_constraint_to_python(condition, self.scope)
                sb.line(f"if {cond}:")
                with sb.block():
                    self.generate(if_true, sb)
                sb.line("else:")
                with sb.block():
                    self.generate(if_false, sb)
            case MatchFail():
                self.ctx.use("TlbModelError")
                sb.line("raise TlbModelError('no matching constructor')")


def _constructor_load_args(
    cons: ResolvedConstructor,
    type_nat_args: list[str],
    type_ti_args: list[str],
) -> list[str]:
    """Map type-level non-output args to a constructor's load_from args.

    type_nat_args and type_ti_args correspond to non-output positions only.
    The constructor's load_from takes: cs, entry nat params (int), type params (TypeInfo).
    """
    parent = cons.parent_type
    used_type_vars = set(_constructor_used_type_vars(cons))

    nat_result: list[str] = []
    ti_result: list[str] = []
    nat_idx = 0
    ti_idx = 0
    for tlp in parent.type_level_params:
        if tlp.is_output:
            continue
        pos = tlp.position
        if tlp.kind == ParamKind.NAT:
            if nat_idx < len(type_nat_args):
                nat_result.append(type_nat_args[nat_idx])
            nat_idx += 1
        else:
            if ti_idx < len(type_ti_args):
                expr = cons.result_param_exprs.get(pos)
                if isinstance(expr, TypeParamRef) and expr.param.name in used_type_vars:
                    ti_result.append(type_ti_args[ti_idx])
            ti_idx += 1

    return nat_result + ti_result


def _constructor_used_type_vars(cons: ResolvedConstructor) -> list[str]:
    """Get the type variable names used in a constructor's fields, in type-level param order."""
    seen: set[str] = set()
    used: list[str] = []
    _collect_type_vars(cons, seen, used)
    if not used:
        return []
    used_set = set(used)
    result: list[str] = []
    for _, expr in sorted(cons.result_param_exprs.items()):
        if isinstance(expr, TypeParamRef) and expr.param.name in used_set:
            result.append(expr.param.name)
    return result


def _tree_needs_probe(tree: MatchTree) -> bool:
    """Check if a match tree reads bits from the stream (needs probe = cs.copy())."""
    if isinstance(tree, MatchBit | MatchTag):
        return True
    if isinstance(tree, MatchConstraint):
        return _tree_needs_probe(tree.if_true) or _tree_needs_probe(tree.if_false)
    return False


def _collect_type_vars(cons: ResolvedConstructor, seen: set[str], result: list[str]) -> None:
    """Collect type var names from all field type expressions."""
    for f in cons.fields:
        _collect_type_vars_from_expr(f.type_expr, seen, result)


def _collect_type_vars_from_expr(expr: ResolvedTypeExpr, seen: set[str], result: list[str]) -> None:
    """Recursively find TypeParamRef names in a type expression."""
    match expr:
        case TypeParamRef(param=param):
            if param.name not in seen:
                result.append(param.name)
                seen.add(param.name)
        case TypeApply(arguments=arguments):
            for arg in arguments:
                if isinstance(arg, TypeParamRef | TypeApply | TupleType | CellRefType):
                    _collect_type_vars_from_expr(arg, seen, result)
        case CellRefType(inner=inner):
            _collect_type_vars_from_expr(inner, seen, result)
        case TupleType(element=element):
            _collect_type_vars_from_expr(element, seen, result)
        case AnonymousRecordType():
            pass


def _check_constraint_to_python(cc: CheckConstraint, scope: NameScope) -> str:
    left = NatExpr(cc.left, scope).local
    right = NatExpr(cc.right, scope).local
    return f"{left} {_COMPARE_OP_STR.get(cc.op, '==')} {right}"
