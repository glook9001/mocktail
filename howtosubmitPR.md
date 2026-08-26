# How to Submit a Pull Request (PR) for Mocktail

This guide outlines best practices for creating clean, isolated Pull Requests (PRs) using Git and the GitHub CLI (`gh`) so that contributions are atomic, easy to review, and do not confuse upstream maintainers.

---

## 1. Remote Setup (Fork & Upstream)

Make sure your repository has both `origin` (upstream target) and `fork` (your personal fork) configured:

```bash
# Verify existing remotes
git remote -v

# Add upstream repository as origin (if not already set)
git remote add origin https://github.com/komaruworld/mocktail.git

# Add your personal fork as fork (if not already set)
git remote add fork https://github.com/<your-username>/mocktail.git
```

---

## 2. Branching Strategy (Avoid Confusing the Maintainer)

### Golden Rules:
1. **Never submit PRs from `main`**: Always use a dedicated branch for each feature, fix, or topic.
2. **One Topic Per PR**: Never mix unrelated fixes (e.g. login fixes + input changes) into the same branch.
3. **Always Branch from Latest Upstream `origin/main`**: This ensures your branch only contains the commits for that specific pull request.

### Creating a Clean Topic Branch:

```bash
# 1. Fetch the latest commits from upstream
git fetch origin

# 2. Create and switch to a new branch derived directly from upstream main
git checkout -b fix-my-feature-name origin/main
```

> [!TIP]
> If you have uncommitted changes in your working tree that you want to move to the new branch:
> ```bash
> git stash
> git checkout -b fix-my-feature-name origin/main
> git stash pop
> ```

---

## 3. Building and Verifying Tests

Before committing, make sure the project compiles and all unit/integration tests pass:

```bash
# Build the target binary and test suites
make -C build mocktail -j$(nproc)

# Run full test suite with ctest
ctest --test-dir build --output-on-failure -j$(nproc)
```

---

## 4. Committing Your Changes

Stage only the files relevant to the topic and write a descriptive Conventional Commit message:

```bash
# Check modified files
git status

# Stage the modified and deleted files
git add -u

# Commit with a clear summary and explanation
git commit -m "fix(input): resolve mouse aiming and key drop during focus transitions

- Detail change 1
- Detail change 2"
```

---

## 5. Pushing to Your Fork

Push your branch to your personal fork (`fork`):

```bash
git push -u fork fix-my-feature-name
```

---

## 6. Creating the Pull Request

### Option A: Using GitHub CLI (`gh`) (Recommended)

```bash
gh pr create \
  --repo komaruworld/mocktail \
  --base main \
  --head <your-username>:fix-my-feature-name \
  --title "fix(input): resolve mouse aiming and key drops" \
  --body "## Summary
- Detailed bullet points of what this PR changes.
- Why this change is necessary.
- Testing and verification results."
```

### Option B: Using GitHub Web UI
1. Go to your fork on GitHub: `https://github.com/<your-username>/mocktail`
2. Click **"Compare & pull request"** on the banner for your branch `fix-my-feature-name`.
3. Verify that the **base repository** is `komaruworld/mocktail` and **base branch** is `main`.
4. Fill in the title and description, then click **"Create pull request"**.

---

## 7. Updating an Existing PR

If you need to make corrections or add more commits to an already-opened PR, **do not open a new PR**. Simply commit to the same local branch and push:

```bash
# Make your edits, then commit:
git add -u
git commit -m "fix(input): address review feedback"

# Push to your fork (GitHub updates the open PR automatically)
git push fork fix-my-feature-name
```

---

## 8. Synchronizing with Upstream Changes (Rebasing)

If upstream `origin/main` has advanced while your PR is in review:

```bash
# 1. Fetch latest changes
git fetch origin

# 2. Rebase your topic branch on top of upstream main
git rebase origin/main

# 3. If there are merge conflicts, resolve them, then run:
# git add <conflicted-files>
# git rebase --continue

# 4. Force-push the rebased history cleanly to your fork
git push fork fix-my-feature-name --force-with-lease
```
