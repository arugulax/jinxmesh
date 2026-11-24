"""site-index.py <site folder>: writes <site>/maps/index.json, the list the web page's map loader works from.

One entry per state or province that is published: where its pieces are (the raw address at GitHub), their
sizes, checksums and corners. Made from the lists in ~/.cache/m9-tiles/vector/pieces (split.py, publish.py).
"""
import glob, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import publish

out = sys.argv[1]
maps = []
for path in sorted(glob.glob(f"{publish.PIECES}/*.json")):
    info = json.load(open(path))
    if "published" not in info:
        continue
    name = info["name"]
    folder = info["published"]["folder"]
    repo = publish.repository(name)
    maps.append({
        "name": name,
        "title": " ".join(w.upper() if w in ("us",) else w.capitalize() for w in name.split("-")).replace(" Of ", " of ").replace(" And ", " and "),
        "country": "United States" if folder == "state" else "Canada",
        "folder": folder,
        "base": f"https://raw.githubusercontent.com/{repo}/main/{folder}/",
        "bytes": info["bytes"],
        "pieces": [{k: p[k] for k in ("file", "bytes", "sha256", "south", "west", "north", "east")} for p in info["pieces"]],
    })
os.makedirs(f"{out}/maps", exist_ok=True)
json.dump({"made": time.strftime("%Y-%m-%d"), "maps": maps}, open(f"{out}/maps/index.json", "w"), separators=(",", ":"))
print(f"{len(maps)} maps, {sum(m['bytes'] for m in maps) / 1e9:.1f} GB, in {out}/maps/index.json")
