// how often to call print_stats
#define PRINT_STATS_EVERY_N_COMMITS 1000 // ~ every 100 seconds

#define DEBUG false

#include <git2.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <iostream>
#include <memory>
#include <chrono>
#include <atomic>

struct Context {
    std::unordered_map<std::string, git_time_t> seen;
    std::unordered_set<std::string> remaining;
    std::unordered_set<std::string> ignore_next_add;
    std::unordered_map<std::string, time_t> result;
    git_time_t time;
    git_oid *oid;
    git_commit *commit;
    // stats
    size_t commits = 0;
    size_t files_found = 0;
    size_t last_commits = 0;
    size_t last_files_found = 0;
    size_t deltas_seen = 0;
    size_t last_deltas_seen = 0;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_print_time;
};

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

            // note: root ends with "/"
            std::string path = root && *root
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

    ctx->deltas_seen++;

    const char *new_path = delta->new_file.path;

    if (
        !new_path ||
        // path is not part of the HEAD tree
        !ctx->remaining.count(new_path) ||
        // path has been processed
        ctx->seen.count(new_path)
    ) {
        return 0;
    }

    const git_diff_file *f = (
        delta->new_file.path ? &delta->new_file
        : &delta->old_file
    );

    if (!f->path) return 0;

    const char *path = f->path;

    const git_diff_file *oldf = &delta->old_file;
    const git_diff_file *newf = &delta->new_file;

    bool has_old = oldf->path && oldf->path[0];
    bool has_new = newf->path && newf->path[0];

    if (DEBUG) {
        // debug
        std::cerr << "path=" << path
            << " commit=" << git_oid_tostr_s(ctx->oid)
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

    ctx->seen[path] = ctx->time;
    ctx->remaining.erase(path);
    ctx->files_found++;

    return 0;
}

void print_stats(Context &ctx)
{
    using namespace std::chrono;

    auto now = steady_clock::now();
    double elapsed = duration_cast<duration<double>>(now - ctx.start_time).count();
    double since_last = duration_cast<duration<double>>(now - ctx.last_print_time).count();

    if (since_last < 0.5) return; // throttle output (2 Hz max)

    ctx.last_print_time = now;

    size_t remaining = ctx.remaining.size();
    size_t total = ctx.files_found + remaining;

    double commits_per_sec = (ctx.commits - ctx.last_commits) / since_last;
    double files_per_sec = (ctx.files_found - ctx.last_files_found) / since_last;
    double deltas_per_sec = (ctx.deltas_seen - ctx.last_deltas_seen) / since_last;
    double progress = total ? (1.0 * ctx.files_found / total) : 0.0;

    ctx.last_commits = ctx.commits;
    ctx.last_files_found = ctx.files_found;
    ctx.last_deltas_seen = ctx.deltas_seen;

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
        << " deltas/s=" << deltas_per_sec
        << " progress=" << (progress * 100.0) << "%\n";
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

    git_revwalk_push_head(walk);

    git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL);

    auto ctx = std::make_unique<Context>();

    ctx->start_time = std::chrono::steady_clock::now();
    ctx->last_print_time = ctx->start_time;

    ctx->remaining = collect_head_tree(repo);

    git_oid oid;

    // loop commits
    while (!git_revwalk_next(&oid, walk)) {

        ctx->commits++;

        if (ctx->commits % PRINT_STATS_EVERY_N_COMMITS == 0) {
            print_stats(*ctx);
        }

        // std::cerr << "commit: " << git_oid_tostr_s(&oid) << "\n"; // debug

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

        ctx->time = git_commit_time(commit);
        ctx->oid = &oid;
        ctx->commit = commit;

        git_diff *diff = nullptr;

        git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
        opts.flags |= GIT_DIFF_INCLUDE_UNTRACKED;

        git_diff_tree_to_tree(&diff, repo, parent_tree, tree, &opts);

        // loop files
        git_diff_foreach(
            diff,
            diff_cb,
            // TODO add binary_cb?
            nullptr,
            nullptr,
            nullptr,
            ctx.get()
        );

        // free memory
        git_diff_free(diff);
        if (parent_tree)
            git_tree_free(parent_tree);
        git_tree_free(tree);
        git_commit_free(commit);

        // stop early
        if (ctx->remaining.empty())
            break;
    }

    git_revwalk_free(walk);
    git_repository_free(repo);

    git_libgit2_shutdown();

    // output result
    for (const auto &kv : ctx->seen) {
        std::cout << kv.second << " " << kv.first << "\n";
    }

}
