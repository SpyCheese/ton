"""CellRefStrategy and GenericCellRefStrategy: emit store/load for ^Type."""

from typing import final, override

from ..context import PyContext
from ..source_builder import SourceBuilder
from ._base import TypeStrategy


@final
class CellRefStrategy(TypeStrategy):
    """^Type for fully concrete inner types: uses a lazy ref wrapper class.

    Each distinct (inner_type, is_special) pair gets one wrapper class,
    shared across all fields that use it. Only used when the inner type
    has no type parameters -- otherwise GenericCellRefStrategy is used.
    """

    def __init__(self, inner: TypeStrategy, ctx: PyContext, is_special: bool = False) -> None:
        self.inner = inner
        self.ctx = ctx

        inner_type = inner.py_type()
        inner_key = inner.descriptor()

        def emit_store_body(sb: SourceBuilder) -> None:
            inner.emit_store("value", "builder", sb)

        def emit_load_body(sb: SourceBuilder) -> None:
            inner.emit_load("_result", "cs", sb)
            sb.line("return _result")

        self.wrapper_name = ctx.get_or_create_ref_wrapper(
            inner_py_type=inner_type,
            inner_class_name=inner_key,
            is_special=is_special,
            store_code_fn=emit_store_body,
            load_code_fn=emit_load_body,
        )

    @override
    def py_type(self) -> str:
        return self.wrapper_name

    @override
    def type_info_expr(self) -> str:
        self.ctx.use("RefType")
        inner_ti = self.inner.type_info_expr()
        inner_py = self.inner.py_type()
        return f"RefType[{inner_py}].instantiate({inner_ti})"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"_ = {builder}.store_ref({value}.serialize_ref())")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        sb.line(f"{target} = {self.wrapper_name}({cs}.load_ref())")

    @override
    def descriptor(self) -> str:
        return self.wrapper_name


@final
class GenericCellRefStrategy(TypeStrategy):
    """^Type when the inner type involves type parameters.

    Uses the runtime Ref[X] and RefType[X] instead of a generated wrapper
    class, because wrapper classes are file-level and can't capture type params.
    """

    def __init__(self, inner: TypeStrategy, ctx: PyContext) -> None:
        self.inner = inner
        self.ctx = ctx
        ctx.use("Ref")

    @override
    def py_type(self) -> str:
        return f"Ref[{self.inner.py_type()}]"

    @override
    def type_info_expr(self) -> str:
        self.ctx.use("RefType")
        return f"RefType[{self.inner.py_type()}].instantiate({self.inner.type_info_expr()})"

    @override
    def type_info_expr_self(self) -> str:
        self.ctx.use("RefType")
        return f"RefType[{self.inner.py_type()}].instantiate({self.inner.type_info_expr_self()})"

    @override
    def emit_store(self, value: str, builder: str, sb: SourceBuilder) -> None:
        sb.line(f"{value}.serialize_to({builder})")

    @override
    def emit_load(self, target: str, cs: str, sb: SourceBuilder) -> None:
        self.ctx.use("Ref")
        inner_ti = self.inner.type_info_expr()
        sb.line(f"{target} = Ref({inner_ti}, {cs}.load_ref())")

    @override
    def descriptor(self) -> str:
        return f"ref_{self.inner.descriptor()}"
