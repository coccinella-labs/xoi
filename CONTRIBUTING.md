# Contributing

Keep each change focused and link it to one issue. Pull requests must include
`Fixes #N` so the completed issue closes when the change is merged.

Use an imperative commit subject ending with its issue number, for example
`Add response size limit (#7)`. Keep the subject at or below 72 characters.
Do not use the em dash character (U+2014) anywhere in a commit message or
tracked text file.

Enable the local commit hook with:

```sh
git config core.hooksPath scripts/hooks
```

Before opening a pull request, build with warnings as errors and run the tests:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DXOI_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Also run clang-format in check mode, clang-tidy, the repository hygiene check,
the em dash check, and the sanitizer build. CI runs these quality gates on
each proposed change.

Use rebase merging. Do not squash or merge commits; preserve the issue-linked
commit history in the pull request.
