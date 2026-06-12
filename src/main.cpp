#include <git2.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <iostream>
#include <memory>

struct Context {
    std::unordered_map<std::string, git_time_t> seen;
    std::unordered_set<std::string> remaining;
    std::unordered_set<std::string> ignore_next_add;
    std::unordered_map<std::string, time_t> result;
    std::vector<std::string> to_remove;
    git_time_t time;
    // const git_oid *oid; // debug
    git_oid *oid; // debug
    git_commit *commit; // debug
};

struct CommitCtx {
    Context *ctx;
    git_time_t time;
    // const git_oid *oid; // debug
    git_oid *oid; // debug
    git_commit *commit; // debug
    // std::vector<std::string> touched;
    std::vector<std::string> to_remove;
};

#if false
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
#endif

static inline std::string join_path(const char *root, const char *name)
{
    if (!root || root[0] == '\0')
        return std::string(name);

    std::string r(root);
    if (r.back() != '/')
        r += '/';

    r += name;
    return r;
}

static int tree_cb(
    const char *root,
    const git_tree_entry *entry,
    void *payload)
{
    auto *ctx = static_cast<Context *>(payload);
    // auto *ctx = static_cast<CommitCtx*>(payload);

    if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB) {
        std::cerr << "no blob\n";
        return 0;
    }

    const char *name = git_tree_entry_name(entry);
    if (!name) {
        std::cerr << "empty name\n";
        return 0;
    }

    std::string path = join_path(root, name);

    // debug
    std::cerr << "path=" << path
        << " commit=" << git_oid_tostr_s(ctx->oid)
        // these are the same times
        // << " ctx->commit->time=" << git_commit_time(ctx->commit)
        // << " ctx->time=" << ctx->time
        << " time=" << ctx->time
        // << " status=" << delta->status
        // << " old=" << (delta->old_file.path ?: "")
        // << " new=" << (delta->new_file.path ?: "")
        << "\n";

    auto it = ctx->remaining.find(path);
    if (it != ctx->remaining.end()) {
        ctx->result[path] = ctx->time;
        ctx->remaining.erase(it); // segfault
        // ctx->to_remove.push_back(path);

        if (ctx->remaining.empty())
            return 1; // stop early
    }

    return 0;
}

// debug
#if false
static int tree_cb(
    const char *root,
    const git_tree_entry *entry,
    void *payload)
{
    auto *ctx = (Context*)payload;

    std::cerr << "CALLBACK\n";

    return 0;
}
#endif

#if false
std::unordered_set<std::string> collect_head_tree(
    git_repository *repo,
    void *payload
)
{
    std::unordered_set<std::string> out;

    auto *ctx = static_cast<Context*>(payload);

    git_object *obj = nullptr;
    if (git_revparse_single(&obj, repo, "HEAD^{tree}") != 0 || !obj)
        return out;

    git_tree *tree = (git_tree *)obj;

    // FIXME why not pass payload == ctx
    git_tree_walk(tree, GIT_TREEWALK_PRE, tree_cb, &out);

    // no!
    // for (auto &p : ctx->to_remove)
    //     ctx->remaining.erase(p);
    // ctx->to_remove.clear();

    git_tree_free(tree);
    return out;
}
#endif

static std::unordered_set<std::string> collect_head_tree(git_repository *repo)
{
    std::unordered_set<std::string> out;

    git_object *obj = nullptr;
    if (git_revparse_single(&obj, repo, "HEAD^{tree}") != 0 || !obj)
        return out;

    git_tree *tree = (git_tree *)obj;

    git_tree_walk(
        tree,
        GIT_TREEWALK_PRE,
        [](const char *root,
           const git_tree_entry *entry,
           void *payload) -> int
        {
            auto *out = static_cast<std::unordered_set<std::string>*>(payload);

            if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB)
                return 0;

            const char *name = git_tree_entry_name(entry);
            if (!name) return 0;

            std::string path = root && *root
                // ? std::string(root) + "/" + name
                ? std::string(root) + name
                : name;

            out->insert(std::move(path));
            return 0;
        },
        &out
    );

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

    const git_diff_file *oldf = &delta->old_file;
    const git_diff_file *newf = &delta->new_file;

    bool has_old = oldf->path && oldf->path[0];
    bool has_new = newf->path && newf->path[0];

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
        << " old_size=" << delta->old_file.size
        << " new_size=" << delta->new_file.size
        << " is_rename=" << ((delta->status == GIT_DELTA_RENAMED) ? "1" : "0")
        << " is_remaining=" << ctx->remaining.count(path)
        << " is_seen=" << ctx->seen.count(path)
        << "\n";

    if (delta->status == GIT_DELTA_DELETED) {
        // this is confusing, because path is in ctx->remaining
        // because path is present in the HEAD tree
        // and there is no "add" commit between the "delete" commit and HEAD
        // so we resolve this contradiction by ignoring the next "add" commit
        ctx->ignore_next_add.emplace(path);
        return 0;
    }

    if (delta->status == GIT_DELTA_ADDED && ctx->ignore_next_add.count(path)) {
        ctx->ignore_next_add.erase(path);
        return 0;
    }

    if (!has_old && has_new) {
        return 0;
    }

    if (has_old && !has_new)
        return 0;

    if (!has_old && !has_new) // unreachable?
        return 0;

    // this requires git_diff_find_similar
    if (delta->status == GIT_DELTA_RENAMED)
        return 0;

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

#if false
static int diff_cb(
    const git_diff_delta *delta,
    float progress,
    void *payload)
{
    auto *ctx = static_cast<Context *>(payload);

    const char *path = nullptr;

    switch (delta->status) {
        case GIT_DELTA_ADDED:
            path = delta->new_file.path;
            break;

        case GIT_DELTA_DELETED:
            path = delta->old_file.path;
            break;

        case GIT_DELTA_MODIFIED:
        case GIT_DELTA_RENAMED:
        case GIT_DELTA_COPIED:
            path = delta->new_file.path ? delta->new_file.path : delta->old_file.path;
            break;

        default:
            return 0;
    }

    if (!path)
        return 0;

    if (ctx->remaining.count(path) && !ctx->seen.count(path)) {
        ctx->seen[path] = ctx->time;
        ctx->remaining.erase(path);
    }

    return 0;
}
#endif

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
    // git_revwalk_sorting(walk, GIT_SORT_TIME | GIT_SORT_TOPOLOGICAL);
    // git_revwalk_sorting(walk, GIT_SORT_TIME);

    // no, this returns empty output
    // git_revwalk_hide_glob(walk, "*");

    git_revwalk_push_head(walk);

    git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL);

    // no, this makes it worse
    // get only commits of the current branch
    // git_revwalk_simplify_first_parent(walk);

    // not working?
    // skip merge commits
    // git_revwalk_hide_glob(walk, "merge");

    // Context ctx; // segfault
    auto ctx = std::make_unique<Context>();

    // ctx->remaining = collect_head_tree(repo, &ctx);
    // ctx->remaining = collect_head_tree(repo, ctx.get());
    ctx->remaining = collect_head_tree(repo);

    git_oid oid;

    while (!git_revwalk_next(&oid, walk)) {

        // std::cerr << "commit: " << git_oid_tostr_s(&oid) << "\n"; // debug

        // auto ctx = std::make_unique<Context>(); / ???

        git_commit *commit;
        git_commit_lookup(&commit, repo, &oid);

        // skip merge commits
        if (git_commit_parentcount(commit) > 1)
            continue;

        git_time_t t = git_commit_time(commit);

        git_tree *tree;
        git_commit_tree(&tree, commit);

        git_tree *parent_tree = nullptr; // first parent

        if (git_commit_parentcount(commit) > 0) { // ?
            git_commit *parent;
            git_commit_parent(&parent, commit, 0);
            git_commit_tree(&parent_tree, parent);
            git_commit_free(parent);
        }

        // FIXME wrong time
        ctx->time = git_commit_time(commit);

        ctx->oid = &oid; // debug
        ctx->commit = commit; // debug



        git_diff *diff = nullptr;

        git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
        opts.flags |= GIT_DIFF_INCLUDE_UNTRACKED;
        // opts.flags |= GIT_DIFF_RENAMES; // error: ‘GIT_DIFF_RENAMES’ was not declared
        // opts.flags |= GIT_DIFF_FIND_RENAMES; // no, this makes it worse

        git_diff_tree_to_tree(&diff, repo, parent_tree, tree, &opts);

        // TODO remove?
        // no effect?
        if (0) {
            // detect file renames
            git_diff_find_options find_opts = GIT_DIFF_FIND_OPTIONS_INIT;
            find_opts.flags |= GIT_DIFF_FIND_RENAMES;
            find_opts.flags |= GIT_DIFF_FIND_RENAMES_FROM_REWRITES;
            find_opts.rename_threshold = 50; // optional tuning
            git_diff_find_similar(diff, &find_opts);
        }

        git_diff_foreach(
            diff,
            diff_cb,
            nullptr,
            nullptr,
            nullptr,
            // &ctx
            ctx.get()
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
    for (const auto &kv : ctx->seen) {
        std::cout << kv.second << " " << kv.first << "\n";
    }

}
