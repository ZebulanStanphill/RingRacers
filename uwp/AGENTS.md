# Commit scope

Keep each commit focused: implement one change or fix per commit, with the
documentation for that change. Split unrelated changes into separate commits,
even when they're made in the same session. The engine changes stay one commit
in the embedded patch, so each of this repository's commits updates
`patches/ringracers-uwp.patch` with just its own change.

# Commit attribution

Every commit created or amended with agent assistance must name the agents and
models that contributed in the commit message. Use the body or trailers; keep the
subject focused on the change. List each participating agent and its actual model,
and preserve existing attribution when amending a commit.

For example:

```text
Agent: Codex (Sol)
Model: gpt-6.1-sol
```

Preserve the human Git author identity. Do not invent model names or attribute
earlier work to the current agent. Apply this convention to embedded patch commit
messages as well as this repository's commits.
