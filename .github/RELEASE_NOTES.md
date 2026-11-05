**btop-claude** is [btop++](https://github.com/aristocratos/btop) with a **Claude usage chart** next to your processes, so you can watch Claude Code's token use alongside CPU, memory and network.

## What the box shows

- A graph of tokens over time: input + output + cache writes. Cache reads are left out of the graph and totals because they would dwarf everything else.
- Token totals for the last 5h, 24h, 7d, 30d and 10 minutes.
- Optional 5-hour and 7-day plan limit meters with reset times (see below).

| Key | Action |
| --- | --- |
| `shift + u` | Show or hide the box |
| `shift + r` | Cycle the graph range: 5h, 24h, 7d, 30d |

Move the box above or below the processes with `claude_position` under Options → proc. If the terminal is too short for both, the box hides itself.

## Downloads

| File | For | How to use it |
| --- | --- | --- |
| `btop-claude-x86_64-linux` | 64-bit Intel/AMD Linux | Make it executable and run it |
| `btop-claude-aarch64-linux` | 64-bit ARM Linux | Make it executable and run it |
| `SHA256SUMS` | Both | Verify a download with `sha256sum -c --ignore-missing SHA256SUMS` |

Needs glibc 2.39 or newer.

## Install

```sh
mkdir -p ~/.local/bin
curl -fL -o ~/.local/bin/btop-claude https://github.com/GPTmadeit/btop/releases/latest/download/btop-claude-$(uname -m)-linux
chmod +x ~/.local/bin/btop-claude
btop-claude
```

It uses the same `~/.config/btop` config and themes as regular btop and doesn't replace it.

## Update

```sh
btop-claude --self-update
```

This needs `curl` and `sha256sum`. It verifies the download against `SHA256SUMS` and checks that the new binary runs before replacing the old one.

## Plan limit meters (optional)

Add this to `~/.claude/settings.json`, using the full path to your binary:

```json
"statusLine": { "type": "command", "command": "/home/YOU/.local/bin/btop-claude --claude-statusline" }
```

Claude Code then passes its plan limits to btop-claude, which also prints them as your status line. Claude Code hides most of its footer keyboard hints while a custom status line is set. Limits are only available on Pro, Max and Ultra plans, and update while a Claude Code session is open.

## Privacy

Usage comes from the session transcripts in `~/.claude/projects`, and only token counts and timestamps are kept. The box makes no network requests and doesn't read your Claude credentials. Only `--self-update` uses the network, and only to reach GitHub.

Only usage from Claude Code on this computer is counted, so claude.ai and other devices aren't included.

## Credits

Built on [btop++](https://github.com/aristocratos/btop) by Aristocratos and contributors, Apache-2.0.
