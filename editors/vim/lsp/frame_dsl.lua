-- Neovim 0.11 and later find this on the runtime path; `vim.lsp.enable('frame_dsl')` turns it on.
-- What the server does is in manual/editors.md.
return {
  cmd = { 'frame-dsl', '--lsp' },
  filetypes = { 'fdsl' },
  -- A definition file is often edited on its own, so the file's own directory is a fine root.
  root_markers = { '.git' },
}
