; # -*-emacs-lisp-*-
; BEGIN { exec $^X, qw(--emacs --batch --no-site-file -l), $0, "I", @INC }

;;; Tests for `make-perl-interpreter' and related functions.

(setq INC (cdr (member "I" command-line-args)))
(setq -I (mapcar
	  (lambda (dir) (concat "-I" dir))
	  INC))

(defun reperl (&rest argv)
  (condition-case nil
      (perl-destruct)
    (error t))
  (setq perl-interpreter
   (apply 'make-perl-interpreter "perl"
	  (append (copy-sequence -I) argv))))

(setq tests
      '(
	(reperl "-MEmacs::Lisp" "-e0")
	(progn
	  (perl-eval-and-call "sub {$x=shift}" (current-buffer))
	  (and (eq (perl-eval "$x") (current-buffer))
	       (progn (condition-case nil (perl-destruct)
			(error t))
		      t)))
	(eq 42 (perl-eval-and-call "sub {$y = defined($x) ? 13 : 42}"))
	(let (msg)
	  (reperl "-MEmacs::Lisp" "-e0")
	  (condition-case err
	      (perl-eval-and-call "sub {die \"good\\n\" unless defined $y}")
	    (perl-error (setq msg (cadr err))))
	  (string-match "^good" msg))
	))

(setq standard-output t)
(princ (format "1..%d\n" (length tests)))
(setq test-number 1)
(mapcar
 (lambda (form)
   (princ (format "%sok %d\n"
		  (if (eval form) "" "not ")
		  test-number))
   (setq test-number (1+ test-number)))
 tests)

(garbage-collect)

;; Don't try to process the rest of the command line.
(kill-emacs)
