#!/usr/bin/env python3

# example use:
# while ./test/fuzztest.py ./build/git-file-times; do :; done

import os
import sys
import random
import shutil
import subprocess
import tempfile
from pathlib import Path
from copy import deepcopy
import shlex
import datetime


if len(sys.argv) != 2:
    print(f"usage: {sys.argv[0]} /path/to/git-file-times")
    sys.exit(1)

GFT = Path(sys.argv[1]).absolute()

random.seed()


REPO = None

clock = 0 # 1970-01-01T00:00:00+0000


def run(*args, cwd=None, env=None, xtrace=False):
    args = list(map(str, args))
    if args[0] == "git":
        if args[1] == "commit":
            # commit time
            ct = int(datetime.datetime.strptime(env['GIT_COMMITTER_DATE'], "%Y-%m-%dT%T%z").timestamp())
            # print(">", shlex.join(args), f"# ct={ct}")
            print(">", f"CT={ct}", shlex.join(args))
        elif xtrace:
            print(">", shlex.join(args))
        else:
            # hide other git commands
            pass
    else:
        print(">", shlex.join(args))
    r = subprocess.run(
        args,
        cwd=cwd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        # stderr=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        # check=True,
    )

    if r.returncode != 0:
        # print(f"command failed: {shlex.join(args)}")
        # print("cwd:", cwd)
        # print(f"stdout:\n{r.stdout}")
        # print("stderr:", r.stderr)
        exc = RuntimeError(f"command failed: {shlex.join(args)}\nstdout:\n{r.stdout}")
        exc.returncode = r.returncode
        exc.stdout = r.stdout
        raise exc

    return r

def git(*args, xtrace=False):
    return run("git", *args, cwd=REPO, xtrace=xtrace)


def now():
    global clock
    clock += 1
    return clock


def commit(msg):
    t = now()

    env = os.environ.copy()
    # date_str = f"{t} +0000"
    date_str = datetime.datetime.fromtimestamp(t, datetime.UTC).strftime("%FT%T%z")
    env["GIT_AUTHOR_DATE"] = date_str
    env["GIT_COMMITTER_DATE"] = date_str

    run(
        "git",
        "commit",
        "-m",
        msg,
        cwd=REPO,
        env=env,
    )

    sha = git("rev-parse", "HEAD").stdout.strip()
    return sha, t

def write_files(state):
    # remove deleted files
    existing = set()

    for p, info in state.items():
        existing.add(p)
        path = REPO / p
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(info["content"])

    for path in REPO.rglob("*"):
        if not path.is_file():
            continue

        rel = str(path.relative_to(REPO))

        # never touch .git
        if rel.startswith(".git/"):
            continue

        if rel not in existing:
            # print(f"write_files: rm {str(path)!r}")
            path.unlink()


def git_add_commit(state, msg, changed_path=None):
    write_files(state)
    if changed_path is None:
        git("add", "-A")
    else:
        if changed_path in state:
            # add/modify
            git("add", "--", changed_path)
        else:
            # delete
            git("add", "-u", "--", changed_path)
    sha, t = commit(msg)

    if 0:
        # debug
        print("COMMIT", t, msg)
        for p, info in state.items():
            print(" ", p, info.get("time"), info.get("pending"))

    for info in state.values():
        if info.get("pending"):
            info["time"] = t
            del info["pending"]
    snapshots[sha] = deepcopy(state)
    return sha, t


def git_merge(branch):
    try:
        git("merge", "--no-ff", branch, "-m", "merge")
        sha = git("rev-parse", "HEAD").stdout.strip()
        t = int(git("show", "--quiet", "--format=%ct", "HEAD").stdout.strip())
        return sha, t

    except RuntimeError:
        print("merge conflict, auto-resolving")

        # resolve every unmerged path
        out = git("diff", "--name-only", "--diff-filter=U").stdout

        for line in out.splitlines():
            path = line.strip()
            if not path:
                continue

            # randomly choose a side
            side = random.choice(["ours", "theirs"])

            try:
                git("checkout", f"--{side}", "--", path)
            except RuntimeError:
                # modify/delete conflict:
                # checkout fails because that side has no file.
                #
                # emulate choosing "deleted"
                if side == "ours":
                    # ours deleted it
                    git("rm", "-f", "--ignore-unmatch", path)
                else:
                    # theirs deleted it
                    git("rm", "-f", "--ignore-unmatch", path)

        git("add", "-A")
        # TODO better commit message?
        # how was the conflict resolved?
        sha, t = commit("merge (auto resolved)")
        return sha, t


def read_worktree(old_state):
    state = {}

    for path in REPO.rglob("*"):
        if not path.is_file():
            continue

        rel = str(path.relative_to(REPO))

        if ".git" in Path(rel).parts:
            continue

        content = path.read_text()

        if rel in old_state and old_state[rel]["content"] == content:
            state[rel] = deepcopy(old_state[rel])
        else:
            state[rel] = {
                "content": content,
                "pending": True,
            }

    return state


def get_branch():
    return git("branch", "--show-current").stdout.strip()


def git_file_at(commit, path):
    r = git("show", f"{commit}:{path}")
    return r.stdout if r.returncode == 0 else None


# ------------------------------------------------------------

tmp = tempfile.mkdtemp(
    prefix="git-file-times-fuzz-",
    dir=f"/run/user/{os.getuid()}", # tmpfs
)
print()
print()
print()
print(f"tmp: {tmp}")

try:
    # os.chdir(tmp)

    REPO = Path(tmp) / "repo"
    # REPO = REPO.absolute()
    REPO.mkdir()
    # print(f"REPO: {REPO}")

    git("init")
    # subprocess.run(
    #     ["git", "init"],
    #     cwd=REPO,
    #     check=True,
    # )

    if 0:
        # debug "git init"
        print(run("ls", "-lA", str(REPO)).stdout)
        print(run("find", str(REPO), "-maxdepth", "2", "-print").stdout)
        print(git("status").stdout)

    git("config", "user.email", "test@test")
    git("config", "user.name", "test")

    if 0:
        # debug "git init"
        print(git("status").stdout)

    state = {}
    branch_states = {}
    snapshots = {}
    branches = ["main"]

    # initial commit
    for i in range(5):
        name = f"file{i}.txt"
        state[name] = {
            "content": f"initial {i}\n",
            "pending": True,
        }

    head, t = git_add_commit(state, "initial")
    commits = [head]
    branch_states[get_branch()] = deepcopy(state)



    # --------------------------------------------------------
    # generate history
    # --------------------------------------------------------

    for step in range(50):

        action = random.random()

        if action < 0.55:
            # modify file
            files = list(state.keys())
            if files and random.random() < 0.7:
                p = random.choice(files)
            else:
                p = None
                for _ in range(100):
                    candidate = f"new{random.randint(0,20)}.txt"
                    # ensure this is really an add, not a modify
                    if candidate not in state:
                        p = candidate
                        break
                if p is None:
                    continue
            content = f"change {step} {random.random()}\n"
            state[p] = {
                "content": content,
                "time": clock + 1,
            }
            sha, t = git_add_commit(
                state,
                f"modify {p}",
                p,
            )
            commits.append(sha)
            branch_states[get_branch()] = deepcopy(state)

        elif action < 0.75:
            # delete file
            if state:
                p = random.choice(list(state))
                del state[p]
                sha, t = git_add_commit(
                    state,
                    f"delete {p}",
                    p,
                )
                commits.append(sha)
                branch_states[get_branch()] = deepcopy(state)

        elif action < 0.88:
            # create branch
            name = f"branch{step}"
            git(
                "checkout",
                "-b",
                name,
            )
            branches.append(name)
            branch_states[name] = deepcopy(state)

        else:
            # merge
            if len(branches) < 2:
                continue
            other = random.choice(branches[:-1])
            # remember parents
            parent1 = git("rev-parse", "HEAD").stdout.strip()
            parent2 = git("rev-parse", other).stdout.strip()

            sha, t = git_merge(other)

            if 0:
                left = snapshots[parent1]
                right = snapshots[parent2]
            elif 0:
                parents = git("show", "-s", "--format=%P", sha).stdout.strip().split()
                print(f"parents after git_merge: {parents}")
                left = snapshots[parents[0]]
                # IndexError: list index out of range @ parents[1]
                right = snapshots[parents[1]]
            elif 0:
                parents = git("show", "-s", "--format=%P", sha).stdout.strip().split()
                print(f"parents after git_merge: {parents}")
                left = snapshots[parents[0]]
                if len(parents) == 2:
                    right = snapshots[parents[1]]
                else:
                    # not a real merge commit
                    # just continue from the only parent
                    right = {}
            elif 1:
                parents = git("rev-list", "--parents", "-n", "1", sha).stdout.split()
                # if len(parents) == 2:
                #     # ordinary commit (including fast-forward result)
                #     state = deepcopy(snapshots[sha])

                # elif len(parents) == 3:
                #     # true merge commit
                #     reconstruct via three-way logic


            if 1:
                # debug
                print("merge snapshot after git_merge:")
                for p, x in state.items():
                    print(" ", x["time"], p)

            if 0:
                # inspect the actual merge result
                state = read_worktree(state)
                # assign merge timestamps to files created/changed by merge
                for info in state.values():
                    if info.get("pending"):
                        info["time"] = t
                        del info["pending"]
            elif 0:
                state = read_worktree({})
                for p, info in state.items():
                    left_blob = left.get(p)
                    right_blob = right.get(p)
                    changed_by_merge = False
                    if left_blob is None and right_blob is None:
                        changed_by_merge = True
                    elif left_blob is None:
                        changed_by_merge = (
                            info["content"] != right_blob["content"]
                        )
                    elif right_blob is None:
                        changed_by_merge = (
                            info["content"] != left_blob["content"]
                        )
                    else:
                        changed_by_merge = (
                            info["content"] != left_blob["content"]
                            and
                            info["content"] != right_blob["content"]
                        )
                    if changed_by_merge:
                        info["time"] = t
                    else:
                        # inherit timestamp
                        if p in left:
                            info["time"] = left[p]["time"]
                        else:
                            info["time"] = right[p]["time"]
            elif 0:
                # state = read_worktree({})
                state = read_worktree(branch_states[get_branch()])
                for p, info in state.items():
                    if p in left and info["content"] == left[p]["content"]:
                        info["time"] = left[p]["time"]
                    elif p in right and info["content"] == right[p]["content"]:
                        info["time"] = right[p]["time"]
                    else:
                        info["time"] = t
            elif 0:
                # dont use read_worktree
                state = {}
                paths = set(left) | set(right)
                for p in paths:
                    a = left.get(p)
                    b = right.get(p)
                    if a and b:
                        if a["content"] == b["content"]:
                            # unchanged
                            state[p] = deepcopy(a)
                        else:
                            # conflict or different versions
                            content = (REPO / p).read_text()
                            if content == a["content"]:
                                state[p] = deepcopy(a)
                            elif content == b["content"]:
                                state[p] = deepcopy(b)
                            else:
                                # manual resolution
                                # file was modified in the merge commit
                                state[p] = {
                                    "content": content,
                                    # FIXME time is wrong?
                                    "time": t,
                                }
                    elif a:
                        # only in ours
                        if (REPO / p).exists():
                            state[p] = deepcopy(a)
                    elif b:
                        # only in theirs
                        if (REPO / p).exists():
                            state[p] = deepcopy(b)
            elif 0:
                state = {}
                paths = set(left) | set(right)
                for p in paths:
                    a = left.get(p)
                    b = right.get(p)
                    exists = (REPO / p).exists()
                    if not exists:
                        continue
                    content = (REPO / p).read_text()
                    if a and content == a["content"]:
                        print("merge commit: content was copied from the left parent")
                        state[p] = deepcopy(a)
                    elif b and content == b["content"]:
                        print("merge commit: content was copied from the right parent")
                        state[p] = deepcopy(b)
                    else:
                        # genuinely new content introduced by merge
                        print("merge commit: content was introduced by the merge commit")
                        state[p] = {
                            "content": content,
                            "time": t,
                        }
            elif 1:
                if len(parents) == 2:
                    # ordinary commit (including fast-forward result)
                    print(f"fast-forward merge commit: {sha}")
                    state = deepcopy(snapshots[sha])
                elif len(parents) == 3:
                    # true merge commit
                    print(f"true merge commit: {sha}")
                    state = {}
                    left = snapshots[parents[1]]
                    right = snapshots[parents[2]]
                    paths = set(left) | set(right)
                    for p in paths:
                        a = left.get(p)
                        b = right.get(p)
                        exists = (REPO / p).exists()
                        if not exists:
                            continue
                        content = (REPO / p).read_text()
                        if a and content == a["content"]:
                            print("merge commit: content was copied from the left parent")
                            state[p] = deepcopy(a)
                        elif b and content == b["content"]:
                            print("merge commit: content was copied from the right parent")
                            state[p] = deepcopy(b)
                        else:
                            # genuinely new content introduced by merge
                            print("merge commit: content was introduced by the merge commit")
                            state[p] = {
                                "content": content,
                                "time": t,
                            }
                        if 1:
                            # debug
                            # git show <merge>:file0.txt
                            print("merge commit:")
                            print(git("show", f"{sha}:{p}", xtrace=True).stdout)
                            print(git("ls-tree", sha, "--", p, xtrace=True).stdout)
                            # git show <left>:file0.txt
                            try:
                                left_show = git("show", f"{parents[1]}:{p}", xtrace=True).stdout
                                print("left parent:")
                                print(left_show)
                                print(git("ls-tree", parents[1], "--", p, xtrace=True).stdout)
                            except RuntimeError as exc:
                                # fatal: path '{p}' exists on disk, but not in '{parents[1]}'
                                print("left parent:", exc.stdout)
                            # git show <right>:file0.txt
                            try:
                                right_show = git("show", f"{parents[2]}:{p}", xtrace=True).stdout
                                print("right parent:")
                                print(right_show)
                                print(git("ls-tree", parents[2], "--", p, xtrace=True).stdout)
                            except RuntimeError as exc:
                                # fatal: path '{p}' exists on disk, but not in '{parents[2]}'
                                print("right parent:", exc.stdout)
                            # git merge-base <left> <right>
                            try:
                                merge_base = git("merge-base", parents[1], parents[2]).stdout.strip()
                                # git show <base>:file0.txt^
                                print(f"merge base:")
                                print(git("show", f"{merge_base}:{p}", xtrace=True).stdout)
                            except RuntimeError as exc:
                                # fatal: path '{p}' exists on disk, but not in '{parents[idx]}'
                                print("merge base:", exc.stdout)



                elif len(parents) > 3:
                    raise NotImplementedError(f"len(parents)={len(parents)}")

            print("storing snapshot", sha)
            snapshots[sha] = deepcopy(state)

            commits.append(sha)

            print("storing branch state", get_branch())
            branch_states[get_branch()] = deepcopy(state)

            # sometimes continue from main
            branch_name = random.choice(branches)
            git("checkout", branch_name)
            state = deepcopy(branch_states[branch_name])

            if 1:
                # verify checkout state
                real = read_worktree({})
                expected = branch_states[branch_name]
                real_contents = {
                    p: x["content"] for p, x in real.items()
                }
                expected_contents = {
                    p: x["content"] for p, x in expected.items()
                }
                if real_contents != expected_contents:
                    print("BRANCH STATE DRIFT:", branch_name)
                    print("real_contents:", real_contents)
                    print("expected_contents:", expected_contents)
                    sys.exit(1)

    if 1:
        # debug
        print("final state:")
        for p in sorted(state):
            print(
                " ",
                state[p]["time"],
                p,
                repr(state[p]["content"])
            )


    # --------------------------------------------------------
    # expected result
    # --------------------------------------------------------

    expected = {
        p: info["time"]
        for p, info in state.items()
    }


    # --------------------------------------------------------
    # actual result
    # --------------------------------------------------------

    result = subprocess.run(
        GFT,
        cwd=REPO,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    if result.returncode != 0:
        raise RuntimeError(f"failed to run {GFT}:\n{result.stderr}")

    actual = {}

    for line in result.stdout.splitlines():
        t, p = line.split(" ", 1)
        actual[p] = int(t)


    # --------------------------------------------------------
    # compare
    # --------------------------------------------------------

    ok = True


    for p in sorted(set(expected) | set(actual)):

        e = expected.get(p)
        a = actual.get(p)

        if e != a:
            ok = False
            print("FAIL", p)
            print(" expected:", e)
            print(" actual:  ", a)
            # help to debug
            print(git("log", "--graph", "--oneline", "--", p, xtrace=True).stdout)
            print(git("log", "--all", "--format=%ct %H %s", "--", p, xtrace=True).stdout)
            for commit_hash in git("log", "--all", "--format=%H", "--", p).stdout.strip().splitlines():
                # print(f"commit_hash: {commit_hash!r}")
                # "git show --stat" is misleading
                # because it prints files also when this commit did not modify them
                # print(git("show", "--oneline", "--stat", commit_hash, xtrace=True).stdout)
                print(git("show", commit_hash, xtrace=True).stdout)
            # find commits by time
            for log_line in git("log", "--all", "--format=%ct %H %s").stdout.strip().splitlines():
                ct, H, s = log_line.split(maxsplit=2)
                ct = int(ct)
                if ct == e:
                    print()
                    print("expected:", log_line)
                    # print(git("show", "--oneline", "--stat", H, xtrace=True).stdout)
                    print(git("show", H, xtrace=True).stdout)
                elif ct == a:
                    print()
                    print("actual:  ", log_line)
                    # print(git("show", "--oneline", "--stat", H, xtrace=True).stdout)
                    print(git("show", H, xtrace=True).stdout)

            break # stop after the first error
    if ok:
        print("PASS")
        shutil.rmtree(tmp)

    else:
        print("repo left at:", REPO)
        # print("seed:", random.getstate())
        sys.exit(1)


finally:
    if "PASS" in locals():
        pass

    # comment this out while debugging
    # shutil.rmtree(tmp)
