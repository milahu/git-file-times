// how often to call print_stats
#define PRINT_STATS_EVERY_N_COMMITS 1000 // ~ every 100 seconds

#define DEBUG false
// #define DEBUG true

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <iostream>
#include <memory>
#include <chrono>
#include <atomic>
#include <cstring>
#include <cassert>

// NOTE this requires a patched version of libgit2
// in src/vendor/libgit2
// https://github.com/libgit2/libgit2/pull/7296
// export all internal git functions
#include <git2.h>

struct OidHash {
    size_t operator()(const git_oid& oid) const noexcept {
        // reduce the 160-bit oid to a 64-bit hash
        // to assign the oid to a bucket in the unordered_set
        // hash collisions are resolved via OidEq
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

struct BlobState {
    std::string path;
    git_time_t time = 0;
    bool has_time = false;
    bool ignore_next_add = false;
};

// TODO which is better, "using" or "typedef"?
// using BlobStateMap = std::unordered_map<git_oid, BlobState, OidHash, OidEq>;
typedef std::unordered_map<git_oid, BlobState, OidHash, OidEq> BlobStateMap;

struct Context {
    BlobStateMap blobs;
    size_t num_remaining;
    git_time_t time;
    git_oid *oid;
    git_commit *commit;
    git_repository *repo;
    std::string tree_root;
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

static BlobStateMap collect_head_blobs(git_repository *repo)
{
    BlobStateMap blobs;

    git_object *obj = nullptr;
    if (git_revparse_single(&obj, repo, "HEAD^{tree}") != 0 || !obj)
        return blobs;

    git_tree *tree = (git_tree *)obj;

    git_tree_walk(
        tree,
        GIT_TREEWALK_PRE,
        [](const char *root,
           const git_tree_entry *entry,
           void *payload) -> int
        {
            auto *blobs =
                static_cast<BlobStateMap*>(payload);

            if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB)
                return 0;

            const git_oid *oid = git_tree_entry_id(entry);
            const char *name = git_tree_entry_name(entry);

            if (!oid || !name)
                return 0;

            std::string path = (
                (root && *root) ? std::string(root) + name
                : std::string(name)                
            );

            BlobState state;
            state.path = std::move(path);

            blobs->emplace(*oid, std::move(state));

            return 0;
        },
        &blobs
    );

    git_tree_free(tree);

    return blobs;
}

std::string join_paths_3(std::string a, std::string b, std::string c) {
    std::string result;
    const bool debug = false;
    // const bool debug = true;
    if (a.size()) {
        // starts_with requires C++20
        if (a.starts_with("/")) {
            if (debug)
                result += " a=" + a.substr(1);
            else
                result += a.substr(1);
        }
        else {
            if (debug)
                result += "a=" + a;
            else
                result += a;
        }
    }
    if (b.size()) {
        // ends_with requires C++20
        if (result.size() && !result.ends_with("/")) {
            result += "/";
        }
        if (b.starts_with("/")) {
            if (debug)
                result += " b=" + b.substr(1);
            else
                result += b.substr(1);
        }
        else {
            if (debug)
                result += " b=" + b;
            else
                result += b;
        }
    }
    assert(c.size());
    if (result.size() && !result.ends_with("/")) {
        result += "/";
    }
    if (debug)
        result += " c=" + c;
    else
        result += c;
    return result;
}

void compare_tree(
    git_tree* old_tree,
    git_tree* new_tree,
    Context* ctx)
{
    assert(new_tree);

    // if (!old_tree) std::cerr << "old_tree is null\n";

    git_iterator* old_it = nullptr;
    git_iterator* new_it = nullptr;

    git_iterator_options iter_opts = GIT_ITERATOR_OPTIONS_INIT;

    int err;

    err = git_iterator_for_tree(&old_it, old_tree, &iter_opts);
    if (err < 0) {
        const git_error *e = git_error_last();
        std::cerr << "error: failed to get iterator for old_tree: "
                << (e ? e->message : "unknown")
                << "\n";
        return;
    }

    err = git_iterator_for_tree(&new_it, new_tree, &iter_opts);
    if (err < 0) {
        const git_error *e = git_error_last();
        std::cerr << "error: failed to get iterator for new_tree: "
                << (e ? e->message : "unknown")
                << "\n";
        return;
    }

    const git_index_entry* old_e = nullptr;
    const git_index_entry* new_e = nullptr;

    git_iterator_current(&old_e, old_it);
    git_iterator_current(&new_e, new_it);

    while (old_e || new_e) {

        int cmp;

        if (!old_e)
            cmp = 1;
        else if (!new_e)
            cmp = -1;
        else
            cmp = strcmp(old_e->path, new_e->path);

        if (cmp < 0) {
            // path existed in parent
            // but disappeared in current
            git_iterator_advance(&old_e, old_it);
        }
        else if (cmp > 0) {
            // path added in current
            // handle_addition(new_e, ctx);
            // const git_oid *oid = &new_e->oid;
            const git_oid *oid = &new_e->id;
            auto it = ctx->blobs.find(*oid);
            if (it != ctx->blobs.end()) {
                auto &info = it->second;
                if (!info.has_time) {
                    info.time = ctx->time;
                    info.has_time = true;
                    ctx->files_found++;
                }
            }
            git_iterator_advance(&new_e, new_it);
        }
        else {
            const git_oid *old_oid = &old_e->id;
            const git_oid *new_oid = &new_e->id;
            // const git_oid *old_oid = git_tree_entry_id(old_e);
            // const git_oid *new_oid = git_tree_entry_id(new_e);

            // if blob changed, mark new blob as "born here"
            // if (git_oid_cmp(&old_e->id, &new_e->id) != 0) {
            if (!git_oid_equal(old_oid, new_oid)) {
                // same path exists in both trees
                // handle_modification(old_e, new_e, ctx);

                auto it = ctx->blobs.find(*new_oid);
                if (it != ctx->blobs.end()) {

                    auto &info = it->second;

                    if (!info.has_time) {
                        info.time = ctx->time;
                        info.has_time = true;
                        ctx->files_found++;
                    }
                }
            }

            git_iterator_advance(&old_e, old_it);
            git_iterator_advance(&new_e, new_it);
        }
    }

    git_iterator_free(old_it);
    git_iterator_free(new_it);
}

void print_stats(Context &ctx)
{
    using namespace std::chrono;

    auto now = steady_clock::now();
    double elapsed = duration_cast<duration<double>>(now - ctx.start_time).count();
    double since_last = duration_cast<duration<double>>(now - ctx.last_print_time).count();

    if (since_last < 0.5) return; // throttle output (2 Hz max)

    ctx.last_print_time = now;

    size_t total = ctx.blobs.size();

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
        // << " files_left=" << remaining
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

    // Topological order is not cache-friendly.
    // git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL);
    // git_revwalk_sorting(walk, GIT_SORT_TIME);
    // git_revwalk_sorting(walk, GIT_SORT_REVERSE);
    git_revwalk_sorting(walk, GIT_SORT_NONE);

    auto ctx = std::make_unique<Context>();

    ctx->start_time = std::chrono::steady_clock::now();
    ctx->last_print_time = ctx->start_time;

    // ctx->remaining = collect_head_tree(repo);
    ctx->blobs = collect_head_blobs(repo);

    if (DEBUG) {
        // std::cerr << "wanted blobs size: " << ctx->blobs.size() << "\n";
        for (auto blob : ctx->blobs) {
            // std::cerr << "wanted blob: " << git_oid_tostr_s(&blob.first) << " " << blob.second.path << "\n";
        }
    }

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
        ctx->repo = repo;

        git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
        opts.flags |= GIT_DIFF_INCLUDE_UNTRACKED;

        compare_tree(parent_tree, tree, ctx.get());

        // free memory
        if (parent_tree)
            git_tree_free(parent_tree);
        git_tree_free(tree);
        git_commit_free(commit);

        // // stop early
        // if (ctx->remaining.empty())
        //     break;
    }

    git_revwalk_free(walk);
    git_repository_free(repo);

    git_libgit2_shutdown();

    // output result
    for (const auto &kv : ctx->blobs) {
        std::cout << kv.second.time << " " << kv.second.path << "\n";
    }

}
