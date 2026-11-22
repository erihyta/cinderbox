# Cinderbox sheets for VS Code

Colours for sheets (`.cbsheet`) and sheet tests (`.cbtest`), comments with `#`, and
`cb_sheet check`'s (and `cb_sheet test`'s) mistakes as problems, for whoever writes sheets outside
Godot. The Godot editor's Sheet screen does more: the mods' names offered as you type, mistakes
marked as you type, Format.

## Install

Copy (or link) this folder into VS Code's extensions folder, then restart VS Code:

| System | Folder |
|---|---|
| Windows | `%USERPROFILE%\.vscode\extensions\cinderbox.cbsheet-0.1.0` |
| macOS, Linux | `~/.vscode/extensions/cinderbox.cbsheet-0.1.0` |

## Mistakes as problems

A task with the `$cb_sheet` problem matcher (`.vscode/tasks.json` in the checkout):

```json
{
	"version": "2.0.0",
	"tasks": [
		{
			"label": "check this sheet",
			"type": "shell",
			"command": "cb_sheet check ${relativeFile}",
			"problemMatcher": "$cb_sheet"
		},
		{
			"label": "run this sheet test",
			"type": "shell",
			"command": "cb_sheet test ${relativeFile}",
			"problemMatcher": "$cb_sheet"
		}
	]
}
```

`cb_sheet` is built with the rest (`bin/cb_sheet`); run it from the checkout's folder.
