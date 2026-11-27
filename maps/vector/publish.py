"""publish.py <region>...: puts the pieces of a state or province (us/vermont, canada/quebec) into the maps
repository, and removes them here once they are there.

The pieces are in ~/.cache/m9-tiles/vector/pieces, with a list <name>.json each (split.py). They go to
state/ or province/ of the repository of their region, <M9_MAPS_OWNER>/jinxmesh-maps-<region> at GitHub
(default owner: arugulax; the regions: REGIONS below), in commits of at most 1 GB, as a push must stay under
2 GB. A repository that is not there yet is made, private, with maps-readme.md as its README ("publish.py
--readme" puts that file into all repositories there are again, after a change to it). The repository is never fetched whole: each commit is made
on a copy without the files that are there already, which is removed afterwards. The list stays here, by it
build-all.py knows the state is done; with a line "published" in it.

The upload takes all of the link unless it is held back: M9_UPLOAD_KBPS, or a number in
~/.cache/m9-tiles/vector/upload-kbps (read at each push), is the most it sends, in kB a second.
"""
import json, os, shutil, socket, subprocess, sys, threading, time

ROOT = os.environ.get("M9_TILES", os.path.expanduser("~/.cache/m9-tiles"))
PIECES, CLONE = f"{ROOT}/vector/pieces", f"{ROOT}/vector/maps-repo"
HERE = os.path.dirname(os.path.abspath(__file__))
OWNER = os.environ.get("M9_MAPS_OWNER", "arugulax")
# who the commits are by: the owner's address at GitHub that gives no mail address away
EMAIL = os.environ.get("M9_MAPS_EMAIL", f"{OWNER}@users.noreply.github.com")
REGIONS = {
    "us-northeast": "connecticut delaware district-of-columbia maine maryland massachusetts new-hampshire new-jersey new-york "
    "pennsylvania rhode-island vermont",
    "us-southeast": "alabama arkansas florida georgia kentucky louisiana mississippi north-carolina south-carolina tennessee "
    "virginia west-virginia puerto-rico us-virgin-islands",
    "us-midwest": "illinois indiana iowa kansas michigan minnesota missouri nebraska north-dakota ohio south-dakota wisconsin",
    "us-southwest": "arizona new-mexico oklahoma texas",
    "us-mountain": "colorado idaho montana nevada utah wyoming",
    "us-pacific": "california hawaii oregon washington",
    "us-alaska": "alaska",
    "ca-east": "new-brunswick newfoundland-and-labrador nova-scotia prince-edward-island quebec",
    "ca-central": "manitoba ontario saskatchewan",
    "ca-west": "alberta british-columbia",
    "ca-north-west": "northwest-territories yukon",
    "ca-north-east": "nunavut",
}
README_NOTE = "What these maps are, how they go on the card, where the data is from"


def repository(name):
    """<owner>/jinxmesh-maps-<region> for a state or province."""
    for region, names in REGIONS.items():
        if name in names.split():
            return f"{OWNER}/jinxmesh-maps-{region}"
    raise RuntimeError(f"{name} is in no region of publish.py")


def ensure(repo):
    """Makes the repository, private, with the README, when it is not there."""
    if subprocess.run(["gh", "repo", "view", repo, "--json", "name"], capture_output=True).returncode == 0:
        return
    region = repo.split("jinxmesh-maps-")[-1]
    shutil.rmtree(CLONE, ignore_errors=True)
    os.makedirs(CLONE)
    try:
        git("init", "-q", "-b", "main")
        shutil.copy(f"{HERE}/maps-readme.md", f"{CLONE}/README.md")
        git("add", "README.md")
        git("commit", "-q", "-m", README_NOTE)
        done = subprocess.run(["gh", "repo", "create", repo, "--private", "--description",
                               f"Vector maps for JinxMesh on the ThinkNode M9: {region}", "--source", CLONE, "--remote", "origin", "--push"],
                              capture_output=True, text=True)
        if done.returncode:
            raise RuntimeError(f"gh repo create {repo}: {done.stderr.strip()[-300:]}")
    finally:
        shutil.rmtree(CLONE, ignore_errors=True)
BATCH = 1_000_000_000


def git(*args, capture=False, options=()):
    done = subprocess.run(["git", "-C", CLONE, "-c", f"user.name={OWNER}", "-c", f"user.email={EMAIL}", *options, *args],
                          capture_output=True, text=True)
    if done.returncode:
        raise RuntimeError(f"git {' '.join(args[:3])}: {done.stderr.strip()[-400:]}")
    return done.stdout.strip() if capture else None


def upload_limit():
    """The most to send, in bytes a second; 0: no limit."""
    text = os.environ.get("M9_UPLOAD_KBPS", "")
    if not text and os.path.exists(f"{ROOT}/vector/upload-kbps"):
        text = open(f"{ROOT}/vector/upload-kbps").read()
    try:
        return max(0, int(float(text.strip()) * 1000))
    except ValueError:
        return 0


def slow_proxy(rate):
    """A proxy on this machine that git pushes through, which lets rate bytes a second out. Returns its port."""
    lock, state = threading.Lock(), [time.monotonic()]

    def wait(count):  # all connections share the rate
        with lock:
            state[0] = max(state[0], time.monotonic() - 0.2) + count / rate
            pause = state[0] - time.monotonic()
        if pause > 0:
            time.sleep(pause)

    def pump(source, sink, limited):
        try:
            while True:
                data = source.recv(16384 if limited else 65536)
                if not data:
                    break
                if limited:
                    wait(len(data))
                sink.sendall(data)
        except OSError:
            pass
        for s, how in ((sink, socket.SHUT_WR), (source, socket.SHUT_RD)):
            try:
                s.shutdown(how)
            except OSError:
                pass

    def serve(client):
        far = None
        try:
            head = b""
            while b"\r\n\r\n" not in head and len(head) < 8192:
                more = client.recv(4096)
                if not more:
                    return
                head += more
            verb, target = head.split(b"\r\n")[0].split()[:2]
            if verb != b"CONNECT":
                client.sendall(b"HTTP/1.1 405 Method Not Allowed\r\n\r\n")
                return
            host, port = target.decode().rsplit(":", 1)
            far = socket.create_connection((host, int(port)), timeout=60)
            far.settimeout(None)
            client.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
            back = threading.Thread(target=pump, args=(far, client, False), daemon=True)
            back.start()
            pump(client, far, True)
            back.join()
        except (OSError, ValueError):
            pass
        finally:
            for s in (client, far):
                if s:
                    s.close()

    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(8)

    def accept():
        while True:
            client, _ = listener.accept()
            threading.Thread(target=serve, args=(client,), daemon=True).start()

    threading.Thread(target=accept, daemon=True).start()
    return listener.getsockname()[1]


PROXY = {}


def push_options():
    rate = upload_limit()
    if not rate:
        return []
    if PROXY.get("rate") != rate:
        PROXY.update(rate=rate, port=slow_proxy(rate))
    return ["-c", f"http.proxy=http://127.0.0.1:{PROXY['port']}"]


def commit(repo, folder, files, message):
    """One commit with these files (paths here) under folder ("": at the top, as (path, name)), pushed."""
    shutil.rmtree(CLONE, ignore_errors=True)
    url = f"https://github.com/{repo}.git"
    done = subprocess.run(["git", "clone", "-q", "--filter=blob:none", "--no-checkout", "--depth", "1", url, CLONE],
                          capture_output=True, text=True)
    if done.returncode:
        raise RuntimeError(f"git clone: {done.stderr.strip()[-400:]}")
    try:
        git("read-tree", "HEAD")
        for path in files:
            path, to = path if isinstance(path, tuple) else (path, f"{folder}/{os.path.basename(path)}")
            blob = git("hash-object", "-w", path, capture=True)
            git("update-index", "--add", "--cacheinfo", f"100644,{blob},{to}")
        if git("write-tree", capture=True) == git("rev-parse", "HEAD^{tree}", capture=True):
            return git("rev-parse", "HEAD", capture=True)  # nothing new
        tree = git("write-tree", capture=True)
        made = git("commit-tree", tree, "-p", "HEAD", "-m", message, capture=True)
        git("push", "-q", "origin", f"{made}:refs/heads/main", options=push_options())
        there = git("ls-remote", "origin", "refs/heads/main", capture=True).split()[0]
        if there != made:
            raise RuntimeError("the push did not arrive")
        return made
    finally:
        shutil.rmtree(CLONE, ignore_errors=True)


def publish(region):
    name = region.split("/")[-1]
    folder = "state" if region.startswith("us/") else "province"
    listing = f"{PIECES}/{name}.json"
    info = json.load(open(listing))
    repo = repository(name)
    if info.get("published"):
        return f"{region}: published before"
    paths = [f"{PIECES}/{p['file']}" for p in info["pieces"]]
    missing = [p for p in paths if not os.path.exists(p)]
    if missing:
        raise RuntimeError(f"pieces are missing here: {' '.join(os.path.basename(p) for p in missing)}")
    ensure(repo)
    batches, size = [[]], 0
    for path in paths:
        if batches[-1] and size + os.path.getsize(path) > BATCH:
            batches.append([])
            size = 0
        batches[-1].append(path)
        size += os.path.getsize(path)
    last = None
    for i, batch in enumerate(batches):
        final = i == len(batches) - 1
        part = "" if len(batches) == 1 else f" ({i + 1} of {len(batches)})"
        last = commit(repo, folder, batch + ([listing] if final else []), f"{folder}/{name}: {len(info['pieces'])} pieces, {info['bytes']:,} bytes{part}")
    info["published"] = dict(commit=last, at=time.strftime("%Y-%m-%d %H:%M:%S"), folder=folder, repository=repo)
    with open(listing + ".part", "w") as out:
        json.dump(info, out, indent=1)
    os.replace(listing + ".part", listing)
    for path in paths:
        os.remove(path)
    return f"{region}: {len(paths)} pieces to {repo} in {len(batches)} commit(s), {last[:8]}"


if __name__ == "__main__":
    bad = 0
    if sys.argv[1:] == ["--readme"]:
        for region in REGIONS:
            repo = f"{OWNER}/jinxmesh-maps-{region}"
            if subprocess.run(["gh", "repo", "view", repo, "--json", "name"], capture_output=True).returncode == 0:
                print(repo, commit(repo, "", [(f"{HERE}/maps-readme.md", "README.md")], README_NOTE)[:8], flush=True)
        sys.exit(0)
    for region in sys.argv[1:]:
        try:
            print(publish(region), flush=True)
        except Exception as error:
            bad += 1
            print(f"{region}: NOT published: {error}", flush=True)
    sys.exit(1 if bad else 0)
