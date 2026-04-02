"""Configuration for well-known type simplifications."""

from dataclasses import dataclass, field

from .sema.types import WellKnownType


@dataclass(frozen=True)
class SimplifyConfig:
    """Controls which well-known type simplifications are active."""

    simplify: frozenset[WellKnownType] = field(default_factory=frozenset)

    @staticmethod
    def all() -> SimplifyConfig:
        return SimplifyConfig(simplify=frozenset(WellKnownType))

    @staticmethod
    def none() -> SimplifyConfig:
        return SimplifyConfig()

    def is_enabled(self, wkt: WellKnownType) -> bool:
        return wkt in self.simplify
