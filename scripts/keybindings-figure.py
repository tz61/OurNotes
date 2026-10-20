#!/usr/bin/env python3
"""Draws docs/images/keybindings.svg, the key bindings figure in the README.

Run it again after changing a shortcut: python3 scripts/keybindings-figure.py
"""
from pathlib import Path
from xml.sax.saxutils import escape

# (section, [(keys, what)]); keys separated by " / " are alternatives, "+" joins a chord
SECTIONS = [
    ("PDF reader, on the page", [
        ("1 … 4", "Select, highlight, underline, note"),
        ("5 … 8", "Text, area, draw, eraser"),
        ("F", "Fit width; F again (within 10 s): fit page"),
        ("H", "Back to the library"),
        ("[ / ]", "Show or hide the left / right panel"),
        ("I / N", "Info / Notes beside the page"),
        ("L", "Dark pages, or the PDF's own colours"),
        ("Ctrl+F", "Find in the document"),
        ("Ctrl+Z / Ctrl+Y", "Undo / redo annotation changes"),
        ("Delete", "Delete the selected annotation"),
        ("Ctrl+= / Ctrl+-", "Zoom in / out (also Ctrl+wheel)"),
        ("Shift+wheel", "Scroll sideways"),
        ("Esc", "Clear the selection, close a comment"),
    ]),
    ("Tabs and window", [
        ("Ctrl+W", "Close the tab"),
        ("Ctrl+Shift+T", "Reopen the last closed tab"),
        ("Ctrl+Tab", "Next tab"),
        ("Ctrl+Shift+Tab", "Previous tab"),
        ("Ctrl+1", "Library tab"),
        ("F11", "Full screen"),
        ("Ctrl+Shift+S", "Sync now"),
    ]),
    ("Library", [
        ("Enter", "Open the PDFs, top to bottom"),
        ("Delete", "To the trash (in a collection: out of it)"),
        ("Shift+Delete", "Delete permanently"),
        ("Esc", "In the search: clear it and the selection"),
        ("Up / Down", "In the search: earlier searches"),
        ("Tab", "In the search: complete; again: the list"),
        ("+ / -", "Expand / collapse all"),
        ("Ctrl+Shift+I", "Add by DOI, ISBN, arXiv id or PMID"),
        ("Ctrl+Shift+O", "New note"),
        ("Ctrl+Shift+A", "Copy citation"),
        ("Ctrl+Shift+C", "Copy bibliography"),
    ]),
    ("Notes", [
        ("Ctrl+B / Ctrl+I / Ctrl+U", "Bold, italic, underline"),
        ("Ctrl+K", "Link"),
        ("Ctrl+M / Ctrl+Shift+M", "Inline / display formula (or $…$, $$…$$)"),
        ("# / - / 1. / >", "Line start, then space: heading, lists, quote"),
        ("Backspace", "At a heading's start: its #s, to edit"),
        ("Ctrl+= / Ctrl+- / Ctrl+0", "Text size (also Ctrl+wheel)"),
        ("Ctrl+Shift+V", "Paste as plain text"),
    ]),
    ("Browser (OurNotes Connector)", [
        ("Alt+Shift+O", "Save the page, with its PDF, to OurNotes"),
    ]),
]
COLUMNS = [[0, 1], [2, 3, 4]]  # sections per column

FONT = "-apple-system, 'Segoe UI', 'Noto Sans', Helvetica, Arial, sans-serif"
MONO = "ui-monospace, 'SF Mono', 'Cascadia Mono', 'DejaVu Sans Mono', Menlo, Consolas, monospace"
INK, MUTED, LINE, CAP, CAP_EDGE, ACCENT = "#1f2328", "#59636e", "#d1d9e0", "#f6f8fa", "#c4ccd4", "#3d74d6"
ROW, HEAD, GAP, PAD, COL_W, KEYS_W = 30, 34, 18, 28, 580, 280
CHAR = 7.4  # width of a monospace character at 12.5px, generously


def keycaps(keys, x, y):
    """Key caps for "Ctrl+Shift+T / Ctrl+W", left to right from x; returns the SVG."""
    out = []
    for n, alternative in enumerate(keys.split(" / ")):
        if n:
            out.append(f'<text x="{x + 3:.1f}" y="{y + 15}" fill="{MUTED}" font-size="12">/</text>')
            x += 14
        for m, key in enumerate(alternative.split("+") if alternative not in ("+", "+ ") else ["+"]):
            if m:
                out.append(f'<text x="{x + 1:.1f}" y="{y + 15}" fill="{MUTED}" font-size="11">+</text>')
                x += 11
            w = max(22, len(key) * CHAR + 12)
            out.append(f'<rect x="{x:.1f}" y="{y}" width="{w:.1f}" height="21" rx="4" fill="{CAP}" '
                       f'stroke="{CAP_EDGE}"/>')
            out.append(f'<rect x="{x:.1f}" y="{y + 19}" width="{w:.1f}" height="2" rx="1" fill="{CAP_EDGE}"/>')
            out.append(f'<text x="{x + w / 2:.1f}" y="{y + 15}" text-anchor="middle" font-family="{MONO}" '
                       f'font-size="12.5" fill="{INK}">{escape(key)}</text>')
            x += w + 3
    return "\n".join(out), x


def main():
    body = []
    heights = []
    for c, sections in enumerate(COLUMNS):
        x0 = PAD + c * (COL_W + 2 * PAD)
        y = PAD + 46
        for s in sections:
            title, rows = SECTIONS[s]
            body.append(f'<text x="{x0}" y="{y + 16}" font-size="15" font-weight="600" fill="{ACCENT}">'
                        f'{escape(title)}</text>')
            y += HEAD
            for keys, what in rows:
                caps, end = keycaps(keys, x0, y)
                body.append(caps)
                tx = max(x0 + KEYS_W, end + 10)
                body.append(f'<text x="{tx:.1f}" y="{y + 15}" font-size="13.5" fill="{INK}">{escape(what)}</text>')
                body.append(f'<line x1="{x0}" y1="{y + ROW - 3.5}" x2="{x0 + COL_W}" y2="{y + ROW - 3.5}" '
                            f'stroke="{LINE}" stroke-width="0.6"/>')
                y += ROW
            y += GAP
        heights.append(y)
    width = 2 * COL_W + 4 * PAD
    height = max(heights) + 20
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" font-family="{FONT}">
<!-- drawn by scripts/keybindings-figure.py -->
<rect x="0.5" y="0.5" width="{width - 1}" height="{height - 1}" rx="12" fill="#ffffff" stroke="{LINE}"/>
<text x="{PAD}" y="{PAD + 20}" font-size="20" font-weight="700" fill="{INK}">OurNotes key bindings</text>
<text x="{width - PAD}" y="{PAD + 20}" text-anchor="end" font-size="12.5" fill="{MUTED}">On macOS, Ctrl is ⌘ Command</text>
{chr(10).join(body)}
</svg>
'''
    out = Path(__file__).resolve().parent.parent / "docs" / "images" / "keybindings.svg"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(svg, encoding="utf-8")
    print(out)


if __name__ == "__main__":
    main()
