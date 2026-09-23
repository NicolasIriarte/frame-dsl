'use strict';

// Plain JavaScript, no build step: `vendor/vscode-languageclient.js` is a bundled file produced by
// `scripts/vendor_vscode_languageclient.sh`, not something this extension compiles from
// node_modules. See editors/README.md and docs/editors.md for why.

const { spawnSync } = require('child_process');
const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('./vendor/vscode-languageclient.js');

let client;

function resolveCommand() {
  const configured = vscode.workspace.getConfiguration('frameDsl').get('path');
  return typeof configured === 'string' && configured.trim().length > 0 ? configured : 'frame-dsl';
}

function activate(context) {
  const command = resolveCommand();

  // A cheap presence probe: the tool prints its usage and exits non-zero with no arguments, which
  // is enough to tell "not found" (ENOENT) apart from "found, and it ran".
  const probe = spawnSync(command, [], { timeout: 3000 });
  if (probe.error && probe.error.code === 'ENOENT') {
    vscode.window.showErrorMessage(
      `Frame-DSL: could not run '${command}'. Set the "frameDsl.path" setting to the frame-dsl ` +
        "binary, or put it on PATH -- 'make install PREFIX=~/.local' does that."
    );
    return;
  }

  const serverOptions = {
    command,
    args: ['--lsp'],
    transport: TransportKind.stdio,
  };

  const clientOptions = {
    documentSelector: [
      { scheme: 'file', language: 'fdsl' },
      { scheme: 'untitled', language: 'fdsl' },
    ],
  };

  client = new LanguageClient('frameDsl', 'Frame-DSL', serverOptions, clientOptions);
  context.subscriptions.push(client);
  client.start();
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
