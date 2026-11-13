# Contributing to Steelbound

> **Draft for legal review.** This text (especially the rights agreement
> below) is a draft. It will be reviewed by counsel before the first
> contribution from a third party is accepted. Nothing here is legal advice.

Thank you for considering a contribution. Because this project keeps a
single license for its own code (PolyForm Noncommercial 1.0.0, see
`LICENSE`) and the author reserves later commercial licensing, every
contribution needs a rights agreement.

## Reporting bugs and ideas

Bug reports and ideas are welcome from everyone and need no rights
agreement. Use the in-game feedback (**F11** with screenshot, **F9** with a
5-second clip, or **Esc → Feedback**) or open an issue with one of the
templates (*Bug report*, *Idea*). See “Reporting bugs” and “Known issues”
in the [README](README.md).

### Labels (suggested set for maintainers)

The issue templates set `bug` or `enhancement` plus `needs-triage`.
Maintainers then sort issues with these labels:

| Label | Use for |
|---|---|
| `bug` | Something does not work as expected |
| `enhancement` | Idea or feature request |
| `needs-triage` | New, not yet looked at |
| `needs-info` | Waiting for details from the reporter (steps, log, settings) |
| `confirmed` | Reproduced by a maintainer |
| `known-issue` | Listed under “Known issues” in the README |
| `duplicate` | Already reported elsewhere (link the original) |
| `crash` | Game or launcher closes or freezes |
| `graphics` | Rendering, textures, shaders, display modes |
| `performance` | Frame rate, stutter, load |
| `input` | Keyboard, mouse, gamepad, original controller |
| `audio` | Sound and music |
| `launcher` | Launcher pages, setup, settings |
| `saves` | Save games, import, save protection |
| `vr` | VR mode (experimental) |
| `docs` | Documentation |
| `wontfix` | Out of scope, for example other game versions for now |

## What I cannot accept

- **No game content:** no XBE or disc-image parts, textures, screenshots,
  audio, movies, saves or packs derived from the game. Test against your
  own copy, but never add game material to pull requests. In issues, the
  only exception is the screenshot or short clip from the in-game feedback
  package that shows the problem.
- **No GPL- or foreign-licensed content:** no code, tables or text copied
  from GPL-licensed projects or from any source whose license is unclear or
  incompatible. Re-derive facts independently and say which independent
  source you used.
- **No secrets:** no credentials, tokens, webhook URLs, machine paths or
  personal data.

Pull requests that contain any of the above will be closed without review.

## Contributor rights agreement (draft, pending legal review)

By submitting a contribution (pull request, patch or file) to this project
you agree to the following:

1. **Grant of rights.** You grant the project author a worldwide,
   perpetual, irrevocable, royalty-free, transferable right to use your
   contribution in this project, including the right to **relicense it under
   other licenses, including commercial ones**, and to sublicense it.
2. **Own authorship.** You confirm that the contribution is your own
   original work (or that you have all rights needed to submit it on these
   terms), that it contains no game content and no GPL- or foreign-licensed
   content as described above, and that it does not violate anyone else's
   rights.
3. **No obligation.** The author is not obliged to accept, keep or use your
   contribution.

## How to agree

For every pull request, either:

- tick the agreement checkbox in the pull-request template (“I agree to the
  contributor rights agreement in CONTRIBUTING.md”), or
- add a sign-off line to each commit message:

  ```
  Signed-off-by: Your Name <you@example.com>
  ```

  (Replace the name and address with your own; the address above is only a
  format example, not a real contact.)

Pull requests without one of the two cannot be merged.

## Practical notes

- One topic per pull request; describe the cause, the change and how you
  tested it.
- New behaviour needs a configuration switch with the old path kept
  available, and must not regress the 120 FPS budget.
- User-visible text goes through language keys (English default); log lines
  stay untranslated English.
- Be aware this project has no network access by design: contributions must
  not add downloads, updates, telemetry or any network use.
