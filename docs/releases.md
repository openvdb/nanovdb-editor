# Cutting a release

The [Release workflow](../.github/workflows/release.yml) builds the selected commit
with the existing wheel workflow and checks the package set before publication.
Release and prerelease modes create a version tag, upload the same files to PyPI,
and publish a GitHub Release with generated notes and SHA-256 checksums. Dev mode
publishes the separate `nanovdb-editor-dev` package to PyPI. It does not change the
editor or its API.

## Prepare and validate

1. Update `pymodule/VERSION.txt` in a pull request. This is the version source for
   both CMake and Python. Use a normalized Python package version, such as `0.1.8`
   for a release or `0.1.8rc1` for a prerelease. Local suffixes and version epochs
   are not supported. Choose a version that is unused on PyPI and has no Git tag.
2. Merge the reviewed changes into `main`, including the version update, after
   the normal native and Python checks pass.
3. Open **Actions > Release > Run workflow**, select `main`, and keep
   `mode=dry_run`. A dry run can also validate a development branch after the
   workflow is available on the default branch.
4. Inspect the successful run and download `nanovdb-editor-release-assets`.
   It contains the wheels, source distribution, and `SHA256SUMS`.

The build runs wheel import checks and the fVDB integration suites against the
built Linux wheel with both pinned and nightly fVDB. The release validator
requires the current build matrix: two Linux x86-64 variants, Linux ARM64,
macOS ARM64, Windows x86-64, Windows ARM64, and one source distribution. It checks
package names, versions, and wheel tags, then runs `twine check --strict`.
The Linux build chooses its final manylinux tag from the linked libraries; the
artifact name alone does not establish the wheel's minimum glibc version.

Neither `dry_run` nor `dry_run_dev` creates a tag, publishes a GitHub Release, or
uploads to PyPI. Each runs the same builds and verification as publication for
its package. These checks cover packaging and the existing wheel tests; they do
not replace the normal native and Python CI suites.

## Publish a release

Run **Release** again on `main`, selecting `release` for a stable version or
`prerelease` for an `a`, `b`, `rc`, or `.dev` version. Both modes publish the
`nanovdb-editor` package to PyPI. A prerelease is also marked as a prerelease on
GitHub. Publication from other branches or tags is refused.

Before building, publication checks the [PyPI release API](https://docs.pypi.org/api/json/#get-a-release)
and rejects a version that already exists. Only a `404 Not Found` allows the run
to continue; connection failures and other responses stop it. Dry runs skip this
check. The lookup does not reserve the version or prove that a deleted version
can be reused; the PyPI upload remains the final check.

The run rebuilds and verifies its selected commit; it does not reuse a previous
dry run's files. Review that commit before starting. After all build and
verification jobs succeed, the workflow:

1. Creates `v<VERSION.txt>` at the run's exact commit and a draft GitHub Release.
2. Attaches the verified packages and checksums to the draft.
3. Uploads those packages to PyPI using the existing `PYPI_API_TOKEN` secret.
4. Publishes the GitHub Release after the PyPI upload succeeds.

The workflow token creates the tag. GitHub does not trigger another workflow
from that token's tag push, so this workflow uploads to PyPI directly. See
[GitHub's workflow trigger rules](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/trigger-a-workflow).
The existing tag publisher remains available for manually pushed tags. Do not
push a release tag while a Release run is in progress. Manual dispatch of the
older **NanoVDB Editor Publish** workflow still targets TestPyPI, and
**NanoVDB Editor Publish Dev** still publishes `nanovdb-editor-dev`.

## Publish a dev package

1. Set `pymodule/VERSION.txt` to a version that is unused for `nanovdb-editor-dev`
   on PyPI, and push the reviewed changes to the branch you want to publish.
   The build keeps this version unchanged; it does not add a `.dev` suffix.
2. Run **Actions > Release > Run workflow** on that branch with `mode=dry_run_dev`.
   Inspect the successful run and its `nanovdb-editor-release-assets` artifact.
3. Run **Release** again on the intended branch commit with `mode=dev`.

Dev mode checks version availability for `nanovdb-editor-dev` before building,
runs the same package and integration checks, and uploads the verified wheels
and source distribution to real PyPI using `PYPI_API_TOKEN`. It creates no Git
tag or GitHub Release. Any branch is allowed; tag refs are refused. Versions in
the separate `nanovdb-editor` project and existing Git tags do not block it.

As with release publication, dev publication rebuilds its selected commit and
does not reuse dry-run artifacts. Do not run the older **NanoVDB Editor Publish
Dev** workflow for the same version at the same time.

## Recover a failed publication

Build or verification failures create no release tag or package upload. Fix the
failure and run again. Release and prerelease modes reject existing tags before
a new publication run.

If publication fails after tag creation, inspect the tag, draft release, and PyPI
files before retrying. A completed tag/draft job is preserved when using **Re-run
failed jobs**; a failed tag/draft job can leave a tag or partially uploaded draft
that requires maintainer cleanup before restarting. Do not move a published tag.

PyPI uploads are not atomic and existing files are not overwritten or silently
skipped. A partially completed upload requires maintainer recovery using the
verified files from that run, after comparing their checksums with the files
already on PyPI. Do not rebuild different files under the same version. Publish
the draft GitHub Release only after the complete package set is on PyPI. A failed
final GitHub publication can be retried with **Re-run failed jobs**.

## Check release tooling locally

```sh
python -m pip install packaging
python -m unittest discover -s scripts/ci -p 'test_check_release.py' -v
python scripts/ci/check_release.py prepare --mode dry_run --ref refs/heads/main
```

When changing the wheel matrix, update the expected artifact set in
`scripts/ci/check_release.py` and its tests together.
