#!/usr/bin/env python3

# FIXME try to use tmpfs as tempdir

import datetime
import os
import pathlib
import shutil
import subprocess
import argparse
import glob
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import shlex


#
# Global state
#

repo_dir = None
original_workdir = os.getcwd()
clock = 0
git_file_times = None
# path -> last modified commit time
expected_times = {}

#
# Helpers
#

def now():
    global clock
    clock += 1
    return clock


def git_env(t):
    env = os.environ.copy()
    date = datetime.datetime.fromtimestamp(
        t,
        datetime.UTC,
    ).strftime("%Y-%m-%dT%H:%M:%S%z")
    env["GIT_AUTHOR_DATE"] = date
    env["GIT_COMMITTER_DATE"] = date
    return env


def run(cmd, *, env=None):
    print(">", shlex.join(cmd))
    subprocess.run(
        cmd,
        cwd=repo_dir,
        env=env,
        check=True,
    )


def mkdir_parent(path):
    pathlib.Path(repo_dir, path).parent.mkdir(
        parents=True,
        exist_ok=True,
    )


#
# Step implementations
#

def init(opts):
    run(["git", "init"])

    # +<> = pallas
    run(["git", "config", "user.name", "+"])
    run(["git", "config", "user.email", ""])


def write(opts):
    path = opts["path"]
    data = opts["content"]
    mkdir_parent(path)
    with open(pathlib.Path(repo_dir, path), "w") as f:
        f.write(data)
    add({"paths": [path]})


def append(opts):
    path = opts["path"]
    data = opts["content"]
    mkdir_parent(path)
    with open(pathlib.Path(repo_dir, path), "a") as f:
        f.write(data)
    add({"paths": [path]})


def copy(opts):
    mkdir_parent(opts["dst"])
    shutil.copy2(
        pathlib.Path(repo_dir, opts["src"]),
        pathlib.Path(repo_dir, opts["dst"]),
    )
    add({"paths": [opts["dst"]]})


def move(opts):
    # FIXME git mv
    mkdir_parent(opts["dst"])
    shutil.move(
        pathlib.Path(repo_dir, opts["src"]),
        pathlib.Path(repo_dir, opts["dst"]),
    )


#def delete(opts):
#    # FIXME git rm
#    os.remove(pathlib.Path(repo_dir, opts["path"]))


def mkdir(opts):
    pathlib.Path(repo_dir, opts["path"]).mkdir(
        parents=True,
        exist_ok=True,
    )


#def rmdir(opts):
#    shutil.rmtree(
#        pathlib.Path(repo_dir, opts["path"]),
#    )


def add(opts):
    paths = opts["paths"]
    run(["git", "add", *paths])


def rm(opts):
    paths = opts.get("paths", [])
    if "path" in opts:
        paths.append(opts["path"])
    run(["git", "rm", "-rf", *paths])

delete = remove = rm


def branch(opts):
    run(["git", "branch", opts["name"]])


def checkout(opts):
    cmd = ["git", "checkout"]
    if opts.get("create"):
        cmd.append("-b")
    cmd.append(opts["name"])
    run(cmd)


def checkout_b(opts):
    cmd = ["git", "checkout", "-b", opts["name"]]
    run(cmd)


def merge(opts):
    t = now()

    cmd = [
        "git",
        "merge",
        "--no-ff",
        "-m",
        opts.get("message", f"merge {opts['branch']}"),
        opts["branch"],
    ]

    run(cmd, env=git_env(t))

    #
    # Merge itself doesn't modify file contents.
    # Therefore expected_times stay unchanged.
    #


r'''
def tag(opts):
    run([
        "git",
        "tag",
        opts["name"],
    ])
'''


def commit(opts):
    """
    opts:

        {
            "message": "...",
            "files": [...]
        }

    'files' is optional.
    If omitted, every tracked file currently staged is assumed
    to have been modified by this commit.
    """

    t = now()
    m = opts.get("message", f"commit {t}")
    # args = ["git", "commit", "-m", m, "--allow-empty"]
    args = ["git", "commit", "-m", m] + opts.get("args", [])
    run(args, env=git_env(t))
    files = opts.get("modified", [])
    for path in files:
        expected_times[path] = t


#
# Optional helpers
#

def rename(opts):
    """
    Git rename.

    Does NOT update expected_times.
    A pure rename is not considered a modification.
    """

    src = opts["src"]
    dst = opts["dst"]

    assert os.path.exists(src), f"src must exist: {src}"
    assert os.path.isfile(src), f"src must be a file: {src}"

    mkdir_parent(dst)

    run(["git", "mv", src, dst])

    if src in expected_times:
        # rename file in expected_times
        expected_times[dst] = expected_times.pop(src)


def touch(opts):
    """
    Change filesystem mtime only.
    """
    path = pathlib.Path(repo_dir, opts["file"])
    path.touch()


def exec(opts):
    """
    Run arbitrary command inside repository.
    """
    run(opts["cmd"])


#
# more helpers for the test runner
#

def load_actual():
    """
    Run git-file-times.

    Returns

        { path : timestamp }
    """

    out = subprocess.check_output(
        [git_file_times],
        cwd=repo_dir,
        text=True,
    )

    actual = {}

    for line in out.splitlines():
        if not line.strip():
            continue

        ts, path = line.split(" ", 1)
        actual[path] = int(ts)

    return actual


def compare_results(case_name):
    actual = load_actual()

    ok = True

    #
    # expected -> actual
    #

    for path in sorted(expected_times):
        e = expected_times[path]
        a = actual.get(path)

        if a != e:
            print(
                f"{case_name}: FAIL: {path}: expected {e}, got {a}"
            )
            ok = False

    #
    # unexpected output
    #

    for path in sorted(actual):
        if path not in expected_times:
            print(
                f"{case_name}: FAIL: unexpected output {path}"
            )
            ok = False

    if ok:
        print(f"{case_name}: PASS")

    return ok


def reset_state(tmpdir):
    global repo_dir
    global clock
    global expected_times
    repo_dir = pathlib.Path(tmpdir)
    # TODO simplify all paths to relative paths
    os.chdir(repo_dir)
    clock = 0
    expected_times = {}


def get_tmpfs_dir():
    # TODO? parse /proc/mounts or /proc/self/mounts or /proc/self/mountinfo
    # find the largest writable tmpfs
    # see also
    # https://github.com/Barro/largest-tmpfs/blob/master/largest-tmpfs.c
    dir = None
    try:
        uid = os.getuid() # unix only
    except Exception:
        uid = 0
    tmpfs_candidates = [
        f"/run/user/{uid}",
    ]
    for candidate in tmpfs_candidates:
        if os.path.exists(candidate):
            dir = candidate
            break
    return dir


def run_case(json_file):
    with open(json_file) as f:
        case = json.load(f)
    steps = case["steps"]
    name = pathlib.Path(json_file).stem
    tmpfs_dir = get_tmpfs_dir()
    print(f"tmpfs_dir: {tmpfs_dir}")
    kwargs = dict(
        prefix="git-file-times-test-",
        dir=tmpfs_dir,
    )
    with tempfile.TemporaryDirectory(**kwargs) as tmpdir:
        reset_state(tmpdir)
        for step in steps:
            action = step[0]
            if len(step) >= 2:
                opts = step[1]
            else:
                opts = None
            fn = globals()[action]
            fn(opts)
        ok = compare_results(name)
        if not ok:
            input(f"keeping tempdir {tmpdir}, hit enter to continue: ")
        return ok


def main():

    global git_file_times

    parser = argparse.ArgumentParser()

    parser.add_argument(
        "git_file_times",
        help="path to git-file-times executable",
    )

    parser.add_argument(
        "case",
        nargs="?",
        help="single test case json",
    )

    args = parser.parse_args()

    git_file_times = os.path.abspath(args.git_file_times)

    if args.case:
        cases = [args.case]
    else:
        cases = sorted(
            glob.glob("test/cases/*.json")
        )

    # cases = list(map(os.path.abspath, cases))

    failed = 0

    for case in cases:
        print()
        print(f"running test case: {case}")
        os.chdir(original_workdir)
        if not run_case(case):
            failed += 1

    print()

    print(
        f"{len(cases)-failed}/{len(cases)} tests passed"
    )

    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
