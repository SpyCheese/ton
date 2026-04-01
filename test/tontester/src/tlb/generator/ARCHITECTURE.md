# TL-B Code Generator Architecture

## What is TL-B?

TL-B (Type Language - Binary) is the schema language for TON blockchain's binary
serialization format. It describes how data structures are serialized into cells
(bit strings with references to other cells). See `grammar.md` for the formal syntax.

Key concepts:
- **Constructors** define how to serialize one variant of a type (tag + fields)
- **Tags** are bit prefixes that identify which constructor was used
- **Implicit params** (`{n:#}`, `{X:Type}`) are not serialized — inferred from context
- **Output params** (`~n`) are computed during deserialization, not known beforehand
- **Cell references** (`^Type`) store a value in a separate cell
- **Special cells** (`!` prefix) are exotic TON cell types (merkle proofs, pruned branches)
- **Inline records** (`[field1:Type1 field2:Type2]`) are anonymous types

## Pipeline

```
TL-B text → Lexer → Parser → AST → Sema → Resolved IR → Codegen → Python
```

### Lexer (`lexer.py`)
Tokenizes TL-B text. Single `IDENT` token type (no LC/UC distinction — case
conventions checked in parser/sema). Handles `#hex_` and `$bin_` tag literals,
`##`, `#<`, `#<=` compound tokens.

### Parser (`parser.py`)
Recursive descent, LL(2), no backtracking. Field types parsed at `conditional`
precedence level (expr95) — application/arithmetic need parentheses in field
position. Produces `ast_nodes.py` types.

### Sema (`sema*.py`)

Eight phases in `sema.py`:

1. **Register types** — collect arities, param kinds (Nat/Type), output positions,
   check consistency across constructors (arity, `~` positions, `!` special)
2. **Resolve constructors** — expression resolution (nat vs type disambiguation),
   scope-based name resolution, constraint resolution, arity checks
3. **Compute tags** — CRC32 auto-tags from canonical text representation
4. **Build match trees** — constructor dispatch (bit prefix + constraint-based),
   follows typedef chains, detects ambiguity
5. **Classify inference** — which Type params support output param propagation
6. **Compute deser plans** — ordered steps (entry bindings from type args,
   ReadField, BindOutputParam, SolveConstraint, CheckConstraint)
7. **Classify types** — is_enum, is_typedef, is_special
8. **Topological sort** — order types so dependencies come before dependents

Key resolved IR types (`sema_types.py`):
- `ResolvedNatExpr` — nat expressions (NatLiteral, NatParamRef, NatFieldValue,
  NatAdd, NatSub, NatMul, NatGetBit, NatTypeArg)
- `ResolvedTypeExpr` — type expressions (TypeApply, TupleType, CellRefType,
  TypeParamRef, AnonymousRecordType)
- `TypeLevelParam` — sentinel for a type-level parameter position (kind, is_output).
  Stored on `ResolvedType.type_level_params`, bound in type scope for name resolution.
- `MatchTree` — dispatch tree (MatchBit, MatchTag, MatchConstraint,
  MatchConstructor, MatchFail)
- `DeserStep` — deserialization plan steps (ReadField, BindParam, BindOutputParam,
  SolveConstraint, CheckConstraint)

Conditionals (`flag?Type`) are a field modifier, not a type expression — stored
as `ResolvedField.condition: ResolvedNatExpr | None`.

`Cell` is an alias for `Any` — `^Cell` is just `^Any`.

Inline records (`[field1:Type1 ...]`) create anonymous `ResolvedType` objects
registered in the type registry. Type params (`{X:Type}`) become external
(contribute to arity), nat params (`{n:#}`) stay internal (resolved by deser plan).
Outer scope does not leak into inline records.

Match tree algorithm (`sema_match.py`):
1. Try constraint split on nat type args (partial — reduces groups)
2. Expand typedef chains (follow first inline field's type constructors)
3. Consume common bit prefix → MatchTag
4. Split on diverging bit → MatchBit, recurse
5. Error if any constructor has no more bits (genuinely ambiguous)

### Python Codegen (`py_codegen.py`, `py_emit.py`, `py_context.py`)

Generates Python dataclasses with `serialize_to`/`load_from` methods.

Architecture:
- **`PyContext`** — shared state: NameScope, import tracker, temp var counter,
  ref wrapper registry
- **`TypeStrategy`** (ABC in `py_emit.py`) — knows how to emit store/load code
  for a type expression. Each strategy also provides `type_info_expr()` (a Python
  expression evaluating to a runtime `TypeInfo` for this type — used when the type
  appears as a generic arg) and `py_type()` (Python type annotation).
  Subclasses: UintStrategy, IntStrategy, BitsStrategy, UserTypeStrategy,
  CellRefStrategy, GenericCellRefStrategy, TupleStrategy, TypeParamStrategy,
  SliceTypeStrategy.
- **`NatExpr`** — wraps a `ResolvedNatExpr` + scope, renders with `.local` (for
  load_from) or `.self_` (for serialize_to). `_DerivedNatExpr` for transformations
  like `({}).bit_length()`.
- **`StrategyBuilder`** — builds `TypeStrategy` for a `ResolvedTypeExpr`. Holds
  constructor-local context (scope). Tracks `used_type_params` for determining
  which type params become class generics.
- **`TypeGenerator`** → **`ConstructorGenerator`** — nested generators for
  type/constructor. TypeGenerator creates the type-level scope, binds
  `TypeLevelParam` sentinels, generates all constructors then the type alias
  and TypeInfo class. ConstructorGenerator creates a child scope for
  constructor-local params and fields.
- **`MatchTreeGenerator`** — generates dispatch code on a `probe` copy of the slice

Scope hierarchy:
- **File scope** (`ctx.scope`) — type names, constructor names
- **Type scope** (child of file scope) — `TypeLevelParam` sentinels bound as
  `_type_arg_{pos}` (nat) or `_t{Name}` (type). Used by TypeInfo and MatchTree.
- **Constructor scope** (child of type scope) — constructor params, fields.
  `BindParam`/`SolveConstraint` deser steps rebind from type-level names to
  constructor-local names.

Field names use `bind_field()` which only checks keywords and sibling collisions,
not parent scope — since fields are accessed via `self.X`.

CellRef handling:
- **Concrete `^Type`** (no type params inside) → lazy wrapper class (`Ref_uint32`,
  `Ref_TickTock`). Deduped by `(descriptor, is_special)`. The `.ref` property
  lazily deserializes and checks for special cells.
- **Parameterized `^Type`** (type or nat params inside) → runtime `Ref[X]` from
  `object.py` with `GenericCellRefStrategy`. Uses `RefType[X].instantiate(inner_ti)`.

Generic TypeInfo classes inherit `InstantiableTypeInfo` (provides `.instantiate()`
for nested generic usage like `MaybeType[T].instantiate(_tT)`). Non-generic
TypeInfo classes inherit plain `TypeInfo`.

Special types (`!` constructors) generate a `deserialize` override on the TypeInfo
that requires `cs.is_special()` instead of rejecting it.

Deser step execution in `_emit_deser_step`:
- `SolveConstraint` — assigns nat param, validates non-negative for computed values
- `BindOutputParam` — extracts output via `.get_output(idx)`, navigates inference chains
- `CheckConstraint` — validates constraint, raises `TlbModelError`
- `ReadField` — emits strategy load, handles conditional fields
- `BindParam` — rebinds type-level name to constructor-local name

### Runtime (`tlb/object.py`)

Support library imported by generated code:
- `TLBRecord` — ABC for serializable records. `get_output(idx)` for output params.
- `TypeInfo[T, *Args]` — protocol for type (de)serialization
- `InstantiableTypeInfo` — TypeInfo with `.instantiate()` for nested generics
- `Ref[X]` — generic lazy cell reference (used by parameterized ^Type codegen path)
- `RefType[X]` — TypeInfo for Ref[X]
- `UintTypeConstructor`, `IntTypeConstructor`, `BitsTypeConstructor` — primitive TypeInfos
- `TupleTypeConstructor[X]` — TypeInfo for `n * Type` (stores count + element TypeInfo)
- `AnyType` — TypeInfo for `Any`/`Cell` (consumes entire slice)
- `TlbModelError` — deserialization errors

### Tests

- Schema files: `tests/tlb/schemas/*.tlb` (one per feature area)
- Generated code: `tests/tlb/generated/*.py` (gitignored, regenerated by `generate_tl.py`)
- Test files: one per schema (`test_codegen_basic.py`, `test_codegen_generics.py`, etc.)
  plus `test_sema.py`, `test_match_tree.py`, `test_lexer.py`, `test_parser.py`
- End-to-end: `test_block_e2e.py` deserializes a real testnet block (seq 49375158)
- All test and generated code passes basedpyright with zero warnings

## What's implemented

- [x] Lexer, parser (full TL-B grammar including extensions)
- [x] Sema (type resolution, expression disambiguation, tags, match trees,
      inference, deser plans, constraint solving, topological sort)
- [x] Python codegen: all field types (uint/int/bits/#/##/#</#<=, Cell/Any,
      conditionals, tuples, cell refs)
- [x] Generics: type params, nat params, nested generics, concrete instantiation
- [x] Output params (~n): Unary encoding, inference chains, NatFieldValue,
      compound SolveConstraint
- [x] Inline records with scope isolation and internal nat params
- [x] Special cells (! constructors) with deserialize override
- [x] End-to-end block.tlb (979 lines, ~380 constructors, zero basedpyright warnings)
- [x] Real block deserialization verified against C++ reference
- [ ] C++ codegen (separate backend, same sema output)
