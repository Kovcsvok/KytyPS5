"""Fork commits by topic, without copies of upstream commits (same subject) or of each other.

Usage: python -I classify.py <net.git> <own.json> <out.txt>
"""
import collections, json, os, re, subprocess, sys

net, own_path, out_path = sys.argv[1:4]
env = {"GIT_NO_LAZY_FETCH": "1", "PATH": os.environ["PATH"]}
upstream = set(subprocess.run(["git", "-C", net, "log", "--format=%s", "refs/heads/main"],
                              capture_output=True, text=True, encoding="utf-8", errors="replace",
                              env=env).stdout.splitlines())


def norm(s):
    return re.sub(r"\s*\(#\d+\)$", "", s.strip()).lower()


upstream = {norm(s) for s in upstream}
own = json.load(open(own_path, encoding="utf-8"))["commits"]

TOPICS = {
    "materialize": r"materiali|srt|walker|descriptor walk|draw prep|scanner|resource plan|user data|ahead of the gpu thread",
    "gpu_thread": r"gpu thread|gpu-thread|render thread|worker thread|pipelin(e|ing) the|two-stage|fan.?out|offload|parallel",
    "readback": r"readback|read back|read-back|download|write-?back|gpu-written|gpu written|gpu.?modified|host.?import|external_memory_host|host pointer|coheren|unified memory",
    "fault": r"\bfault|page fault|protect|unprotect|watch(ed|er)",
    "tracker_upload": r"tracker|upload|dirty|invalidat",
    "submit_flush": r"submit|flush|command buffer|cmdbuf|release_mem|interrupt|\beop\b|fence|timeline",
    "bindings": r"rebind|binding|descriptor|push constant|bindless",
    "locks": r"\block|mutex|contention|shared_mutex|atomic",
    "wolverine": r"wolverine|ppsa03671",
    "perf": r"\bperf|faster|speed ?up|fps|stutter|hitch|hot path|overhead|cheaper|cost",
}
rx = {t: re.compile(p, re.I) for t, p in TOPICS.items()}

seen = set()
hits = collections.defaultdict(list)
kept = 0
for repo, commits in own.items():
    for c in commits:
        subject = c.get("subject", "")
        key = norm(subject)
        if not key or key in upstream or key in seen:
            continue
        seen.add(key)
        kept += 1
        for t, r in rx.items():
            if r.search(subject):
                hits[t].append((c.get("date", ""), repo, c["sha"], subject))

with open(out_path, "w", encoding="utf-8") as out:
    out.write(f"distinct non-upstream subjects: {kept}\n")
    for t in TOPICS:
        rows = sorted(hits[t], reverse=True)
        per_repo = collections.Counter(r[1] for r in rows)
        out.write(f"\n## {t}: {len(rows)} commits; top repos: "
                  + ", ".join(f"{r} {n}" for r, n in per_repo.most_common(8)) + "\n")
        for date, repo, sha, subject in rows:
            out.write(f"{date} {repo:32.32s} {sha} {subject[:150]}\n")
print("distinct", kept, {t: len(v) for t, v in hits.items()})
