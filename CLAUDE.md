# bOrca on the uConsole

This is bOrca, boorch's C fork of Orca-c, worked on branch `spec/borca-fixes`, which grew from boorch's `4f349cd`. Remotes: `origin` is ianhundere/bOrca-d (push here), `borca` is boorch/bOrca, and `upstream` is hundredrabbits/Orca-c.

Branches on `origin` (Ian's decision, 2026-10-10):
- `spec/borca-fixes` is the working branch. Push each finished story there.
- `main` is bOrca's release line. It is at `v2.0.0` (`09fac7f`, the Phase 2 tip) since 2026-10-11, fast-forwarded from `v1.0.0` (`bb86eaf`), which was force-moved once from the JS Orca on 2026-10-10. It moves only at tagged releases, by fast-forward, and every move or tag push needs Ian's go-ahead. Never force-push it.
- `js-orca` keeps the JS Orca that `main` held.

- The work is specified in `_bmad-output/specs/spec-borca-fixes-improvements/`. Start with `SPEC.md`; its frontmatter lists the companions to read with it.
- The spec gate report is `_bmad-output/stage-gates/spec-2026-10-07.md`. Verified facts are in `docs/reference/facts.md`.
- `_bmad-output/test-artifacts/spec-verification/run-checks.sh` re-runs the gate's code checks against `4f349cd`.

## This device is a live instrument

`borca-tty.service` runs the appliance engine on isolated CPU 3. The rules in `~/projects/sys-restore-uconsole/AGENTS.md` apply here too.

- Ask before any `apt`, `systemctl`, reboot or Bluetooth change. Never run `apt upgrade`.
- Only one sequencer drives MIDI. Stop `borca-tty.service` first, after asking, or run test builds with a unique `BORCA_ALSA_CLIENT_NAME` and a scratch `XDG_CONFIG_HOME`.
- Never edit `~/projects/default.orca` or `~/.config/borca/orca.conf`, and never touch `~/src/borca`, the provisioner's cache.
- Build and test with `nice -n 19 taskset -c 0-2`, so core 3 stays free.
- After installing any bOrca binary, run `verify`. Any FAIL means stop and report.
