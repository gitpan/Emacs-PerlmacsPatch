use ExtUtils::MakeMaker;
use strict;
use File::Copy;
use File::Find;

my ($perlmacs_ver, $emacs_ver, $emacs_src, $emacs_tar_gz, @configure_opts);
my $aborted = "\n### $0 aborted\n";
my $gzip_size_at_least = 14_000_000;
my ($pure_emacs_src);  # unpatched tree for making diffs

$| = 1;  # for when we ask questions

check_perl_ver (5.005);
find_perlmacs_ver ();  # finds perlmacs and emacs version #s in version.el

for my $i (0 .. $#ARGV) {

  if ($ARGV[$i] =~ /^-*h/) {
    die "\nUsage: perl Makefile.PL [EMACS_SRC=dir/emacs-$emacs_ver\[.tar.gz]] [MakeMaker-opts] [-- configure-opts]\n";
  }

  if ($ARGV[$i] =~ /^EMACS_SRC=(.+)/) {
    ($1 =~ /\.tar\.gz$/ ? $emacs_tar_gz : $emacs_src) = $1;
    splice @ARGV, $i, 1;
  }

  if ($ARGV[$i] =~ /^PURE_EMACS_SRC=(.+)/) {
    $pure_emacs_src = $1;
    splice @ARGV, $i, 1;
  }

  next unless $ARGV[$i] eq '--';
  @configure_opts = splice @ARGV, $i;
  shift @configure_opts;

  for (@configure_opts) {
    s|\$|\$\$|g;
    if (s|'|'\\''|g) { $_ = "'$_'" }
  }
  last;
}

grep { /^--p/ } @configure_opts
  or push @configure_opts, '--prefix=$(PREFIX)';

find_emacs_src () unless defined $emacs_src;
unpack_emacs_src () if defined $emacs_tar_gz;
prep_emacs_src ();

WriteMakefile (
	       VERSION => $perlmacs_ver,
	       NAME => 'Emacs::PerlmacsPatch',
	       dist => { DIST_DEFAULT => 'emacs-perlmacs.diff tardist' },
	       );

# Makefile must depend on version.el, but MakeMaker makes no provision
# for this, as far as I can tell.
# So bludgeon it into shape.
rename ("Makefile", "Makefile.tmp") or exit;
open MFT, "Makefile.tmp" or die $!;
open MF, ">Makefile" or die $!;
while (<MFT>) {
  s|^CONFIGDEP =|CONFIGDEP = perlmacs/lisp/version.el|;
  print MF or die $!;
}
unlink "Makefile.tmp";
exit;


sub MY::post_initialize {
  return "PURE_EMACS_SRC = "
    .($pure_emacs_src || "/src/emacs-$emacs_ver")."\n";
}

sub MY::top_targets {
  return <<END;

all : $emacs_src/build $emacs_src/build/Makefile
	cd $emacs_src/build && \$(MAKE) all

$emacs_src/build/Makefile : Makefile
	cd $emacs_src/build && PERL=\$(FULLPERL) ../configure @configure_opts

$emacs_src/build :
	mkdir \$\@
END
}

sub MY::test {
    # Something's wrong with the exit stati returned to Test::Harness.
    return <<END;

test : all
	cd $emacs_src/build/perl && HARNESS_IGNORE_EXITCODE=1 make test

testdb : all
	cd $emacs_src/build/perl && HARNESS_IGNORE_EXITCODE=1 make testdb
END
}

sub MY::install {
  return <<END;

install : all
	cd $emacs_src/build && \$(MAKE) install
END
}

sub MY::postamble {
  return <<END;

emacs-perlmacs.diff : $emacs_src $emacs_src/src $emacs_src/lisp \$(PURE_EMACS_SRC)
	test -d perlmacs-$perlmacs_ver || ln -s $emacs_src perlmacs-$perlmacs_ver
	test ! -f emacs-$emacs_ver/src/perlmacs.c
	test -d emacs-$emacs_ver || ln -s \$(PURE_EMACS_SRC) emacs-$emacs_ver
	diff -cr --exclude=build --exclude=\\*.elc --exclude=version.el \\
		--exclude=CVS --exclude=README\\* --exclude=BUGS \\
		emacs-$emacs_ver perlmacs-$perlmacs_ver \\
		> \$\@.tmp || test \$\$? = 1
	mv -f \$\@.tmp \$\@

clean ::
	-cd $emacs_src/build && \$(MAKE) clean
	\$(RM_F) perlmacs-* emacs-perlmacs.diff.tmp

distclean ::
	\$(RM_RF) $emacs_src/build
END
}

sub check_perl_ver {
    my ($wanted) = @_;
    return if $] >= $wanted;
    die <<DEAD;

Your Perl (version $]) is too old for this Perlmacs patch.
Please use Perl $wanted or higher.

### $0 aborted.
DEAD
}

sub analyze_version_file {
    my $verfile = shift;
    my ($p, $e);
    local (*VER);
    open VER, $verfile or die "\nCan't open $verfile: $!\n$aborted";
    map {
	$p = $1 if /defconst perlmacs-version "(.*?)"/;
	$e = $1 if /defconst emacs-version "(.*?)"/;
    } <VER>;
    return ($p, $e);
}

sub find_perlmacs_ver {
    my $verfile = "perlmacs/lisp/version.el";
    print "Finding Perlmacs version...";
    ($perlmacs_ver, $emacs_ver) = analyze_version_file ($verfile);
    defined ($perlmacs_ver) or
	die "\nCan't deduce Perlmacs version from contents of $verfile.\n$aborted";
    defined ($emacs_ver) or
	die "\nCan't deduce required Emacs version from contents of $verfile.\n$aborted";
    print "$perlmacs_ver\n";
}

sub find_emacs_src {
    my $gzipname = "emacs-$emacs_ver.tar.gz";
    my $dirname = "emacs-$emacs_ver";
    my $maybe_dir = "";
    my $def_url = "ftp://metalab.unc.edu/pub/gnu/emacs/$gzipname";
    my ($ans, $ua, $url, $request, $response);

    if (-f "perlmacs/ChangeLog") {
      $emacs_src = "perlmacs";
      return;
    }

    $emacs_src = $dirname;
    return if defined $emacs_tar_gz || -e $emacs_src;

    # prefer gzipped since it avoids asking the user whether to patch
    # the found tree.
    foreach (qw(. .. ../.. ../../..)) {
	next unless -f "$_/$gzipname"
	    && -s "$_/$gzipname" >= $gzip_size_at_least;
	($emacs_tar_gz = "$_/$gzipname") =~ s:^\./::;
	return;
    }
    foreach (qw(.. ../.. ../../..)) {
	next unless -d "$_/$dirname";
	$maybe_dir = "$_/$dirname";
	last;
    }
    print "\nWhere should I find the Emacs $emacs_ver source tree to patch?\n";
    print "Specify a tar.gz file, a directory, `net' to download now,\n";
    print "or `q' to give up.\n\n";

    while (1) {
	print "Emacs source location? [$maybe_dir] ";
	$ans = <STDIN>;
	chomp $ans;

	if ($ans eq '') {
	    next if $maybe_dir eq '';
	    $emacs_src = $maybe_dir;
	    return;
	}
	die $aborted if ("quit" =~ /^$ans/);
	if ($ans =~ /\.gz$/) {
	    if (-f $ans) {
		$emacs_tar_gz = $ans;
		return;
	    }
	    print "That is not a file.  Did you mistype the name?\n";
	    next;
	}
	last if $ans eq 'net';
	if (-d $ans) {
	    $emacs_src = $ans;
	    return;
	}
	print "\nPlease enter a filename ending in .gz or a directory name.\n";
    }

    # here they have asked for a download.
    print "\nNOTE: $gzipname is over "
	.int($gzip_size_at_least/1_000_000)." megabytes.\n";
    print "Do you really want me to try to download it? [y] ";
    die $aborted unless <STDIN> =~ /^($|[yY])/;

    require LWP::UserAgent;
    require URI;
    require URI::Heuristic;

    $ua = new LWP::UserAgent;
    $request = HTTP::Request->new ('GET');

    while (1) {
	print "Download $gzipname from where? [$def_url] ";
	$ans = <STDIN>;
	chomp $ans;

	$ans = $def_url if $ans eq '';
	die $aborted if ("quit" =~ /^$ans/);

	if (! $ans =~ /$gzipname$/) {
	    $ans =~ s:/$::;
	    $ans .= "/$gzipname";
	}

	# snarfed from lwp-request
	if ($ans =~ /^\w+:/) {  # is there any scheme specification
	    $url = URI->new($ans);
	} else {
	    $url = URI::Heuristic::uf_uri($ans);
        }
	$request->url($url);
	print "Attempting to fetch $ans\n(this will take a while)...";
	$response = $ua->request($request, $gzipname);
	if ($response->is_success && -s $gzipname >= $gzip_size_at_least) {
	    print "success!!\n";
	    $emacs_tar_gz = $gzipname;
	    return;
	}
	$def_url = 'q' if $ans eq $def_url;
	print "failed.\nType `q' if you want to give up.\n";
    }
}

sub unpack_emacs_src {
    print "Unpacking $emacs_tar_gz...";
    system ("gzip -dc $emacs_tar_gz |tar xf -")
	and die $aborted;
    print "done\n";
}

sub prep_emacs_src {
    -d $emacs_src or die "\n$emacs_src is not a directory.\n$aborted";
    print "Using source in $emacs_src.\n";
    my $verfile = "$emacs_src/lisp/version.el";
    my ($p, $e) = analyze_version_file ($verfile);
    defined ($e)
	or die "\nCan't find Emacs version number in $verfile.\n$aborted";
    $e eq $emacs_ver or die <<DEAD;

Emacs source version mismatch.
$emacs_src contains version $e.
This patch requires Emacs $emacs_ver.

### $0 aborted.
DEAD
    if (defined $p) {
	return if $p eq $perlmacs_ver;
	die <<DEAD;

$emacs_src contains Perlmacs version $p.
This patch for Perlmacs $perlmacs_ver must be applied to a fresh
Emacs $emacs_ver tree.

### $0 aborted.
DEAD
    }
    print "Patching $emacs_src...\n";
    my $cmd = "patch -p0";
    $cmd = "( cd $emacs_src; patch -p1 )" if $emacs_src ne "emacs-$emacs_ver";
    open PATCH, "|$cmd" or die "\nCan't run `$cmd'.\n$aborted";
    open DIFF, "emacs-perlmacs.diff"
	or die "\nCan't open emacs-perlmacs.diff: $!\n$aborted";
    print PATCH while read DIFF, $_, 4096;
    close PATCH;
    $? and die "\n`$cmd' returned status $?\n$aborted";

    for my $dir ('perl', 'perl/lib', 'perl/lib/Emacs', 'perl/t', 'src/xs') {
	print "mkdir $emacs_src/$dir\n";
	mkdir "$emacs_src/$dir", 0777;
    }

    my @files;
    find sub { push @files, $File::Find::name if -f }, "perlmacs";
    for (@files) {
	s|^perlmacs/||;
	if (-e "$emacs_src/$_" && ! -w "$emacs_src/$_") {
	    print "Removing $emacs_src/$_\n";
	    unlink "$emacs_src/$_"
		or die "Can't remove $emacs_src/$_: $!\n$aborted";
	}
	print "Copying perlmacs/$_ to $emacs_src/$_\n";
	copy "perlmacs/$_", "$emacs_src/$_"
	    or die "Can't copy file: $!\n$aborted";
    }
}
