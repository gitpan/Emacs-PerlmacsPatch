;; perl.el -- interactive for Perl embedded in Emacs
;; Copyright (C) 1998,1999 by John Tobey.  All rights reserved.

;; This file is NOT a part of GNU Emacs and is NOT distributed by
;; the Free Software Foundation.
;; However, this file may be used, distributed, and modified under
;; the same terms as GNU Emacs.  See the file COPYING for details.

;; This software is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.


(require 'perl-core)

(defvar perl-interpreter-args nil
  "Default command line arguments for initializing a Perl interpreter.
This should be a list of strings, not including the program invocation
name.  \"-e0\" will be appended to the list to ensure that the
interpreter does not attempt to read its script from the standard input.
See `make-perl-interpreter'.")

;; This gets called from C the first time a Perl function is called.
(defun make-perl-interpreter (&rest cmdline)
  "Create and return a new Perl interpreter object.
If arguments are given, they will be parsed as the interpreter's command
line, with the first argument used as the invocation name.  Otherwise,
the Perl command line will be (cons (car command-line-args)
(append perl-interpreter-args '(\"-e0\"))).

It is important to include a script name or \"-e\" option when running
interactively, because otherwise Perl tries to read its script from the
standard input.

NOTE: Using multiple simultaneous Perl interpreters currently is
not supported."
;;; AARGH! what's wrong with highlighting and indentation here? "
  (prog1
      (apply 'primitive-make-perl
	     (or cmdline
		 (cons (car command-line-args)  ; propagate argv[0]
		       (append perl-interpreter-args '("-e0")))))
    ;; Alas, this hook isn't called in batch mode.
    (add-hook 'kill-emacs-hook 'perl-destruct)))

;; Do some dynamic interface generation, because it makes the code cleaner.
;; Maybe should go in an `eval-when-compile'?
(mapcar
 (lambda (type)
   (eval (list 'defun
	       (intern (concat "perl-" type "-p"))
	       '(object)
	       (concat "Return t if OBJECT is a Perl " type ".")
	       (list 'eq
		     '(type-of object)
		     (list 'quote
			   (intern (concat "perl-" type)))))))
 '("interpreter"
   "scalar"
   "array"
   "hash"
   "code"
   "glob"))

;(defun perl-interpreter-p (object)
;  "Return t if OBJECT is a Perl interpreter."
;  (eq (type-of object) 'perl-interpreter))

(defun perl-eval-expression (expression &optional prefix)
  "Evaluate EXPRESSION as Perl code and print its value in the minibuffer.
With prefix arg, evaluate in list context."
  (interactive (list (read-from-minibuffer "Eval Perl: ")
		     current-prefix-arg))
  (message (prin1-to-string
	    (perl-eval expression
		       (if prefix 'list-context 'scalar-context)))))

(defun perl-eval-region (start end)
  "Execute the region as Perl code."
  (interactive "r")
  (perl-eval (buffer-substring start end)))

(defun perl-eval-buffer ()
  "Execute the current buffer as Perl code."
  (interactive)
  (perl-eval (buffer-string)))

(defun perl-load-file (filename)
  "Apply Perl's `require' operator to FILENAME."
  (interactive "FLoad Perl file: ")
  (perl-eval-and-call "sub {require $_[0]}" (expand-file-name filename)))

(provide 'perl)
