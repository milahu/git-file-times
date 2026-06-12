#include <git2.h>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <iostream>

struct Context {
    std::unordered_map<std::string, git_time_t> seen;
    std::unordered_set<std::string> remaining;
    git_time_t time;
    // const git_oid *oid; // debug
    git_oid *oid; // debug
    git_commit *commit; // debug
};

static int tree_cb(
    const char *root,
    const git_tree_entry *entry,
    void *payload)
{
    auto *out = static_cast<std::unordered_set<std::string> *>(payload);

    const char *name = git_tree_entry_name(entry);
    if (!name) return 0;

    std::string path = root ? std::string(root) + name : name;

    if (git_tree_entry_type(entry) == GIT_OBJECT_BLOB) {
        out->insert(path);
    }

    return 0;
}

std::unordered_set<std::string> collect_head_tree(git_repository *repo)
{
    std::unordered_set<std::string> out;

    git_object *obj = nullptr;
    if (git_revparse_single(&obj, repo, "HEAD^{tree}") != 0 || !obj)
        return out;

    git_tree *tree = (git_tree *)obj;

    git_tree_walk(tree, GIT_TREEWALK_PRE, tree_cb, &out);

    git_tree_free(tree);
    return out;
}

static int diff_cb(
    const git_diff_delta *delta,
    float progress,
    void *payload)
{
    auto *ctx = static_cast<Context *>(payload);

    /*
    const git_diff_file *file = &delta->new_file;
    if (!file->path)
        return 0;

    std::string path(file->path);
    */

    const git_diff_file *f = (
        delta->new_file.path ? &delta->new_file
        : &delta->old_file
    );

    if (!f->path) return 0;

    // std::string path = f->path; // slow
    // std::string_view path(f->path);
    const char *path = f->path;

    // std::cerr << "path: " << path << "\n"; // debug
    // std::cerr << "path=" << path << " commit=" << git_oid_tostr_s(&oid) << " time=" << ctx->time << "\n"; // debug
    // std::cerr << "path=" << path << " commit=" << git_oid_tostr_s(ctx->oid) << " time=" << ctx->time << "\n"; // debug
    // debug
    std::cerr << "path=" << path
        << " commit=" << git_oid_tostr_s(ctx->oid)
        // these are the same times
        // << " ctx->commit->time=" << git_commit_time(ctx->commit)
        // << " ctx->time=" << ctx->time
        << " time=" << ctx->time
        << " status=" << delta->status
        << " old=" << (delta->old_file.path ?: "")
        << " new=" << (delta->new_file.path ?: "")
        << "\n";

    // no, this is unreachable because of ctx->remaining.erase(path);
    /*
    if (
        // path is part of the HEAD tree
        ctx->remaining.count(path) &&
        // path has been processed
        ctx->seen.count(path)
    ) {
        std::cerr << "path2: " << path << "\n"; // debug
    }
    */

    // no, this returns wrong file times
    // because later commits can have more recent file times
    // = because the first commit does not always have the latest file time
    if (
        // path is part of the HEAD tree
        ctx->remaining.count(path) &&
        // path has not been processed
        !ctx->seen.count(path)
    ) {
        ctx->seen[path] = ctx->time;
        ctx->remaining.erase(path);
    }

    /*
    if (
        // path is part of the HEAD tree
        ctx->remaining.count(path)
    ) {
        // note: the default ctx->seen[path] is zero
        // ctx->seen[path] = std::max(ctx->seen[path], ctx->time);
        auto &val = ctx->seen[path];
        val = (val == 0) ? ctx->time : std::max(val, ctx->time);
    }
    */

    return 0;
}

int main() {
    git_libgit2_init();

    git_repository *repo = nullptr;
    git_revwalk *walk = nullptr;

    if (git_repository_open(&repo, ".") != 0) {
        std::cerr << "failed to open repo\n";
        return 1;
    }

    git_revwalk_new(&walk, repo);

    // no effect?
    git_revwalk_sorting(walk, GIT_SORT_TIME | GIT_SORT_TOPOLOGICAL);
    // git_revwalk_sorting(walk, GIT_SORT_TIME);

    // no, this returns empty output
    // git_revwalk_hide_glob(walk, "*");

    git_revwalk_push_head(walk);

    // get only commits of the current branch
    // git_revwalk_simplify_first_parent(walk);

    // not working?
    // skip merge commits
    // git_revwalk_hide_glob(walk, "merge");

    Context ctx;
    ctx.remaining = collect_head_tree(repo);

    git_oid oid;

    while (!git_revwalk_next(&oid, walk)) {

        // std::cerr << "commit: " << git_oid_tostr_s(&oid) << "\n"; // debug

        git_commit *commit;
        git_commit_lookup(&commit, repo, &oid);

        // skip merge commits
        if (git_commit_parentcount(commit) > 1)
            continue;

        git_time_t t = git_commit_time(commit);

        git_tree *tree;
        git_commit_tree(&tree, commit);

        git_tree *parent_tree = nullptr;
        if (git_commit_parentcount(commit) > 0) {
            git_commit *parent;
            git_commit_parent(&parent, commit, 0);
            git_commit_tree(&parent_tree, parent);
            git_commit_free(parent);
        }

        ctx.time = git_commit_time(commit);

        git_diff *diff = nullptr;

        git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
        opts.flags |= GIT_DIFF_INCLUDE_UNTRACKED;
        // opts.flags |= GIT_DIFF_RENAMES;

        // redundant?
        /*
        if (parent_tree) {
            git_diff_tree_to_tree(&diff, repo, parent_tree, tree, &opts);
        } else {
            git_diff_tree_to_tree(&diff, repo, nullptr, tree, &opts);
        }
        */
        git_diff_tree_to_tree(&diff, repo, parent_tree, tree, &opts);

        // handle file renames
        // TODO remove?
        git_diff_find_options find_opts = GIT_DIFF_FIND_OPTIONS_INIT;
        find_opts.flags |= GIT_DIFF_FIND_RENAMES;
        find_opts.flags |= GIT_DIFF_FIND_RENAMES_FROM_REWRITES;
        find_opts.rename_threshold = 50; // optional tuning
        git_diff_find_similar(diff, &find_opts);

        ctx.oid = &oid; // debug
        ctx.commit = commit; // debug

        git_diff_foreach(
            diff,
            diff_cb,
            nullptr,
            nullptr,
            nullptr,
            &ctx
        );

        // no, this requires ctx->remaining.erase(path);
        /*
        if (ctx.remaining.empty())
            break;
        */
    }

    git_revwalk_free(walk);
    git_repository_free(repo);

    git_libgit2_shutdown();

    // output result
    for (const auto &kv : ctx.seen) {
        std::cout << kv.second << " " << kv.first << "\n";
    }
}
