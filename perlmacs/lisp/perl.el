;; perl.el -- utilities for Perl embedded in Emacs
;; Copyright (C) 1998,1999 by John Tobey.  All rights reserved.

;; This file is NOT a part of GNU Emacs and is NOT distributed by
;; the Free Software Foundation.
;; However, this file may be used, distributed, and modified under
;; the same terms as GNU Emacs.  See the file COPYING for details.

;; This software is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.


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
;;; " AARGH! what's wrong with highlighting and indentation here?
  (prog1
      (apply 'primitive-make-perl
	     (or cmdline
		 (cons (car command-line-args)  ; propagate argv[0]
		       (append perl-interpreter-args '("-e0")))))
    ;; Alas, this hook isn't called in batch mode.
    (add-hook 'kill-emacs-hook 'perl-destruct)))

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

;;; rough and ready substitute for C-u M-| perl -pe 's/foo/bar/g'
(defun perl-replace-regexp (pattern replacement prefix)
  "Replace things after point matching PATTERN (a Perl regular expression)
with REPLACEMENT.  If given a numeric prefix, REPLACEMENT is a Perl
expression to be evaluated."
  (interactive (query-replace-read-args "Replace Perl regexp" t))
  ;; Compile the Perl code once.
  (or (boundp 'perl-replace-regexp-code)
      (setq perl-replace-regexp-code
	    (perl-eval "sub {
			  my ($string, $patt, $repl, $e_flag) = @_;
			  my $num;
			  if ($e_flag) {
			    $num = $string =~ s/$patt/$repl/eeg;
			  } else {

			    # Perl's quoting rules are too hellish
			    # for me.  This works some of the time.

			    # \t becomes a tab, etc.
			    $repl =~ s|([\\@\\$])|\\\\$1|g;
			    $repl = eval qq{qq\\@$repl\\@};

			    $repl =~ s|([{}\\\\])|\\\\$1|g;
			    $repl = eval qq{sub { qq{$repl}}};
			    $num = $string =~ s/$patt/&$repl/eg;
			  }
			  return ($string, $num);
			}")))
  (push-mark)
  (let ((vals (funcall perl-replace-regexp-code
		       'list-context
		       (buffer-substring (point) (point-max))
		       pattern
		       replacement
		       (and prefix 1))))
    (delete-region (point) (point-max))
    (insert (car vals))
    (message "Replaced %d occurrence%s"
	     (cadr vals)
	     (if (= (cadr vals) 1) "" "s"))))
