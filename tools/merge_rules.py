"""Merges the rules of a sheet that say the same thing first: the same "on" or "while" line and the
same subject (subject, side, kind, template). Each merged rule's own place and timing (at, delay,
chance, cooldown, wait) go onto its actions' lines, so the merged rule does exactly what the rules
did. A rule nothing merges with is left as it is, and so are comments and predictions. The sheet is
rewritten: look at it before you commit.

    python tools/merge_rules.py <sheet.rules> [<sheet.rules> ...]
"""
import io
import re
import sys

WHO = ("subject", "side", "kind", "template")
OWN = ("at", "delay", "chance", "cooldown", "wait")


def first_word(line):
    return line.split()[0] if line.split() else ""


def merge(path):
    raw = io.open(path, encoding="utf-8", newline="").read()
    nl = "\r\n" if "\r\n" in raw else "\n"
    # Blocks: a blank line ends one. A rule block has its comments, its first line, the lines under it.
    chunks = re.split(r"\n[ \t]*\n", raw.replace("\r\n", "\n").strip("\n"))
    blocks = []
    for chunk in chunks:
        lines = chunk.split("\n")
        body = [l for l in lines if l.strip() and not l.lstrip().startswith("#")]
        if not body or not re.match(r"^(on|while) ", body[0].strip()) or any(l.lstrip().startswith("#") for l in lines[lines.index(body[0]):]):
            blocks.append({"text": chunk})
            continue
        comments = lines[: lines.index(body[0])]
        rest = [l.strip() for l in body[1:]]
        blocks.append({
            "comments": comments,
            "head": body[0].strip(),
            "who": [l for l in rest if first_word(l) in WHO],
            "own": [l for l in rest if first_word(l) in OWN],
            "actions": [l for l in rest if first_word(l) not in WHO + OWN],
            "text": chunk,
        })
    groups = {}
    for b in blocks:
        if "head" in b:
            groups.setdefault((b["head"], tuple(sorted(b["who"]))), []).append(b)
    out = []
    merged = 0
    for b in blocks:
        if "head" not in b:
            out.append(b["text"])
            continue
        group = groups[(b["head"], tuple(sorted(b["who"])))]
        if len(group) == 1:
            out.append(b["text"])
            continue
        if b is not group[0]:
            continue  # (written with the first of its group)
        lines = []
        names = [c.lstrip("# ").strip() for g in group for c in g["comments"]]
        if all(len(g["comments"]) == 1 and " " not in g["comments"][0].lstrip("# ").strip() for g in group):
            lines.append("# " + ", ".join(names))
        else:
            for g in group:
                lines += g["comments"]
        lines.append(b["head"])
        lines += ["    " + w for w in b["who"]]
        for g in group:
            suffix = " ".join(g["own"])
            for a in g["actions"]:
                if suffix:
                    # (Before an expression, which runs to the end of the line.)
                    m = re.search(r"\s(=|volume_by)\s", a)
                    a = a[: m.start()] + " " + suffix + a[m.start():] if m else a + " " + suffix
                lines.append("    " + a)
        merged += len(group) - 1
        out.append("\n".join(lines))
    io.open(path, "w", encoding="utf-8", newline="").write(("\n\n".join(out) + "\n").replace("\n", nl))
    print("%s: %d rules merged into others" % (path, merged))


for p in sys.argv[1:]:
    merge(p)
