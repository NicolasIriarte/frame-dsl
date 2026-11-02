;;; frame-dsl-mode.el --- Major mode for Frame-DSL definition files  -*- lexical-binding: t; -*-

;; Version: 0.2.0
;; Package-Requires: ((emacs "26.1"))
;; Keywords: languages

;;; Commentary:

;; Font locking, comments and indentation for the `.fdsl' files described in manual/language.md.
;;
;; This is a regular-expression mode rather than a parser.  It knows the shape of the language and
;; not its meaning: `valof(len)' is a keyword and a paren to it, and a field named like an attribute
;; value is coloured as one.  That is the layer underneath: with `frame-dsl --lsp' running -- `M-x
;; eglot' -- highlighting, diagnostics and formatting all come from the compiler instead, and this is
;; what a buffer looks like in the moment before the server answers.  docs/editors.md says why.
;;
;; The three word lists below are the only place a word is written down, and a test in
;; test/unit/9_editor_support.c compares them against the compiler's own tables.  The failure this
;; guards against is quiet: the language grows a keyword, the editor does not, and the file you are
;; looking at stops being the file the compiler reads.  Add a word here and nowhere else.

;;; Code:

(require 'flymake)
(require 'subr-x)

(defgroup frame-dsl nil
  "Major mode for Frame-DSL definition files."
  :group 'languages
  :prefix "frame-dsl-")

(defcustom frame-dsl-indent-offset 2
  "Columns of indentation per nesting level.
Two, because that is what every file under examples/ is written at."
  :type 'integer
  :group 'frame-dsl
  :safe #'integerp)

;; ---------------------------------------------------------------------------------------------
;; The words.  Checked against src/lexer.c and src/parser.c by test/unit/9_editor_support.c.

(defconst frame-dsl-keywords
  '("pkt" "valof" "block")
  "Reserved words.  None of these may be used as an identifier.")

(defconst frame-dsl-attribute-names
  '("bit_order" "byte_order" "cal" "decode" "if")
  "Attribute names this version of the language knows.
These are not reserved: they are meaningful only after `@', and a field may
perfectly well be called `bit_order'.  Highlighted only where they mean
something, which is why the patterns below anchor on the `@'.")

(defconst frame-dsl-attribute-values
  '("msb_first" "lsb_first" "big_endian" "little_endian" "auto" "manual")
  "Values an attribute may take.  Not reserved either, for the same reason.")

;; ---------------------------------------------------------------------------------------------
;; Syntax

(defvar frame-dsl-mode-syntax-table
  (let ((table (make-syntax-table)))
    ;; A comment runs from `#' to the end of the line, and that is the only form there is.
    (modify-syntax-entry ?#  "<" table)
    (modify-syntax-entry ?\n ">" table)
    ;; Underscores are part of a name, so `\_<' boundaries and M-f land where a reader expects.
    (modify-syntax-entry ?_  "_" table)
    ;; Brackets pair like braces.  That is load-bearing for indentation: a multi-line attribute
    ;; group is a `[' ... `]' block and indents as one.
    (modify-syntax-entry ?\[ "(]" table)
    (modify-syntax-entry ?\] ")[" table)
    ;; Arithmetic, so that a width expression is punctuation rather than word characters.
    (dolist (character '(?+ ?- ?* ?/ ?= ?: ?? ?\; ?@))
      (modify-syntax-entry character "." table))
    table)
  "Syntax table for `frame-dsl-mode'.")

;; ---------------------------------------------------------------------------------------------
;; Font lock
;;
;; Ordered most specific first: an earlier pattern wins the characters it claims, so the field-name
;; rule cannot steal the name of a packet and the attribute rules cannot colour a field that merely
;; shares a word with one.

(defvar frame-dsl-font-lock-keywords
  (let ((name "[A-Za-z_][A-Za-z0-9_]*"))
    `(
      ;; `pkt NAME' -- the packet being declared.
      (,(concat "\\_<pkt\\_>[ \t]+\\(" name "\\)") 1 font-lock-type-face)

;; `import "path" as name;' -- neither word is reserved, so both are matched in that shape only.
      ("^[ \t]*\\(import\\)[ \t]+\"" 1 font-lock-keyword-face)
      ("\"[ \t]+\\(as\\)[ \t]+[A-Za-z_]" 1 font-lock-keyword-face)

      ;; `block NAME {' at file level -- a block being declared to be reused -- and `name: NAME'
      ;; where a field uses one: a name where a width would be can only be a block.
      (,(concat "\\_<block\\_>[ \t]+\\(" name "\\)") 1 font-lock-type-face)
      (,(concat "^[ \t]*" name "[ \t]*:[ \t]*\\(" name "\\(?:\\." name "\\)*\\)[ \t]*[{;=:?]") 1 font-lock-type-face)

      ;; `enum NAME: width {' -- not reserved either, so matched only in that shape at file level.
      (,(concat "^[ \t]*\\(enum\\)[ \t]+\\(" name "\\)[ \t]*:")
       (1 font-lock-keyword-face)
       (2 font-lock-type-face))

            ;; `cal NAME(param) = ...' and its `inverse (param) = ...'.  Neither word is reserved -- each
      ;; means something only where a calibration is declared -- so both are matched in that shape
      ;; rather than listed with the keywords, and a field called `cal' stays a field.
      (,(concat "^[ \t]*\\(cal\\)[ \t]+\\(" name "\\)[ \t]*(")
       (1 font-lock-keyword-face)
       (2 font-lock-function-name-face))
      ("\\_<\\(inverse\\)[ \t]*([ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*)[ \t]*=" 1 font-lock-keyword-face)
      ;; `@cal: NAME' -- the value is a calibration's name, not one of the fixed attribute values.
      (,(concat "@[ \t]*cal[ \t]*:[ \t]*\\(" name "\\)") 1 font-lock-function-name-face)

      ;; `@bit_order: msb_first;' -- the name and the value, each only where it means something.
      (,(concat "@[ \t]*\\(" (regexp-opt frame-dsl-attribute-names) "\\)\\_>")
       1 font-lock-builtin-face)
      (,(concat "@[ \t]*" (regexp-opt frame-dsl-attribute-names) "[ \t]*:[ \t]*\\("
                (regexp-opt frame-dsl-attribute-values) "\\)\\_>")
       1 font-lock-constant-face)
      ;; An attribute the compiler will reject, so that a typo looks wrong before it is compiled.
      (,(concat "@[ \t]*\\(" name "\\)") 1 font-lock-warning-face)

      ;; The reserved words.
      (,(concat "\\_<" (regexp-opt frame-dsl-keywords) "\\_>") . font-lock-keyword-face)

      ;; `NAME { ... };' or `NAME;' on a line of its own -- a block pasted in without an instance
      ;; name.  After the keywords, so that `block {' keeps its keyword face: a highlight here only
      ;; lands on characters nothing above has claimed.
      (,(concat "^[ \t]*\\(" name "\\(?:\\." name "\\)*\\)[ \t]*[{;]") 1 font-lock-type-face)

      ;; `valof(len)' -- the field whose value gives this one its width.
      (,(concat "\\_<valof\\_>[ \t]*([ \t]*\\(" name "\\)") 1 font-lock-variable-name-face)

      ;; A field being declared: its name, optionally carrying a reservation, before the colon.
      (,(concat "^[ \t]*\\(" name "\\)[ \t]*\\(\\[[^]\n]*\\]\\)?[ \t]*:")
       (1 font-lock-variable-name-face)
       ;; The reservation is the one number in the language written in bytes.  Different face, so it
       ;; does not read as part of the name.
       (2 font-lock-preprocessor-face nil t))

      ;; Numbers, in all three bases the lexer accepts.
      ("\\_<0[xX][0-9A-Fa-f]+\\_>" . font-lock-constant-face)
      ("\\_<0[bB][01]+\\_>"        . font-lock-constant-face)
      ("\\_<[0-9]+\\_>"            . font-lock-constant-face)))
  "Font-lock patterns for `frame-dsl-mode'.")

;; ---------------------------------------------------------------------------------------------
;; Indentation
;;
;; Nesting depth times the offset, which is the whole rule.  It works because every construct that
;; nests -- a packet, a block, a multi-line attribute group -- is delimited by a pair the syntax
;; table already knows about, so `syntax-ppss' has counted them and there is nothing to parse here.

(defun frame-dsl--calculate-indent ()
  "Return the column this line should start at."
  (save-excursion
    (beginning-of-line)
    (let* ((state (syntax-ppss))
           (depth (car state)))
      (if (nth 4 state)
          ;; Inside a comment that began on an earlier line.  There is no such thing in this
          ;; language, but a mode that reindents by depth inside one would be wrong if there were.
          (current-indentation)
        ;; A line that closes a block belongs to the level the block was opened at.
        (when (looking-at "[ \t]*[]})]")
          (setq depth (1- depth)))
        (* frame-dsl-indent-offset (max 0 depth))))))

(defun frame-dsl-indent-line ()
  "Indent the current line.
Point keeps its position in the text rather than being dragged to the indentation,
unless it was in the indentation to begin with."
  (interactive)
  (let ((indent (frame-dsl--calculate-indent))
        (offset (- (point-max) (point))))
    (indent-line-to indent)
    (when (< (point) (- (point-max) offset))
      (goto-char (- (point-max) offset)))))

;; ---------------------------------------------------------------------------------------------
;; Checking as you type.  The buffer is piped to `frame-dsl - --check --stdin-name FILE', so what is
;; checked is what is on screen, imports resolve from beside FILE, and the answer is the compiler's
;; own -- nothing here knows what a mistake looks like.

(defcustom frame-dsl-program "frame-dsl"
  "The frame-dsl executable: a name on `exec-path', or an absolute file name.
`make install PREFIX=~/.local' puts it in ~/.local/bin."
  :type 'string
  :group 'frame-dsl)

(defcustom frame-dsl-flymake t
  "Non-nil turns on `flymake-mode' in Frame-DSL buffers when `frame-dsl-program' is found.
The backend is registered either way, so `M-x flymake-mode' works when this is nil."
  :type 'boolean
  :group 'frame-dsl)

(defvar-local frame-dsl--flymake-process nil
  "The check in flight for this buffer, so a newer one can replace it.")

(defconst frame-dsl--check-line-regexp
  "^\\(.+?\\):\\(?:\\([0-9]+\\):\\([0-9]+\\):\\)? \\(error\\|warning\\): \\(.*\\)$"
  "One line of `frame-dsl --check' output: file, optional line and column, severity, message.")

(defun frame-dsl--import-line (source name file)
  "The line in SOURCE whose `import' brought in FILE, or 1 when none did directly.
NAME is the file SOURCE is checked as; imports are written relative to it."
  (let ((relative (file-relative-name file (file-name-directory name))))
    (with-current-buffer source
      (save-excursion
        (save-restriction
          (widen)
          (goto-char (point-min))
          (if (re-search-forward (concat "^[ \t]*import[ \t]+\"" (regexp-quote relative) "\"")
                                 nil t)
              (line-number-at-pos (match-beginning 0))
            1))))))

(defun frame-dsl--flymake-parse (source name)
  "Turn the current buffer's `--check' output into Flymake diagnostics for SOURCE.
NAME is the file SOURCE was checked as.  A problem in an imported file is shown
on the `import' that brought it in, since that file is not the one on screen."
  (let (diagnostics)
    (goto-char (point-min))
    (while (re-search-forward frame-dsl--check-line-regexp nil t)
      (let* ((file (match-string 1))
             (line (and (match-string 2) (string-to-number (match-string 2))))
             (column (and (match-string 3) (string-to-number (match-string 3))))
             (type (if (equal (match-string 4) "warning") :warning :error))
             (message (match-string 5))
             (here (string= (expand-file-name file) (expand-file-name name)))
             (region (if here
                         (flymake-diag-region source (or line 1) column)
                       (flymake-diag-region source (frame-dsl--import-line source name file)))))
        (unless here
          (setq message (format "in %s%s: %s" (file-relative-name file (file-name-directory name))
                                (if line (format ":%d:%d" line column) "") message)))
        (when region
          (push (flymake-make-diagnostic source (car region) (cdr region) type message)
                diagnostics))))
    (nreverse diagnostics)))

(defun frame-dsl-flymake (report-fn &rest _args)
  "Flymake backend: check the buffer with `frame-dsl-program', reporting to REPORT-FN."
  (unless (executable-find frame-dsl-program)
    (error "Cannot find `%s'; see `frame-dsl-program'" frame-dsl-program))
  (when (process-live-p frame-dsl--flymake-process)
    (kill-process frame-dsl--flymake-process))
  (let ((source (current-buffer))
        (name (or buffer-file-name (expand-file-name (buffer-name)))))
    (save-restriction
      (widen)
      (setq frame-dsl--flymake-process
            (make-process
             :name "frame-dsl-flymake"
             :noquery t
             :connection-type 'pipe
             :buffer (generate-new-buffer " *frame-dsl-flymake*")
             :command (list frame-dsl-program "-" "--check" "--stdin-name" name)
             :sentinel
             (lambda (process _event)
               (when (memq (process-status process) '(exit signal))
                 (unwind-protect
                     (if (not (with-current-buffer source (eq process frame-dsl--flymake-process)))
                         (flymake-log :warning "Cancelling obsolete check %s" process)
                       (with-current-buffer (process-buffer process)
                         (let ((diagnostics (frame-dsl--flymake-parse source name)))
                           ;; Status 2 with nothing parseable is the tool failing, not the file.
                           (if (and (null diagnostics) (eq (process-exit-status process) 2))
                               (funcall report-fn :panic :explanation (buffer-string))
                             (funcall report-fn diagnostics)))))
                   (kill-buffer (process-buffer process)))))))
      (process-send-region frame-dsl--flymake-process (point-min) (point-max))
      (process-send-eof frame-dsl--flymake-process))))

;; ---------------------------------------------------------------------------------------------
;; The language server.  `frame-dsl --lsp' answers with the compiler's own diagnostics, formatting and
;; highlighting, so with it running this mode's regular expressions are the layer underneath rather
;; than the whole story.  Both clients are registered; neither is required, and neither is loaded here.

(defvar eglot-server-programs)
(declare-function eglot-alternatives "eglot" (alternatives))

(defun frame-dsl-server-command ()
  "The command that starts the language server, from `frame-dsl-program'."
  (list frame-dsl-program "--lsp"))

(with-eval-after-load 'eglot
  ;; A function rather than a list, so changing `frame-dsl-program' after loading still takes effect.
  (add-to-list 'eglot-server-programs
               (cons 'frame-dsl-mode (lambda (&rest _) (frame-dsl-server-command)))))

(with-eval-after-load 'lsp-mode
  (declare-function lsp-register-client "lsp-mode" (client))
  (declare-function make-lsp-client "lsp-mode" (&rest args))
  (declare-function lsp-stdio-connection "lsp-mode" (command &optional test-command))
  (declare-function lsp-activate-on "lsp-mode" (&rest languages))
  (defvar lsp-language-id-configuration)
  (add-to-list 'lsp-language-id-configuration '(frame-dsl-mode . "fdsl"))
  (lsp-register-client
   (make-lsp-client :new-connection (lsp-stdio-connection #'frame-dsl-server-command)
                    :activation-fn (lsp-activate-on "fdsl")
                    :server-id 'frame-dsl)))

;; ---------------------------------------------------------------------------------------------
;; Formatting.  The layout is the tool's (`frame-dsl --format'), not this mode's: the buffer goes
;; through it and comes back, and a buffer that does not parse is left exactly as it is.

(defun frame-dsl-format-buffer ()
  "Format the buffer with `frame-dsl --format'.
Point stays where it was in the text.  When the buffer does not parse, nothing
changes and the errors are shown instead."
  (interactive)
  (unless (executable-find frame-dsl-program)
    (user-error "Cannot find `%s'; see `frame-dsl-program'" frame-dsl-program))
  (let ((source (current-buffer))
        (output (generate-new-buffer " *frame-dsl-format*"))
        (name (or buffer-file-name (expand-file-name (buffer-name)))))
    (unwind-protect
        (let ((status (save-restriction
                        (widen)
                        (call-process-region (point-min) (point-max) frame-dsl-program nil output nil
                                             "-" "--format" "--stdin-name" name))))
          (if (not (eql status 0))
              (user-error "Not formatted: %s"
                          (string-trim (with-current-buffer output (buffer-string))))
            (save-restriction
              (widen)
              ;; Only the differences are replaced, so point, marks and undo survive a reformat.
              (replace-buffer-contents output))
            (message "Formatted")))
      (kill-buffer output)
      (set-buffer source))))

;; ---------------------------------------------------------------------------------------------

;;;###autoload
(define-derived-mode frame-dsl-mode prog-mode "Frame-DSL"
  "Major mode for editing Frame-DSL definition files.

\\{frame-dsl-mode-map}"
  :syntax-table frame-dsl-mode-syntax-table
  (setq-local font-lock-defaults '(frame-dsl-font-lock-keywords))
  (setq-local comment-start "# ")
  (setq-local comment-end "")
  (setq-local comment-start-skip "#+[ \t]*")
  (setq-local comment-use-syntax t)
  (setq-local indent-line-function #'frame-dsl-indent-line)
  (setq-local electric-indent-chars (append "}])" electric-indent-chars))
  ;; A definition file is a column-aligned thing; tabs in one are a nuisance to everybody reading it
  ;; at a different tab width.
  (setq-local indent-tabs-mode nil)
  (add-hook 'flymake-diagnostic-functions #'frame-dsl-flymake nil t)
  (when (and frame-dsl-flymake (executable-find frame-dsl-program))
    (flymake-mode 1)))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.fdsl\\'" . frame-dsl-mode))

(provide 'frame-dsl-mode)

;;; frame-dsl-mode.el ends here
