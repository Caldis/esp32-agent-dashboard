"""Source-level architecture checks for firmware Modules.

Run:
    python tools/test_firmware_architecture.py
"""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_snapshot_apply_module_exists_and_exposes_result_interface() -> None:
    header = read("main/harness/agent_snapshot_apply.h")
    source = read("main/harness/agent_snapshot_apply.c")

    assert "agent_snapshot_apply_result_t" in header
    assert "agent_snapshot_apply_json" in header
    assert "dropped_count" in header
    assert "agent_snapshot_apply_json" in source
    assert "snapshot_dropped_agents" in source


def test_agent_commands_delegates_snapshot_apply() -> None:
    source = read("main/harness/agent_commands.c")

    assert '#include "agent_snapshot_apply.h"' in source
    assert "agent_snapshot_apply_json(json, end, &result)" in source
    assert "merge_agent_object" not in source
    assert "silently drop overflow" not in source


def test_snapshot_apply_is_in_firmware_build() -> None:
    cmake = read("main/CMakeLists.txt")

    assert '"harness/agent_snapshot_apply.c"' in cmake


def test_health_reports_dropped_agents() -> None:
    source = read("main/harness/agent_commands.c")
    state = read("main/agent_state.h")

    assert "snapshot_dropped_agents" in state
    assert '\\"snapshot_dropped_agents\\":%u' in source


def test_config_persists_motion_reduced_without_unlocked_state_read() -> None:
    source = read("main/harness/agent_commands.c")

    assert "bool motion_reduced_value" in source
    assert 'persist_u8("motion_red", motion_reduced_value ? 1 : 0)' in source
    assert 'persist_u8("motion_red", s->motion_reduced ? 1 : 0)' not in source


def main() -> int:
    tests = [
        test_snapshot_apply_module_exists_and_exposes_result_interface,
        test_agent_commands_delegates_snapshot_apply,
        test_snapshot_apply_is_in_firmware_build,
        test_health_reports_dropped_agents,
        test_config_persists_motion_reduced_without_unlocked_state_read,
    ]
    failures = 0
    for test in tests:
        try:
            test()
        except Exception as exc:  # pragma: no cover
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
