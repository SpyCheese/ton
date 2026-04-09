"""Lazy dictionary backed by a TON HashmapAug cell.

Wraps a HashmapAugE cell and provides dict-like read/write access.
Reads traverse the tree lazily. Writes are tracked in a sorted overlay
and merged during iteration and serialization.

Regular (non-augmented) hashmaps use UnitTypeInfo as the extra type,
which takes 0 bits on the wire.
"""

from collections.abc import Iterator
from typing import Protocol, final, override

from bitarray import bitarray
from bitarray.util import ba2int, int2ba
from pytoniq_core import Builder, Cell, Slice
from sortedcontainers import SortedDict

from .hashmap_auto import (
    HashmapAugType,
    HmLabel,
    ahm_edge,
    ahmn_fork,
    ahmn_leaf,
    hml_long,
    hml_same,
    hml_short,
)
from .object import InstantiableTypeInfo, TypeInfo, UnitTypeInfo


class _Deleted:
    """Sentinel for deleted keys in the overlay."""

    pass


_DELETED = _Deleted()


class Augmentation[V, E](Protocol):
    """Augmentation callbacks for HashmapAug."""

    @property
    def extra_ti(self) -> TypeInfo[E]: ...
    def eval_leaf(self, value: V) -> E: ...
    def merge(self, left: E, right: E) -> E: ...
    def eval_empty(self) -> E: ...


@final
class _UnitAug:
    @property
    def extra_ti(self) -> TypeInfo[None]:
        return UnitTypeInfo

    def eval_leaf(self, value: object) -> None:
        _ = value
        return None

    def merge(self, left: None, right: None) -> None:
        _ = left
        _ = right
        return None

    def eval_empty(self) -> None:
        return None


UnitAug = _UnitAug()


def _label_bits(label: HmLabel) -> bitarray:
    match label:
        case hml_short(s=s):
            return s
        case hml_long(s=s):
            return s
        case hml_same(v=v, n=n):
            return bitarray([v]) * n


def _iter_edge[V, E](
    edge: ahm_edge[V, E], prefix: bitarray, key_bits: int
) -> Iterator[tuple[int, V]]:
    """Lazily yield (key, value) pairs in sorted order from a hashmap edge."""
    prefix = prefix + _label_bits(edge.label)
    match edge.node:
        case ahmn_leaf(value=value):
            yield (ba2int(prefix), value)
        case ahmn_fork(left=left, right=right):
            yield from _iter_edge(left.ref, prefix + bitarray([False]), key_bits)
            yield from _iter_edge(right.ref, prefix + bitarray([True]), key_bits)


def _lookup_edge[V, E](edge: ahm_edge[V, E], key: bitarray, pos: int) -> V | None:
    """Look up a single key by traversing the tree."""
    label = _label_bits(edge.label)
    if key[pos : pos + len(label)] != label:
        return None
    pos += len(label)
    match edge.node:
        case ahmn_leaf(value=value):
            return value
        case ahmn_fork(left=left, right=right):
            if key[pos]:
                return _lookup_edge(right.ref, key, pos + 1)
            else:
                return _lookup_edge(left.ref, key, pos + 1)


def _merge_sorted[V](
    tree: Iterator[tuple[int, V]],
    overlay: Iterator[tuple[int, V]],
    skip: frozenset[int],
) -> Iterator[tuple[int, V]]:
    """Merge two sorted iterators, skipping keys in `skip` from the tree iterator."""
    t = next(tree, None)
    o = next(overlay, None)
    while t is not None or o is not None:
        if t is not None and t[0] in skip:
            t = next(tree, None)
            continue
        if t is None:
            assert o is not None
            yield o
            o = next(overlay, None)
        elif o is None:
            yield t
            t = next(tree, None)
        elif t[0] < o[0]:
            yield t
            t = next(tree, None)
        elif t[0] > o[0]:
            yield o
            o = next(overlay, None)
        else:
            yield o
            o = next(overlay, None)
            t = next(tree, None)


def _common_prefix(keys: list[bitarray], lo: int, hi: int, pos: int) -> bitarray:
    first = keys[lo]
    length = 0
    while pos + length < len(first):
        bit = first[pos + length]
        if all(keys[i][pos + length] == bit for i in range(lo, hi)):
            length += 1
        else:
            break
    return first[pos : pos + length]


def _split_at_bit(keys: list[bitarray], lo: int, hi: int, pos: int) -> int:
    for i in range(lo, hi):
        if keys[i][pos]:
            return i
    return hi


def _serialize_label(builder: Builder, label: bitarray, max_len: int) -> None:
    # k = ceil(log2(max_len + 1))
    # hml_short ('0'): 2n + 2 bits
    # hml_long  ('10'): 2 + k + n bits
    # hml_same  ('11'): 3 + k bits

    n = len(label)
    k = max_len.bit_length() if max_len > 0 else 0
    is_same = n > 0 and (label.all() or not label.any())
    if is_same and n > 1 and k < 2 * n - 1:
        hml_same(max_len, v=bool(label[0]), n=n).serialize_to(builder)
    elif k < n:
        hml_long(max_len, n=n, s=label).serialize_to(builder)
    else:
        hml_short(max_len, n, len=n, s=label).serialize_to(builder)


@final
class HashmapDict[V, E = None]:
    """Lazy dictionary backed by a HashmapAugE n X Y cell.

    Reads traverse the tree on demand. The parsed root is cached after
    first access. Writes go into a sorted overlay (SortedDict) and are
    merged with the tree during iteration and serialization.
    """

    def __init__(
        self,
        key_bits: int,
        value_ti: TypeInfo[V],
        extra_ti: TypeInfo[E] = UnitTypeInfo,
        aug: Augmentation[V, E] = UnitAug,
        cell: Cell | None = None,
        allow_empty: bool = True,
    ) -> None:
        self._key_bits = key_bits
        self._value_ti = value_ti
        self._extra_ti = extra_ti
        self._aug = aug
        self._cell = cell
        self._allow_empty = allow_empty
        self._root: ahm_edge[V, E] | None = None
        self._root_parsed = False
        self._overlay: SortedDict[int, V | _Deleted] = SortedDict()

    @property
    def key_bits(self) -> int:
        return self._key_bits

    @property
    def value_ti(self) -> TypeInfo[V]:
        return self._value_ti

    def _get_root(self) -> ahm_edge[V, E] | None:
        if not self._root_parsed:
            if self._cell is not None:
                cs = self._cell.begin_parse()
                self._root = HashmapAugType[V, E]().load_from(
                    cs, self._key_bits, self._value_ti, self._extra_ti
                )
            self._root_parsed = True
        return self._root

    def _key_ba(self, key: int) -> bitarray:
        return int2ba(key, self._key_bits)

    def _tree_iter(self) -> Iterator[tuple[int, V]]:
        root = self._get_root()
        if root is not None:
            yield from _iter_edge(root, bitarray(), self._key_bits)

    def _overlay_live(self) -> Iterator[tuple[int, V]]:
        for k, v in self._overlay.items():
            if not isinstance(v, _Deleted):
                yield (k, v)

    def is_empty(self) -> bool:
        return next(self.items(), None) is None

    def __getitem__(self, key: int) -> V:
        if key in self._overlay:
            val = self._overlay[key]
            if isinstance(val, _Deleted):
                raise KeyError(key)
            return val
        root = self._get_root()
        if root is None:
            raise KeyError(key)
        result = _lookup_edge(root, self._key_ba(key), 0)
        if result is None:
            raise KeyError(key)
        return result

    def __setitem__(self, key: int, value: V) -> None:
        self._overlay[key] = value

    def __delitem__(self, key: int) -> None:
        if key not in self:
            raise KeyError(key)
        self._overlay[key] = _Deleted()

    def __contains__(self, key: object) -> bool:
        if not isinstance(key, int):
            return False
        if key in self._overlay:
            return not isinstance(self._overlay[key], _Deleted)
        root = self._get_root()
        if root is None:
            return False
        return _lookup_edge(root, self._key_ba(key), 0) is not None

    def get(self, key: int, default: V | None = None) -> V | None:
        try:
            return self[key]
        except KeyError:
            return default

    def items(self) -> Iterator[tuple[int, V]]:
        """Iterate in sorted key order, merging tree and overlay lazily."""
        if not self._overlay:
            yield from self._tree_iter()
            return
        skip = frozenset(self._overlay.keys())
        yield from _merge_sorted(self._tree_iter(), self._overlay_live(), skip)

    def keys(self) -> Iterator[int]:
        for k, _ in self.items():
            yield k

    def values(self) -> Iterator[V]:
        for _, v in self.items():
            yield v

    def __iter__(self) -> Iterator[int]:
        return self.keys()

    def __len__(self) -> int:
        return sum(1 for _ in self.items())

    def to_dict(self) -> dict[int, V]:
        return dict(self.items())

    def serialize_to(self, builder: Builder) -> None:
        """Serialize the hashmap.

        HashmapAugE (allow_empty): $0 extra for empty, $1 ^root extra for non-empty.
        HashmapAug (!allow_empty): root edge directly in the builder.
        """
        if self._allow_empty:
            self._serialize_hashmap_e(builder)
        else:
            self._serialize_hashmap(builder)

    def _serialize_hashmap_e(self, builder: Builder) -> None:
        entries = list(self.items())
        if not entries:
            _ = builder.store_uint(0, 1)
            self._aug.extra_ti.serialize_value(self._aug.eval_empty(), builder)
        else:
            root_cell, root_extra = self._build_root(entries)
            _ = builder.store_uint(1, 1)
            _ = builder.store_ref(root_cell)
            self._aug.extra_ti.serialize_value(root_extra, builder)

    def _serialize_hashmap(self, builder: Builder) -> None:
        entries = list(self.items())
        assert entries, "non-empty Hashmap cannot be serialized as empty"
        root_cell, _ = self._build_root(entries)
        _ = builder.store_slice(root_cell.begin_parse())

    def _build_root(self, entries: list[tuple[int, V]]) -> tuple[Cell, E]:
        keys = [int2ba(k, self._key_bits) for k, _ in entries]
        vals = [v for _, v in entries]
        return _build_hashmap(
            keys, vals, 0, len(keys), 0, self._key_bits, self._value_ti, self._aug
        )

    @classmethod
    def load_from(
        cls,
        cs: Slice,
        key_bits: int,
        value_ti: TypeInfo[V],
        extra_ti: TypeInfo[E] = UnitTypeInfo,
        allow_empty: bool = True,
        aug: Augmentation[V, E] = UnitAug,
    ) -> HashmapDict[V, E]:
        """Load a hashmap from a slice.

        allow_empty=True: HashmapAugE format ($0 extra for empty, $1 ^root extra)
        allow_empty=False: HashmapAug format (root edge directly from cs)
        """
        if allow_empty:
            if cs.load_bit():
                cell = cs.load_ref()
                _ = extra_ti.load_from(cs)
                return cls(key_bits, value_ti, extra_ti, aug, cell, allow_empty=True)
            _ = extra_ti.load_from(cs)
            return cls(key_bits, value_ti, extra_ti, aug, allow_empty=True)
        else:
            b = Builder()
            _ = b.store_slice(cs)
            return cls(key_bits, value_ti, extra_ti, aug, b.end_cell(), allow_empty=False)

    @classmethod
    def type_info(
        cls,
        key_bits: int,
        value_ti: TypeInfo[V],
        allow_empty: bool = True,
    ) -> TypeInfo[HashmapDict[V]]:
        """Create a TypeInfo for HashmapDict — used by Ref[Hashmap] and generics."""
        return HashmapDictTypeInfo[V]().instantiate(key_bits, value_ti, allow_empty)


@final
class HashmapDictTypeInfo[V](InstantiableTypeInfo[HashmapDict[V], int, TypeInfo[V], bool]):
    @override
    def serialize_value(self, value: HashmapDict[V], builder: Builder) -> None:
        value.serialize_to(builder)

    @override
    def load_from(
        self,
        cs: Slice,
        key_bits: int,
        value_ti: TypeInfo[V],
        allow_empty: bool,
    ) -> HashmapDict[V]:
        return HashmapDict[V].load_from(cs, key_bits, value_ti, allow_empty=allow_empty)


def _build_hashmap[V, E](
    keys: list[bitarray],
    vals: list[V],
    lo: int,
    hi: int,
    pos: int,
    key_bits: int,
    value_ti: TypeInfo[V],
    aug: Augmentation[V, E],
) -> tuple[Cell, E]:
    """Build a hashmap cell from sorted key/value arrays. Returns (cell, extra)."""
    assert lo < hi
    prefix = _common_prefix(keys, lo, hi, pos)
    pos += len(prefix)
    b = Builder()
    _serialize_label(b, prefix, key_bits - (pos - len(prefix)))
    if hi - lo == 1:
        extra = aug.eval_leaf(vals[lo])
        aug.extra_ti.serialize_value(extra, b)
        value_ti.serialize_value(vals[lo], b)
        return (b.end_cell(), extra)
    else:
        split = _split_at_bit(keys, lo, hi, pos)
        left_cell, left_extra = _build_hashmap(
            keys, vals, lo, split, pos + 1, key_bits, value_ti, aug
        )
        right_cell, right_extra = _build_hashmap(
            keys, vals, split, hi, pos + 1, key_bits, value_ti, aug
        )
        _ = b.store_ref(left_cell)
        _ = b.store_ref(right_cell)
        extra = aug.merge(left_extra, right_extra)
        aug.extra_ti.serialize_value(extra, b)
        return (b.end_cell(), extra)
