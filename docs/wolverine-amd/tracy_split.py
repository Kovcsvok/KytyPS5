import csv
import collections
import sys

O = sys.argv[1]


def load(fname, exact):
    rows = []
    with open(f"{O}/{fname}", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            if r["name"] == exact:
                rows.append((int(r["ns_since_start"]), int(r["exec_time_ns"]), r["thread"]))
    return rows


Z = {
    "present": ("u_Presenter__Impl__Present.csv", "Libs::Graphics::Presenter::Impl::Present"),
    "readmem": ("u_BufferCache__ReadMemory.csv", "Libs::Graphics::BufferCache::ReadMemory"),
    "readmem_gpu": ("u_ReadMemory_GPU_thread_.csv", "BufferCache::ReadMemory(GPU thread)"),
    "wait_wr": ("u_WaitWriteReadback.csv", "BufferCache::WaitWriteReadback"),
    "fault": ("u_HandleFault.csv", "Libs::Graphics::RenderContext::HandleFault"),
    "invalidate": ("u_InvalidateMemory.csv", "Libs::Graphics::BufferCache::InvalidateMemory"),
    "sched_wait": ("u_CommandScheduler__Wait.csv", "Libs::Graphics::CommandScheduler::Wait"),
    "process": ("u_CommandProcessor__Process.csv", "CommandProcessor::Process"),
    "waitwork": ("u_GuestGpu__WaitForWork.csv", "GuestGpu::WaitForWork"),
    "proccmd": ("u_ProcessCommands_command_.csv", "GuestGpu::ProcessCommands(command)"),
    "cqr": ("u_CopyQueueReadback.csv", "BufferCache::CopyQueueReadback"),
    "begin_wr": ("u_BeginWriteReadback.csv", "Libs::Graphics::BufferCache::BeginWriteReadback"),
    "finish_wr": ("u_FinishWriteReadback.csv", "Libs::Graphics::BufferCache::FinishWriteReadback"),
    "land": ("u_LandQueueReadback.csv", "Libs::Graphics::BufferCache::LandQueueReadback"),
    "begin_q": ("u_BeginQueueReadback.csv", "BufferCache::BeginQueueReadback"),
}
data = {k: load(*v) for k, v in Z.items()}
for k, v in data.items():
    if not v:
        print("EMPTY", k)

switch = min(t for t, _, _ in data["begin_q"])
pres = sorted(t for t, _, _ in data["present"])
start, end = pres[0], pres[-1]
halves = {"A alap": (start, switch), "B queue1": (switch + 1_000_000_000, end)}  # skip 1 s after switch

gpu_thread = collections.Counter(th for _, _, th in data["process"]).most_common(1)[0][0]
print(f"capture {start/1e9:.1f}-{end/1e9:.1f} s, switch at {switch/1e9:.1f} s, GPU thread {gpu_thread}")

for hname, (a, b) in halves.items():
    dur = (b - a) / 1e9
    frames = sum(1 for t in pres if a <= t < b)
    fps = frames / dur
    print(f"\n=== {hname}: {dur:.1f} s, {frames} presents, {fps:.1f} fps, {1000/fps:.0f} ms/frame")

    def per_thread(key):
        acc = collections.defaultdict(lambda: [0, 0])
        for t, d, th in data[key]:
            if a <= t < b:
                acc[th][0] += d
                acc[th][1] += 1
        return acc

    def fmt(acc, th):
        s, n = acc.get(th, [0, 0])
        return f"{s/1e6/frames:6.1f} ms/fr {n/frames:6.1f}/fr {(s/n/1e3 if n else 0):7.0f} us"

    rm = per_thread("readmem")
    wwr = per_thread("wait_wr")
    flt = per_thread("fault")
    inv = per_thread("invalidate")
    sw = per_thread("sched_wait")
    threads = sorted(rm, key=lambda th: -rm[th][0])[:4]
    for th in threads:
        print(f" guest thread {th}:")
        print(f"   HandleFault        {fmt(flt, th)}")
        print(f"   ReadMemory         {fmt(rm, th)}")
        print(f"   WaitWriteReadback  {fmt(wwr, th)}")
        s_rm = rm[th][0]
        s_w = wwr.get(th, [0, 0])[0]
        print(f"   ReadMemory - wait  {(s_rm - s_w)/1e6/frames:6.1f} ms/fr  (GPU-thread round trips + rest)")
        print(f"   InvalidateMemory   {fmt(inv, th)}")
        print(f"   Scheduler::Wait    {fmt(sw, th)}")
    print(f" GPU thread {gpu_thread}:")
    for key in ("process", "waitwork", "proccmd", "begin_wr", "finish_wr", "land", "begin_q", "cqr", "readmem_gpu", "sched_wait"):
        print(f"   {key:12s}       {fmt(per_thread(key), gpu_thread)}")
