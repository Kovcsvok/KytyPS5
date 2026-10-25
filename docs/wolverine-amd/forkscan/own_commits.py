"""Commits of every active fork branch that upstream main lacks, each counted once.

Usage: python -I own_commits.py <net.git> <forks.json> <out.json>
A commit belongs to the first branch that has it, parents before their forks, then by stars,
so a fork of a fork keeps only what it added.
"""
import collections, json, subprocess, sys

net, forks_path, out_path = sys.argv[1:4]


def git(*args, stdin=None):
    return subprocess.run(["git", "-C", net, *args], input=stdin, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", env={"GIT_NO_LAZY_FETCH": "1", "PATH": __import__("os").environ["PATH"]}).stdout


forks = json.load(open(forks_path, encoding="utf-8"))
parents = {f["repo"]: f["parent"] for f in forks}


def depth(repo):
    d = 0
    while repo in parents:
        repo, d = parents[repo], d + 1
    return d


forks.sort(key=lambda f: (depth(f["repo"]), -f["stars"], f["pushed"]))
owner = {}
per_branch = []
for f in forks:
    for b in f.get("branches", []):
        shas = git("rev-list", b["sha"], "^refs/heads/main").split()
        new = [s for s in shas if s not in owner]
        for s in new:
            owner[s] = (f["repo"], b["name"])
        per_branch.append({"repo": f["repo"], "parent": f["parent"], "stars": f["stars"],
                           "branch": b["name"], "ahead": len(shas), "own": len(new)})

meta = {}
lines = git("log", "--no-walk=unsorted", "--date=short", "--format=%H%x09%ad%x09%an%x09%s", "--stdin",
            stdin="\n".join(owner) + "\n")
for line in lines.splitlines():
    sha, date, author, subject = line.split("\t", 3)
    meta[sha] = {"date": date, "author": author, "subject": subject}

by_repo = collections.defaultdict(list)
for sha, (repo, branch) in owner.items():
    m = meta.get(sha, {})
    by_repo[repo].append({"sha": sha[:10], "branch": branch, **m})
for repo in by_repo:
    by_repo[repo].sort(key=lambda c: c.get("date", ""))
json.dump({"branches": per_branch, "commits": by_repo}, open(out_path, "w", encoding="utf-8"),
          ensure_ascii=False, indent=1)
print("unique non-upstream commits", len(owner), "repos with own commits", len(by_repo))
for repo, cs in sorted(by_repo.items(), key=lambda kv: -len(kv[1]))[:40]:
    print(f"{len(cs):6d} {repo}  {cs[0].get('date', '')}..{cs[-1].get('date', '')}")
