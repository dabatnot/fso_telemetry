from __future__ import annotations

import json
import os
import tempfile
from dataclasses import dataclass
from pathlib import Path

from .models import AvCoreConfig


@dataclass(frozen=True)
class ConfigLoadResult:
    config: AvCoreConfig
    error: str | None = None


class ConfigStore:
    def __init__(self, path: Path):
        self.path = path

    @staticmethod
    def default() -> AvCoreConfig:
        return AvCoreConfig()

    def load(self) -> ConfigLoadResult:
        if not self.path.exists():
            config = self.default()
            try:
                self.save(config)
            except OSError as exc:
                return ConfigLoadResult(config, f"Impossible de créer la configuration : {exc}")
            return ConfigLoadResult(config)

        try:
            payload = json.loads(self.path.read_text(encoding="utf-8"))
            return ConfigLoadResult(AvCoreConfig.model_validate(payload))
        except (OSError, ValueError) as exc:
            return ConfigLoadResult(
                self.default(),
                f"Configuration invalide conservée sur disque : {exc}",
            )

    def save(self, config: AvCoreConfig) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        payload = config.model_dump(mode="json", by_alias=True)
        temporary_path: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                dir=self.path.parent,
                prefix=f".{self.path.name}.",
                suffix=".tmp",
                delete=False,
                newline="\n",
            ) as stream:
                temporary_path = Path(stream.name)
                json.dump(payload, stream, ensure_ascii=False, indent=2)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary_path, self.path)
            temporary_path = None
        finally:
            if temporary_path is not None:
                temporary_path.unlink(missing_ok=True)
