# Projects

Where the work is: each folder here is a view of its own (V03), which the cli and the ide host beside them. `view new <name>` makes one here, `view load <name>` hosts it again, `view save as <name>` copies a view here, and `view rename <name>` renames one; `./run.sh --<name>` runs one alone. The build compiles each, as it compiles the examples. How each works is in [usage](../docs/usage.md#12-the-cli).

Git ignores everything here but this file, so each person's projects stay theirs, and a project can be a git repository of its own. A project worth sharing becomes an example in `src/runtime/examples/`, or recipes in the library.

A project takes a name no other view the build compiles has, as `wave`, `triangle` or `library`: the build holds one view of each name, and fails, naming both, when two share one.
