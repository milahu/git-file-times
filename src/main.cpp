// how often to call print_stats
// #define PRINT_STATS_EVERY_N_COMMITS 10000
#define PRINT_STATS_EVERY_N_COMMITS 1000
// #define PRINT_STATS_EVERY_N_COMMITS 100
// #define PRINT_STATS_EVERY_N_COMMITS 10

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
#include <malloc.h>

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

struct TreeFrame
{
    git_tree *current;
    std::vector<git_tree*> parents;
    std::vector<size_t> pidx;
    size_t cidx;
    std::string root;
    bool owns_trees = false;
};

struct Context {
    BlobStateMap blobs;
    size_t num_remaining;
    git_time_t time;
    git_oid *oid;
    git_commit *commit;
    git_repository *repo;
    std::string tree_root;
    bool is_merge = false;
    std::vector<git_tree*> parent_trees;
    // stats
    size_t commits = 0;
    size_t files_found = 0;
    size_t last_commits = 0;
    size_t last_files_found = 0;
    size_t deltas_seen = 0;
    size_t last_deltas_seen = 0;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_print_time;
    bool done_compare_stats = false;
};

// debug memleak
static size_t trees_loaded = 0;
static size_t trees_freed = 0;
static size_t commits_loaded = 0;
static size_t commits_freed = 0;

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

static bool tree_contains_blob_at_path(
    git_tree *tree,
    const std::string &path,
    const git_oid *wanted,
    Context *ctx)
{
    git_tree_entry *entry = nullptr;

    if (git_tree_entry_bypath(&entry, tree, path.c_str()) != 0)
        return false;

    bool found = false;

    if (git_tree_entry_type(entry) == GIT_OBJECT_BLOB) {
        found = git_oid_equal(
            git_tree_entry_id(entry),
            wanted
        );
    }

    git_tree_entry_free(entry);
    return found;
}

void print_stats(Context &ctx)
{
    using namespace std::chrono;

    auto now = steady_clock::now();
    double elapsed = duration_cast<duration<double>>(now - ctx.start_time).count();
    double since_last = duration_cast<duration<double>>(now - ctx.last_print_time).count();

    // if (since_last < 0.5) return; // throttle output (2 Hz max)

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
        << " progress=" << (progress * 100.0) << "%"
        // debug OOM
        << " uordblks=" << mallinfo2().uordblks
        << "\n";
    // // debug memleak
    // std::cerr << "  trees:"
    //     << " loaded=" << trees_loaded
    //     << " freed=" << trees_freed
    //     << " alive=" << (trees_loaded - trees_freed)
    //     << "\n";
    // std::cerr << "  commits:"
    //     << " loaded=" << commits_loaded
    //     << " freed=" << commits_freed
    //     << " alive=" << (commits_loaded - commits_freed)
    //     << "\n";
    // no, this is constant
    // std::cerr << "  blobs=" << ctx.blobs.size() << "\n";

    // no effect
    // // fix memleak?
    // git_odb *odb;
    // git_repository_odb(&odb, ctx.repo);
    // git_odb_refresh(odb);
    // git_odb_free(odb);
}

static void mark_blob_modified(
    const git_oid *coid,
    const std::string &path,
    Context *ctx)
{
    auto it = ctx->blobs.find(*coid);

    if (it == ctx->blobs.end())
        return;

    if (it->second.has_time)
        return;

    it->second.time = ctx->time;
    it->second.has_time = true;

    ctx->num_remaining--;
    ctx->files_found++;

    if (DEBUG) {
        std::cerr
            << "setting time: path=" << path
            << " time=" << ctx->time
            << " commit=" << git_oid_tostr_s(ctx->oid)
            << "\n";
    }
}

static const git_tree_entry *get_entry(
    git_tree *tree,
    size_t idx)
{
    if (!tree)
        return nullptr;

    if (idx >= git_tree_entrycount(tree))
        return nullptr;

    return git_tree_entry_byindex(tree, idx);
}

static void compare_tree_nway(
    git_tree *root_current,
    const std::vector<git_tree*> &root_parents,
    const std::string &root_path,
    Context *ctx)
{
    std::vector<TreeFrame> stack;

    stack.push_back({
        root_current,
        root_parents,
        std::vector<size_t>(root_parents.size(), 0),
        0,
        root_path,
        false // root trees owned by caller
    });

    while (!stack.empty())
    {
        TreeFrame frame = std::move(stack.back());
        stack.pop_back();

        const size_t current_n =
            git_tree_entrycount(frame.current);

        // walk current tree entries
        while (frame.cidx < current_n)
        {
            const git_tree_entry *ce =
                git_tree_entry_byindex(
                    frame.current,
                    frame.cidx);

            const char *cname =
                git_tree_entry_name(ce);

            git_object_t ctype =
                git_tree_entry_type(ce);

            const git_oid *coid =
                git_tree_entry_id(ce);

            //
            // synchronize parent indices
            //

            // entries with identical names
            std::vector<const git_tree_entry*> matches(frame.parents.size(), nullptr);

            // walk parent trees
            for (size_t pi = 0; pi < frame.parents.size(); ++pi)
            {
                git_tree *parent = frame.parents[pi];
                const size_t parent_n = git_tree_entrycount(parent);

                // walk parent tree entries
                while (frame.pidx[pi] < parent_n)
                {
                    const git_tree_entry *pe = git_tree_entry_byindex(parent, frame.pidx[pi]);
                    const char *pname = git_tree_entry_name(pe);
                    int cmp = strcmp(pname, cname);
                    if (cmp < 0)
                    {
                        // entry was deleted from the current tree
                        // TODO:
                        // rename tracking:
                        // deleted blob oid
                        // deleted path
                        frame.pidx[pi]++;
                        matches[pi] = nullptr;
                        // continue walking parent tree entries
                        // until cmp==0 or cmp>0 or end of parent tree
                        continue;
                    }
                    if (cmp == 0)
                    {
                        // found match between current and parent tree
                        matches[pi] = pe;
                        frame.pidx[pi]++;
                        break;
                    }

                    // cmp > 0
                    // entry was added to the current tree
                    // current entry missing in this parent
                    matches[pi] = nullptr;
                    break;
                }
            }

            // process current entry

            std::string path =
                frame.root.empty()
                ? std::string(cname)
                : frame.root + "/" + cname;

            bool identical_in_any_parent = false;
            bool identical_in_all_parents = true;

            // parent trees for the next iteration
            std::vector<git_tree*> next_ptrees;

            // TODO why the if condition? why not always do this?
            if (ctype == GIT_OBJECT_TREE)
            {
                next_ptrees.reserve(matches.size());
            }

            // find entries with identical contents
            for (size_t pi = 0; pi < matches.size(); ++pi)
            {
                const git_tree_entry *pe = matches[pi];
                if (!pe)
                {
                    // no parent entry with identical name
                    continue;
                }
                // parent entry with identical name
                git_object_t ptype = git_tree_entry_type(pe);
                if (ptype != ctype)
                {
                    // type changed
                    // TODO? emit add/delete events
                    identical_in_all_parents = false;
                    // for tracking of renames, we need all add/delete events
                    // so we need to recurse into subtrees
                    if (ptype == GIT_OBJECT_TREE)
                    {
                        // parent tree
                        const git_oid *poid = git_tree_entry_id(pe);
                        git_tree *ptree = nullptr;
                        git_tree_lookup(&ptree, ctx->repo, poid);
                        // NOTE we do not keep track of parent commits
                        // because we only care about the current commit
                        // and whether files changed from parent commits
                        next_ptrees.push_back(ptree);
                    }
                    continue;
                }
                const git_oid *poid = git_tree_entry_id(pe);
                if (git_oid_equal(poid, coid))
                {
                    // parent entry with identical content
                    // entry was copied from the parent tree
                    identical_in_any_parent = true;
                    // no. for tracking of renames, we need all add/delete events
                    // break;
                }
                else
                {
                    // parent entry with different content
                    identical_in_all_parents = false;
                }
                // if (ctype == GIT_OBJECT_TREE)
                if (ptype == GIT_OBJECT_TREE)
                {
                    // parent tree
                    git_tree *ptree = nullptr;
                    git_tree_lookup(&ptree, ctx->repo, poid);
                    // NOTE we do not keep track of parent commits
                    // because we only care about the current commit
                    // and whether files changed from parent commits
                    next_ptrees.push_back(ptree);
                }
            }

            if (identical_in_all_parents)
            {
                frame.cidx++;
                continue;
            }

            if (ctype == GIT_OBJECT_BLOB)
            {
                // current blob
                if (!identical_in_any_parent)
                {
                    // blob was added by the current commit
                    // we found the last-modified time of this file
                    // TODO add option: dont treat renames as modifications
                    // tracking renames is non-trivial
                    // because we have to keep track of all
                    // "add blob" and "delete blob" events
                    // and pair these events by blob ID
                    auto it = ctx->blobs.find(*coid);
                    if (it != ctx->blobs.end())
                    {
                        if (!it->second.has_time)
                        {
                            it->second.time = ctx->time;
                            it->second.has_time = true;
                            ctx->files_found++;
                            // output result
                            // emit the last-modified time as soon as possible
                            std::cout << it->second.time << " " << it->second.path << "\n";
                        }
                    }
                }
                // else: blob was copied from some parent commit

                size_t nparents = next_ptrees.size();
                // TODO what if nparents>0
                // recurse into subtrees to compare their entries
            }
            else if (ctype == GIT_OBJECT_TREE)
            {
                // current subtree
                if (!identical_in_any_parent)
                {
                    // subtree was added by the current commit
                    // recurse into subtrees to compare their entries
                    git_tree *ctree = nullptr;
                    git_tree_lookup(&ctree, ctx->repo, coid);
                    size_t nparents = next_ptrees.size();
                    // TODO verify: does this work when nparents==0
                    stack.push_back({
                        ctree,
                        std::move(next_ptrees),
                        std::vector<size_t>(nparents, 0),
                        0,
                        path,
                        true
                    });
                }
                else // identical_in_any_parent == true
                {
                    // subtree was copied from some parent commit
                    for (git_tree *t : next_ptrees)
                    {
                        git_tree_free(t);
                    }
                }
            }
            frame.cidx++;
        }

        //
        // flush remaining parent entries
        //

        for (size_t pi = 0;
             pi < frame.parents.size();
             ++pi)
        {
            git_tree *parent =
                frame.parents[pi];

            const size_t parent_n =
                git_tree_entrycount(parent);

            while (frame.pidx[pi] < parent_n)
            {
                const git_tree_entry *pe =
                    git_tree_entry_byindex(
                        parent,
                        frame.pidx[pi]);

                //
                // delete event
                //

                // TODO:
                // rename tracking

                frame.pidx[pi]++;
            }
        }

        //
        // free owned trees
        //

        if (frame.owns_trees)
        {
            git_tree_free(
                frame.current);

            for (git_tree *t :
                 frame.parents)
            {
                git_tree_free(t);
            }
        }
    }
}

int main() {

    // no effect?
    // GIT_OBJECT_CACHE_SIZE=0
    // git_libgit2_opts(GIT_OPT_SET_CACHE_MAX_SIZE, 0);

    git_libgit2_init();

    git_repository *repo = nullptr;
    git_revwalk *walk = nullptr;

    // open repo
    if (git_repository_open(&repo, ".") != 0) {
        std::cerr << "failed to open repo\n";
        return 1;
    }

    // walk commits in repo
    git_revwalk_new(&walk, repo);

    // start walking at the HEAD commit
    git_revwalk_push_head(walk);

    // Topological order is not cache-friendly.
    // git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL);
    // git_revwalk_sorting(walk, GIT_SORT_TIME);
    // git_revwalk_sorting(walk, GIT_SORT_REVERSE);
    git_revwalk_sorting(walk, GIT_SORT_NONE);

    // NOTE by default, git_revwalk visits all parents of merge commits
    // only in the "simplify_first_parent" mode
    // git_revwalk visits only the first parent of merge commits
    // git_revwalk_simplify_first_parent(walk);

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

    if (DEBUG) {
        for (auto &[oid, info] : ctx->blobs) {
            if (info.path == "ci.nix") {
                std::cerr
                    << "720: HEAD ci.nix blob="
                    << git_oid_tostr_s(&oid)
                    << "\n";
            }
        }
    }

    git_oid oid;

    // loop commits
    while (!git_revwalk_next(&oid, walk)) {

        ctx->commits++;

        if (ctx->commits % PRINT_STATS_EVERY_N_COMMITS == 0) {
            // std::cerr << "commits=" << ctx->commits << "\n";

            print_stats(*ctx);

            // std::cerr << "  done_compare_stats=false\n";
            ctx->done_compare_stats = false;
        }

        // std::cerr << "commit: " << git_oid_tostr_s(&oid) << "\n"; // debug

        git_commit *commit;
        git_commit_lookup(&commit, repo, &oid);
        commits_loaded++;

        // // skip merge commits
        // if (git_commit_parentcount(commit) > 1)
        //     continue;

        ctx->is_merge = git_commit_parentcount(commit) > 1;

        if (DEBUG) {
            if (ctx->is_merge) {
                std::cerr << "760: commit=" << git_oid_tostr_s(&oid)
                    << " parentcount=" << git_commit_parentcount(commit)
                    << "\n";
            }
        }

        // get parent trees
        for (unsigned i = 0; i < git_commit_parentcount(commit); i++)
        {
            git_commit *parent = nullptr;
            git_tree *ptree = nullptr;

            git_commit_parent(&parent, commit, i);
            commits_loaded++;
            git_commit_tree(&ptree, parent);
            trees_loaded++;

            ctx->parent_trees.push_back(ptree);

            git_commit_free(parent);
            commits_freed++;
        }

        git_time_t t = git_commit_time(commit);

        git_tree *tree;
        git_commit_tree(&tree, commit);
        trees_loaded++;

        git_tree *parent_tree = nullptr; // first parent

        if (git_commit_parentcount(commit) > 0) {
            // commit is not the root commit
            git_commit *parent;
            git_commit_parent(&parent, commit, 0);
            commits_loaded++;
            git_commit_tree(&parent_tree, parent);
            trees_loaded++;
            git_commit_free(parent);
            commits_freed++;
        }

        ctx->time = git_commit_time(commit);
        ctx->oid = &oid;
        ctx->commit = commit;
        ctx->repo = repo;

        git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
        opts.flags |= GIT_DIFF_INCLUDE_UNTRACKED;

        compare_tree_nway(tree, ctx->parent_trees, "", ctx.get());

        // free memory
        // TODO cache trees and subtrees
        if (parent_tree) {
            git_tree_free(parent_tree);
            trees_freed++;
        }
        for (auto *t : ctx->parent_trees) {
            git_tree_free(t);
            trees_freed++;
        }
        ctx->parent_trees.clear();
        git_tree_free(tree);
        trees_freed++;
        git_commit_free(commit);
        commits_freed++;

        // // stop early
        // if (ctx->remaining.empty())
        //     break;
    }

    git_revwalk_free(walk);
    git_repository_free(repo);

    git_libgit2_shutdown();

    // no. output is done in compare_tree_nway
    // // output result
    // for (const auto &kv : ctx->blobs) {
    //     std::cout << kv.second.time << " " << kv.second.path << "\n";
    // }

}
