"""Put the tools/ directory on sys.path for the whole test session.

Several test modules import sibling host modules directly (``import
bridge_runtime``, ``from awaiting_classifier import ...``). Those only resolve
when tools/ is importable. Individually the tests self-insert their own dir, but
collecting the suite in one ``pytest tools/`` run needs this shared hook — which
is what lets CI run the FULL host suite at once instead of file-by-file.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
