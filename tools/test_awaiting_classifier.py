"""Quick smoke for classify_awaiting() — confirms all 5 kinds fire on
representative inputs. Not pytest, just an executable spec.

Run::
    python tools/test_awaiting_classifier.py
"""

from __future__ import annotations

import sys

from awaiting_classifier import classify_awaiting


CASES = [
    # (label, args, expected_kind)
    ("approve — permission required",
     ("pre_tool_use_permission", "", "Bash", "git push --force origin master"),
     "approve"),

    ("pick — three numbered options",
     ("stop",
      "Which migrate strategy do you want?\n"
      "1) inline — fold the old function into the call sites\n"
      "2) defer  — leave a deprecation shim, ship cleanup next week\n"
      "3) abort  — rollback and revisit\n",
      "", ""),
     "pick"),

    ("pick — bulleted options",
     ("stop",
      "Pick one of these:\n- inline\n- defer\n- abort\n",
      "", ""),
     "pick"),

    ("clarify — keyword 'did you mean'",
     ("stop",
      "I see auth.py and auth/__init__.py — did you mean the package or the module?",
      "", ""),
     "clarify"),

    ("clarify — keyword 'could you clarify'",
     ("stop",
      "Found 12 matches. Could you clarify whether you want all of them updated or just the ones in src/?",
      "", ""),
     "clarify"),

    ("type — open-ended question",
     ("stop",
      "Got it. What should the new branch be called?",
      "", ""),
     "type"),

    ("type — last sentence is the question",
     ("stop",
      "Refactored. Tests pass locally. What commit message would you like?",
      "", ""),
     "type"),

    ("continue — generic 'done'",
     ("stop",
      "All set. Refactor of src/auth.py is committed and pushed to origin.",
      "", ""),
     "continue"),

    ("continue — no assistant text",
     ("stop", "", "", ""),
     "continue"),
]


def main() -> int:
    fails = 0
    for label, args, want in CASES:
        kind, ctx = classify_awaiting(*args)
        ok = (kind == want)
        if not ok:
            fails += 1
        status = "PASS" if ok else "FAIL"
        ctx_str = " | ".join(ctx)[:80]
        print(f"  [{status}] {label}")
        print(f"          got kind={kind!r:11}  want {want!r}")
        print(f"          ctx → {ctx_str}")
    print()
    if fails:
        print(f"{fails}/{len(CASES)} FAILED")
        return 1
    print(f"all {len(CASES)} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
