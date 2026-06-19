"""Runtime discovery for the host bridge.

The bridge should work when esp-harness is installed as a normal Python
package. Local source-tree fallbacks are for development, not a hidden
requirement baked into import order.
"""

from __future__ import annotations

import importlib
import os
import sys
from pathlib import Path
from typing import Callable, Mapping


ImportModule = Callable[[str], object]
InsertPath = Callable[[Path], None]

CLIENT_MODULE = "esp_harness.client"
ENV_PYTHONPATH = "ESP_HARNESS_PYTHONPATH"
ENV_ROOT = "ESP_HARNESS_ROOT"


def _repo_sibling_source(repo_root: Path) -> Path:
    return repo_root.parent / "esp-harness" / "tools" / "esp-harness" / "src"


def candidate_source_paths(
    *,
    env: Mapping[str, str] | None = None,
    repo_root: Path | None = None,
) -> list[Path]:
    env = env or os.environ
    repo_root = repo_root or Path(__file__).resolve().parents[1]

    out: list[Path] = []
    explicit_pythonpath = env.get(ENV_PYTHONPATH)
    if explicit_pythonpath:
        out.append(Path(explicit_pythonpath))

    explicit_root = env.get(ENV_ROOT)
    if explicit_root:
        out.append(Path(explicit_root) / "tools" / "esp-harness" / "src")

    out.append(_repo_sibling_source(repo_root))

    seen: set[str] = set()
    unique: list[Path] = []
    for path in out:
        key = str(path)
        if key in seen:
            continue
        seen.add(key)
        unique.append(path)
    return unique


def _default_insert_path(path: Path) -> None:
    path_s = str(path)
    if path_s not in sys.path:
        sys.path.insert(0, path_s)


def _forget_esp_harness_modules() -> None:
    for name in list(sys.modules):
        if name == "esp_harness" or name.startswith("esp_harness."):
            sys.modules.pop(name, None)


def _client_module_missing(exc: ModuleNotFoundError) -> bool:
    return exc.name in ("esp_harness", CLIENT_MODULE)


def import_esp_harness_client(
    *,
    env: Mapping[str, str] | None = None,
    repo_root: Path | None = None,
    import_module: ImportModule = importlib.import_module,
    insert_path: InsertPath = _default_insert_path,
) -> object:
    try:
        return import_module(CLIENT_MODULE)
    except ModuleNotFoundError as first_error:
        if not _client_module_missing(first_error):
            raise
        last_error: Exception = first_error

    for path in candidate_source_paths(env=env, repo_root=repo_root):
        if not path.exists():
            continue
        insert_path(path)
        _forget_esp_harness_modules()
        try:
            return import_module(CLIENT_MODULE)
        except ModuleNotFoundError as exc:
            if not _client_module_missing(exc):
                raise
            last_error = exc

    raise RuntimeError(
        "Could not import esp_harness.client. Install esp-harness with "
        "`pip install -e <esp-harness>/tools/esp-harness`, or set "
        f"{ENV_PYTHONPATH} to the package src directory, or set {ENV_ROOT} "
        "to the esp-harness repository root."
    ) from last_error
