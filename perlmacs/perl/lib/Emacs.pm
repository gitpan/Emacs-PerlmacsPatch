# Emacs.pm - redefine Perl's system primitives to work inside of Emacs.

package Emacs;

use Emacs::Lisp ();
use Carp ();
use Tie::Handle;

use strict;
use vars qw ( $VERSION @ISA );


$VERSION = '0.11';

sub import {
    my $callpkg = caller;
    tie (*STDIN, 'Emacs::Minibuffer');
    tie (*STDOUT, 'Emacs::Stream', \*::standard_output);
    tie (*STDERR, 'Emacs::Minibuffer');
    $SIG{__WARN__} = \&do_warn;
    tie (%ENV, 'Emacs::ENV');
    tie (%SIG, 'Emacs::SIG');

    no strict 'refs';
    *{"$callpkg\::main"} = \&main;
    # XXX Also need to redefine `open' to use Emacs locking.
    # And how about `select'?
}

sub main {
    package main;
    if (@_) {
	return Emacs::_main (@_);
    } else {
	return Emacs::_main ($0, @ARGV);
    }
}

sub do_warn {
    my $msg = shift;
    chomp $msg;
    print STDERR $msg;
}


package Emacs::ENV;

sub TIEHASH	{ return bless {}, $_[0]; }
sub FETCH	{ return &Emacs::Lisp::getenv ($_[1]); }
sub STORE	{ &Emacs::Lisp::setenv ($_[1], $_[2]); }

# XXX Need to write tests for these.

sub DELETE	{ &Emacs::Lisp::setenv ($_[1], undef); }
sub EXISTS	{ return defined &FETCH; }

sub FIRSTKEY {
    my ($pe, $str);

    $pe = Emacs::Lisp::Object::symbol_value (\*::process_environment);
    return undef if $pe->is_nil;
    $str = $pe->car->to_perl;
    $str =~ s/=.*//s;
    return $str;
}

sub NEXTKEY {
    my ($self, $lastkey) = @_;
    my ($tail, $str);

    for ($tail = Emacs::Lisp::Object::symbol_value (\*::process_environment);
	 not $tail->is_nil;
	 $tail = $tail->cdr)
    {
	if ($tail->car->to_perl =~ /^\Q$lastkey\E=/s) {
	    $tail = $tail->cdr;
	    return undef if $tail->is_nil;
	    $str = $tail->car->to_perl;
	    $str =~ s/=.*//s;
	    return $str;
	}
    }
    return undef;
}

sub CLEAR { &Emacs::Lisp::set (\*::process_environment, undef); }


package Emacs::Stream;

use vars ('@ISA');
@ISA = ('Tie::Handle');

sub TIEHANDLE { return bless $_[1], $_[0]; }

sub WRITE {
    my ($stream, $output, $length, $offset) = @_;
    Emacs::Lisp::princ (substr ($output, $offset, $length),
			Emacs::Lisp::symbol_value ($stream));
}

sub PRINT {
    my ($stream, @items) = @_;
    Emacs::Lisp::princ (join ('', @items),
			Emacs::Lisp::symbol_value ($stream));
}


package Emacs::Minibuffer;

use vars ('@ISA');
@ISA = ('Tie::Handle');

sub TIEHANDLE { return bless [], $_[0]; }

sub WRITE {
    my ($stream, $output, $length, $offset) = @_;
    Emacs::Lisp::message (substr ($output, $offset, $length));
}

sub READ {
    die ("read() from STDIN is not implemented in Perlmacs");
}

sub READLINE {
    return Emacs::Lisp::read_string ("Enter input: ");
}


package Emacs::SIG;

sub TIEHASH { return bless [], $_[0]; }

# XXX Should map USR1 and USR2 to the Emacs 20.5 support.
sub signal_unsettable {
    return $_[0] !~ /_/;
}

sub FETCH {
    my ($self, $sig) = @_;

    return 'EMACS' if signal_unsettable ($sig);
    { local $^W = 0; untie (%SIG); }
    my $handler = $SIG{$sig};
    tie (%SIG, 'Emacs::SIG');
    return $handler;
}

sub STORE {
    my ($self, $sig, $handler) = @_;

    if (signal_unsettable ($sig)) {
	return if $handler eq 'EMACS';
	die ("Can't set signals under Emacs");
    }
    { local $^W = 0; untie (%SIG); }
    $SIG{$sig} = $handler;
    tie (%SIG, 'Emacs::SIG');
    return $handler;
}

sub DELETE {
    my ($self, $sig) = @_;

    die ("Can't set signals under Emacs") if signal_unsettable ($sig);
    { local $^W = 0; untie (%SIG); }
    my $handler = delete $SIG{$sig};
    tie (%SIG, 'Emacs::SIG');
    return $handler;
}

sub EXISTS {
    my ($self, $sig) = @_;

    { local $^W = 0; untie (%SIG); }
    my $ret = exists $SIG{$sig};
    tie (%SIG, 'Emacs::SIG');
    return $ret;
}

sub CLEAR {}

sub FIRSTKEY {
    die "Can't iterate signals under Emacs";
}

sub NEXTKEY {
    die "Can't iterate signals under Emacs";
}

1;
__END__


=head1 NAME

Emacs - Support for Perl embedded in GNU Emacs

=head1 SYNOPSIS

    perlmacs -w -MEmacs -e main -- --display :0.0 file.txt

    #! /usr/bin/perlmacs
    use Emacs;
    use Emacs::Lisp;
    setq { $mail_self_blind = t; };
    exit main ($0, "-q", @ARGV);


=head1 DESCRIPTION

This module replaces C<STDIN>, C<STDOUT>, C<STDERR>, C<%ENV>, and
C<%SIG> with versions that work safely within an Emacs session.  It
also defines a function named I<main>, which launches an Emacs editing
session from within a script.

=head2 main (CMDLINE)

When you C<use Emacs> in a B<perlmacs> script, a Perl sub named
C<main> may be used to invoke the Emacs editor.  This makes it
possible to put customization code, which would normally appear as
Lisp in F<~/.emacs>, into a Perl script.  For example, this startup
code

    (setq
     user-mail-address "gnaeus@perl.moc"
     mail-self-blind t
     mail-yank-prefix "> "
     )

    (put 'eval-expression 'disabled nil)

    (global-font-lock-mode 1 t)
    (set-face-background 'highlight "maroon")
    (set-face-background 'region "Sienna")

could be placed in a file with the following contents:

    #! /usr/local/bin/perlmacs

    use Emacs;
    use Emacs::Lisp;

    setq {
	$user_mail_address = 'gnaeus@perl.moc';
	$mail_self_blind = t;
	$mail_yank_prefix = '> ';
	$eval_expression{\*disabled} = undef;
    };

    &global_font_lock_mode(1, t);
    &set_face_background(\*highlight, "maroon");
    &set_face_background(\*region, "Sienna");

    exit main($0, "-q", @ARGV);

When you wanted to run Emacs, you would invoke this program.

The arguments to C<main> correspond to the C<argv> of the C<main>
function in a C program.  The first argument should be the program's
invocation name, as in this example.  B<-q> inhibits running
F<~/.emacs> (which is the point, after all).

=head2 STDIN

Reading a line from Perl's C<STDIN> filehandle causes a string to be
read from the minibuffer with the prompt C<"Enter input: ">.  To show
a different prompt, use:

    use Emacs::Lisp;
    $string = &read_string ("Prompt: ");

=head2 STDOUT

Printing to Perl's C<STDOUT> filehandle inserts text into the current
buffer as though typed, unless you have changed the Lisp variable
C<standard-output> to do something different.

=head2 STDERR and `warn'

Perl's C<warn> operator and C<STDERR> filehandle are redirected to the
minibuffer.

=head2 %ENV

Access to C<%ENV> is redirected to the Lisp variable
C<process-environment>.

=head2 %SIG

Setting signal handlers is not currently permitted under Emacs.


=head1 BUGS

=over 4

=item * Problems with `main'.

The C<main> sub may open an X display and not close it.  That is the
most obvious of many problems with C<main>.

The thing is, Emacs was not written with the expectation of being
embedded in another program, least of all a language interpreter such
as Perl.  Therefore, when Emacs is told to exit, it believes the
process is really about to exit, and it neglects to tidy up after
itself.

For best results, the value returned by C<main> should be passed to
Perl's C<exit> soon, as in this code:

    exit (main($0, @args));

=back


=head1 COPYRIGHT

Copyright (C) 1998,1999,2000 by John Tobey,
jtobey@john-edwin-tobey.org.  All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; see the file COPYING.  If not, write to the
  Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston,
  MA 02111-1307  USA


=head1 SEE ALSO

L<perl>, L<Emacs::Lisp>, B<emacs>.

=cut
