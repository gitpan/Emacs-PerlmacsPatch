# -*-perl-*-
# Tests for behavior after nonlocal jumps.

BEGIN { $| = 1; $^W = 1 }
use Emacs::Lisp;

# Avoid warning "used only once":
*arith_error = *arith_error;

## This sort of thing apparently does not work under Perl.
#sub x::DESTROY { &error ("1234") }
#       sub {
#	 my ($x, $y);
#	 $x = bless \$y, 'x';
#	 eval { undef $x };
#	 ($@ =~ /^1234/, $@);
#       },

@tests =
    (
     sub {
	 eval { &error ("314159") };
	 ($@ =~ /^314159/, $@);
     },
    sub {
	eval q(&error ("26535"));
	($@ =~ /^26535/, $@);
    },
    sub {
	eval { &perl_eval (q(die 89793)) };
	($@ =~ /^89793 at /, $@);
    },
    sub {
	eval { &perl_eval (q(&perl_eval (q(die 83279)))) };
	($@ =~ /^83279 at /, $@);
    },
    sub {
	eval { &perl_eval (q(&perl_eval (q(&error ("50288"))))) };
	($@ =~ /^50288/, $@);
    },
    sub {
	use Emacs::Lisp qw($err $x);
	sub signalbla { &signal (\*arith_error, [26, 4, 33]) }
	# note: the following performs two deep copies.
	&eval (&read (q((condition-case var
			 (progn (setq x 8)
			  (perl-call "::signalbla")
			  (setq x 3))
			 (arith-error (setq err (cdr var)))))));
	$x == 8 && "@$err" eq "26 4 33";
    },
    sub {
	eval { &perl_call (sub { die 23846 }) };
	($@ =~ /^23846 at /, $@);
    },
    sub {
	eval { &perl_call (sub { &error ("41971") }) };
	return ($@ =~ /^41971/, $@);

	# FIXME: This works too. weird.
	# $@ appears empty, but the test still passes!
	return ($@ =~ /^419df71/, $@);
    },
    sub {
	69399 == catch \*arith_error, sub {
	    &throw (\*arith_error, 69399);
	};
    },
    sub {
	37510 == catch \*arith_error, sub {
	    &perl_eval (q(&throw (\*arith_error, 37510)));
	};
    },
    sub {
	eval { &perl_eval ("goto L;") };
	return ($@ =~ /label/, $@);
      L:
	# actually, it wouldn't be bad behavior to come here, but it's
	# not currently supported.
	#return (0, $@);
	return (1, $@);
    },
    sub {
	local $x = 0;
	local $^W = 0;
	eval { for (1..10) { &perl_eval (q($x = $_; last if $_>5)) } };
	($x == 6 && $@ =~ /last/, "$x $@");
    },

    # More tests needed:
    # - die from within condition-case handler
    # - throw/signal from within $SIG{__DIE__}
    # - throw/signal from within FETCH, DESTROY, sort, etc.
    # - uncaught errors (in a subprocess)
    # - during global destruction
    # - variations on existing tests
    );

print "1..".(@tests+2)."\n";
$test_number = 1;
sub report {
    my ($ok, $comment) = @_;
    print (($ok ? "" : "not "), "ok $test_number");
    if (defined $comment) {
	$comment =~ s/\s+/ /g;
	print " # $comment\n";
	} else {
	    print "\n";
	}
    $test_number ++;
}
for my $test (@tests) {
    report &$test();
}

# With perl 5.6.0, the argument of the first "die" in the program is
# printed here, followed by ".\n".  Is it me or perl?

# This "fixes" it.  Ugh!
close STDERR;

END {
    &garbage_collect;
    eval { &error ("58209") };
    report ($@ =~ /^58209/ ? 1 : 0, $@);
    &garbage_collect;
    eval {&perl_destruct};

    # FIXME:  Weird.  This works too, but $@ is reported as empty
    #report (($@ =~ /xxxtop-level/), $@);
    report (($@ =~ /top-level/), $@);
    &garbage_collect;
    $? = 0;
}
