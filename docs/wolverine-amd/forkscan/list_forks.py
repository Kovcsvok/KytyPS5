"""Active forks of KytyPS5/KytyPS5, forks of forks too, with every branch and its head.

Usage: TOK=... python -I list_forks.py <out.json>
Errors are kept in the output (never skipped silently).
"""
import json, os, sys, urllib.request, concurrent.futures as cf

TOK = os.environ["TOK"]
OUT = sys.argv[1]
SINCE = "2026-09-20"


def get(url):
    req = urllib.request.Request(url, headers={"Authorization": "Bearer " + TOK,
                                               "Accept": "application/vnd.github+json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.load(r)
    except Exception as e:
        return {"error": str(e)}


def list_forks(repo):
    out, page = [], 1
    while True:
        d = get(f"https://api.github.com/repos/{repo}/forks?sort=newest&per_page=100&page={page}")
        if not isinstance(d, list):
            print("fork list error", repo, d, file=sys.stderr)
            break
        if not d:
            break
        out += d
        page += 1
    return out


seen, queue, active, errors = set(), ["KytyPS5/KytyPS5"], [], []
while queue:
    repo = queue.pop()
    for f in list_forks(repo):
        name = f["full_name"]
        if name in seen:
            continue
        seen.add(name)
        if f.get("forks_count", 0) > 0:
            queue.append(name)
        if f["pushed_at"] and f["pushed_at"] >= SINCE and name != "Kovcsvok/KytyPS5":
            active.append({"repo": name, "parent": repo, "pushed": f["pushed_at"],
                           "stars": f["stargazers_count"], "forks": f["forks_count"],
                           "default": f["default_branch"]})
print("forks seen", len(seen), "active since", SINCE, len(active), file=sys.stderr)


def branches(entry):
    out, page = [], 1
    while True:
        b = get(f"https://api.github.com/repos/{entry['repo']}/branches?per_page=100&page={page}")
        if not isinstance(b, list):
            entry["error"] = b.get("error")
            break
        if not b:
            break
        out += [{"name": x["name"], "sha": x["commit"]["sha"]} for x in b]
        page += 1
    entry["branches"] = out
    return entry


with cf.ThreadPoolExecutor(8) as ex:
    active = list(ex.map(branches, active))
json.dump(active, open(OUT, "w", encoding="utf-8"), indent=1)
print("branches", sum(len(a["branches"]) for a in active),
      "errors", sum(1 for a in active if a.get("error")), file=sys.stderr)
