/* Support for calling Perl from Lisp.
   Copyright (c) 1998,1999 by John Tobey.  All rights reserved.

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


/* FIXME:  Some Glibc ipc header needs this.  */
#if !(defined _SVID_SOURCE || defined _XOPEN_SOURCE)
#define _SVID_SOURCE
#endif

#include "config.h"  /* ...hoping this is Emacs' config, not Perl's! */
#include "lisp.h"
#include <paths.h>

#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>
#include "perlmacs.h"

#include <setjmp.h>
#include "eval.h"

/* Node in a doubly-linked list of Perl references to Lisp objects.
   Each such reference holds a pointer to a struct sv_lisp.
   They are linked together for marking during gc.  */
struct sv_lisp
  {
    Lisp_Object obj;		/* must be first */
    SV *prev;
    SV *next;
  };
#define SV_LISP_NEXT(sv) (((struct sv_lisp *) SvIVX (sv))->next)
#define SV_LISP_PREV(sv) (((struct sv_lisp *) SvIVX (sv))->prev)

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

struct perl_interpreter_object
  {
    PerlInterpreter *interp;

    /* The int returned by perl_parse() or perl_run().  Not gcpro'd.  */
    Lisp_Object status;

    /* One of Qparsing, Qparsed, Qrunning, Qran, Qdestructing, Qdestructed,
       and Qbad.  Not gcpro'd  */
    Lisp_Object phase;

    /* Doubly linked list of Lisp objects referenced in Perl (for GC).  */
    SV *protlist;

    /* Analogous to PL_start_env and PL_top_env (see scope.h).  */
    union either_frame
      {
	struct lisp_frame l;
	struct perl_frame p;
      } start;

    union either_framep
      {
	struct lisp_frame *l;
	struct perl_frame *p;
      } top;

    /* The Emacs::Lisp::Object stash, cached for performance.
       No ref is held, and god forbid anyone should delete it!  */
    HV *elo_stash;

    /* Hack so that we know whether to return to perl_call_emacs_main
       when `kill-emacs' is called.  */
    char in_emacs_main;
  };

struct perl_interpreter_object *top_level_perl, *current_perl;
EXFUN (Fperl_destruct, 1);
static Lisp_Object internal_perl_call P_ ((int nargs, Lisp_Object *args,
					   int do_eval,
					   Lisp_Object (*conv) P_ ((SV *sv))));
static void init_perl ();

#define INIT_PERL do { if (!current_perl) init_perl (); } while (0)

/* We will need a way to find the interpreter that owns a given SV.
   Currently, multiple interpreters are not supported, so just return
   the global interpreter.  */
#define PERLMACS_SV_INTERP(sv) current_perl

#define CHECK_PERL(obj)					\
  { if (! FOREIGN_OBJECTP (obj)				\
	|| XFOREIGN_OBJECT (obj)->vptr != &interp_vtbl)	\
      wrong_type_argument (Qperl_interpreter, obj); }
#define XPERL_INTERPRETER(obj)						\
   ((struct perl_interpreter_object *) (XFOREIGN_OBJECT (obj)->data))

/* How to tell when a Perl object's interpreter is no longer usable.  */
#define LISP_SV_INTERP(obj)					\
   (XPERL_INTERPRETER (XFOREIGN_OBJECT (obj)->lisp_data))
#define PERL_DEFUNCT_P(perl)			\
   (EQ ((perl)->phase, Qdestructed)		\
    || EQ ((perl)->phase, Qbad))

#define CACHE_ELO_STASH(perl)						\
   { if (! perl->elo_stash)						\
       perl->elo_stash = Perl_gv_stashpv ("Emacs::Lisp::Object", 1); }
#define BARE_SV_LISPP(sv)						 \
   (SvOBJECT (sv) && SvSTASH (sv) == PERLMACS_SV_INTERP (sv)->elo_stash)
#define XBARE_SV_LISP(sv) (*((Lisp_Object *) SvIVX (sv)))
#define FAST_SV_LISPP(sv) (SvROK (sv) && BARE_SV_LISPP (SvRV (sv)))

#define LISP_SVP(obj)							  \
   (FOREIGN_OBJECTP (obj) && XFOREIGN_OBJECT (obj)->vptr == &generic_vtbl)
#define XLISP_SV(obj)				\
   ((SV *) XFOREIGN_OBJECT (obj)->data)

#define PERL_OBJECT_P(obj)					\
   (FOREIGN_OBJECTP (obj)					\
    && XFOREIGN_OBJECT (obj)->vptr->type_of == lisp_sv_type_of)


static Lisp_Object perl_interpreter, Qperl_interpreter;
static Lisp_Object Qmake_perl_interpreter, Qperl_error;
static Lisp_Object Qvoid_context, Qscalar_context, Qlist_context;
Lisp_Object Qparsing, Qparsed, Qran, Qdestructed, Qbad;

static struct Lisp_Foreign_Object_VTable interp_vtbl;
static struct Lisp_Foreign_Object_VTable generic_vtbl;
static struct Lisp_Foreign_Object_VTable code_vtbl;

/* For returning from `Emacs::main'.  */
static struct catchtag perlmacs_catch;

/* Frame is Lisp (eval.c:unbind_to).
   Return to Perl.  */
static Lisp_Object
pop_lisp_frame (interp)
     Lisp_Object interp;
{
  struct lisp_frame *lfp;

  lfp = XPERL_INTERPRETER (interp)->top.l;
  if (lfp->prev && lfp->prev->prev)
    lfp->prev->prev->lisp_error = lfp->lisp_error;
  XPERL_INTERPRETER (interp)->top.p = lfp->prev;
  return make_number (0);
}

/* Frame is Perl (perlmacs_funcall or perlmacs_call_emacs_main).
   Enter a new Lisp frame.  */
static void
push_lisp_frame (lfp)
     struct lisp_frame *lfp;
{
  lfp->prev = current_perl->top.p;
  lfp->jumpret = 0;
  lfp->lisp_error = Qnil;
  current_perl->top.l = lfp;
  record_unwind_protect (pop_lisp_frame, perl_interpreter);
}

/* Frame is Perl (Emacs.xs:_main).  */
int
perlmacs_call_emacs_main (argc, argv, envp)
     int argc;
     char **argv;
     char **envp;
{
  struct lisp_frame lf;

  if (current_perl->top.p->prev)
    Perl_croak ("Attempt to enter Emacs recursively");

  push_lisp_frame (&lf);

  current_perl->in_emacs_main = 1;
  /* based on eval.c:Fcatch() */
  if (! _setjmp (perlmacs_catch.jmp))
    emacs_main(argc, argv, envp);
  current_perl->in_emacs_main = 0;

  return XINT (perlmacs_catch.val);
}

/* Frame is none or Perl (emacs.c:Fkill_emacs).  */
void
perlmacs_exit (status)
     int status;
{
  if (! top_level_perl)
    /* This is perl-within-emacs.  We're done.  */
    exit (status);

  if (current_perl->in_emacs_main)
    {
      /* This is emacs-within-perl.  Cause a return from
	 perlmacs_call_emacs_main.  */
      if (catchlist)
	abort ();
      catchlist = &perlmacs_catch;
      unwind_to_catch (catchlist, make_number (status));
      /* NOTREACHED */
    }
  /* Lisp outside of emacs_main called `kill-emacs'.
     Oblige by exiting the Perl way.  */
  Perl_my_exit (status);
}

/* Frame is Perl (Lisp.xs:BOOT).  */
void
perlmacs_init ()
{
  if (top_level_perl)
    {
      char *dummy_argv[] = { "perl", 0 };

      perl_interpreter = new_foreign_object (&interp_vtbl, top_level_perl,
					     make_number (0));

      /* May be overridden during Emacs::main.  */
      noninteractive = 1;

      init_lisp (1, dummy_argv, 0);
    }
  CACHE_ELO_STASH (current_perl);
}

/* Frame is none (emacs.c:emacs_main)
   or Perl (Emacs.pm:main by way of emacs.c:emacs_main).  */
void
perlmacs_init_lisp (argc, argv, skip_args)
     int argc;
     char **argv;
     int skip_args;
{
  if (top_level_perl)
    /* Fix up `command-line-args'.  The rest of Lisp initialization
       already happened when Emacs::Lisp was loaded.  */
    /* FIXME: There is a lot of black magic in init_cmdargs(),
       so I'm not sure whether this is right.  */
    init_cmdargs (argc, argv, skip_args);
  else
    init_lisp (argc, argv, skip_args);
}

/* Frame is Lisp
   (perlmacs_funcall by way of eval.c:internal_condition_case_1).  */
static Lisp_Object
funcall_1 (args_as_string)
     Lisp_Object args_as_string;
{
  int nargs, len;
  Lisp_Object *args;
  Lisp_Object retval;
  struct gcpro gcpro1;
  struct lisp_frame *lfp;

  len = XSTRING (args_as_string)->size;
  nargs = len / sizeof (Lisp_Object);
  args = (Lisp_Object *) alloca (len);  /* paranoid about alignment */
  bcopy (XSTRING (args_as_string)->data, args, len);

  GCPRO1 (*args);
  gcpro1.nvars = nargs;  /* I got this from Fapply().  */
  retval = Ffuncall (nargs, args);
  UNGCPRO;

  /* If there are errors, we don't arrive here.  */
  lfp = current_perl->top.l;
  lfp->lisp_error = Qnil;
  lfp->jumpret = 0;
  return retval;
}

/* Called during perlmacs_funcall in the event of a Lisp error.
   Return so that catchlist et al. will be restored properly by
   eval.c:unwind_to_catch.  When perlmacs_funcall sees that we set lisp_error,
   it will be safe to raise a Perl exception.

   Frame is Lisp
   (perlmacs_funcall by way of eval.c:internal_condition_case_1).  */
static Lisp_Object
perlmacs_error_handler (e)
     Lisp_Object e;
{
  current_perl->top.l->lisp_error = e;
  return make_number (0);
}

/* Frame is Perl
   (Lisp.xs: Emacs::Lisp::funcall or Emacs::Lisp::Object::funcall).  */
Lisp_Object
perlmacs_funcall (nargs, args)
     int nargs;
     Lisp_Object *args;
{
  Lisp_Object args_as_string;
  struct lisp_frame lf;
  Lisp_Object retval;
  int count = specpdl_ptr - specpdl;

  /* Ensure that Perl's context is restored, even if Lisp does a nonlocal
     jump.  */
  push_lisp_frame (&lf);

  /* Package the arg list into a single Lisp object for
     internal_condition_case_1.  Using a string is probably more efficient
     than using a list.  */
  args_as_string = make_unibyte_string ((char *) args,
					nargs * sizeof (Lisp_Object));
  retval = internal_condition_case_1 (funcall_1, args_as_string,
				      Qerror, perlmacs_error_handler);

  /* Return to the Perl frame.  */
  unbind_to (count, retval);

  if (lf.jumpret)
    {
      dTHR;
      if (lf.jumpret == 3)
	/* There was a Perl error.  Pop Perl's state to the last `eval'.  */
	PL_restartop = Perl_die_where (PL_in_eval ? 0 : SvPV (ERRSV, PL_na));

      JMPENV_JUMP (lf.jumpret);
    }
  if (! NILP (lf.lisp_error))
    {
      /* There was a Lisp error.  Make it look like an error to Perl, in
	 case there are no more Lisp frames or this Perl frame contains
	 an `eval'.  */
      char *errstr;
      CV *croak_cv;

      errstr = XSTRING (Ferror_message_string (lf.lisp_error))->data;

      /* Call Carp::croak if it is loaded, since it can give better context
	 information.  */
      /* FIXME: but sometimes it gives *too much* information.  */
      croak_cv = perl_get_cv ("Carp::croak", 0);
      if (croak_cv)
	{
	  /* This code is based on an example from perlcall.pod.  */
	  dSP;
	  PUSHMARK (sp);
	  XPUSHs (Perl_sv_2mortal (Perl_newSVpv (errstr, 0)));
	  PUTBACK;
	  perl_call_sv ((SV *) croak_cv, G_VOID);
	}
      Perl_croak ("%s", errstr);
    }
  return retval;
}

/* Frame is Perl (eval.c:unbind_to, from call_perl or,
   indirectly, abort_perl_call).
   Return to Lisp.  */
static Lisp_Object
pop_perl_frame (interp)
     Lisp_Object interp;
{
  struct perl_interpreter_object *perl;
  struct perl_frame *pfp;

  perl = XPERL_INTERPRETER (interp);
  pfp = perl->top.p;

  if (pfp->need_jmpenv && ! PERL_DEFUNCT_P (perl))
    {
      dSP;
      if (cxstack_ix > -1)
	{
	  SV **newsp;	/* used implicitly by POPBLOCK */
	  I32 gimme;	/* used implicitly by POPBLOCK */
	  PERL_CONTEXT *cx;
	  I32 optype;	/* used implicitly by POPEVAL */

	  Perl_dounwind (0);

	  /* This code is a copy of parts of pp_ctl.c:pp_leavetry.  */
	  POPBLOCK (cx, PL_curpm);
	  POPEVAL (cx);
	  sp = newsp;
	  LEAVE;
	}

      /* Work around Perl's shyness of core dumps ("panic: POPSTACK").  */
      if (! PL_curstackinfo->si_prev)
	abort ();
      POPSTACK;

      /* This is normally done by JMPENV_POP, but that macro assumes it is
	 used in the same C stack frame as dJMPENV, which is not the
	 case here.  */
      PL_top_env = pfp->old_top_env;
    }

  perl->top.l = pfp->prev;
  return make_number (0);
}

/* Frame is Lisp (internal_perl_call and Fperl_eval).
   Enter a new Perl frame.  */
static void
push_perl_frame (struct perl_frame *pfp, char need_jmpenv)
{
  dSP;
  PERL_CONTEXT *cx;
  I32 gimme = G_VOID;

  /* Refuse to enter Perl if Emacs is shutting down, since data structures
     are likely to be in an inconsistent state.  */
  if (EQ (emacs_phase, Qdestructing))
    error ("Attempt to enter Perl while exiting Emacs");

  pfp->prev = current_perl->top.l;
  current_perl->top.p = pfp;
  record_unwind_protect (pop_perl_frame, perl_interpreter);

  pfp->need_jmpenv = need_jmpenv;
  if (! need_jmpenv)
    return;

  /* Save the Perl's top longjmp structure pointer.
     Normally, this isn't necessary, because pop_perl_frame has only one
     JMPENV to pop and could do `PL_top_env = PL_top_env->je_prev'.
     But I have seen this code

	 catch \*::foo, sub { &perl_eval (q(&throw(\*::foo, 345))) }

     result in PL_top_env pointing below the stack pointer.  (Or above
     it, if you are standing on your head.  On the wrong side, anyway.)

     I have not figured out whether this is due to a bug in my code.  */
  pfp->old_top_env = PL_top_env;

  /* PUSHSTACK saves and resets PL_curstack, PL_stack_base, PL_stack_sp,
     and cxstack_ix.  cxstack_ix is really PL_curstackinfo->si_cxix.
     cxstack_ix counts PUSHBLOCK and PUSHSUBST instances.

     Although PUSHSTACK, POPSTACK, and POPSTACK_TO operate on PL_curstackinfo,
     POPSTACK_TO uses the curstack type as its argument.  PL_curstack remains
     constant between calls to PUSHSTACK with one exception, in pp_split,
     which I do not understand.  Why POPSTACK_TO doesn't use PL_curstackinfo
     instead of PL_curstack is not clear to me, but may be related to
     pp_split.

     I'm not 100% sure PUSHSTACK is needed here.  I originally added it
     to prevent `goto' from exiting the frame (and thus bypassing Lisp's
     unwinding mechanism and crashing the program).  */
  PUSHSTACK;

  /* This is a copy of parts of pp_ctl.c:pp_entertry.  */
  ENTER;
  SAVETMPS;
  PUSHBLOCK (cx, CXt_EVAL, sp);
  {
    /* PUSHEVAL dereferences PL_op, which may be 0 here.  */
    OP my_op;

    if (! PL_op)
      {
	my_op.op_type = OP_ENTERTRY;
	PL_op = &my_op;
      }
    PUSHEVAL (cx, 0, 0);
    if (PL_op == &my_op)
      PL_op = 0;
  }
  PL_in_eval = 1;
  sv_setpv (ERRSV, "");
  /*PUTBACK;*/ if (PL_stack_sp != sp) abort ();
}

/* Frame is Perl (perlmacs.c:call_perl).
   Called when Perl throws an exception.  Does not return.  */
static void
abort_perl_call (jumpret)
     int jumpret;
{
  struct perl_frame *pfp = current_perl->top.p;
  Lisp_Object err;
  int len;

  /* To return to Lisp, we need to call pop_perl_frame.
     But don't do it directly, let Lisp's stack unwinding code do it.  */

  pfp->prev->jumpret = jumpret;
  if (! NILP (pfp->prev->lisp_error))
    /* Lisp has already seen this error, either because Lisp generated it
       or because we were here two frames ago.  */
    Fsignal (Qnil, pfp->prev->lisp_error);

  /* Put Perl's $@ into a format Lisp can understand.  This is the reverse
     of the Ferror_message_string() call in `perlmacs_funcall'.  */
  {
    dTHR;
    err = perlmacs_sv_to_lisp (ERRSV);
  }

  /* Get rid of the newline so the error message looks nice in the
     minibuffer.  FIXME: Will this mess up multibyte strings?  */
  if (STRINGP (err))
    {
      len = STRING_BYTES (XSTRING (err)) - 1;
      if (XSTRING (err)->data [len] == '\n')
	/* XXX See if a space looks any better than ^J.
	   I'm scared to shorten Lisp's strings from under its feet.  */
	XSTRING (err)->data [len] = ' ';
    }
  Fsignal (Qperl_error, Fcons (err, Qnil));
}

/* Frame is Perl (perlmacs.c:call_perl).
   Convert the value or values (if any) returned by `perl-call' or `perl-eval'
   to a Lisp object.  */
static Lisp_Object
perlmacs_retval (ctx, numret, conv)
     I32 ctx;
     I32 numret;
     Lisp_Object (*conv) P_ ((SV *sv));
{
  Lisp_Object *vals;
  int i;

  switch (ctx & (G_VOID | G_SCALAR | G_ARRAY))
    {
    case G_VOID:
      return Qnil;
    case G_SCALAR:
      {
	dTHR;
	return (*conv) (*PL_stack_sp);
      }
    default:  /* G_ARRAY */
      if (numret == 0)
	return Qnil;
      {
	dTHR;
	PL_stack_sp -= numret;
	vals = (Lisp_Object *) alloca (numret * sizeof (Lisp_Object));
	for (i = 0; i < numret; i++)
	  vals [i] = (*conv) (PL_stack_sp [i+1]);
	return Flist (numret, vals);
      }
    }
}

enum perl_entry
  {
    PERL_CALL_SV,
    PERL_EVAL_SV,
    SV_FREE,
    EVAL_AND_CALL,
    CALL_RAW
  };

/* Frame is Perl (perlmacs.c, multiple functions).
   Call the specified Perl API function and pop to the previous Lisp frame.  */
/* Note: Perl 5.005 requires stdarg.  */
static Lisp_Object
call_perl (enum perl_entry entry, ...)
{
  va_list ap;
  SV *sv;
  I32 ctx;
  Lisp_Object (*conv) P_ ((SV *sv));
  char *str;
  dJMPENV;
  dTHR;
  int ret;
  Lisp_Object retval;
  I32 numret;
  struct lisp_frame *lfp;
  int count;

  /* Subtract 1 for the record_unwind_protect in push_perl_frame.  */
  count = specpdl_ptr - specpdl - 1;

  /* JMPENV_PUSH does sigsetjmp and ensures that, unless the process gets
     a signal, Perl will return to this function.
     Perl calls longjmp with an arg of 3 in the case of eval errors
     and 2 when it wants to exit.
     (However, Lisp is not as nice, and fails to provide a way to catch
     every possible `throw'.)  */
  JMPENV_PUSH (ret);
  if (ret)
    abort_perl_call (ret);

  va_start (ap, entry);
  switch (entry)
    {
    case PERL_EVAL_SV:
      sv = va_arg (ap, SV *);
      ctx = va_arg (ap, I32);
      conv = va_arg (ap, void *);

      numret = perl_eval_sv (sv, ctx);
      if (SvTRUE (ERRSV))
	abort_perl_call (3);
      retval = perlmacs_retval (ctx, numret, conv);
      break;

    case PERL_CALL_SV:
      sv = va_arg (ap, SV *);
      ctx = va_arg (ap, I32);
      conv = va_arg (ap, void *);

      numret = perl_call_sv (sv, ctx);
      retval = perlmacs_retval (ctx, numret, conv);
      break;

    case EVAL_AND_CALL:
      str = va_arg (ap, char *);
      ctx = va_arg (ap, I32);

      sv = perl_eval_pv (str, 1);
      POPMARK;
      if (! SvOK (sv))
	/* FIXME: The second argument to perl_eval_pv ("croak_on_error")
	   doesn't seem to work for syntax errors.  I don't know how
	   to distinguish syntax errors from expressions that evaluate
	   to undef.  */
	Perl_croak ("`perl-eval-and-call' string evaluated as undef;"
		    " possible syntax error");

      numret = perl_call_sv (sv, ctx);
      retval = perlmacs_retval (ctx, numret, perlmacs_sv_to_lisp);
      break;

    case SV_FREE:
      sv = va_arg (ap, SV *);

      Perl_sv_free (sv);
      retval = Qnil;
      break;
    }
  va_end (ap);

  /* Indicate to perlmacs_funcall that there were no errors.  */
  lfp = current_perl->top.p->prev;
  lfp->lisp_error = Qnil;
  lfp->jumpret = 0;

  return unbind_to (count, retval);
}

DEFUN ("get-perl-interpreter", Fget_perl_interpreter, Sget_perl_interpreter,
  0, 0, 0,
  "Return the current Perl interpreter object, or `nil' if there is none.")
  ()
{
  return perl_interpreter;
}

DEFUN ("set-perl-interpreter", Fset_perl_interpreter, Sset_perl_interpreter,
  1, 1, 0,
  "Make INTERPRETER the current Perl interpreter.\n\
\n\
Returns its argument.")
  (interpreter)
     Lisp_Object interpreter;
{
  CHECK_PERL (interpreter);

  /* Multiple Perl interpreters are not supported.  */
  if (XPERL_INTERPRETER (interpreter) != current_perl)
    abort ();

  /* But if they were supported, here's a tiny fraction of what we
     would do:  */
  current_perl = XPERL_INTERPRETER (interpreter);
  perl_interpreter = interpreter;
  return interpreter;
}

/* Perl interpreter foreign object methods.  Frame is always Lisp.  */

static Lisp_Object
lisp_perl_type_of (object)
     Lisp_Object object;
{
  return Qperl_interpreter;
}

static void
lisp_perl_destroy (object)
     Lisp_Object object;
{
  struct perl_interpreter_object *perl = XPERL_INTERPRETER (object);

  if (EQ (emacs_phase, Qdestructing))
    {
      /* Emacs is shutting down.  All Perl references to Lisp objects
	 are becoming invalid, so undef them.  */
      SV *sv, *tmp;
      struct sv_lisp *prot;

      for (sv = perl->protlist; sv; sv = tmp)
	{
	  tmp = SV_LISP_NEXT (sv);

	  /* Disable the DESTROY method.  */
	  SvOBJECT_off (sv);

	  xfree ((Lisp_Object *) SvIVX (sv));
	  Perl_sv_setsv (sv, &PL_sv_undef);
	}
      /* Don't continue with perl_destruct; it's not safe.  */
      return;
    }

  if (perl == top_level_perl)
    return;

  /* We can't get here from within a Perl call, because this function
     is called only when the interpreter is garbage collected,
     and it is gcpro'd when running.  */
  if (! PERL_DEFUNCT_P (perl))
    Fperl_destruct (object);
  perl_free (perl->interp);
  xfree (perl);
}

static void
lisp_perl_mark (object)
     Lisp_Object object;
{
  struct perl_interpreter_object *perl;
  SV *sv;
  struct lisp_frame *lfp;

  perl = XPERL_INTERPRETER (object);
  for (sv = perl->protlist; sv; sv = SV_LISP_NEXT (sv))
    mark_object ((Lisp_Object *) SvIVX (sv));

  /* Mark error data from pending exceptions.  */
  for (lfp = perl->top.l; lfp; lfp = (lfp->prev ? lfp->prev->prev : 0))
    mark_object (&lfp->lisp_error);
}

static struct Lisp_Foreign_Object_VTable interp_vtbl =
  {
    lisp_perl_type_of,		/* TYPE-OF */
    lisp_perl_destroy,		/* Destructor.  */
    lisp_perl_mark,		/* MARK */
    0,				/* TO-STRING */
    0,				/* EQUAL */
    0				/* CALL */
  };

/* Perl's destructor for Lisp objects.  This has to be registered in Emacs and
   not Emacs::Lisp's boot routine, because Perl may store Lisp references
   (args to perl-call) without Emacs::Lisp being loaded.  */
/* Frame is Perl (pp_hot.c:Perl_pp_entersub from sv.c:sv_clear).  */
static
XS (XS_Emacs__Lisp__Object_DESTROY)
{
  dXSARGS;
  SV *sv = SvRV (ST (0));
  struct sv_lisp *prot = (struct sv_lisp *) SvIV (sv);

  /* Remove our argument from the Perl interpreter's list of gc-protected
     objects.  */
  if (prot->prev)
    SV_LISP_NEXT (prot->prev) = prot->next;
  else
    PERLMACS_SV_INTERP (sv)->protlist = prot->next;

  if (prot->next)
    SV_LISP_PREV (prot->next) = prot->prev;
  xfree (prot);

  XSRETURN_EMPTY;
}

static
XS (XS_Emacs__constant)
{
  dXSARGS;
  char *name;

  if (items != 1)
    Perl_croak ("Usage: Emacs::constant(name)");

  name = SvPV (ST (0), PL_na);
  if (strEQ (name, "PERLMACS_VERSION"))
    XSRETURN_PV (PERLMACS_VERSION);
  if (strEQ (name, "INCLUDE_PATH"))
    XSRETURN_PV (PATH_INCLUDESEARCH);
  XSRETURN_UNDEF;
}

/* Perl initialization callback passed as argument to perl_parse().
   Frame is Perl (perl.c:perl_parse).  */
static void
perlmacs_xs_init ()
{
  extern void xs_init P_ ((void));

  /* Set PL_perl_destruct_level here and not in syms_of_perlmacs because
     PL_perl_destruct_level may be a macro that dereferences a pointer
     that was invalid until now.  */
#ifdef DEBUGGING
  /* Warn of unbalanced scope, unfreed temps, etc.  This should perhaps
     go away once the program is more stable.  */
  PL_perl_destruct_level = 2;
#else
  if (! top_level_perl)
    /* Try to free everything when shutting down Perl.  */
    PL_perl_destruct_level = 1;
#endif

  /* Call the default version of this function in the generated file
     perlxsi.c.  */
  xs_init ();

  newXS ("Emacs::Lisp::Object::DESTROY",
	 XS_Emacs__Lisp__Object_DESTROY, __FILE__);
  newXS ("Emacs::constant",
	 XS_Emacs__constant, __FILE__);
}

/* Construct an interpreter object and the underlying Perl interpreter.
   Frame is Lisp (Fmake_perl_interpreter) or none (perlmacs_main).  */
static struct perl_interpreter_object *
new_perl (in_lisp)
     int in_lisp;
{
  struct perl_interpreter_object *perl;

  perl = (struct perl_interpreter_object *)
    xmalloc (sizeof (struct perl_interpreter_object));
  bzero (perl, sizeof *perl);

  perl->interp = perl_alloc ();
  if (! perl->interp)
    {
      if (in_lisp)
	Fsignal (Qerror, memory_signal_data);
      else
	exit (1);
    }
  perl_construct (perl->interp);

  if (in_lisp)
    {
      perl->top.l = &perl->start.l;
      perl->top.l->lisp_error = Qnil;
    }
  else
    perl->top.p = &perl->start.p;

  perl->status = make_number (0);
  return perl;
}

/* Be like the perl program.  See perlmain.c:main.
   Frame is none (emacs.c:main).  */
int
perlmacs_main (argc, argv, env)
     int argc;
     char **argv;
     char **env;
{
  int status;

  PERL_SYS_INIT (&argc, &argv);
  perl_init_i18nl10n (1);

  top_level_perl = new_perl (0);
  current_perl = top_level_perl;
  top_level_perl->phase = Qparsing;
  status = perl_parse (top_level_perl->interp, perlmacs_xs_init,
		       argc, argv, 0);

  if (! status)
    {
      top_level_perl->phase = Qrunning;
      status = perl_run (top_level_perl->interp);
    }

  top_level_perl->phase = Qdestructing;
  perl_destruct (top_level_perl->interp);
  perl_free (top_level_perl->interp);

  PERL_SYS_TERM ();
  return status;
}

DEFUN ("primitive-make-perl", Fprimitive_make_perl, Sprimitive_make_perl,
  0, MANY, 0,
  "Used internally by `make-perl-interpreter'.\n\
Create and return a new Perl interpreter object.")
  (nargs, args)
     int nargs;
     Lisp_Object *args;
{
  char *def_argv[] = { "pmacs", "-e0" };
  char **argv;
  struct perl_interpreter_object *perl;
  Lisp_Object this_perl;
  struct perl_frame pf;
  int count = specpdl_ptr - specpdl;
  struct gcpro gcpro1;
  int i;

  if (current_perl)
    error ("Multiple Perl interpreters are not supported");

  if (nargs)
    {
      argv = (char **) alloca (nargs * sizeof (char *));
      for (i = 0; i < nargs; i++)
	{
	  CHECK_STRING (args [i], 0);
	  argv [i] = XSTRING (args [i])->data;
	}
    }
  else
    {
      argv = def_argv;
      nargs = sizeof def_argv / sizeof def_argv [0];
    }

  /* Test whether the user pressed ^G before entering a potentially
     long-lasting bit of Perl.  */
  QUIT;

  perl = new_perl (1);
  /* `this_perl' is for gc protection.  It is not really meaningful unless
     multiple interpreters are supported, since perl_interpreter is
     staticpro'd and can't otherwise be changed.  */
  this_perl = new_foreign_object (&interp_vtbl, perl, make_number (0));

  perl->phase = Qparsing;
  current_perl = perl;
  perl_interpreter = this_perl;
  push_perl_frame (&pf, 0);

  GCPRO1 (this_perl);
  perl->status =
    make_number (perl_parse (perl->interp, perlmacs_xs_init, nargs, argv, 0));

  if (! EQ (perl->status, make_number (0)))
    {
      perl->phase = Qbad;
      /* FIXME: Leaks?  */
      perl_free (perl->interp);
      current_perl = 0;
      this_perl = perl_interpreter = Qnil;
    }
  else
    perl->phase = Qparsed;
  UNGCPRO;

  CACHE_ELO_STASH (perl);
  return unbind_to (count, this_perl);
}

DEFUN ("perl-run", Fperl_run, Sperl_run, 0, 1, 0,
  "Run the specified Perl interpreter.\n\
If no argument is given, run the current interpreter.  Return what would be\n\
perl's exit status.")
  (interpreter)
     Lisp_Object interpreter;
{
  struct perl_interpreter_object *perl;
  struct perl_frame pf;
  int count = specpdl_ptr - specpdl;
  struct gcpro gcpro1;

  if (NILP (interpreter))
    interpreter = perl_interpreter;

  CHECK_PERL (interpreter);
  perl = XPERL_INTERPRETER (interpreter);
  if (perl->top.l->prev
      || ! (EQ (perl->phase, Qparsed)
	    || EQ (perl->phase, Qran)))
    error ("Bad calling sequence for `perl-run'");

  perl->phase = Qrunning;
  current_perl = perl;
  perl_interpreter = interpreter;
  push_perl_frame (&pf, 0);

  GCPRO1 (interpreter);
  perl->status = make_number (perl_run (perl->interp));
  UNGCPRO;

  perl->phase = Qran;
  return unbind_to (count, perl->status);
}

DEFUN ("perl-destruct", Fperl_destruct, Sperl_destruct, 0, 1, 0,
  "Attempt to shut down the specified Perl interpreter.\n\
If no arg is given, shut down the current Perl interpreter.")
  (interpreter)
     Lisp_Object interpreter;
{
  struct perl_interpreter_object *perl;
  struct perl_frame pf;
  int count = specpdl_ptr - specpdl;
  struct gcpro gcpro1;

  if (NILP (interpreter))
    {
      interpreter = perl_interpreter;
      if (NILP (interpreter))
	return Qnil;
    }

  CHECK_PERL (interpreter);
  perl = XPERL_INTERPRETER (interpreter);
  if (perl == top_level_perl)
    error ("Attempt to destruct a top-level Perl");

  /* perl->top.l == 0 only during global destruction.  */
  if ((perl->top.l && perl->top.l->prev)
      || (! EQ (perl->phase, Qparsed)
	  && ! EQ (perl->phase, Qran)))
    error ("Bad calling sequence for `perl-destruct'");

  perl->phase = Qdestructing;
  current_perl = perl;
  perl_interpreter = interpreter;
  push_perl_frame (&pf, 0);

  GCPRO1 (interpreter);
  perl_destruct (perl->interp);
  UNGCPRO;

  if (perl->protlist)
    {
      long count;
      SV *sv, *tmp;
      struct sv_lisp *prot;

      for (count = 0, sv = perl->protlist; sv; count++, sv = tmp)
	{
	  tmp = SV_LISP_NEXT (sv);
	  xfree ((Lisp_Object *) SvIVX (sv));
	}
      if (PL_perl_destruct_level >= 2)
	Perl_warn ("In perl-destruct: %ld unfreed Lisp objects!\n", count);
    }
  perl->phase = Qdestructed;
  current_perl = 0;
  perl_interpreter = Qnil;
  /* gcpro the interpreter while unbinding.  */
  unbind_to (count, interpreter);
  return Qnil;
}

DEFUN ("perl-status", Fperl_status, Sperl_status, 0, 1, 0,
  "Return what would be perl's exit status.")
  (interpreter)
     Lisp_Object interpreter;
{
  if (NILP (interpreter))
    interpreter = perl_interpreter;

  CHECK_PERL (interpreter);
  return XPERL_INTERPRETER (interpreter)->status;
}

DEFUN ("perl-phase", Fperl_phase, Sperl_phase, 0, 1, 0,
  "Return the Perl interpreter's phase, one of the following symbols:\n\
parsing, parsed, running, ran, destructing, destructed, bad.")
  (interpreter)
     Lisp_Object interpreter;
{
  if (NILP (interpreter))
    interpreter = perl_interpreter;

  CHECK_PERL (interpreter);
  return XPERL_INTERPRETER (interpreter)->phase;
}

/* Fire up a Perl interpreter for use with Lisp functions that act
   implicitly on one.
   Don't call this function directly.  Use INIT_PERL.  */
static void
init_perl ()
{
  if (NILP (Ffboundp (Qmake_perl_interpreter)))
    Fset_perl_interpreter (Fprimitive_make_perl (0, 0));
  else
    Fset_perl_interpreter (call0 (Qmake_perl_interpreter));
}

/* There should be no need to check argument type in the Lisp_Foreign_Object
   methods, so long as they are called only internally.  */

static Lisp_Object
lisp_sv_type_of (object)
     Lisp_Object object;
{
  char *str;
  SV *sv = (SV *) XFOREIGN_OBJECT (object)->data;

  /* Look to sv.c:sv_reftype for inspiration.  */

  if (SvOBJECT(sv))
    /* FIXME: Let's return something more interesting,
       like the object's stash.  */
    return intern ("perl-object");

  switch (SvTYPE (sv))
    {
    case SVt_NULL:
    case SVt_IV:
    case SVt_NV:
    case SVt_RV:
    case SVt_PV:
    case SVt_PVIV:
    case SVt_PVNV:
    case SVt_PVMG:
    case SVt_PVBM:	str = SvROK (sv) ? "perl-ref" : "perl-scalar"; break;
    case SVt_PVLV:	str = "perl-lvalue";	break;
    case SVt_PVAV:	str = "perl-array";	break;
    case SVt_PVHV:	str = "perl-hash";	break;
    case SVt_PVCV:	str = "perl-code";	break;
    case SVt_PVGV:	str = "perl-glob";	break;
    case SVt_PVFM:	str = "perl-format";	break;
    case SVt_PVIO:	str = "perl-io";	break;
    default:		str = "perl-unknown";	break;
    }

  return intern (str);
}

static void
lisp_sv_mark (object)
     Lisp_Object object;
{
  /* Mark the interpreter.  */
  mark_object (&XFOREIGN_OBJECT (object)->lisp_data);
}

static void
lisp_sv_destroy (object)
     Lisp_Object object;
{
  struct perl_interpreter_object *perl;
  SV *sv;

  /* Check the interpreter's status to avoid a crash after `perl-destruct'.
     During Emacs destruction, even this is dangerous, so the only thing
     allowed is to free memory.
     (The solution is to call `perl-destruct' *before* Emacs starts to
     destruct.)  */
  if (EQ (emacs_phase, Qdestructing))
    return;

  perl = LISP_SV_INTERP (object);
  if (PERL_DEFUNCT_P (perl)
      || EQ (perl->phase, Qdestructing))
    return;

  sv = (SV *) XFOREIGN_OBJECT (object)->data;

  if (! perl->top.p || SvREFCNT (sv) || ! SvOBJECT (sv))
    {
      SvREFCNT_dec (sv);
      return;
    }
  call_perl (SV_FREE, sv);
}

static struct Lisp_Foreign_Object_VTable generic_vtbl =
  {
    lisp_sv_type_of,	/* TYPE-OF method.  */
    lisp_sv_destroy,	/* Destructor.  */
    lisp_sv_mark,	/* MARK method.  Mark the interpreter.  */
    0,			/* TO-STRING method.  FIXME: Write one!  */
    0,			/* EQUAL method.  FIXME: Write one!  */
    0			/* CALL method.  Invalid as a function.  */
  };

static Lisp_Object
lisp_cv_call (self, nargs, args)
     Lisp_Object self;
     int nargs;
     Lisp_Object *args;
{
  Lisp_Object *new_args;
  int new_nargs = nargs + 2;

  if (PERL_DEFUNCT_P (LISP_SV_INTERP (self)))
    error ("Attempt to call Perl code in a destructed interpreter");

  new_args = (Lisp_Object *) alloca ((nargs + 2) * sizeof (Lisp_Object));
  new_args [0] = self;
  if (EQ (args [0], Qscalar_context)
      || EQ (args [0], Qlist_context)
      || EQ (args [0], Qvoid_context))
    {
      bcopy (args, new_args + 1, nargs * sizeof args[0]);
      return internal_perl_call (nargs + 1, new_args, 0, perlmacs_sv_to_lisp);
    }
  else
    {
      new_args [1] = Qscalar_context;
      bcopy (args, new_args + 2, nargs * sizeof args[0]);
      return internal_perl_call (nargs + 2, new_args, 0, perlmacs_sv_to_lisp);
    }
}

static struct Lisp_Foreign_Object_VTable code_vtbl =
  {
    lisp_sv_type_of,	/* TYPE-OF method.  */
    lisp_sv_destroy,	/* Destructor.  Use SV's.  */
    lisp_sv_mark,	/* MARK method.  Mark the interpreter.  */
    0,			/* TO-STRING method.  Use default.  */
    0,			/* EQUAL method.  Use default.  */
    lisp_cv_call	/* CALL method.  */
  };

Lisp_Object
perlmacs_lisp_wrap_sv (sv)
     SV *sv;
{
  if (SvROK (sv))
    sv = SvRV (sv);

  SvREFCNT_inc (sv);
  return new_foreign_object (SvTYPE (sv) == SVt_PVCV
			     ? &code_vtbl : &generic_vtbl,
			     sv, perl_interpreter);
}

Lisp_Object
perlmacs_sv_to_lisp (sv)
     SV *sv;
{
  Lisp_Object retval;
  SV *sv1, *sv2;

  if (! sv)
    return Qnil;  /* grrr.... */

  if (! SvOK (sv))  /* undef equals nil */
    return Qnil;
  if (SvIOK (sv) && (IV) XINT (SvIV (sv)) == SvIV (sv))
    return make_number (SvIV (sv));

#ifdef LISP_FLOAT_TYPE
  if (SvNIOK (sv))
    return make_float (SvNV (sv));
#endif

  if (SvPOK (sv))
    {
      STRLEN len;
      char *pc = SvPV (sv, len);
      return make_unibyte_string (pc, (int) len);
    }

  /* Don't distinguish, e.g., AV* from RV->AV*.  Perl can't pass us an AV*,
     but this way, C can avoid allocating RV's.  */
  sv1 = SvROK (sv) ? SvRV (sv) : sv;

  if (BARE_SV_LISPP (sv1))
    return XBARE_SV_LISP (sv1);

  switch (SvTYPE (sv1))
    {
    case SVt_NULL:
    case SVt_IV:
    case SVt_NV:
    case SVt_RV:
    case SVt_PV:
    case SVt_PVIV:
    case SVt_PVNV:
    case SVt_PVMG:
    case SVt_PVBM:
    case SVt_PVLV:
    case SVt_PVHV:
    case SVt_PVCV:
    case SVt_PVFM:
    case SVt_PVIO:
      if (SvROK (sv1))
	{
	  sv2 = SvRV (sv1);

	  /* Convert array ref ref to vector.  */
	  if (SvTYPE (sv2) == SVt_PVAV)
	    {
	      SV **ary;
	      I32 key;
	      Lisp_Object *ptr;

	      ary = AvARRAY ((AV *) sv2);
	      key = AvFILLp ((AV *) sv2) + 1;
	      retval = Fmake_vector (make_number (key), Qnil);
	      ptr = XVECTOR (retval)->contents;
	      while (key--)
		ptr [key] = perlmacs_sv_to_lisp (ary [key]);

	      return retval;
	    }
	}
      break;

    case SVt_PVAV:
      /* Convert arrayref to list.  */
      {
	SV **ary;
	I32 key;

	ary = AvARRAY((AV *) sv1);
	key = AvFILLp((AV *) sv1) + 1;
	retval = Qnil;
	while (key)
	  retval = Fcons (perlmacs_sv_to_lisp (ary [--key]), retval);

	return retval;
      }
      break;

    case SVt_PVGV:
      /* Check for glob in package main.  */
      if (GvSTASH (sv1) == PL_defstash)
	{
	  /* Convert globref to symbol.  */
	  char *pc;
	  const char *pcc;
	  const char *end;
	  char *name;

	  end = GvNAME (sv1) + GvNAMELEN (sv1);
	  name = alloca (1 + GvNAMELEN (sv1));
	  for (pcc = GvNAME (sv1), pc = name; pcc != end; ++pcc, ++pc)
	    *pc = (*pcc == '_') ? '-' : (*pcc == '-') ? '_' : *pcc;
	  *pc = '\0';

	  return intern (name);
	}
      break;
    }

  return perlmacs_lisp_wrap_sv (sv);
}

SV *
perlmacs_sv_wrap_lisp (obj)
     Lisp_Object obj;
{
  struct sv_lisp *prot;
  SV *sv;
  SV *rv;

  /* Protect object from garbage collection during the SV lifetime.  */
  prot = (struct sv_lisp *) xmalloc (sizeof (struct sv_lisp));
  prot->obj = obj;
  prot->prev = 0;
  prot->next = current_perl->protlist;
  sv = Perl_newSViv ((IV) prot);
  if (current_perl->protlist)
    SV_LISP_PREV (current_perl->protlist) = sv;
  current_perl->protlist = sv;

  /* FIXME: I doubt whether I'm adequately freeing these rv's.  */
  rv = Perl_sv_bless (Perl_newRV_noinc (sv), current_perl->elo_stash);
  return rv;
}

SV *
perlmacs_lisp_to_sv (obj)
     Lisp_Object obj;
{
  if (NILP (obj))
    return &PL_sv_undef;

  /* Unconvert wrapped Perl data.  */
  if (PERL_OBJECT_P (obj))
    return Perl_newRV (XLISP_SV (obj));

  switch (XTYPE (obj))
    {
    case Lisp_Int:
      return Perl_newSViv ((IV) XINT (obj));

#ifdef LISP_FLOAT_TYPE
    case Lisp_Float:
      return Perl_newSVnv (XFLOAT (obj)->data);
#endif
    case Lisp_String:
      return Perl_newSVpvn (XSTRING (obj)->data,
			    (STRLEN) XSTRING (obj)->size);

    case Lisp_Symbol:
      {
	/* Convert symbol to globref.  */
	EMACS_INT len = XSYMBOL (obj)->name->size;
	char *name = (char *) alloca (len + 3);
	char *pc = name;
	const char *pcc = &(XSYMBOL (obj)->name->data [0]);
	const char *end = pcc + len;

	*pc++ = ':';
	*pc++ = ':';
	for (; pcc != end; ++pcc, ++pc)
	  *pc = (*pcc == '_') ? '-' : (*pcc == '-') ? '_' : *pcc;
	*pc = '\0';
	return Perl_newRV ((SV *) Perl_gv_fetchpv (name, 1, SVt_PVGV));
      }
    case Lisp_Cons:
      {
	I32 i;
	Lisp_Object list;
	for (i = 1, list = XCDR (obj); 1; i++, list = XCDR (list))
	  {
	    if (NILP (list))
	      {
		/* Convert list to arrayref.  */
		AV *av = Perl_newAV ();

		Perl_av_extend (av, i);
		for (i = 0, list = obj; !NILP (list);
		     i++, list = XCDR (list))
		  Perl_av_store (av, i, perlmacs_lisp_to_sv (XCAR (list)));
		return Perl_newRV_noinc ((SV *) av);
	      }
	    if (!CONSP (list))
	      break;
	  }
      }
      break;
    case Lisp_Vectorlike:
      if (VECTORP (obj))
	{
	  /* Convert vector to array ref ref.  */
	  int i = XVECTOR (obj)->size;
	  Lisp_Object *ptr = XVECTOR (obj)->contents;
	  AV *av = Perl_newAV ();

	  Perl_av_extend (av, i);
	  while (i--)
	    Perl_av_store (av, i, perlmacs_lisp_to_sv (ptr [i]));
	  return Perl_newRV_noinc (Perl_newRV_noinc ((SV *) av));
	}
      break;

      /* FIXME: What other object types are intelligently wrappable?  */

    default:
      break;
    }
  return perlmacs_sv_wrap_lisp (obj);
}

DEFUN ("perl-object-p", Fperl_object_p, Sperl_object_p, 1, 1, 0,
  "Return t if OBJECT is any type of Perl data or code.")
     (object)
     Lisp_Object object;
{
  if (PERL_OBJECT_P (object))
    return Qt;
  return Qnil;
}

DEFUN ("perl-to-lisp", Fperl_to_lisp, Sperl_to_lisp, 1, 1, 0,
  "Return OBJECT as the corresponding Lisp type.\n\
If the object is not Perl data or cannot be converted to Lisp,\n\
it is returned unchanged.")
     (object)
     Lisp_Object object;
{
  if (LISP_SVP (object))
    return perlmacs_sv_to_lisp (XLISP_SV (object));
  return object;
}

DEFUN ("perl-wrap", Fperl_wrap, Sperl_wrap, 1, 1, 0,
  "Return OBJECT as a blessed reference of Perl package Emacs::Lisp::Object.")
     (object)
     Lisp_Object object;
{
  return perlmacs_lisp_wrap_sv (perlmacs_sv_wrap_lisp (object));
}

DEFUN ("make-perl-data", Fmake_perl_data, Smake_perl_data, 1, 1, 0,
  "Return OBJECT as the corresponding Perl type.\n\
If the object is not Perl data or cannot be converted to Perl,\n\
return OBJECT as a blessed reference of package Emacs::Lisp::Object.")
     (object)
     Lisp_Object object;
{
  return perlmacs_lisp_wrap_sv (perlmacs_lisp_to_sv (object));
}

static Lisp_Object
internal_perl_call (nargs, args, do_eval, conv)
     int nargs;
     Lisp_Object *args;
     int do_eval;
     Lisp_Object (*conv) P_ ((SV *sv));
{
  SV *sv;
  Lisp_Object code;
  I32 ctx;
  struct perl_frame pf;
  int i;

  code = args [0];
  if (EQ (args [1], Qscalar_context))
    nargs--, args++, ctx = G_SCALAR;
  else if (EQ (args [1], Qlist_context))
    nargs--, args++, ctx = G_ARRAY;
  else if (EQ (args [1], Qvoid_context))
    nargs--, args++, ctx = G_VOID;
  else
    ctx = G_SCALAR;

  push_perl_frame (&pf, 1);
  {
    dSP;

    EXTEND (sp, nargs - 1);
    PUSHMARK (sp);
    for (i = 1; i < nargs; i++)
      *++sp = sv_2mortal (perlmacs_lisp_to_sv (args [i]));
    PUTBACK;
  }
  if (do_eval)
    return call_perl (EVAL_AND_CALL, XSTRING (code)->data, ctx);
  else
    return call_perl (PERL_CALL_SV, sv_2mortal (perlmacs_lisp_to_sv (code)),
		      ctx, conv);
}

static Lisp_Object
internal_perl_eval (string, context, conv)
     Lisp_Object string, context;
     Lisp_Object (*conv) P_ ((SV *sv));
{
  SV *sv;
  I32 ctx;
  struct perl_frame pf;

  CHECK_STRING (string, 0);
  INIT_PERL;

  if (EQ (context, Qnil) || EQ (context, Qscalar_context))
    ctx = G_SCALAR;
  else if (EQ (context, Qlist_context))
    ctx = G_ARRAY;
  else if (EQ (context, Qvoid_context))
    ctx = G_VOID;
  else
    Fsignal (Qerror,
	     Fcons (build_string ("Unknown context for Perl evaluation"),
		    Fcons (context, Qnil)));

  push_perl_frame (&pf, 1);
  sv = sv_2mortal (perlmacs_lisp_to_sv (string));
  return call_perl (PERL_EVAL_SV, sv, ctx, conv);
}

DEFUN ("perl-eval", Fperl_eval, Sperl_eval, 1, 2, 0,
  "Evaluate STRING as Perl code, returning the value of the last expression.\n\
If specified, CONTEXT must be either `scalar-context', `list-context', or\n\
`void-context'.  By default, a scalar context is supplied.")
     (string, context)
     Lisp_Object string, context;
{
  return internal_perl_eval (string, context, perlmacs_sv_to_lisp);
}

DEFUN ("perl-eval-raw", Fperl_eval_raw, Sperl_eval_raw, 1, 2, 0,
  "Evaluate STRING as Perl code, returning its value as Perl data.\n\
This function is exactly the same as `perl-eval' except in that it does not\n\
convert its result to Lisp.")
     (string, context)
     Lisp_Object string, context;
{
  return internal_perl_eval (string, context, perlmacs_lisp_wrap_sv);
}

DEFUN ("perl-call", Fperl_call, Sperl_call, 1, MANY, 0,
  "Call a Perl sub or coderef with arguments.\n\
\n\
The first argument to `perl-call' may be a string containing the sub name\n\
or an object of type `perl-code' (a Perl coderef).\n\
\n\
The second argument to `perl-call' specifies the calling context if it is\n\
one of the symbols `scalar-context', `list-context', or `void-context'.\n\
If the second argument to `perl-call' is none of these, a scalar context\n\
is used, and the second argument, if present, is prepended to the list of\n\
remaining args.  The remaining args are converted to Perl and passed to\n\
the sub or coderef.\n\
\n\
\n\
(perl-call SUB &optional CONTEXT &rest ARGS)")
  (nargs, args)
     int nargs;
     Lisp_Object *args;
{
  INIT_PERL;
  return internal_perl_call (nargs, args, 0, perlmacs_sv_to_lisp);
}

DEFUN ("perl-call-raw", Fperl_call_raw, Sperl_call_raw, 1, MANY, 0,
  "Call a Perl sub or coderef and return its result as Perl data.\n\
This function is exactly the same as `perl-call' except in that it does not\n\
convert its result to Lisp.\n\
\n\
\n\
(perl-call-raw SUB &optional CONTEXT &rest ARGS)")
  (nargs, args)
     int nargs;
     Lisp_Object *args;
{
  INIT_PERL;
  return internal_perl_call (nargs, args, 0, perlmacs_lisp_wrap_sv);
}

DEFUN ("perl-eval-and-call", Fperl_eval_and_call, Sperl_eval_and_call,
       1, MANY, 0,
  "Same as `perl-call' but evaluate the first arg to get the coderef.\n\
\n\
The first argument should be a string of Perl code which evaluates to a\n\
sub name or coderef.  The remaining arguments are treated the same as in\n\
`perl-call'.\n\
\n\
\n\
(perl-eval-and-call STRING &optional CONTEXT &rest ARGS)")
  (nargs, args)
     int nargs;
     Lisp_Object *args;
{
  INIT_PERL;
  CHECK_STRING (args [0], 0);
  return internal_perl_call (nargs, args, 1, 0);
}

void
syms_of_perlmacs ()
{
  Qmake_perl_interpreter = intern ("make-perl-interpreter");
  staticpro (&Qmake_perl_interpreter);

  Qparsing = intern ("parsing");
  staticpro (&Qparsing);
  Qparsed = intern ("parsed");
  staticpro (&Qparsed);
  Qran = intern ("ran");
  staticpro (&Qran);
  Qdestructed = intern ("destructed");
  staticpro (&Qdestructed);
  Qbad = intern ("bad");
  staticpro (&Qbad);

  Qvoid_context = intern ("void-context");
  staticpro (&Qvoid_context);
  Qscalar_context = intern ("scalar-context");
  staticpro (&Qscalar_context);
  Qlist_context = intern ("list-context");
  staticpro (&Qlist_context);

  Qperl_interpreter = intern ("perl-interpreter");
  staticpro (&Qperl_interpreter);

  if (! top_level_perl)
    perl_interpreter = Qnil;
  staticpro (&perl_interpreter);

  Qperl_error = intern ("perl-error");
  staticpro (&Qperl_error);

  Fput (Qperl_error, Qerror_conditions,
	Fcons (Qperl_error, Fcons (Qerror, Qnil)));
  Fput (Qperl_error, Qerror_message,
	build_string ("Perl error"));

  Fprovide (intern ("perl-core"));

  defsubr (&Sget_perl_interpreter);
  defsubr (&Sset_perl_interpreter);
  defsubr (&Sprimitive_make_perl);
  defsubr (&Sperl_run);
  defsubr (&Sperl_destruct);
  defsubr (&Sperl_status);
  defsubr (&Sperl_phase);
  defsubr (&Sperl_object_p);
  defsubr (&Sperl_to_lisp);
  defsubr (&Sperl_wrap);
  defsubr (&Smake_perl_data);
  defsubr (&Sperl_eval);
  defsubr (&Sperl_eval_raw);
  defsubr (&Sperl_call);
  defsubr (&Sperl_call_raw);
  defsubr (&Sperl_eval_and_call);
}
