/* eval.h -- 
   This file is NOT part of GNU Emacs, but its contents are taken
   from eval.c, which is.  */

/* Copyright (C) 1985, 1986, 1987, 1993, 1994, 1995 Free Software Foundation, Inc.
   Copyright (C) 1998,1999 John Tobey

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


/* This definition is duplicated in alloc.c and keyboard.c */
/* Putting it in lisp.h makes cc bomb out! */

struct backtrace
  {
    struct backtrace *next;
    Lisp_Object *function;
    Lisp_Object *args;	/* Points to vector of args. */
    int nargs;		/* Length of vector.
			   If nargs is UNEVALLED, args points to slot holding
			   list of unevalled args */
    char evalargs;
    /* Nonzero means call value of debugger when done with this operation. */
    char debug_on_exit;
  };

extern struct backtrace *backtrace_list;

/* This structure helps implement the `catch' and `throw' control
   structure.  A struct catchtag contains all the information needed
   to restore the state of the interpreter after a non-local jump.

   Handlers for error conditions (represented by `struct handler'
   structures) just point to a catch tag to do the cleanup required
   for their jumps.

   catchtag structures are chained together in the C calling stack;
   the `next' member points to the next outer catchtag.

   A call like (throw TAG VAL) searches for a catchtag whose `tag'
   member is TAG, and then unbinds to it.  The `val' member is used to
   hold VAL while the stack is unwound; `val' is returned as the value
   of the catch form.

   All the other members are concerned with restoring the interpreter
   state.  */
struct catchtag
  {
    Lisp_Object tag;
    Lisp_Object val;
    struct catchtag *next;
    struct gcpro *gcpro;
    jmp_buf jmp;
    struct backtrace *backlist;
    struct handler *handlerlist;
    int lisp_eval_depth;
    int pdlcount;
    int poll_suppress_count;
  };

extern struct catchtag *catchlist;
