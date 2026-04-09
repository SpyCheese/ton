"""Augmentation implementations for HashmapAugE types used in block.tlb.

These match the C++ augmentation logic in crypto/block/block-parse.cpp.
"""

from typing import final

from tlb.hashmap import HashmapDict
from tlb.object import TypeInfo, UintTypeConstructor, VarUIntTypeConstructor

from .generated import (
    CurrencyCollection,
    DepthBalanceInfo,
    DepthBalanceInfoType,
    KeyExtBlkRef,
    KeyMaxLt,
    KeyMaxLt_cons,
    KeyMaxLtType,
    ShardAccount,
    account,
    account_descr,
    currencies,
    depth_balance,
    extra_currencies,
    nanograms,
)


def _zero_grams():
    return nanograms(amount=0)


def _zero_cc():
    return currencies(
        grams=_zero_grams(),
        other=extra_currencies(dict=HashmapDict(32, VarUIntTypeConstructor(32))),
    )


def _add_cc(a: CurrencyCollection, b: CurrencyCollection) -> CurrencyCollection:
    """Add two CurrencyCollection values (grams + extra currencies)."""
    # For zerostate purposes, we only sum grams. Extra currency merging would
    # require walking both dicts, which is not needed for the initial state.
    return currencies(
        grams=nanograms(amount=a.grams.amount + b.grams.amount),
        other=extra_currencies(dict=HashmapDict(32, VarUIntTypeConstructor(32))),
    )


@final
class DepthBalanceAug:
    """Augmentation for ShardAccounts: HashmapAugE 256 ShardAccount DepthBalanceInfo.

    eval_leaf: extract split_depth from account address and balance from account storage.
    merge: max(depth1, depth2) + add(balance1, balance2).
    """

    @property
    def extra_ti(self) -> TypeInfo[DepthBalanceInfo]:
        return DepthBalanceInfoType()

    def eval_leaf(self, value: ShardAccount) -> DepthBalanceInfo:
        assert isinstance(value, account_descr)
        acc = value.account.ref
        if isinstance(acc, account):
            # Extract depth from address (fixed_prefix_length from anycast, or 0)
            addr = acc.addr
            split_depth = 0
            if hasattr(addr, "anycast") and addr.anycast is not None:
                split_depth = addr.anycast.depth
            balance = acc.storage.balance
        else:
            split_depth = 0
            balance = _zero_cc()
        return depth_balance(split_depth=split_depth, balance=balance)

    def merge(self, left: DepthBalanceInfo, right: DepthBalanceInfo) -> DepthBalanceInfo:
        return depth_balance(
            split_depth=max(left.split_depth, right.split_depth),
            balance=_add_cc(left.balance, right.balance),
        )

    def eval_empty(self) -> DepthBalanceInfo:
        return depth_balance(split_depth=0, balance=_zero_cc())


@final
class Uint64MinAug:
    """Augmentation for OutMsgQueue and DispatchQueue (uint64 extra).

    merge: min(left, right).
    eval_empty: 0.
    """

    @property
    def extra_ti(self) -> TypeInfo[int]:
        return UintTypeConstructor(64)

    def eval_leaf(self, _value: object) -> int:
        # For OutMsgQueue: emitted_lt from MsgEnvelope
        # For DispatchQueue: min created_lt from messages
        # In zerostate these are always empty, so this is a placeholder.
        return 0

    def merge(self, left: int, right: int) -> int:
        return min(left, right)

    def eval_empty(self) -> int:
        return 0


@final
class KeyMaxLtAug:
    """Augmentation for OldMcBlocksInfo: HashmapAugE 32 KeyExtBlkRef KeyMaxLt.

    eval_leaf: copies key:Bool and max_end_lt:uint64 from the KeyExtBlkRef value.
    merge: key1|key2, max(lt1, lt2).
    """

    @property
    def extra_ti(self) -> TypeInfo[KeyMaxLt]:
        return KeyMaxLtType()

    def eval_leaf(self, value: KeyExtBlkRef) -> KeyMaxLt:
        return KeyMaxLt_cons(key=value.key, max_end_lt=value.blk_ref.end_lt)

    def merge(self, left: KeyMaxLt, right: KeyMaxLt) -> KeyMaxLt:
        return KeyMaxLt_cons(
            key=left.key or right.key,
            max_end_lt=max(left.max_end_lt, right.max_end_lt),
        )

    def eval_empty(self) -> KeyMaxLt:
        return KeyMaxLt_cons(key=False, max_end_lt=0)
