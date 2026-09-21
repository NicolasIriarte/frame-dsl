;;; frame-dsl-mode.el --- Major mode for Frame-DSL definition files  -*- lexical-binding: t; -*-

;; Version: 0.1.0
;; Package-Requires: ((emacs "26.1"))
;; Keywords: languages

;;; Commentary:

;; Font locking, comments and indentation for the `.fdsl' files described in manual/language.md.
;;
;; This is a regular-expression mode rather than a parser.  It knows the shape of the language and
;; not its meaning: `valof(len)' is a keyword and a paren to it, and a field named like an attribute
;; value is coloured as one.  docs/highlighting.md says why that is the right trade for now, and
;; where the alternatives (tree-sitter, and the language server of issue #30) fit.
;;
;; The three word lists below are the only place a word is written down, and a test in
;; test/unit/9_editor_support.c compares them against the compiler's own tables.  The failure this
;; guards against is quiet: the language grows a keyword, the editor does not, and the file you are
;; looking at stops being the file the compiler reads.  Add a word here and nowhere else.

;;; Code:

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
  '("bit_order" "byte_order" "cal")
  "Attribute names this version of the language knows.
These are not reserved: they are meaningful only after `@', and a field may
perfectly well be called `bit_order'.  Highlighted only where they mean
something, which is why the patterns below anchor on the `@'.")

(defconst frame-dsl-attribute-values
  '("msb_first" "lsb_first" "big_endian" "little_endian")
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
    (dolist (character '(?+ ?- ?* ?/ ?= ?: ?\; ?@))
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
  (setq-local indent-tabs-mode nil))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.fdsl\\'" . frame-dsl-mode))

(provide 'frame-dsl-mode)

;;; frame-dsl-mode.el ends here
