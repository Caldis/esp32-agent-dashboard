"""包入口:python -m tools.hooks_admin <action> ..."""
import sys
from .cli import main

if __name__ == "__main__":
    sys.exit(main())
