# OurNotes in a chat with Claude (MCP)

`ournotes-cli mcp` is a [Model Context Protocol](https://modelcontextprotocol.io) server:
a chat in the Claude desktop app (or another MCP client) can search the library, read its
PDFs and notes, see your highlights, cite and export, and, if you allow it, write notes,
change fields and tags, file items and mark passages. It runs on your computer, started
by the chat app, and talks to it over standard input and output; nothing goes anywhere
but to the chat you are in.

## Setting it up in the Claude desktop app

Open *Settings → Developer → Edit Config*, or the file itself:

| System  | Config file                                                     |
|---------|-----------------------------------------------------------------|
| macOS   | `~/Library/Application Support/Claude/claude_desktop_config.json` |
| Windows | `%APPDATA%\Claude\claude_desktop_config.json`                     |
| Linux   | `~/.config/Claude/claude_desktop_config.json`                     |

and add OurNotes under `mcpServers` (keep what else is in the file):

```json
{
  "mcpServers": {
    "ournotes": {
      "command": "/home/you/.local/bin/ournotes-cli",
      "args": ["mcp"]
    }
  }
}
```

`command` is the full path of `ournotes-cli` (a `~` is not expanded):

- Linux, installed with `scripts/install-linux.sh`: `/home/you/.local/bin/ournotes-cli`
- macOS: `/Applications/OurNotes.app/Contents/MacOS/ournotes-cli`
- Windows: the folder you unzipped OurNotes into, as
  `C:\\Users\\you\\OurNotes\\ournotes-cli.exe` (backslashes doubled in JSON)

Quit the Claude app completely and start it again. In a chat, the **+** button under the
message box → *Connectors* then lists *ournotes*, with a switch that turns it on or off.
Claude asks before it uses a tool the first time, and you can allow it once or always.

### Letting it change the library

With `"args": ["mcp"]` the chat can only read. To let it write notes and change items
too, add `--write`:

```json
"args": ["mcp", "--write"]
```

What it changes goes out with the app's next sync and shows in an open app within
seconds; *Tools → Sync History* lists it as `ournotes-cli mcp`. Nothing is deleted for
good: `trash_items` moves to the trash, where you can restore it.

### Another library

The library is the one the app uses (`~/OurNotes` unless changed in the app). For another:
`"args": ["--data-dir", "/path/to/library", "mcp"]`.

## Other MCP clients

Any client that starts a local (stdio) server works the same way: the command
`ournotes-cli`, the argument `mcp` (and `--write`). For Claude Code:

```sh
claude mcp add ournotes -- ~/.local/bin/ournotes-cli mcp
```

(Claude Code can also run `ournotes-cli` itself: see [agents.md](agents.md).) The
claude.ai website cannot start programs on your computer, so it cannot use this server.

## The tools

Reading (always offered):

| Tool | What it gives |
|------|---------------|
| `list_libraries` | your libraries and group libraries |
| `list_collections` | every collection's path (`FA26/CS8803`), key and number of items |
| `search_items` | items by words (titles and authors; every field and the notes; or the PDFs' text too), collection, tags, unfiled or in the trash; 50 at a time unless asked for more |
| `get_item` | one item's fields, notes and attachments |
| `read_text` | a PDF's text, 20 pages at a time from any page, or a note's |
| `get_annotations` | your highlights, underlines, notes and drawings in an item's PDFs |
| `list_tags`, `get_related` | tags; the items marked as related |
| `format_citations` | a bibliography or in-text citations in a style (APA, IEEE, ...) |
| `export_items` | BibTeX, RIS or CSL JSON |
| `get_link` | a Markdown link that opens an item, a note or a page in OurNotes |

Changing (with `--write`):

| Tool | What it does |
|------|--------------|
| `add_note`, `edit_note` | a note (in Markdown) under an item or of its own; replaced or added to |
| `set_fields` | an item's title, date, abstract, DOI and other fields |
| `tag_items`, `file_items` | tags added and removed; items put into a collection |
| `create_collection` | a new collection |
| `annotate_pdf` | a highlight, underline or note on a passage of a PDF, found by its words |
| `add_items` | items by DOI, arXiv ID, ISBN or PMID (looked up online) |
| `trash_items` | items, notes or attachments moved to the trash |

Notes are written in Markdown; coloured text is `<span style="color: #ff6666">...</span>`
in it (the app's colours: red `#ff6666`, blue `#2ea8e5`, green `#5fb236`, ...), and
`<u>`, `<s>`, `<sub>`, `<sup>` work too. To change a note you formatted, Claude reads its
HTML with `get_item`, changes what it should, and sends it back whole with `format: html`,
so your colours stay.

Each tool runs the `ournotes-cli` command of the same purpose (`items`, `show`, `text`,
`add-note`, ...), on the library as it is at that moment, so the chat sees what the app
has just changed and the app sees what the chat changed.

## Things to ask

- "In my FA26/CS8803 collection, which papers are about attention? Summarise each in two
  lines."
- "Read the PDF of *Attention Is All You Need* and explain section 3; link the pages you
  use."
- "What did I highlight in that paper? Put my highlights and comments into a note under
  it." (needs `--write`)
- "Give me the APA references of everything tagged *exam*."
- "Find items without a date and fill them in from their PDFs." (needs `--write`)

## When it does not show up

The Claude app writes what the server says to its logs: `~/Library/Logs/Claude/` on
macOS, `%APPDATA%\Claude\logs\` on Windows, `~/.config/Claude/logs/` on Linux, in
`mcp-server-ournotes.log`. The first line the server writes there names the library it
opened. Run the command by hand to see that it starts:

```sh
~/.local/bin/ournotes-cli mcp      # waits for messages; Ctrl+D ends it
```
