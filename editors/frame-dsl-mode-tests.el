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
      (indent-region (point-min) (point-max))
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
                  (indent-region (point-min) (point-max))
                  (should (string= before (buffer-string)))))
            (kill-buffer buffer)))))))

(provide 'frame-dsl-mode-tests)

;;; frame-dsl-mode-tests.el ends here
