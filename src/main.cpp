// how often to call print_stats
#define PRINT_STATS_EVERY_N_COMMITS 100 // ~ every 10 seconds

#define DEBUG true

#include <git2.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <iostream>
#include <memory>
#include <chrono>
#include <atomic>
#include <cstring>

struct BlobInfo {
    git_time_t time = 0;
    bool done = false;
};

struct OidHash {
    size_t operator()(const git_oid& oid) const noexcept {
        uint64_t v;
        memcpy(&v, oid.id, sizeof(v));
        return std::hash<uint64_t>{}(v);
    }
};

struct OidEq {
    bool operator()(const git_oid& a, const git_oid& b) const noexcept {
        return git_oid_equal(&a, &b);
    }
};

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

    std::unordered_map<git_oid, git_time_t, OidHash, OidEq> blob_time;
    std::unordered_set<git_oid, OidHash, OidEq> remaining_blobs;
    std::unordered_map<std::string, git_oid> head_paths;

    size_t total_blobs = 0;

    // stats
    size_t commits = 0;
    size_t files_found = 0;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_print_time;
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
        if (DEBUG) std::cerr << "no blob\n";
        return 0;
    }

    const char *name = git_tree_entry_name(entry);
    if (!name) {
        if (DEBUG) std::cerr << "empty name\n";
        return 0;
    }

    std::string path = join_path(root, name);

    // debug
    if (DEBUG) std::cerr << "path=" << path
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

    if (DEBUG) {
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
    }

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
        ctx->files_found++;
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

void print_stats(Context &ctx)
{
    using namespace std::chrono;

    auto now = steady_clock::now();
    double elapsed = duration_cast<duration<double>>(now - ctx.start_time).count();
    double since_last = duration_cast<duration<double>>(now - ctx.last_print_time).count();

    if (since_last < 0.5) return; // throttle output (2 Hz max)

    ctx.last_print_time = now;

    // size_t remaining = ctx.remaining.size();
    size_t remaining = ctx.remaining_blobs.size();

    size_t total = ctx.files_found + remaining;

    double commits_per_sec = ctx.commits / (elapsed + 1e-9);
    double files_per_sec = ctx.files_found / (elapsed + 1e-9);
    double progress = total ? (1.0 * ctx.files_found / total) : 0.0;

    double eta = (1 - progress) * total / files_per_sec;

    std::cerr
        << "stats: "
        << "commits=" << ctx.commits
        << " time=" << elapsed
        << " eta=" << eta
        << " commits/s=" << commits_per_sec
        << " files_done=" << ctx.files_found
        << " files_left=" << remaining
        << " files/s=" << files_per_sec
        << " progress=" << (progress * 100.0) << "%\n";
}

void collect_head_tree(
    git_repository* repo,
    git_tree* tree,
    const std::string& prefix,
    Context& ctx)
{
    size_t n = git_tree_entrycount(tree);

    for (size_t i = 0; i < n; ++i) {

        auto* e = git_tree_entry_byindex(tree, i);

        std::string path =
            prefix +
            git_tree_entry_name(e);

        switch (git_tree_entry_type(e)) {

        case GIT_OBJECT_BLOB: {

            git_oid oid =
                *git_tree_entry_id(e);

            ctx.head_paths[path] = oid;

            ctx.remaining_blobs.emplace(oid);

            break;
        }

        case GIT_OBJECT_TREE: {

            git_tree* sub;

            git_tree_lookup(
                &sub,
                repo,
                git_tree_entry_id(e)
            );

            collect_head_tree(
                repo,
                sub,
                path + "/",
                ctx
            );

            git_tree_free(sub);

            break;
        }

        default:
            break;
        }
    }
}

inline void resolve_blob(
    Context& ctx,
    const git_oid* oid,
    git_time_t time)
{
    auto it =
        ctx.remaining_blobs.find(*oid);

    if (it == ctx.remaining_blobs.end())
        return;

    ctx.blob_time[*oid] = time;

    ctx.remaining_blobs.erase(it);
    ctx.files_found++;
}

void compare_trees(
    git_repository* repo,
    git_tree* parent,
    git_tree* current,
    git_time_t time,
    Context& ctx
)
{
    const size_t n_old = parent ? git_tree_entrycount(parent) : 0;
    const size_t n_new = current ? git_tree_entrycount(current) : 0;

    size_t i = 0, j = 0;

    while (i < n_old || j < n_new)
    {
        const git_tree_entry* e_old =
            (i < n_old)
                ? git_tree_entry_byindex(parent, i)
                : nullptr;

        const git_tree_entry* e_new =
            (j < n_new)
                ? git_tree_entry_byindex(current, j)
                : nullptr;

        const char* name_old =
            e_old ? git_tree_entry_name(e_old) : nullptr;

        const char* name_new =
            e_new ? git_tree_entry_name(e_new) : nullptr;

        // pick lexicographically smaller path
        int cmp;

        if (!e_old) cmp = 1;
        else if (!e_new) cmp = -1;
        else cmp = strcmp(name_old, name_new);

        // --- only in old tree (deletion) ---
        if (cmp < 0)
        {
            // old file removed in this commit
            // ignore for "latest modification of HEAD blob"
            ++i;
            continue;
        }

        // --- only in new tree (addition) ---
        if (cmp > 0)
        {
            const git_tree_entry* ne = e_new;

            git_object_t t = git_tree_entry_type(ne);

            if (t == GIT_OBJECT_BLOB)
            {
                const git_oid* oid = git_tree_entry_id(ne);

                auto it = ctx.remaining_blobs.find(*oid);

                if (it != ctx.remaining_blobs.end())
                {
                    ctx.blob_time[*oid] = time;
                    ctx.remaining_blobs.erase(it);
                    ctx.files_found++;
                }
            }
            else if (t == GIT_OBJECT_TREE)
            {
                git_tree* sub;

                git_tree_lookup(
                    &sub,
                    repo,
                    git_tree_entry_id(ne)
                );

                compare_trees(
                    repo,
                    nullptr,
                    sub,
                    time,
                    ctx
                );

                git_tree_free(sub);
            }

            ++j;
            continue;
        }

        // --- same name exists in both trees ---
        const git_tree_entry* oe = e_old;
        const git_tree_entry* ne = e_new;

        git_object_t ot = git_tree_entry_type(oe);
        git_object_t nt = git_tree_entry_type(ne);

        const git_oid* oid_old = git_tree_entry_id(oe);
        const git_oid* oid_new = git_tree_entry_id(ne);

        // blob -> blob
        if (ot == GIT_OBJECT_BLOB && nt == GIT_OBJECT_BLOB)
        {
            if (!git_oid_equal(oid_old, oid_new))
            {
                auto it = ctx.remaining_blobs.find(*oid_new);

                if (it != ctx.remaining_blobs.end())
                {
                    ctx.blob_time[*oid_new] = time;
                    ctx.remaining_blobs.erase(it);
                    ctx.files_found++;
                }
            }

            ++i;
            ++j;
            continue;
        }

        // tree -> tree
        if (ot == GIT_OBJECT_TREE && nt == GIT_OBJECT_TREE)
        {
            git_tree* old_sub;
            git_tree* new_sub;

            git_tree_lookup(&old_sub, repo, oid_old);
            git_tree_lookup(&new_sub, repo, oid_new);

            compare_trees(
                repo,
                old_sub,
                new_sub,
                time,
                ctx
            );

            git_tree_free(old_sub);
            git_tree_free(new_sub);

            ++i;
            ++j;
            continue;
        }

        // type change (rare: file->dir or dir->file)
        if (nt == GIT_OBJECT_BLOB)
        {
            const git_oid* oid = oid_new;

            auto it = ctx.remaining_blobs.find(*oid);

            if (it != ctx.remaining_blobs.end())
            {
                ctx.blob_time[*oid] = time;
                ctx.remaining_blobs.erase(it);
            }
        }

        ++i;
        ++j;
    }
}

int main() {
    git_libgit2_init();

    git_repository *repo = nullptr;
    git_revwalk *walk = nullptr;

    if (git_repository_open(&repo, ".") != 0) {
        std::cerr << "failed to open repo\n";
        return 1;
    }

    auto ctx = std::make_unique<Context>();

    ctx->start_time = std::chrono::steady_clock::now();
    ctx->last_print_time = ctx->start_time;

    git_object* obj;

    git_revparse_single(&obj, repo, "HEAD^{tree}");

    collect_head_tree(repo, (git_tree*)obj, "", *ctx);

    ctx->total_blobs = ctx->remaining_blobs.size();

    // git_revwalk* walk;

    git_revwalk_new(&walk, repo);

    git_revwalk_push_head(walk);

    git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL);

    git_oid oid;

    while (
        !ctx->remaining_blobs.empty() &&
        !git_revwalk_next(&oid, walk)
    )
    {

        ctx->commits++;

        if (ctx->commits % PRINT_STATS_EVERY_N_COMMITS == 0) {
            print_stats(*ctx);
        }

        git_commit* commit;

        git_commit_lookup(&commit, repo, &oid);

        git_tree* tree;

        git_commit_tree(&tree, commit);

        if (git_commit_parentcount(commit))
        {
            git_commit* parent;
            git_commit_parent(&parent, commit, 0);

            git_tree* parent_tree;

            git_commit_tree(&parent_tree, parent);

            compare_trees(
                repo,
                parent_tree,
                tree,
                git_commit_time(commit),
                *ctx
            );

            git_tree_free(parent_tree);
            git_commit_free(parent);
        }
        else
        {
            compare_trees(
                repo,
                nullptr,
                tree,
                git_commit_time(commit),
                *ctx
            );
        }

        git_tree_free(tree);
        git_commit_free(commit);

    }

    // output result
    for (auto const& [path, oid] : ctx->head_paths)
    {
        auto it = ctx->blob_time.find(oid);

        if (it == ctx->blob_time.end())
            continue;

        std::cout
            << it->second
            << " "
            << path
            << "\n";
    }



    #if false

    // ctx->remaining = collect_head_tree(repo, &ctx);
    // ctx->remaining = collect_head_tree(repo, ctx.get());
    ctx->remaining = collect_head_tree(repo);

    git_oid oid;

    while (!git_revwalk_next(&oid, walk)) {

        ctx->commits++;

        if (ctx->commits % PRINT_STATS_EVERY_N_COMMITS == 0) {
            print_stats(*ctx);
        }

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

        if (1) {

            // depth-first search
            // find the latest commit of this file in this branch

            for (auto it = ctx->remaining.begin(); it != ctx->remaining.end(); ) {
                const std::string &path = *it;

                if (DEBUG) std::cerr << "path=" << path << "\n";

                git_tree_entry *entry = nullptr;
                int err = git_tree_entry_bypath(&entry, tree, path.c_str());

                if (err == 0) {
                    ctx->result[path] = t;
                    if (DEBUG) std::cerr << "  t=" << t << "\n";
                    it = ctx->remaining.erase(it); // remove from future work
                    ctx->files_found++;
                } else {
                    ++it;
                }
            }

            git_tree_free(tree);
            git_commit_free(commit);

            if (ctx->remaining.empty())
                break;

            continue;

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

    #endif

}
