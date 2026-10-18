"""Generate the GitHub wiki pages from the repo docs.

The repo is the source: README.md, the datasheet and the development log.
This splits the README into pages, copies the datasheet and the log, and
rewrites links for the wiki (pages by name, repo files as GitHub URLs,
images as raw URLs).  Output: docs/wiki/.

  python tools/make_wiki.py

Publish: the wiki is its own git repo.  Enable the wiki on GitHub, create
any first page there, then:
  git clone https://github.com/ramiss/RX58_ESP32C5.wiki.git
  copy docs/wiki/*.md into it, commit, push.
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "docs" / "wiki"
GH = "https://github.com/ramiss/RX58_ESP32C5"
BLOB = f"{GH}/blob/main/"
RAW = "https://raw.githubusercontent.com/ramiss/RX58_ESP32C5/main/"

# README "## " sections -> wiki pages, in sidebar order.  None = the intro.
PAGES = [
    ("Home", [None, "At a glance"]),
    ("Hardware", ["Hardware"]),
    ("Flashing", ["Flashing"]),
    ("Host-Integration", ["Using it from a host"]),
    ("Lap-Timers", ["Lap timers: antenna or no antenna"]),
    ("Behaviour", ["Behaviour reference", "What's verified", "Limitations"]),
    ("Building", ["Building from source", "Repository layout"]),
    ("License-and-Credits", ["License"]),
]
FILES = {   # repo file -> wiki page
    "docs/RX58_ESP32C5_SPI_Interface.md": "Datasheet",
    "docs/DEVELOPMENT_LOG.md": "Development-Log",
    "README.md": "Home",
}
TITLES = {"Host-Integration": "Host integration", "Lap-Timers": "Lap timers",
          "License-and-Credits": "License and credits", "Development-Log": "Development log"}


def slug(heading):
    """GitHub's heading anchor: lowercase, punctuation dropped, spaces -> '-'."""
    s = heading.strip().lower()
    s = re.sub(r"[^\w\- ]", "", s)
    return s.replace(" ", "-")


def split_sections(text):
    """README -> {section heading or None: markdown}, keeping order."""
    out, cur, buf = {}, None, []
    for line in text.splitlines():
        m = re.match(r"^## (.+)$", line)
        if m:
            out[cur] = "\n".join(buf).strip("\n")
            cur, buf = m.group(1).strip(), [line]
        else:
            buf.append(line)
    out[cur] = "\n".join(buf).strip("\n")
    return out


def anchor_map(sections):
    """README anchor -> page, for every heading inside each page."""
    amap = {}
    for page, keys in PAGES:
        for k in keys:
            for h in re.findall(r"^#{2,4} (.+)$", sections.get(k, ""), re.M):
                amap[slug(h)] = page
    return amap


def rewrite(md, src_dir, amap):
    """Rewrite links in markdown that lived at src_dir (relative to the repo)."""
    def fix(target):
        if re.match(r"^[a-z]+:", target):
            return target                                   # absolute URL
        path, _, frag = target.partition("#")
        if not path:                                        # same-file anchor
            page = amap.get(frag)
            return f"{page}#{frag}" if page and src_dir == "" else target
        rel = (Path(src_dir) / path).as_posix() if src_dir else path
        rel = str(Path(rel).as_posix())
        rel = re.sub(r"[^/]+/\.\./", "", rel)               # docs/../README.md -> README.md
        if rel in FILES:
            page = FILES[rel]
            if rel == "README.md" and frag:
                page = amap.get(frag, "Home")
            return page + (f"#{frag}" if frag else "")
        if re.search(r"\.(svg|png|jpg|jpeg|gif)$", rel, re.I):
            return RAW + rel
        return BLOB + rel + (f"#{frag}" if frag else "")

    md = re.sub(r"\]\(([^)\s]+)\)", lambda m: f"]({fix(m.group(1))})", md)
    md = re.sub(r'(src|srcset)="([^"]+)"', lambda m: f'{m.group(1)}="{fix(m.group(2))}"', md)
    return md


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    readme = (REPO / "README.md").read_text(encoding="utf-8")
    sections = split_sections(readme)
    amap = anchor_map(sections)
    missing = [k for _, keys in PAGES for k in keys if k not in sections]
    if missing:
        raise SystemExit(f"README sections not found: {missing}")

    written = []
    for page, keys in PAGES:
        parts = [sections[k] for k in keys]
        md = rewrite("\n\n".join(parts), "", amap)
        if page == "Home":
            md = re.sub(r"^# .+\n", "", md)                 # the wiki titles Home itself
            # Added after rewrite(): these are already wiki page links.
            md += ("\n\n## Pages\n\n" + "\n".join(
                f"- [{TITLES.get(p, p.replace('-', ' '))}]({p})" for p, _ in PAGES[1:])
                + "\n- [Datasheet](Datasheet): the host interface specification"
                  "\n- [Development log](Development-Log): how it was built and tested")
        (OUT / f"{page}.md").write_text(md.rstrip() + "\n", encoding="utf-8")
        written.append(page)

    for src, page in FILES.items():
        if src == "README.md":
            continue
        md = (REPO / src).read_text(encoding="utf-8")
        md = rewrite(md, str(Path(src).parent.as_posix()), amap)
        (OUT / f"{page}.md").write_text(md.rstrip() + "\n", encoding="utf-8")
        written.append(page)

    side = ["**RX58_ESP32C5**", ""]
    for page in written:
        side.append(f"- [{TITLES.get(page, page.replace('-', ' '))}]({page})")
    (OUT / "_Sidebar.md").write_text("\n".join(side) + "\n", encoding="utf-8")
    (OUT / "_Footer.md").write_text(
        f"Generated from the [repository]({GH}) (README, datasheet, development log) by "
        f"`tools/make_wiki.py`. Edit the repository, not the wiki.\n", encoding="utf-8")
    print("wrote", ", ".join(written), "+ _Sidebar, _Footer ->", OUT)


if __name__ == "__main__":
    main()
