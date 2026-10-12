#!/usr/bin/env python3
"""Render the MIMO Markdown progress document into a readable PDF.

This deliberately small renderer keeps PDF generation reproducible on CPU-only
machines where Pandoc/LaTeX are unavailable. It preserves the Markdown source
as styled text; equations remain readable in their LaTeX source notation.
"""

from __future__ import annotations

import argparse
import re
import textwrap
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

from matplotlib.backends.backend_pdf import PdfPages
from matplotlib.figure import Figure


PAGE_WIDTH = 8.27
PAGE_HEIGHT = 11.69
BODY_WIDTH = 96
LINES_PER_PAGE = 61


def format_markdown(source: str) -> list[str]:
    """Apply lightweight Markdown styling suitable for monospaced PDF text."""
    output: list[str] = []
    in_code = False

    for raw_line in source.splitlines():
        line = raw_line.rstrip()
        if line.startswith("~~~"):
            in_code = not in_code
            continue

        if in_code or line.startswith("    "):
            prefix = "    "
            content = line.strip()
        elif line.startswith("#"):
            level = len(line) - len(line.lstrip("#"))
            prefix = "" if level <= 2 else "  "
            content = line[level:].strip().upper()
        elif re.match(r"^\s*[-*] ", line):
            prefix = "  - "
            content = re.sub(r"^\s*[-*] ", "", line)
        else:
            prefix = ""
            content = line

        if not content:
            output.append("")
            continue

        width = max(20, BODY_WIDTH - len(prefix))
        wrapped = textwrap.wrap(
            content,
            width=width,
            break_long_words=True,
            break_on_hyphens=False,
            replace_whitespace=False,
        )
        output.extend(prefix + part if i == 0 else " " * len(prefix) + part
                      for i, part in enumerate(wrapped))

    return output


def render(source_path: Path, output_path: Path) -> None:
    lines = format_markdown(source_path.read_text(encoding="utf-8"))
    metadata = {
        "Title": "ns3sionna MIMO-Abstraktionsmodell",
        "Author": "ns3sionna MIMO project",
        "Subject": "Generated from ns3sionna_mimo_model.md",
    }

    with PdfPages(output_path, metadata=metadata) as pdf:
        page_count = max(1, (len(lines) + LINES_PER_PAGE - 1) // LINES_PER_PAGE)
        for page_index in range(page_count):
            figure = Figure(figsize=(PAGE_WIDTH, PAGE_HEIGHT))
            figure.patch.set_facecolor("white")
            page = lines[page_index * LINES_PER_PAGE:(page_index + 1) * LINES_PER_PAGE]
            figure.text(
                0.065,
                0.955,
                "\n".join(page),
                va="top",
                ha="left",
                family="DejaVu Sans Mono",
                parse_math=False,
                fontsize=7.6,
                linespacing=1.27,
                color="#111111",
            )
            figure.text(
                0.5,
                0.025,
                f"{source_path.name}  |  Seite {page_index + 1}/{page_count}",
                va="bottom",
                ha="center",
                family="DejaVu Sans",
                fontsize=7,
                color="#666666",
            )
            pdf.savefig(figure)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    render(args.source, args.output)


if __name__ == "__main__":
    main()
