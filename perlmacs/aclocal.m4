dnl Autoconf support for embedded Perl.
dnl Copyright (C) 1998 by John Tobey.  All rights reserved.
dnl This file is NOT part of GNU Emacs and is NOT distributed by
dnl the Free Software Foundation.  However, this file may be used
dnl and modified under the same terms as GNU Emacs.

dnl Find out whether the perl program is available.
dnl If it is, $PERL will contain its name, otherwise $PERL will be empty.
dnl If perl works, $ac_cv_prog_perl_version will hold its version number.
AC_DEFUN(AC_PROG_PERL,
[AC_BEFORE([$0], AC_EMBED_PERL)dnl
AC_BEFORE([$0], AC_PROG_PERL_XS)dnl
AC_BEFORE([$0], AC_PROG_XSUBPP)dnl
AC_CHECK_PROG(PERL, perl, perl)
if test -n "$PERL"; then
  AC_PROG_PERL_VERSION
fi
])

dnl Internal macro used by AC_PROG_PERL.
AC_DEFUN(AC_PROG_PERL_VERSION,
[AC_CACHE_CHECK([whether ${PERL-perl} works and knows its version number],
ac_cv_prog_perl_version,
[ac_cv_prog_perl_version=no
# Make perl print its version number.
changequote(, )dnl
ac_l_bracket="[" ac_r_bracket="]"  # trying to outsmart Autoconf here.
changequote([, ])dnl
if AC_TRY_COMMAND([${PERL-perl} -e "print \$$ac_r_bracket" > conftest]); then
  ac_cv_prog_perl_version=`cat conftest`
fi
rm -f conftest
])])

dnl Check whether the Perl interpreter can be embedded in programs using
dnl the method described in perlembed(1).
dnl If Perl can be embedded, $ac_embed_perl will be "yes", and the values
dnl of $PERL_LDOPTS and $PERL_CCOPTS will be set as appropriate.
dnl Otherwise, $ac_embed_perl will be either "no" or "disabled", and
dnl $PERL_LDOPTS and $PERL_CCOPTS will be empty.
AC_DEFUN(AC_EMBED_PERL,
[AC_MSG_CHECKING(for embeddable Perl)
dnl I don't know if I should be like AC_PATH_X here and run
dnl AC_ARG_ENABLE(perl,...) ..... maybe best left to configure.in .
if test "x$enable_perl" = xno; then
  # The user explicitly disabled Perl.
  ac_embed_perl=disabled
else
  AC_CACHE_VAL(ac_cv_embed_perl,
    [PERL_CCOPTS=`${PERL-perl} -MExtUtils::Embed -e ccopts 2>/dev/null`
    PERL_LDOPTS=`${PERL-perl} -MExtUtils::Embed -e ldopts 2>/dev/null`
    ac_save_CFLAGS="$CFLAGS"
    CFLAGS="$PERL_CCOPTS"
    ac_save_LIBS="$LIBS"
    LIBS="$PERL_LDOPTS"
    AC_TRY_LINK([#define main bad2proto
#include <EXTERN.h>
#include <perl.h>
#undef main],
      [PerlInterpreter *i = perl_alloc()],
dnl FIXME: Maybe should rework this, in case ccopts or ldopts contain
dnl problematic characters for quoting.
      [ac_cv_embed_perl="ac_embed_perl=yes \
			 PERL_CCOPTS='$PERL_CCOPTS' \
			 PERL_LDOPTS='$PERL_LDOPTS'"],
      [ac_cv_embed_perl="ac_embed_perl=no PERL_CCOPTS= PERL_LDOPTS="])
    CFLAGS="$ac_save_CFLAGS"
    LIBS="$ac_save_LIBS"
  ])
  eval "$ac_cv_embed_perl"
fi
AC_MSG_RESULT($ac_embed_perl)
])
