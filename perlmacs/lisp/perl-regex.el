;; perl-regex.el -- Search and replace functions using Perl regular expressions
;; Copyright (C) 1999 by John Tobey.  All rights reserved.

;; This file is NOT a part of GNU Emacs and is NOT distributed by
;; the Free Software Foundation.
;; However, this file may be used, distributed, and modified under
;; the same terms as GNU Emacs.  See the file COPYING for details.

;; This software is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.


(require 'perl)
(require 'perl-data)


;;; rough and ready substitute for M-> C-u M-| perl -pe 's/foo/bar/g'
(defun perl-replace-regexp (pattern replacement prefix)
  "Replace things after point matching PATTERN (a Perl regular expression)
with REPLACEMENT.  If given a numeric prefix, REPLACEMENT is a Perl
expression to be evaluated."
  (interactive (query-replace-read-args "Replace Perl regexp" t))
  (push-mark)
  (or (perl-true
       (perl-code-defined "Emacs::_runtime::replace_regexp"))
      (perl-eval "

sub Emacs::_runtime::replace_regexp {
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
}
"
		 ))
  (let* ((old (buffer-substring (point) (point-max)))
	 (vals
	  (perl-call "Emacs::_runtime::replace_regexp"
		     'list-context
		     old pattern replacement (and prefix 1))))

    (cond ((not (string-equal (car vals) old))
	   (delete-region (point) (point-max))
	   (insert (car vals))))

    (message "Replaced %d occurrence%s"
	     (cadr vals)
	     (if (= (cadr vals) 1) "" "s"))))

(provide 'perl-regex)
