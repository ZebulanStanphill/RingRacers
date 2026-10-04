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
