from __future__ import annotations

import runpy


def main() -> None:
    runpy.run_module("docs.library.small_pdf_helpers.main", run_name="__main__")


if __name__ == "__main__":
    main()

