"""Tests for generated code with special (exotic) cell types."""

import pytest
from generated.special_cells import (
    MerkleProofType,
    MerkleUpdateType,
)
from tlb.object import TlbModelError, UintTypeConstructor


class TestMerkleProofType:
    def test_deserialize_rejects_ordinary_cell(self):
        """MerkleProofType.deserialize should reject non-special cells."""
        from pytoniq_core import Builder

        # Build a regular cell with the right tag
        b = Builder()
        _ = b.store_uint(0x03, 8)  # merkle proof tag
        with pytest.raises(TlbModelError, match="special"):
            _ = MerkleProofType[int]().deserialize(b.end_cell(), UintTypeConstructor(32))


class TestMerkleUpdateType:
    def test_deserialize_rejects_ordinary_cell(self):
        """MerkleUpdateType.deserialize should reject non-special cells."""
        from pytoniq_core import Builder

        b = Builder()
        _ = b.store_uint(0x04, 8)  # merkle update tag
        with pytest.raises(TlbModelError, match="special"):
            _ = MerkleUpdateType[int]().deserialize(b.end_cell(), UintTypeConstructor(32))
