"""Print the body of docs/releases/<tag>.md for a GitHub release.

Strips the OKF frontmatter and the leading blank lines, so the file can stay
a regular docs/ page while the release shows only the notes.

Usage: python scripts/release_notes.py docs/releases/v1.7.0.md > body.md
"""
import sys


def release_body(text):
    lines = text.splitlines()
    if lines and lines[0].strip() == "---":
        for i in range(1, len(lines)):
            if lines[i].strip() == "---":
                lines = lines[i + 1:]
                break
    while lines and not lines[0].strip():
        lines.pop(0)
    return "\n".join(lines).rstrip() + "\n"


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: release_notes.py <notes.md>")
    with open(sys.argv[1], encoding="utf-8") as f:
        sys.stdout.buffer.write(release_body(f.read()).encode("utf-8"))
