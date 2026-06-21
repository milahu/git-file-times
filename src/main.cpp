// how often to call print_stats
#define PRINT_STATS_EVERY_N_COMMITS 10000
// #define PRINT_STATS_EVERY_N_COMMITS 1000
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

#include "nadeausoftware_rss.h"

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
    std::vector<git_oid> added_blobs;
    std::vector<git_oid> deleted_blobs;
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
        // << " uordblks=" << mallinfo2().uordblks
        // << " rss=" << (getCurrentRSS() / 1024.0 / 1024.0) << "MiB"
        << " rss=" << (getCurrentRSS() / 1024 / 1024) << "MiB"
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

static void emit_add_blob(
    const git_oid *oid,
    Context *ctx)
{
    ctx->added_blobs.push_back(*oid);
}

static void emit_delete_blob(
    const git_oid *oid,
    Context *ctx)
{
    ctx->deleted_blobs.push_back(*oid);
}

static void emit_tree_as_adds(
    git_tree *tree,
    Context *ctx)
{
    std::vector<git_tree*> stack;
    stack.push_back(tree);

    while (!stack.empty())
    {
        git_tree *t = stack.back();
        stack.pop_back();

        size_t n = git_tree_entrycount(t);

        for (size_t i = 0; i < n; ++i)
        {
            const git_tree_entry *e = git_tree_entry_byindex(t, i);
            git_object_t type = git_tree_entry_type(e);
            const git_oid *oid = git_tree_entry_id(e);

            if (type == GIT_OBJECT_BLOB)
            {
                // emit_delete_blob(oid, ctx);
                emit_add_blob(oid, ctx);
            }
            else if (type == GIT_OBJECT_TREE)
            {
                git_tree *subtree = nullptr;
                git_tree_lookup(&subtree, ctx->repo, oid);
                stack.push_back(subtree);
            }
        }

        if (t != tree)
            git_tree_free(t);
    }
}

static void emit_tree_as_deletes(
    git_tree *tree,
    Context *ctx)
{
    std::vector<git_tree*> stack;
    stack.push_back(tree);

    while (!stack.empty())
    {
        git_tree *t = stack.back();
        stack.pop_back();

        size_t n = git_tree_entrycount(t);

        for (size_t i = 0; i < n; ++i)
        {
            const git_tree_entry *e = git_tree_entry_byindex(t, i);
            git_object_t type = git_tree_entry_type(e);
            const git_oid *oid = git_tree_entry_id(e);

            if (type == GIT_OBJECT_BLOB)
            {
                // emit_add_blob(oid, ctx);
                emit_delete_blob(oid, ctx);
            }
            else if (type == GIT_OBJECT_TREE)
            {
                git_tree *sub = nullptr;
                git_tree_lookup(&sub, ctx->repo, oid);
                stack.push_back(sub);
            }
        }
        if (t != tree)
            git_tree_free(t);
    }
}

void emit_entry_as_add(
    const git_tree_entry *e,
    Context *ctx)
{
    git_object_t type = git_tree_entry_type(e);
    const git_oid *oid = git_tree_entry_id(e);
    if (type == GIT_OBJECT_BLOB)
    {
        emit_add_blob(oid, ctx);
        return;
    }
    if (type == GIT_OBJECT_TREE)
    {
        git_tree *tree = nullptr;
        git_tree_lookup(&tree, ctx->repo, oid);
        size_t n = git_tree_entrycount(tree);
        for (size_t i = 0; i < n; i++)
        {
            auto e = git_tree_entry_byindex(tree, i);
            emit_entry_as_add(e, ctx);
        }
        git_tree_free(tree);
    }
}

void emit_entry_as_delete(
    const git_tree_entry *e,
    Context *ctx)
{
    git_object_t type = git_tree_entry_type(e);
    const git_oid *oid = git_tree_entry_id(e);
    if (type == GIT_OBJECT_BLOB)
    {
        emit_delete_blob(oid, ctx);
        return;
    }
    if (type == GIT_OBJECT_TREE)
    {
        git_tree *tree = nullptr;
        git_tree_lookup(&tree, ctx->repo, oid);
        size_t n = git_tree_entrycount(tree);
        for (size_t i = 0; i < n; i++)
        {
            auto e = git_tree_entry_byindex(tree, i);
            emit_entry_as_delete(e, ctx);
        }
        git_tree_free(tree);
    }
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
                        emit_entry_as_delete(pe, ctx);
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

            // TODO remove?
            std::string path =
                frame.root.empty()
                ? std::string(cname)
                : frame.root + "/" + cname;

            // match = same name in parent tree
            // TODO? rename to has_same_name
            bool match_in_any_parent = false;

            // TODO? rename to explained
            // identical = same content in parent tree
            bool identical_in_any_parent = false;

            // identical = same content in parent tree
            // bool identical_in_all_parents = true;
            // false for the root commit
            bool identical_in_all_parents = !frame.parents.empty();

            // parent trees for the next stack iteration
            // parents that could still explain this subtree
            // TODO? rename to recurse_parents
            std::vector<git_tree*> next_ptrees;
            // std::vector<git_tree*> recurse_parents;
            next_ptrees.reserve(matches.size());

            // pending "delete entry" events
            // pending "add entry" events
            // we can emit the actual events
            // only after we have processed all parent trees
            std::vector<const git_tree_entry*> deletes;
            std::vector<const git_tree_entry*> adds;

            // find entries with identical contents
            for (size_t pi = 0; pi < matches.size(); ++pi)
            {
                const git_tree_entry *pe = matches[pi];
                if (!pe)
                {
                    // no parent entry with identical name
                    // TODO emit add/delete events?
                    // later?
                    // if (!match_in_any_parent)
                    // {
                    //     emit_entry_as_add(ce, ctx);
                    // }
                    continue;
                }
                match_in_any_parent = true;
                // parent entry with identical name
                git_object_t ptype = git_tree_entry_type(pe);
                if (ptype != ctype)
                {
                    // type changed
                    identical_in_all_parents = false;
                    // FIXME defer emitting these events
                    // we do not want these events if identical_in_any_parent==true
                    // todo_emit_entry_as_delete.emplace(pe);
                    // todo_emit_entry_as_add.emplace(ce);
                    // emit_entry_as_delete(pe, ctx);
                    // emit_entry_as_add(ce, ctx);
                    deletes.push_back(pe);
                    // adds.push_back(ce); // TODO why not?
                    if (ctype == GIT_OBJECT_TREE)
                    {
                        // TODO what?!
                        // ptype != ctype
                        // ctype == GIT_OBJECT_TREE
                        // ptype == GIT_OBJECT_BLOB
                        // -> git_tree_lookup makes no sense with pe
                        git_tree *ptree = nullptr;
                        git_tree_lookup(&ptree, ctx->repo, git_tree_entry_id(pe));
                        next_ptrees.push_back(std::move(ptree));
                    }
                    continue;
                }
                // ctype == ptype
                const git_oid *poid = git_tree_entry_id(pe);
                if (git_oid_equal(poid, coid))
                {
                    // this parent explains the entry
                    // parent entry with identical content
                    // entry was copied from the parent tree
                    // this is the most common case (?)
                    identical_in_any_parent = true;

                    // // no. for tracking of renames, we need all add/delete events
                    // // break;
                    // continue;

                    // TODO? only for ctype==GIT_OBJECT_TREE
                    // this parent explains the current subtree
                    // discard other pending parents
                    for (git_tree *t : next_ptrees)
                        git_tree_free(t);
                    next_ptrees.clear();

                    // stop comparing entries
                    // FIXME? but then "identical_in_all_parents" is wrong
                    break;
                }
                if (ptype == GIT_OBJECT_BLOB)
                {
                    // parent blob with different content
                    // ctype == ptype == GIT_OBJECT_BLOB
                    // git_oid_equal(poid, coid) == false
                    // this is the second-most common case (?)
                    identical_in_all_parents = false;
                    emit_delete_blob(poid, ctx);
                    emit_add_blob(coid, ctx);
                    continue;
                }
                // parent tree with different content
                // ctype == ptype == GIT_OBJECT_TREE
                // git_oid_equal(poid, coid) == false
                identical_in_all_parents = false;
                // recurse into subtrees to compare their entries
                git_tree *ptree = nullptr;
                git_tree_lookup(&ptree, ctx->repo, poid);
                next_ptrees.push_back(ptree);
            }

            if (!match_in_any_parent) {
                // entry was not copied from any parent tree
                // entry was added to the current tree
                emit_entry_as_add(ce, ctx);
            }

            // if (identical_in_all_parents)
            if (identical_in_any_parent)
            {
                // nothing was changed
                // the current entry was copied from some parent
                frame.cidx++;
                continue;
            }

            for (auto *pe : deletes)
                emit_entry_as_delete(pe, ctx);

            for (auto *pe : adds)
                emit_entry_as_add(pe, ctx);

            // WTF?
            // if (!match_in_any_parent)
            //     emit_entry_as_add(ce, ctx);
            // else
            //     emit_entry_as_add(ce, ctx);

            // no. duplicate
            // if (ctype == GIT_OBJECT_BLOB)
            // {
            //     // current blob
            //     if (!identical_in_any_parent)
            //     {
            //         // blob was added by the current commit
            //         // TODO modified or added or renamed?
            //         emit_add_blob(coid, ctx);
            //     }
            //     // else: blob was copied from some parent commit
            // }
            // else if (ctype == GIT_OBJECT_TREE)
            // {
            //     if (!next_ptrees.empty())
            //     {
            //         // recurse into subtrees to compare their entries
            //         git_tree *ctree = nullptr;
            //         git_tree_lookup(&ctree, ctx->repo, coid);
            //         size_t nparents = next_ptrees.size();
            //         // TODO verify: does this work when nparents==0
            //         stack.push_back({
            //             ctree,
            //             std::move(next_ptrees),
            //             std::vector<size_t>(nparents, 0),
            //             0,
            //             path,
            //             true
            //         });
            //     }
            //     else
            //     {
            //         // subtree first appears here
            //         emit_entry_as_add(ce, ctx);
            //     }
            // }

            // size_t nparents = next_ptrees.size();
            // TODO what if nparents>0
            // recurse into subtrees to compare their entries

            if (ctype == GIT_OBJECT_TREE && !next_ptrees.empty())
            {
                git_tree *ctree = nullptr;
                git_tree_lookup(&ctree, ctx->repo, coid);
                size_t nparents = next_ptrees.size();
                stack.push_back({
                    ctree,
                    std::move(next_ptrees),
                    std::vector<size_t>(nparents, 0),
                    0,
                    path,
                    true
                });
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

// process blob events created by
// emit_add_blob and emit_delete_blob
static void process_blob_events(Context *ctx)
{
    std::unordered_set<git_oid, OidHash, OidEq> adds;
    std::unordered_set<git_oid, OidHash, OidEq> dels;

    // FIXME this does not support
    // simultaneous rename and copy operations
    // so when a file A was both renamed to B and copied to C
    // then the "copied to C" part is ignored

    for (auto &oid : ctx->added_blobs)
        adds.insert(oid);

    for (auto &oid : ctx->deleted_blobs)
        dels.insert(oid);

    // remove renames/copies
    for (auto it = adds.begin(); it != adds.end(); )
    {
        auto dit = dels.find(*it);
        if (dit != dels.end())
        {
            dels.erase(*it);
            it = adds.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // remaining adds are true introductions
    for (const auto &oid : adds)
    {
        auto kv = ctx->blobs.find(oid);
        if (kv == ctx->blobs.end())
            continue;
        if (!kv->second.has_time)
        {
            kv->second.time = ctx->time;
            kv->second.has_time = true;
            ctx->files_found++;
            // output result as soon as possible
            // std::cout << ctx->time << " " << kv->second.path << "\n";
            std::cout << kv->second.time << " " << kv->second.path << "\n";
            // TODO erase is wrong?
            // // remove blob from our todo list
            // ctx->blobs.erase(kv);
        }
    }

    ctx->added_blobs.clear();
    ctx->deleted_blobs.clear();

    // // free memory
    // ctx->added_blobs.shrink_to_fit();
    // ctx->deleted_blobs.shrink_to_fit();
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

        process_blob_events(ctx.get());

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

        // TODO restore. stop early if we have all file times
        // or what is the current stop condition?
        // the program stops if (files_done == ctx->blobs.size())
        // but why...?
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
