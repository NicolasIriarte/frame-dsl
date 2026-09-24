;;; frame-dsl-mode-tests.el --- Tests for frame-dsl-mode  -*- lexical-binding: t; -*-

;;; Commentary:

;; Run from the repository root:
;;
;;     make test-editors
;;
;; or by hand:
;;
;;     emacs -Q --batch -L editors -l editors/frame-dsl-mode-tests.el \
;;           -f ert-run-tests-batch-and-exit
;;
;; These need Emacs; `make test-editors' says so and passes when it is not installed, because a
;; machine without Emacs can still build and test everything else in this project.  The half of the
;; mode that can be checked *without* Emacs -- that its word lists still match the compiler's -- is
;; in test/unit/9_editor_support.c and runs everywhere.

;;; Code:

(require 'ert)
(require 'frame-dsl-mode)

(defvar frame-dsl-tests-directory
  (file-name-directory (or load-file-name buffer-file-name))
  "Where this file lives, so examples/ can be found relative to it.")

(defun frame-dsl-tests--indent-buffer ()
  "Reindent the whole buffer, without `indent-region's progress messages.

Under --batch every `message' goes to stderr, so the \"Indenting region...\" pair it prints for
each call would otherwise land in the middle of `make test' once per example file."
  (let ((inhibit-message t))
    (indent-region (point-min) (point-max))))

(defun frame-dsl-tests--fontify (text)
  "Put TEXT in a Frame-DSL buffer, fontify it, and return the buffer."
  (let ((buffer (generate-new-buffer " *frame-dsl-test*")))
    (with-current-buffer buffer
      (insert text)
      (frame-dsl-mode)
      (font-lock-ensure))
    buffer))

(defun frame-dsl-tests--face-of (text needle)
  "Fontify TEXT and return the face on the first character of NEEDLE."
  (let ((buffer (frame-dsl-tests--fontify text)))
    (unwind-protect
        (with-current-buffer buffer
          (goto-char (point-min))
          (unless (search-forward needle nil t)
            (error "%S does not appear in the sample" needle))
          (let ((face (get-text-property (match-beginning 0) 'face)))
            ;; font-lock hands back a face or a list of them, depending on how it got there.
            (if (listp face) (car face) face)))
      (kill-buffer buffer))))

;; ---------------------------------------------------------------------------------------------

(ert-deftest frame-dsl-tests-extension-selects-the-mode ()
  "A .fdsl file gets this mode without anybody configuring it."
  (should (eq 'frame-dsl-mode
              (cdr (assoc "\\.fdsl\\'" auto-mode-alist))))
  (with-temp-buffer
    (setq buffer-file-name "/tmp/example.fdsl")
    (set-auto-mode)
    (should (eq major-mode 'frame-dsl-mode))
    (setq buffer-file-name nil)))

(ert-deftest frame-dsl-tests-each-kind-of-thing-looks-different ()
  "The six things a reader has to tell apart are told apart."
  (let ((sample (concat "@bit_order: msb_first;\n"
                        "\n"
                        "pkt TM_DATAGRAM_02 {\n"
                        "  some_header: 8 = 0xAA;   # a fixed value\n"
                        "  len:         32;\n"
                        "  data[128]:   valof(len) * 8;\n"
                        "};\n")))
    (should (eq (frame-dsl-tests--face-of sample "pkt ") 'font-lock-keyword-face))
    (should (eq (frame-dsl-tests--face-of sample "TM_DATAGRAM_02") 'font-lock-type-face))
    (should (eq (frame-dsl-tests--face-of sample "bit_order") 'font-lock-builtin-face))
    (should (eq (frame-dsl-tests--face-of sample "msb_first") 'font-lock-constant-face))
    (should (eq (frame-dsl-tests--face-of sample "some_header") 'font-lock-variable-name-face))
    (should (eq (frame-dsl-tests--face-of sample "0xAA") 'font-lock-constant-face))
    (should (eq (frame-dsl-tests--face-of sample "32;") 'font-lock-constant-face))
    (should (eq (frame-dsl-tests--face-of sample "valof") 'font-lock-keyword-face))
    ;; The `#' itself is the delimiter face, the text after it the comment face -- which is Emacs
    ;; being consistent with every other language, not this mode being clever.
    (should (eq (frame-dsl-tests--face-of sample "# a fixed")
                'font-lock-comment-delimiter-face))
    (should (eq (frame-dsl-tests--face-of sample "a fixed value") 'font-lock-comment-face))
    ;; The field a valof() names is a field, not a keyword argument.
    (should (eq (frame-dsl-tests--face-of sample "len)") 'font-lock-variable-name-face))
    ;; The reservation is not part of the name: it is the one number written in bytes.
    (should (eq (frame-dsl-tests--face-of sample "[128]") 'font-lock-preprocessor-face))))

(ert-deftest frame-dsl-tests-a-pasted-block-looks-like-a-block ()
  "A block pasted in without an instance name is coloured as the block it is."
  (let ((sample (concat "pkt TM_ACK {\n"
                        "  Header { id = 0x82; };\n"
                        "  vendor.Stamp;\n"
                        "  block {\n"
                        "    x: 8;\n"
                        "  };\n"
                        "};\n")))
    (should (eq (frame-dsl-tests--face-of sample "Header") 'font-lock-type-face))
    (should (eq (frame-dsl-tests--face-of sample "vendor.Stamp") 'font-lock-type-face))
    ;; The attribute scope stays a keyword: it is followed by `{' too.
    (should (eq (frame-dsl-tests--face-of sample "block") 'font-lock-keyword-face))))

(ert-deftest frame-dsl-tests-an-unknown-attribute-looks-wrong ()
  "A typo in an attribute name is visible before the file is compiled."
  (should (eq (frame-dsl-tests--face-of "@byte_ordre: big_endian;\n" "byte_ordre")
              'font-lock-warning-face)))

(ert-deftest frame-dsl-tests-a-comment-stays-a-comment ()
  "A keyword inside a comment is comment text, whatever it spells."
  (let ((sample "# pkt TM { valof(len) 0xAA };\npkt A { x: 8 = 1; };\n"))
    (should (eq (frame-dsl-tests--face-of sample "pkt TM") 'font-lock-comment-face))
    (should (eq (frame-dsl-tests--face-of sample "valof") 'font-lock-comment-face))
    (should (eq (frame-dsl-tests--face-of sample "0xAA") 'font-lock-comment-face))
    ;; ...and the code after it is still code.
    (should (eq (frame-dsl-tests--face-of sample "pkt A") 'font-lock-keyword-face))))

(ert-deftest frame-dsl-tests-comment-commands-use-hash ()
  "M-; comments with the only comment syntax the language has."
  (with-temp-buffer
    (insert "len: 32;\n")
    (frame-dsl-mode)
    (comment-region (point-min) (point-max))
    (should (string-prefix-p "#" (buffer-string)))
    (uncomment-region (point-min) (point-max))
    (should (string= (buffer-string) "len: 32;\n"))))

(ert-deftest frame-dsl-tests-indentation-matches-the-examples ()
  "Reindenting from flush left produces what examples/ is written at."
  (let ((flat (concat "@bit_order: msb_first;\n"
                      "[ @byte_order: little_endian; ]\n"
                      "pkt TM_MIXED {\n"
                      "marker: 16 = 0xCAFE;\n"
                      "[\n"
                      "@bit_order: msb_first;\n"
                      "@byte_order: little_endian;\n"
                      "]\n"
                      "block {\n"
                      "voltage: 32;\n"
                      "};\n"
                      "crc: 16;\n"
                      "};\n"))
        (want (concat "@bit_order: msb_first;\n"
                      "[ @byte_order: little_endian; ]\n"
                      "pkt TM_MIXED {\n"
                      "  marker: 16 = 0xCAFE;\n"
                      "  [\n"
                      "    @bit_order: msb_first;\n"
                      "    @byte_order: little_endian;\n"
                      "  ]\n"
                      "  block {\n"
                      "    voltage: 32;\n"
                      "  };\n"
                      "  crc: 16;\n"
                      "};\n")))
    (with-temp-buffer
      (insert flat)
      (frame-dsl-mode)
      (frame-dsl-tests--indent-buffer)
      (should (string= (buffer-string) want)))))

(ert-deftest frame-dsl-tests-every-example-fontifies ()
  "Every file the project ships goes through the mode without raising.

The point is not the colours, it is that nothing here errors on real input -- a
regular expression that backtracks forever or a syntax table that unbalances a
file is the way this mode breaks, and it breaks on somebody's buffer."
  (let ((examples (expand-file-name "../examples" frame-dsl-tests-directory)))
    (should (file-directory-p examples))
    (let ((files (directory-files-recursively examples "\\.fdsl\\'")))
      (should (> (length files) 4))
      (dolist (file files)
        (let ((buffer (find-file-noselect file)))
          (unwind-protect
              (with-current-buffer buffer
                (should (eq major-mode 'frame-dsl-mode))
                (font-lock-ensure)
                ;; Reindenting a file that is already correctly indented must change nothing.
                ;; Every valid example is; the invalid ones are invalid to the *checker*, not to
                ;; the layout, so they are held to the same standard.
                (let ((before (buffer-string)))
                  (frame-dsl-tests--indent-buffer)
                  (should (string= before (buffer-string)))))
            (kill-buffer buffer)))))))

;; ---------------------------------------------------------------------------------------------
;; Flymake.  These run the real `frame-dsl', which `make test-editors' points `frame-dsl-program' at;
;; by hand without it on the PATH they skip rather than fail.

(defun frame-dsl-tests--flymake (directory file text)
  "Check TEXT as FILE inside DIRECTORY with the Flymake backend; return its diagnostics."
  (let ((buffer (generate-new-buffer " *frame-dsl-flymake-test*"))
        (reported 'none))
    (unwind-protect
        (with-current-buffer buffer
          (insert text)
          (setq buffer-file-name (expand-file-name file directory))
          (let ((frame-dsl-flymake nil))
            (frame-dsl-mode))
          (frame-dsl-flymake (lambda (diagnostics &rest _) (setq reported diagnostics)))
          (with-timeout (10 (error "No report from frame-dsl-flymake"))
            (while (eq reported 'none)
              (accept-process-output nil 0.05)))
          (mapcar (lambda (diagnostic)
                    (list (line-number-at-pos (flymake-diagnostic-beg diagnostic))
                          (buffer-substring (flymake-diagnostic-beg diagnostic)
                                            (flymake-diagnostic-end diagnostic))
                          (flymake-diagnostic-type diagnostic)
                          (flymake-diagnostic-text diagnostic)))
                  reported))
      (with-current-buffer buffer (set-buffer-modified-p nil) (setq buffer-file-name nil))
      (kill-buffer buffer))))

(ert-deftest frame-dsl-tests-flymake-reports-an-unsaved-error ()
  "The buffer as typed is checked, not the file on disk, which need not exist."
  (skip-unless (executable-find frame-dsl-program))
  (let* ((directory (make-temp-file "frame-dsl-flymake" t))
         (diagnostics (frame-dsl-tests--flymake
                       directory "unsaved.fdsl"
                       "pkt TM {\n  id: 8 = 1;\n  h: Heder;\n};\n")))
    (unwind-protect
        (progn
          (should (= 1 (length diagnostics)))
          (should (equal 3 (nth 0 (car diagnostics))))
          (should (equal "h" (nth 1 (car diagnostics))))
          (should (eq :error (nth 2 (car diagnostics))))
          (should (string-match-p "Heder.*\\[check 23\\]" (nth 3 (car diagnostics)))))
      (delete-directory directory t))))

(ert-deftest frame-dsl-tests-flymake-is-quiet-on-a-clean-buffer ()
  "Every example checks clean."
  (skip-unless (executable-find frame-dsl-program))
  (let ((examples (expand-file-name "../examples" frame-dsl-tests-directory)))
    (dolist (file (directory-files examples t "\\.fdsl\\'"))
      (should (equal (list file nil)
                     (list file (frame-dsl-tests--flymake
                                 examples (file-name-nondirectory file)
                                 (with-temp-buffer
                                   (insert-file-contents file)
                                   (buffer-string)))))))))

(ert-deftest frame-dsl-tests-flymake-puts-an-imported-error-on-its-import ()
  "A mistake in another file is shown on the line that imports it, naming where it is."
  (skip-unless (executable-find frame-dsl-program))
  (let ((directory (make-temp-file "frame-dsl-flymake" t)))
    (unwind-protect
        (progn
          (make-directory (expand-file-name "lib" directory))
          (with-temp-file (expand-file-name "lib/link.fdsl" directory)
            (insert "block Header {\n  id: 8 = 1\n};\n"))
          (let ((diagnostics (frame-dsl-tests--flymake
                              directory "top.fdsl"
                              "# header\nimport \"lib/link.fdsl\";\npkt TM {\n  h: Header;\n};\n")))
            (should (= 1 (length diagnostics)))
            (should (equal 2 (nth 0 (car diagnostics))))
            (should (string-prefix-p "in lib/link.fdsl:3:1: " (nth 3 (car diagnostics))))))
      (delete-directory directory t))))

;; ---------------------------------------------------------------------------------------------
;; Formatting.

(ert-deftest frame-dsl-tests-format-buffer-rewrites-and-keeps-point ()
  "The buffer comes back in the tool's layout, with point on the same text."
  (skip-unless (executable-find frame-dsl-program))
  (with-temp-buffer
    (insert "pkt TM {\n\tid:8=1;\n  sequence :16;\n};\n")
    (let ((frame-dsl-flymake nil))
      (frame-dsl-mode))
    (goto-char (point-min))
    (search-forward "sequence")
    (frame-dsl-format-buffer)
    (should (equal (buffer-string) "pkt TM {\n  id:       8 = 1;\n  sequence: 16;\n};\n"))
    (should (looking-back "sequence" (line-beginning-position)))))

(ert-deftest frame-dsl-tests-format-buffer-leaves-a-broken-buffer-alone ()
  "A buffer that does not parse is not touched, and the error says why."
  (skip-unless (executable-find frame-dsl-program))
  (with-temp-buffer
    (insert "pkt TM {\n  id: 8\n};\n")
    (let ((frame-dsl-flymake nil))
      (frame-dsl-mode))
    (let ((error (should-error (frame-dsl-format-buffer) :type 'user-error)))
      (should (string-match-p "expected ';'" (cadr error))))
    (should (equal (buffer-string) "pkt TM {\n  id: 8\n};\n"))))

;; ---------------------------------------------------------------------------------------------
;; The language server, through eglot -- the client this mode is written against.

(ert-deftest frame-dsl-tests-eglot-knows-how-to-start-the-server ()
  "Eglot is told what to run, and it is whatever `frame-dsl-program' names."
  (skip-unless (require 'eglot nil t))
  (let ((entry (assq 'frame-dsl-mode eglot-server-programs)))
    (should entry)
    (let ((frame-dsl-program "/somewhere/frame-dsl"))
      (should (equal (funcall (cdr entry)) '("/somewhere/frame-dsl" "--lsp"))))))

(ert-deftest frame-dsl-tests-eglot-reports-the-compilers-diagnostics ()
  "A buffer managed by eglot shows what the server says about it."
  (skip-unless (and (require 'eglot nil t) (executable-find frame-dsl-program)))
  (let* ((directory (make-temp-file "frame-dsl-eglot" t))
         (file (expand-file-name "broken.fdsl" directory)))
    (unwind-protect
        (progn
          (with-temp-file file (insert "pkt TM {\n  id: 8\n};\n"))
          (let ((buffer (find-file-noselect file)))
            (unwind-protect
                (with-current-buffer buffer
                  (let ((frame-dsl-flymake nil))
                    (frame-dsl-mode))
                  ;; `eglot-ensure' waits for `post-command-hook', which never runs in batch; this is
                  ;; what it would have called.
                  (apply #'eglot (eglot--guess-contact))
                  ;; Eglot keeps what the server published and hands it to Flymake when Flymake asks;
                  ;; in batch nothing asks on its own.
                  (flymake-mode 1)
                  (with-timeout (20 (error "The server never reported anything"))
                    (while (null (flymake-diagnostics))
                      (accept-process-output nil 0.05)
                      (flymake-start)))
                  (let ((diagnostic (car (flymake-diagnostics))))
                    (should (string-match-p "expected ';'" (flymake-diagnostic-text diagnostic)))
                    ;; Eglot's own severity, which Flymake maps to the error face.
                    (should (memq (flymake-diagnostic-type diagnostic) '(:error eglot-error)))))
              (with-current-buffer buffer
                (when (and (fboundp 'eglot-current-server) (eglot-current-server))
                  (eglot-shutdown (eglot-current-server))))
              (kill-buffer buffer))))
      (delete-directory directory t))))

(provide 'frame-dsl-mode-tests)

;;; frame-dsl-mode-tests.el ends here
