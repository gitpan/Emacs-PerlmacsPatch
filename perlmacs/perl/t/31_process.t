# -*-cperl-*-

BEGIN {
    $^W = 1;
    $| = 1;
}
use Emacs::Lisp;
use Emacs;

sub test_a {
    $ENV{var} = "";
    &Emacs::Lisp::setenv("var", 'yoohoo!');
    $ENV{var} eq 'yoohoo!';
}

sub test_b {
    save_excursion {
	&set_buffer (&get_buffer_create ("test"));
	$ENV{TEST} = 'yowsa';
	&call_process($^X, undef, t, undef,
		      '--perl', '-e', 'print $ENV{TEST}');
	&buffer_string() eq 'yowsa';
    };
}

foreach (sort keys %main::) {
    next unless /^test_/;
    push @tests, \&$_ if defined &$_;
}
print "1..", scalar @tests, "\n";

$test_number = 1;
for my $test (@tests) {
  print (&$test() ? "ok $test_number\n" : "not ok $test_number\n");
  $test_number ++;
}
