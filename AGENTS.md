# Project Instructions

## Git discipline

- This project lives inside a larger dirty parent repository. Never use broad staging or commit commands that can capture parent-repo changes by accident.
- Always scope `git add` to the specific `deck` files being changed.
- Always scope commits to the intended `deck` work only.
- Do not stage build artifacts, `.env`, or unrelated generated files unless the user explicitly asks for them in a commit.
- Prefer narrow, path-limited git commands over repo-wide commands.

## Commit workflow

- Reusable approval exists for `git add` and `git commit`, but that does not change the requirement to keep staging narrow and intentional.
- Before committing, verify exactly which `deck` files are being included.
