#pragma once

#include <iosfwd>

#include "cli/commands.h"

// `ournotes-cli mcp`: a Model Context Protocol server on standard input and output, so that
// a chat with Claude (the desktop app) or another MCP client can read the library, and with
// --write change it. Its tools run the commands of ournotes-cli (docs/mcp.md).

struct McpOptions {
    on::fs::path dataDir;  // the library; opened afresh for each tool call
    bool write = false;    // the tools that change the library offered too
};

// Answers JSON-RPC messages, one per line, from `in` on `out` until `in` ends.
void runMcpServer(CommandMap& commands, const McpOptions& options, std::istream& in, std::ostream& out);

// The `mcp` command (registerAllCommands).
void registerMcpCommand(CommandMap& commands);
