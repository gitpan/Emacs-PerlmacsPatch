;; perl-data.el -- Lisp accessors for Perl data types
;; Copyright (C) 1999 by John Tobey.  All rights reserved.

;; This file is NOT a part of GNU Emacs and is NOT distributed by
;; the Free Software Foundation.
;; However, this file may be used, distributed, and modified under
;; the same terms as GNU Emacs.  See the file COPYING for details.

;; This software is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

(require 'perl-core)

(perl-eval "sub Emacs::_runtime::true { $_[0] ? \\*::t : undef }")
(defun perl-true (scalar)
  (perl-call "Emacs::_runtime::true" 'scalar-context scalar))

(condition-case nil
    (perl-eval "require Data::Access;")
  ;; This list is not exhaustive.
  (perl-error (perl-eval "
use Tie::Scalar  ();
use Tie::Array   ();
use Tie::Hash    ();
use IO::Handle   ();
use Symbol       qw( qualify );

sub SCALAR::SCALAR	($)	  { \\${Symbol::qualify ($_[0], caller)} }
sub SCALAR::DEFINED     (\\$)     { defined ${$_[0]} }

sub ARRAY::ARRAY	($)	  { \\@{Symbol::qualify ($_[0], caller)} }
sub ARRAY::DEFINED      (\\@)     { defined @{$_[0]} }
sub ARRAY::SPLICE	(\\@;$$$) { my $a = shift; splice (@$a, @_) }

sub HASH::HASH		($)	  { \\%{$_[0]} }
sub HASH::DEFINED       (\\%)     { defined %{$_[0]} }

sub GLOB::GLOB		($)	  { \\*{Symbol::qualify ($_[0], caller)} }
sub GLOB::OPEN		(*;$)	  { open @_ }
sub GLOB::SYSOPEN	(*$$;$)	  { sysopen shift, shift, @_ }
sub GLOB::READLINE	(*)	  { readline $_[0] }

sub CODE::CODE		($)	  { \\&{Symbol::qualify ($_[0], caller)} }
sub CODE::PROTOTYPE	(\\&)	  { prototype $_[0] }
sub CODE::DEFINED       (\\%)     { defined &{$_[0]} }

for (qw(
	SCALAR::FETCH.....Tie::StdScalar::FETCH
	SCALAR::STORE.....Tie::StdScalar::STORE

	ARRAY::FETCHSIZE..Tie::StdArray::FETCHSIZE
	ARRAY::STORESIZE..Tie::StdArray::STORESIZE
	ARRAY::EXTEND.....Tie::StdArray::EXTEND
	ARRAY::STORE......Tie::StdArray::STORE
	ARRAY::FETCH......Tie::StdArray::FETCH
	ARRAY::CLEAR......Tie::StdArray::CLEAR
	ARRAY::POP........Tie::StdArray::POP
	ARRAY::PUSH.......Tie::StdArray::PUSH
	ARRAY::SHIFT......Tie::StdArray::SHIFT
	ARRAY::UNSHIFT....Tie::StdArray::UNSHIFT

	HASH::STORE.......Tie::StdHash::STORE
	HASH::FETCH.......Tie::StdHash::FETCH
	HASH::FIRSTKEY....Tie::StdHash::FIRSTKEY
	HASH::NEXTKEY.....Tie::StdHash::NEXTKEY
	HASH::EXISTS......Tie::StdHash::EXISTS
	HASH::DELETE......Tie::StdHash::DELETE
	HASH::CLEAR.......Tie::StdHash::CLEAR

	GLOB::PRINT.......IO::Handle::print
	GLOB::PRINTF......IO::Handle::printf
	GLOB::GETC........IO::Handle::getc
	GLOB::READ........IO::Handle::read
	GLOB::SYSREAD.....IO::Handle::sysread
	GLOB::WRITE.......IO::Handle::write
	GLOB::SYSWRITE....IO::Handle::syswrite
	GLOB::CLOSE.......IO::Handle::close
	GLOB::EOF.........IO::Handle::eof
	))
{
    my ($to, $from) = split /\\.+/;
    *$to = \\&$from;
}
")))

;;; Dynamically generate data accessors.
;;; E.g., (perl-scalar "x") returns the variable $x.
;;; (perl-scalar-fetch (perl-scalar "x")) returns its converted value.
;;; (perl-scalar-fetch-raw (perl-scalar "x")) returns its unconverted value.

(let ((lispify (function
		(lambda (to from conv)
		  (fset (intern (concat "perl-" to))
			(list 'lambda '(&rest args)
			      (list 'apply
				    (list 'quote conv)
				    from 'args)))))))
  (mapcar
   (lambda (meths)
     (let* ((type (car meths))
	    (lower (downcase type)))
       (funcall lispify lower (concat type "::" type) 'perl-call-raw)
       (mapcar
	(lambda (meth)
	  (let ((lisp (concat lower "-" (downcase meth)))
		(perl (concat type "::" meth)))
	    (funcall lispify lisp perl 'perl-call)
	    (funcall lispify (concat lisp "-raw") perl 'perl-call-raw)))
	(cdr meths))))

   '(("SCALAR" "DEFINED" "FETCH" "STORE")

     ("ARRAY" "DEFINED" "FETCHSIZE" "STORESIZE" "EXTEND" "STORE" "FETCH"
      "CLEAR" "POP" "PUSH" "SHIFT" "UNSHIFT" "SPLICE")

     ("HASH" "DEFINED" "STORE" "FETCH" "FIRSTKEY" "NEXTKEY" "EXISTS"
      "DELETE" "CLEAR")

     ("GLOB" "OPEN" "SYSOPEN" "READLINE" "PRINT" "PRINTF"
      "GETC" "READ" "SYSREAD" "WRITE" "SYSWRITE" "CLOSE" "EOF")

     ("CODE" "DEFINED" "PROTOTYPE"))))

(provide 'perl-data)
