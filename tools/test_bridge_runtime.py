"""Executable tests for bridge_runtime.py.

Run:
    python tools/test_bridge_runtime.py
"""

from __future__ import annotations

import tempfile
import sys
from pathlib import Path

import bridge_runtime


class FakeImporter:
    def __init__(self, *, fail_times: int = 0):
        self.fail_times = fail_times
        self.calls: list[str] = []
        self.module = object()

    def __call__(self, name: str):
        self.calls.append(name)
        if self.fail_times > 0:
            self.fail_times -= 1
            raise ModuleNotFoundError(f"No module named {name!r}", name=name)
        return self.module


def test_prefers_installed_package_without_inserting_local_path() -> None:
    with tempfile.TemporaryDirectory() as td:
        harness_root = Path(td) / "esp-harness"
        src = harness_root / "tools" / "esp-harness" / "src"
        src.mkdir(parents=True)

        importer = FakeImporter()
        inserted: list[Path] = []

        module = bridge_runtime.import_esp_harness_client(
            env={"ESP_HARNESS_ROOT": str(harness_root)},
            repo_root=Path(td) / "esp32-agent-dashboard",
            import_module=importer,
            insert_path=inserted.append,
        )

        assert module is importer.module
        assert importer.calls == ["esp_harness.client"]
        assert inserted == []


def test_uses_explicit_env_source_when_installed_package_missing() -> None:
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "harness-src"
        src.mkdir()

        importer = FakeImporter(fail_times=1)
        inserted: list[Path] = []

        module = bridge_runtime.import_esp_harness_client(
            env={"ESP_HARNESS_PYTHONPATH": str(src)},
            repo_root=Path(td) / "repo",
            import_module=importer,
            insert_path=inserted.append,
        )

        assert module is importer.module
        assert importer.calls == ["esp_harness.client", "esp_harness.client"]
        assert inserted == [src]


def test_error_explains_how_to_configure_runtime() -> None:
    importer = FakeImporter(fail_times=10)

    try:
        bridge_runtime.import_esp_harness_client(
            env={},
            repo_root=Path("Z:/definitely-missing/repo"),
            import_module=importer,
            insert_path=lambda _path: None,
        )
    except RuntimeError as exc:
        msg = str(exc)
    else:
        raise AssertionError("expected RuntimeError")

    assert "pip install -e" in msg
    assert "ESP_HARNESS_PYTHONPATH" in msg
    assert "ESP_HARNESS_ROOT" in msg


def test_fallback_reloads_parent_package_when_installed_package_lacks_client() -> None:
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        old_pkg = root / "old" / "esp_harness"
        new_pkg = root / "new" / "esp_harness"
        old_pkg.mkdir(parents=True)
        new_pkg.mkdir(parents=True)
        (old_pkg / "__init__.py").write_text("ORIGIN = 'old'\n", encoding="utf-8")
        (new_pkg / "__init__.py").write_text("ORIGIN = 'new'\n", encoding="utf-8")
        (new_pkg / "client.py").write_text("CLIENT_ORIGIN = 'new'\n", encoding="utf-8")

        old_sys_path = list(sys.path)
        old_modules = {
            name: sys.modules.get(name)
            for name in ("esp_harness", "esp_harness.client")
        }
        try:
            sys.path.insert(0, str(root / "old"))
            module = bridge_runtime.import_esp_harness_client(
                env={"ESP_HARNESS_PYTHONPATH": str(root / "new")},
                repo_root=root / "repo",
            )
            assert getattr(module, "CLIENT_ORIGIN") == "new"
        finally:
            sys.path[:] = old_sys_path
            for name, module in old_modules.items():
                if module is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = module


def test_bridge_script_uses_runtime_module_instead_of_hardcoded_src_path() -> None:
    script = (Path(__file__).parent / "claude_buddy_bridge.py").read_text(
        encoding="utf-8"
    )

    assert "_ESP_HARNESS_SRC" not in script
    assert "import_esp_harness_client" in script


def main() -> int:
    tests = [
        test_prefers_installed_package_without_inserting_local_path,
        test_uses_explicit_env_source_when_installed_package_missing,
        test_error_explains_how_to_configure_runtime,
        test_fallback_reloads_parent_package_when_installed_package_lacks_client,
        test_bridge_script_uses_runtime_module_instead_of_hardcoded_src_path,
    ]
    failures = 0
    for test in tests:
        try:
            test()
        except Exception as exc:  # pragma: no cover - executable test runner
            failures += 1
            print(f"[FAIL] {test.__name__}: {exc}")
        else:
            print(f"[PASS] {test.__name__}")
    if failures:
        print(f"{failures}/{len(tests)} failed")
        return 1
    print(f"all {len(tests)} passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
