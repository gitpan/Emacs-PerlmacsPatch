; # -*-emacs-lisp-*-
; BEGIN { exec $^X, qw(--emacs --batch --no-site-file -l), $0, "I", @INC }

;;; Tests of `perl-eval' and `perl-call'.
;;; (More needed.)

(setq tests
      '(
	(apply (perl-eval "sub { @INC = @_ }")
	       (cdr (member "I" command-line-args)))
	(perl-eval "use Emacs::Lisp; 1;")
	(and (eq 'bla
		 (catch 'oofda
		   (setq x 3)
		   (perl-eval "&throw (\\*oofda, \\*bla)")
		   (setq x 5)))
	     (eq x 3))
	(eq 'oofda
	    (catch 'bla
	      (perl-eval "&perl_eval (q(&throw (\\*bla, \\*oofda)))")))
	))

(setq standard-output t)
(princ (format "1..%d\n" (length tests)))
(setq test-number 1)
(mapcar
 (lambda (form)
   (princ
    (format "%sok %d\n"
	    (if (eval form)
		""
	      "not ")
	    test-number))
   (setq test-number (1+ test-number)))
 tests)

(garbage-collect)
(kill-emacs)
