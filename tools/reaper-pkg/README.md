# Native64 process-tree reaper

Unmodified public [Plagman/reaper](https://github.com/Plagman/reaper) at
`fb26ffe691718ae149095b2b478ad88628463017`, with its MIT license. Steam's shipped
helper is i386; this static musl build uses Brook's supported x86-64 Linux ABI.
No Steam client, account files or game data are included.

Build with `nix-build tools/reaper-pkg --no-out-link`. The package check uses the
public orphan control: the helper must wait after its direct child exits and
propagate an adopted grandchild's success or failure. Run that control on Brook
too before replacing a private client's `steamrt64/reaper`; retain its original.
This requires real `PR_SET_CHILD_SUBREAPER` and child adoption, not a success stub.
