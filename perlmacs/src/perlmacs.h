/* Passing data and execution control between Perl and Lisp.
   Copyright (c) 1998,1999,2000 by John Tobey.  All rights reserved.

This file is *NOT* part of GNU Emacs.
However, this file may be distributed and modified under the same
terms as GNU Emacs.

GNU Emacs is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs; see the file COPYING.  If not, write to
the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */


#ifndef _PERLMACS_H
#define _PERLMACS_H

#ifdef __cplusplus
extern "C" {
#endif

/* FIXME: I suspect we can do without struct perl_frame.  */
struct perl_frame
  {
    /* Link to next outer stack frame.  */
    struct lisp_frame *prev;

    /* Value of PL_top_env on frame entry and exit.  */
    JMPENV *old_top_env;

    /* Whether call_perl will happen in this frame.
       (call_perl entails some complexity.)  */
    char need_jmpenv;
  };

struct lisp_frame
  {
    /* Link to next outer stack frame.  */
    struct perl_frame *prev;

    /* Argument for JMPENV_JUMP.  */
    int jumpret;

    /* The Lisp error data during a Lisp exception.  */
    Lisp_Object lisp_error;
  };

struct emacs_perl_interpreter
  {
    PerlInterpreter *interp;

    /* The int returned by perl_parse() or perl_run().  Not gcpro'd.  */
    Lisp_Object status;

    /* One of Qparsing, Qparsed, Qrunning, Qran, Qdestructing,
       and Qdestructed.  Not gcpro'd.  */
    Lisp_Object phase;

    /* Doubly linked list of Lisp objects referenced in Perl (for GC).  */
    struct sv_lisp *protlist;

    /* Analogous to PL_start_env and PL_top_env (see scope.h).  */
    union either_frame
      {
	struct lisp_frame l;
	struct perl_frame p;
      } start;

    struct lisp_frame *frame_l;
    struct perl_frame *frame_p;

    /* The Emacs::Lisp::Object stash, cached for performance.
       No ref is held, and god forbid anyone should delete it!  */
    HV *elo_stash;

    /* Hack so that we know whether to return to perl_call_emacs_main
       when `kill-emacs' is called.  */
    char in_emacs_main;
  };

/* Perlmacs hook for when Emacs::Lisp is loaded.  */
extern void perlmacs_init _((ARGSproto));

/* Convert a Perl return value or argument to a Lisp function
   into a Lisp object.
   This is the default, deep-copying, user-friendly conversion.
   For special cases, use perlmacs_sv_as_lisp().  */
extern Lisp_Object perlmacs_sv_to_lisp _((SV *sv));

/* Same as above, only reversing "Perl" and "Lisp".  */
extern SV *perlmacs_lisp_to_sv _((Lisp_Object obj));

/* Wrap the argument in an object of the other language type.
   These functions support shallow copying.
   perlmacs_sv_as_lisp increments its argument's reference count.
   perlmacs_sv_as_lisp_noinc does not.  */
extern Lisp_Object perlmacs_sv_as_lisp _((SV *sv));
extern Lisp_Object perlmacs_sv_as_lisp_noinc _((SV *sv));
extern SV *perlmacs_lisp_as_sv _((Lisp_Object obj));

/* Enter a Lisp stack frame, either command-line-style or
   function-call-style.  */
extern int perlmacs_call_emacs_main _((int argc, char **argv, char **envp));
extern Lisp_Object perlmacs_funcall _((int nargs, Lisp_Object *args));

/* Node in a doubly-linked list of Perl references to Lisp objects.
   Each such reference holds a pointer to a struct sv_lisp.
   They are linked together for marking during gc.  */
struct sv_lisp
  {
    Lisp_Object obj;
    SV *sv;
    struct sv_lisp *prev;
    struct sv_lisp *next;
  };

/* Data access macros similar to those defined in lisp.h.
   Use sv_isa instead of sv_derived_from because there is no need
   to derive classes from Emacs::Lisp::Object, because Lisp objects
   know their type.  */
#define SV_LISPP(sv) Perl_sv_isa (sv, "Emacs::Lisp::Object")
#define XSV_LISP(sv) (((struct sv_lisp *) SvIVX (SvRV (sv)))->obj)

#define XPERL_INTERPRETER(obj)						\
   ((struct emacs_perl_interpreter *) (XFOREIGN_OBJECT (obj)->data))

/* Return the struct emacs_perl_interpreter that owns sv.  This only
   works for SVs that are Lisp objects.  */
#define XSV_INTERP(sv)							\
   XPERL_INTERPRETER (XFOREIGN_OBJECT (XSV_LISP (sv))->lisp_data)

#ifdef __cplusplus
}
#endif

#endif  /* _PERLMACS_H */
