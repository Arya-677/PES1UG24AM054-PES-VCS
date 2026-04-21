# PES-VCS Lab Report

Name: ARYA DEVANG  
SRN: PES1UG24AM054  
Repository: [Arya-677/PES1UG24AM054-PES-VCS](https://github.com/Arya-677/PES1UG24AM054-PES-VCS)

## Implementation Summary

Completed files:

- `object.c`
- `tree.c`
- `index.c`
- `commit.c`

Local screenshot artifacts are saved in the `screenshots/` directory as both terminal-style `.png` images and raw `.txt` captures.

## Screenshot Checklist

- Phase 1A: `screenshots/1A.png`
- Phase 1B: `screenshots/1B.png`
- Phase 2A: `screenshots/2A.png`
- Phase 2B: `screenshots/2B.png`
- Phase 3A: `screenshots/3A.png`
- Phase 3B: `screenshots/3B.png`
- Phase 4A: `screenshots/4A.png`
- Phase 4B: `screenshots/4B.png`
- Phase 4C: `screenshots/4C.png`
- Final integration test: `screenshots/final-integration.png`

## Notes

- The assignment target platform is Ubuntu 22.04, where the provided build commands should work directly after installing OpenSSL development headers.
- On this macOS workspace, the build was verified with temporary include and library flags for Homebrew OpenSSL:
  `CFLAGS='-Wall -Wextra -O2 -I/opt/homebrew/opt/openssl@3/include'`
  `LDFLAGS='-L/opt/homebrew/opt/openssl@3/lib -lcrypto'`
- The provided `Index` struct is large, so local artifact-generation commands were run with a higher shell stack limit on macOS. This does not change the repository contents or the Ubuntu-targeted implementation.

## Verification Commands

- `make all`
- `./test_objects`
- `./test_tree`
- `PES_AUTHOR="ARYA DEVANG <PES1UG24AM054>" make test-integration`

## Phase 5: Branching and Checkout

### Q5.1

To implement `pes checkout <branch>`, I would first read `.pes/refs/heads/<branch>` to get the target commit hash. Then I would update `.pes/HEAD` so it contains `ref: refs/heads/<branch>`. After that, I would read the target commit object, follow its `tree` pointer, recursively walk all tree objects, and rewrite the working directory to match that snapshot.

The working directory update is the hard part. Files that exist in the current branch but not the target branch must be removed. Files present in the target branch must be recreated from blob objects, and nested directories must be created as needed. Permissions such as executable bits must also be restored from the tree modes. Checkout becomes complex because it is not just metadata movement inside `.pes/`; it is a destructive synchronization between committed state and the real filesystem.

### Q5.2

To detect a dirty-working-directory conflict using only the index and object store, I would compare three versions of each tracked path:

1. The version recorded in the index.
2. The current working-directory file.
3. The version in the target branch's tree.

The index stores the staged blob hash and metadata for the current branch snapshot the user is working from. For each tracked file, I would use metadata checks first (`mtime`, `size`) to detect likely modification. If metadata differs, I would hash the current file and compare it with the index hash. If the working file differs from the index, the file is dirty. Then I would compare the target branch's blob hash for that path. If the target branch also wants a different blob at that same path, checkout must refuse because switching would overwrite uncommitted work.

### Q5.3

In detached HEAD state, `.pes/HEAD` contains a commit hash directly instead of a branch reference. New commits would still be created normally, but `head_update()` would advance the detached HEAD itself rather than updating a branch file. That means the new commits exist, but no branch name points to them.

The user can recover those commits by creating a branch or ref that points to the detached commit chain before it becomes unreachable. For example, they could create a new branch file under `.pes/refs/heads/` containing the latest detached commit hash and then repoint `HEAD` to that branch. As long as some reference points to those commits, they remain part of reachable history.

## Phase 6: Garbage Collection and Space Reclamation

### Q6.1

I would implement garbage collection with a mark-and-sweep algorithm.

Mark phase:

1. Start from every reference in `.pes/refs/heads/` and also include `.pes/HEAD` if it is detached.
2. For each referenced commit, mark that commit hash as reachable.
3. Parse each reachable commit and mark its tree hash and parent commit hash.
4. Recursively parse each reachable tree and mark every child tree hash and blob hash.
5. Continue until no new hashes are discovered.

Sweep phase:

1. Walk every file under `.pes/objects/`.
2. Reconstruct the full 64-character hash from its shard path.
3. Delete the object if its hash is not in the reachable set.

The best data structure for reachability is a hash set keyed by the 32-byte object ID or its 64-character hex form, because membership checks must be fast while traversing many objects.

For a repository with 100,000 commits and 50 branches, the number of visited commits is usually much closer to the number of unique commits than `100,000 * 50`, because branches heavily share history. In the worst practical case you would visit about 100,000 commits, plus one tree per commit snapshot, plus all blobs and subtrees reachable from those trees. The traversal is therefore linear in the total number of reachable objects, not the number of branch-path combinations.

### Q6.2

Running GC concurrently with commit creation is dangerous because commit creation publishes objects in stages. A race can happen like this:

1. A commit process writes new blob and tree objects.
2. Before it writes the final commit object and updates the branch ref, those new objects are not yet reachable from any reference.
3. A concurrent GC scans refs, does not see those new objects as reachable, and deletes them.
4. The commit process then writes a commit object that points to tree or blob hashes that GC has already removed.

That leaves the repository with a commit that references missing objects, which is corruption.

Git avoids this by making garbage collection conservative. Real Git keeps recently created unreachable objects for a grace period, uses lock files, and coordinates reference updates carefully so that objects are not deleted while concurrent writers may still publish refs to them. In practice, Git treats object creation and ref updates as a protocol and avoids immediately deleting objects that only appear unreachable for a short time.
