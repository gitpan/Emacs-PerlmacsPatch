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


/* FIXME:  Some Glibc ipc header needs this for Perl 5.005.  */
#if !(defined _SVID_SOURCE || defined _XOPEN_SOURCE)
#define _SVID_SOURCE
#endif

#include "config.h"  /* ...hoping this is Emacs' config, not Perl's! */
#include "lisp.h"

#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>
#include "perlmacs.h"

#include <setjmp.h>
#include "eval.h"

#ifdef PERL_REVISION
#  if PERL_REVISION >= 5 && PERL_VERSION >= 6
#    define HAVE_PERL_5_6
#  endif
#endif

struct emacs_perl_interpreter *top_level_perl, *current_perl;
EXFUN (Fperl_destruct, 1);
typedef Lisp_Object (*sv_to_lisp_converter_t) P_ ((SV *sv));
static Lisp_Object internal_perl_call P_ ((int nargs, Lisp_Object *args,
					   int do_eval,
					   sv_to_lisp_converter_t conv));
static void init_perl ();

#define INIT_PERL do { if (!current_perl) init_perl (); } while (0)

#define PERL_IS_TOP_LEVEL(perl) (perl == top_level_perl)

#define CHECK_PERL_INTERPRETER(obj)			\
  { if (! FOREIGN_OBJECTP (obj)				\
	|| XFOREIGN_OBJECT (obj)->vptr != &interp_vtbl)	\
      wrong_type_argument (Qperl_interpreter, obj); }

/* How to tell when a Perl object's interpreter is no longer usable.  */
#define LISP_SV_INTERP(obj)					\
   (XPERL_INTERPRETER (XFOREIGN_OBJECT (obj)->lisp_data))
#define PERL_DEFUNCT_P(perl)			\
   (EQ ((perl)->phase, Qdestructed))

#define CACHE_ELO_STASH(perl)						\
   { if (! perl->elo_stash)						\
       perl->elo_stash = Perl_gv_stashpv ("Emacs::Lisp::Object", 1); }
#define BARE_SV_LISPP(sv)						 \
   (SvOBJECT (sv) && SvSTASH (sv) == current_perl->elo_stash)
#define XBARE_SV_LISP(sv) (*((Lisp_Object *) SvIVX (sv)))

#define LISP_SVP(obj)							  \
   (FOREIGN_OBJECTP (obj) && XFOREIGN_OBJECT (obj)->vptr == &sv_vtbl)
#define XLISP_SV(obj)				\
   ((SV *) XFOREIGN_OBJECT (obj)->data)

#define PERL_OBJECT_P(obj)					\
   (FOREIGN_OBJECTP (obj)					\
    && XFOREIGN_OBJECT (obj)->vptr->type_of == lisp_sv_type_of)


static Lisp_Object Vperl_interpreter, Qperl_interpreter, Qperl_value;
static Lisp_Object Qmake_perl_interpreter, Qperl_error;
static Lisp_Object Qvoid_context, Qscalar_context, Qlist_context;
static Lisp_Object Qparsing, Qparsed, Qran, Qdestructed;

static struct Lisp_Foreign_Object_VTable interp_vtbl;
static struct Lisp_Foreign_Object_VTable sv_vtbl;

/* For returning from `Emacs::main'.  */
static struct catchtag perlmacs_catch;

/* Frame is Lisp (eval.c:unbind_to).
   Return to Perl.  */
static Lisp_Object
pop_lisp_frame (interp)
     Lisp_Object interp;
{
  struct lisp_frame *lfp;
  struct emacs_perl_interpreter *perl;

  lfp = XPERL_INTERPRETER (interp)->frame_l;
  if (lfp->prev && lfp->prev->prev)
    lfp->prev->prev->lisp_error = lfp->lisp_error;
  perl = XPERL_INTERPRETER (interp);
  perl->frame_l = 0;
  perl->frame_p = lfp->prev;
  return make_number (0);
}

/* Frame is Perl (perlmacs_funcall or perlmacs_call_emacs_main).
   Enter a new Lisp frame.  */
static void
push_lisp_frame (lfp)
     struct lisp_frame *lfp;
{
  lfp->prev = current_perl->frame_p;
  lfp->jumpret = 0;
  lfp->lisp_error = Qnil;
  current_perl->frame_p = 0;
  current_perl->frame_l = lfp;
  record_unwind_protect (pop_lisp_frame, Vperl_interpreter);
}

/* Frame is Perl (Emacs.xs:_main).  */
int
perlmacs_call_emacs_main (argc, argv, envp)
     int argc;
     char **argv;
     char **envp;
{
  struct lisp_frame lf;

  if (current_perl->frame_p->prev)
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
funcall_1 (argv)
     Lisp_Object argv;
{
  Lisp_Object retval;
  struct gcpro gcpro1;
  struct Lisp_Vector *v = XVECTOR (argv);
  struct lisp_frame *lfp;

  GCPRO1 (argv);
  retval = Ffuncall (v->size, v->contents);
  UNGCPRO;

  /* If there are errors, we don't arrive here.  */
  lfp = current_perl->frame_l;
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
  current_perl->frame_l->lisp_error = e;
  return make_number (0);
}

/* Frame is Perl
   (Lisp.xs: Emacs::Lisp::funcall or Emacs::Lisp::Object::funcall).  */
/* Call Ffuncall safely from Perl.  */
Lisp_Object
perlmacs_funcall (nargs, args)
     int nargs;
     Lisp_Object *args;
{
  struct lisp_frame lf;
  Lisp_Object retval;
  int count = specpdl_ptr - specpdl;

  /* Ensure that Perl's context is restored, even if Lisp does a nonlocal
     jump.  */
  push_lisp_frame (&lf);

  retval = internal_condition_case_1 (funcall_1, Fvector (nargs, args),
				      Qerror, perlmacs_error_handler);

  /* Return to the Perl frame.  */
  unbind_to (count, retval);

  if (lf.jumpret)
    {
      dTHR;
      if (lf.jumpret == 3)
	/* There was a Perl error.  Pop Perl's state to the last `eval'.  */
	{
	  STRLEN len;
	  char *message;

	  message = SvPV (ERRSV, len);
#ifdef HAVE_PERL_5_6
	  PL_restartop = Perl_die_where (PL_in_eval ? 0 : message, len);
#else
	  PL_restartop = Perl_die_where (PL_in_eval ? 0 : message);
#endif
	}

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

/* Frame is Perl (eval.c: unbind_to, from call_perl or,
   indirectly, abort_perl_call).
   Return to Lisp.  */
static Lisp_Object
pop_perl_frame (interp)
     Lisp_Object interp;
{
  struct emacs_perl_interpreter *perl;
  struct perl_frame *pfp;

  perl = XPERL_INTERPRETER (interp);
  pfp = perl->frame_p;

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

	  /* This code is a copy of parts of pp_ctl.c: pp_leavetry.  */
	  POPBLOCK (cx, PL_curpm);
	  POPEVAL (cx);
	  sp = newsp;
	  LEAVE;
	}
      FREETMPS;  /* FIXME: Should we do this elsewhere too/instead?
		    Maybe in perl-run and primitive-make-perl?  */

      /* Work around Perl's shyness of core dumps ("panic: POPSTACK").  */
      if (! PL_curstackinfo->si_prev)
	abort ();
      POPSTACK;

      /* This is normally done by JMPENV_POP, but that macro assumes it is
	 used in the same C stack frame as dJMPENV, which is not the
	 case here.  */
      PL_top_env = pfp->old_top_env;
    }

  perl->frame_p = 0;
  perl->frame_l = pfp->prev;
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

  pfp->prev = current_perl->frame_l;
  current_perl->frame_l = 0;
  current_perl->frame_p = pfp;
  record_unwind_protect (pop_perl_frame, Vperl_interpreter);

  pfp->need_jmpenv = need_jmpenv;
  if (! need_jmpenv)
    return;

  /* Save the Perl's top longjmp structure pointer.

     Normally, this isn't necessary, because pop_perl_frame has only one
     JMPENV to pop, and we could use JMPENV_POP in the frame that uses
     JMPENV_PUSH.  But a Lisp `throw' bypasses Perl's jump levels.

     FIXME: Maybe when popping frames during a throw, we should force
     a return to the next outer jump level so that Perl code which expects
     this will work.  Maybe Fthrow should be changed to support a
     "catchall".  */
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
  Perl_sv_setpv (ERRSV, "");
}

/* Frame is Perl (perlmacs.c:call_perl).
   Called when Perl throws an exception.  Does not return.  */
static void
abort_perl_call (jumpret)
     int jumpret;
{
  struct perl_frame *pfp = current_perl->frame_p;
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
	/* FIXME: Would it be okay to shorten a Lisp string in place?
	   If so, we could avoid the ' ' between . and "  */
	XSTRING (err)->data [len] = ' ';
    }
  Fsignal (Qperl_error, Fcons (err, Qnil));
}

/* Frame is Perl (perlmacs.c: call_perl).
   Convert the value or values returned by Perl to a Lisp object.
   CALL_PERL leaves the values on the Perl stack for us to pick up.
   This function implements the Lisp semantics of Perl's contexts
   (scalar, list, void).  */
static Lisp_Object
perlmacs_retval (ctx, numret, conv)
     I32 ctx;
     I32 numret;
     sv_to_lisp_converter_t conv;
{
  Lisp_Object *vals;
  int i;

  switch (ctx)
    {
    default:  /* G_VOID */
      return Qnil;
    case G_SCALAR:
      {
	dTHR;
	return (*conv) (*PL_stack_sp);
      }
    case G_ARRAY:
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
    PERL_MG_GET
  };

/* Frame is Perl (perlmacs.c, multiple functions).
   Call the specified Perl API function and pop to the previous Lisp frame.  */
/* Note: Perl 5.005 already requires stdarg.  */
static Lisp_Object
call_perl (enum perl_entry entry, ...)
{
  va_list ap;
  SV *sv;
  I32 ctx;
  sv_to_lisp_converter_t conv;
  char *str;
  dJMPENV;
  dTHR;
  int ret;
  Lisp_Object retval;
  I32 numret;
  struct lisp_frame *lfp;
  int count;

  /* Subtract 1 for the record_unwind_protect in PUSH_PERL_FRAME.  */
  count = specpdl_ptr - specpdl - 1;

  /* JMPENV_PUSH does sigsetjmp and ensures that, unless the process gets
     a signal, Perl will return to this function.
     Perl calls longjmp with an arg of 3 in the case of eval errors
     and 2 when it wants to exit.
     (However, Lisp is not as nice, and fails to provide a way to catch
     every possible `throw'.  See the comments in PUSH_PERL_FRAME.)  */
  JMPENV_PUSH (ret);
  if (ret)
    abort_perl_call (ret);

  va_start (ap, entry);
  switch (entry)
    {
    case PERL_EVAL_SV:
      sv = va_arg (ap, SV *);
      ctx = va_arg (ap, I32);
      conv = va_arg (ap, sv_to_lisp_converter_t);

      /* perl_eval_sv() doesn't support the lack of G_EVAL, so we must
	 check the value of $@ and "die" if it is true.  */
      numret = perl_eval_sv (sv, ctx);
      if (SvTRUE (ERRSV))
	abort_perl_call (3);
      retval = perlmacs_retval (ctx, numret, conv);
      break;

    case PERL_CALL_SV:
      sv = va_arg (ap, SV *);
      ctx = va_arg (ap, I32);
      conv = va_arg (ap, sv_to_lisp_converter_t);

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

    case PERL_MG_GET:
      sv = va_arg (ap, SV *);

      Perl_mg_get (sv);
      retval = Qnil;
      break;
    }
  va_end (ap);

  /* Indicate to perlmacs_funcall that there were no errors.  */
  lfp = current_perl->frame_p->prev;
  lfp->lisp_error = Qnil;
  lfp->jumpret = 0;

  return unbind_to (count, retval);
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
  struct emacs_perl_interpreter *perl = XPERL_INTERPRETER (object);

  if (EQ (emacs_phase, Qdestructing) && PERL_IS_TOP_LEVEL (perl))
    {
      /* Emacs is shutting down.  All Perl references to Lisp objects
	 are becoming invalid, so undef them.  */
      struct sv_lisp *prot, *tmp;

      for (prot = perl->protlist; prot; prot = tmp)
	{
	  tmp = prot->next;

	  /* Disable the DESTROY method.  */
	  SvOBJECT_off (prot->sv);

	  Perl_sv_setsv (prot->sv, &PL_sv_undef);
	  xfree (prot);
	}
    }

  /* FIXME: Should test for the Perl that ran Emacs, not just the
     top level Perl.  */
  if (EQ (emacs_phase, Qdestructing) || PERL_IS_TOP_LEVEL (perl))
    return;

  /* We can't get here from within a Perl call, because this function
     is called only when the interpreter is garbage collected,
     and it is gcpro'd when running.  */
  if (! PERL_DEFUNCT_P (perl))
    Fperl_destruct (object);
  perl_free (perl->interp);
  PL_curinterp = 0;
  xfree (perl);
  if (perl == current_perl)
    current_perl = 0;
}

static void
lisp_perl_mark (object)
     Lisp_Object object;
{
  struct emacs_perl_interpreter *perl;
  struct sv_lisp *prot;
  struct lisp_frame *lfp;

  perl = XPERL_INTERPRETER (object);
  for (prot = perl->protlist; prot; prot = prot->next)
    mark_object (&prot->obj);

  /* Mark error data from pending exceptions.  */
  for (lfp = perl->frame_l; lfp; lfp = (lfp->prev ? lfp->prev->prev : 0))
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

/* Perl initialization callback passed as argument to perl_parse().
   Frame is Perl (perl.c:perl_parse).  */
static void
perlmacs_xs_init ()
{
  extern void xs_init P_ ((void));
  extern XS (boot_Emacs);

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

  /* Define our own XSubs from xs/perlxs.xs.  */
  newXS ("Emacs::boot_Emacs", boot_Emacs, __FILE__);

  if (PERL_IS_TOP_LEVEL (current_perl))
    {
      /* If this ever fails to work because PL_origargv goes away,
	 just save argv[0] somewhere and use it here.  */
      char *dummy_argv[] = { PL_origargv [0], 0 };
      STRLEN n_a;

      Vperl_interpreter = new_foreign_object (&interp_vtbl, current_perl,
					      make_number (0));

      /* May be overridden by EMACS_MAIN.  */
      noninteractive = 1;

      init_lisp (1, dummy_argv, 0);
    }
  CACHE_ELO_STASH (current_perl);
}

/* Construct an interpreter object and the underlying Perl interpreter.
   Frame is Lisp (Fmake_perl_interpreter) or none (perlmacs_main).  */
static struct emacs_perl_interpreter *
new_perl (in_lisp)
     int in_lisp;
{
  struct emacs_perl_interpreter *perl;

  perl = (struct emacs_perl_interpreter *)
    xmalloc (sizeof (struct emacs_perl_interpreter));
  bzero (perl, sizeof *perl);

  perl->interp = perl_alloc ();
  perl_construct (perl->interp);

  if (in_lisp)
    {
      perl->frame_l = &perl->start.l;
      perl->frame_l->lisp_error = Qnil;
    }
  else
    perl->frame_p = &perl->start.p;

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

  top_level_perl = current_perl = new_perl (0);
  current_perl->phase = Qparsing;
  status = perl_parse (current_perl->interp, perlmacs_xs_init,
		       argc, argv, 0);

  if (! status)
    {
      current_perl->phase = Qrunning;
      status = perl_run (current_perl->interp);
    }

  current_perl->phase = Qdestructing;
  perl_destruct (current_perl->interp);
  perl_free (current_perl->interp);

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
  struct emacs_perl_interpreter *perl;
  Lisp_Object this_perl;
  struct perl_frame pf;
  int count = specpdl_ptr - specpdl;
  struct gcpro gcpro1;
  int i;

#ifndef MULTIPLICITY
  if (current_perl)
    error ("Multiple Perl interpreters are not supported");
#endif

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
  Vperl_interpreter = this_perl;
  push_perl_frame (&pf, 0);

  GCPRO1 (this_perl);
  perl->status =
    make_number (perl_parse (perl->interp, perlmacs_xs_init, nargs, argv, 0));

  perl->phase = Qparsed;
  CACHE_ELO_STASH (perl);

  UNGCPRO;
  return unbind_to (count, this_perl);
}

DEFUN ("perl-run", Fperl_run, Sperl_run, 0, 1, 0,
  "Run the specified Perl interpreter.\n\
If no argument is given, run the current interpreter.  Return what would be\n\
perl's exit status.")
  (interpreter)
     Lisp_Object interpreter;
{
  struct emacs_perl_interpreter *perl;
  struct perl_frame pf;
  int count = specpdl_ptr - specpdl;
  struct gcpro gcpro1;

  if (NILP (interpreter))
    interpreter = Vperl_interpreter;

  CHECK_PERL_INTERPRETER (interpreter);
  perl = XPERL_INTERPRETER (interpreter);
  if (perl->frame_l->prev)
    error ("perl-run: interpreter is already running");

  if (! (EQ (perl->phase, Qparsed) || EQ (perl->phase, Qran)))
    Fsignal (Qerror,
	     Fcons (build_string ("Bad calling sequence for `perl-run'"),
		    Fcons (perl->phase, Qnil)));

  perl->phase = Qrunning;
  current_perl = perl;
  Vperl_interpreter = interpreter;
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
  struct emacs_perl_interpreter *perl;
  struct perl_frame pf;
  int count = specpdl_ptr - specpdl;
  struct gcpro gcpro1;
  struct sv_lisp *prot, *tmp;

  if (NILP (interpreter))
    {
      interpreter = Vperl_interpreter;
      if (NILP (interpreter))
	return Qnil;
    }

  CHECK_PERL_INTERPRETER (interpreter);
  perl = XPERL_INTERPRETER (interpreter);
  if (PERL_IS_TOP_LEVEL (perl))
    error ("Attempt to destruct a top-level Perl");

  /* perl->frame_l == 0 only during global destruction.  */
  if (perl->frame_l && perl->frame_l->prev)
    error ("Attempt to kill Perl from within Perl; use `M-x top-level RET'"
	   " first");

  if (!(EQ (perl->phase, Qparsed) || EQ (perl->phase, Qran)))
    Fsignal (Qerror,
	     Fcons (build_string ("Bad calling sequence for `perl-destruct'"),
		    Fcons (perl->phase, Qnil)));

  perl->phase = Qdestructing;
  current_perl = perl;
  Vperl_interpreter = interpreter;
  push_perl_frame (&pf, 0);

  GCPRO1 (interpreter);
  perl_destruct (perl->interp);
  UNGCPRO;

  for (prot = perl->protlist; prot; prot = tmp)
    {
      tmp = prot->next;
      xfree (prot);
    }

  perl->phase = Qdestructed;
  current_perl = 0;
  Vperl_interpreter = Qnil;

  /* gcpro the interpreter while unbinding.  */  /* FIXME: Why?  */
  unbind_to (count, interpreter);
  return Qnil;
}

DEFUN ("perl-status", Fperl_status, Sperl_status, 0, 1, 0,
  "Return what would be perl's exit status.")
  (interpreter)
     Lisp_Object interpreter;
{
  if (NILP (interpreter))
    interpreter = Vperl_interpreter;

  CHECK_PERL_INTERPRETER (interpreter);
  return XPERL_INTERPRETER (interpreter)->status;
}

DEFUN ("perl-phase", Fperl_phase, Sperl_phase, 0, 1, 0,
  "Return the Perl interpreter's phase, one of the following symbols:\n\
parsing, parsed, running, ran, destructing, destructed, bad.")
  (interpreter)
     Lisp_Object interpreter;
{
  if (NILP (interpreter))
    interpreter = Vperl_interpreter;

  CHECK_PERL_INTERPRETER (interpreter);
  return XPERL_INTERPRETER (interpreter)->phase;
}

/* Fire up a Perl interpreter for use with Lisp functions that act
   implicitly on one.
   Don't call this function directly.  Use INIT_PERL.  */
static void
init_perl ()
{
  Lisp_Object qmpi;

  qmpi = intern ("make-perl-interpreter");
  if (NILP (Ffboundp (qmpi)))
    Vperl_interpreter = Fprimitive_make_perl (0, 0);
  else
    Vperl_interpreter = call0 (qmpi);

  if (NILP (Vperl_interpreter))
    error ("Perl initialization failed");
}

/* There should be no need to check argument type in the Lisp_Foreign_Object
   methods, so long as they are called only internally.  */

static Lisp_Object
lisp_sv_type_of (object)
     Lisp_Object object;
{
  return Qperl_value;
}

static void
lisp_sv_mark (object)
     Lisp_Object object;
{
  /* Mark the interpreter.  */
  mark_object (&XFOREIGN_OBJECT (object)->lisp_data);
}

/* Called from Fgarbage_collect.  */
static void
lisp_sv_destroy (object)
     Lisp_Object object;
{
  struct emacs_perl_interpreter *perl;
  SV *sv;

  /* Check the interpreter's status to avoid a crash after `perl-destruct'.
     During Emacs destruction, even this is dangerous.
     (The solution is to call `perl-destruct' *before* Emacs starts to
     destruct.)  */
  if (EQ (emacs_phase, Qdestructing))
    return;

  perl = LISP_SV_INTERP (object);
  if (PERL_DEFUNCT_P (perl)
      || EQ (perl->phase, Qdestructing))
    return;

  sv = (SV *) XFOREIGN_OBJECT (object)->data;

  if (SvREFCNT (sv) > 1 || ! perl->frame_p || ! SvROK (sv))
    {
      SvREFCNT_dec (sv);
      return;
    }
  call_perl (SV_FREE, sv);
}

static Lisp_Object
lisp_sv_call (self, nargs, args)
     Lisp_Object self;
     int nargs;
     Lisp_Object *args;
{
  Lisp_Object *new_args;
  int new_nargs = nargs + 2;

  if (PERL_DEFUNCT_P (LISP_SV_INTERP (self)))
    error ("Attempt to call Perl code in a destructed interpreter");

  /* XXX SET_PERL(XFOREIGN_OBJECT (self)->lisp_data) */
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

static struct Lisp_Foreign_Object_VTable sv_vtbl =
  {
    lisp_sv_type_of,	/* TYPE-OF method.  */
    lisp_sv_destroy,	/* Destructor.  */
    lisp_sv_mark,	/* MARK method.  Mark the interpreter.  */
    0,			/* TO-STRING method.  FIXME: Write one!  */
    0,			/* EQUAL method.  FIXME: Write one!  */
    lisp_sv_call	/* CALL method.  */
  };

/* Stick a Perl thing in a Lisp object.  */
Lisp_Object
perlmacs_sv_as_lisp_noinc (sv)
     SV *sv;
{
  return new_foreign_object (&sv_vtbl, sv, Vperl_interpreter);
}

/* Like perlmacs_sv_as_lisp_noinc, but the caller is not assumed
   to own a reference to sv.  */
Lisp_Object
perlmacs_sv_as_lisp (sv)
     SV *sv;
{
  SvREFCNT_inc (sv);
  return perlmacs_sv_as_lisp_noinc (sv);
}

/* Convert a Perl return value or Lisp function argument into a Lisp object.
   This is the default, deep-copying, user-friendly conversion.  */
Lisp_Object
perlmacs_sv_to_lisp (sv)
     SV *sv;
{
  Lisp_Object retval;
  STRLEN len;
  char *pc;

  if (! sv)
    return Qnil;  /* grrr.... */

  if (SvGMAGICAL (sv))
    {
      if (current_perl->frame_l)
	call_perl (PERL_MG_GET, sv);
      else
	Perl_mg_get (sv);
    }

  if (! SvOK (sv))  /* undef equals nil */
    return Qnil;

  if (SvROK (sv))
    {
      sv = SvRV (sv);

      if (BARE_SV_LISPP (sv))
	return XBARE_SV_LISP (sv);

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
	  /* I don't know what SVt_PVBM is.  A studied thing?  Boyer-Moore?
	     Anyway, sv.c:sv_reftype groups it with the above types,
	     and who am I to differ?  */
	case SVt_PVBM:

	  /* Convert array ref ref to vector.  */
	  /* FIXME: Factor this, code, the list constructor for arrayrefs,
	     and the list-context perlmacs_retval code into one or two
	     functions.  */
	  if (SvROK (sv) && SvTYPE (SvRV (sv)) == SVt_PVAV)
	    {
	      AV *av = (AV *) SvRV (sv);
	      SV **ary;
	      I32 key;
	      Lisp_Object *ptr;

	      ary = AvARRAY (av);
	      key = AvFILLp (av) + 1;
	      retval = Fmake_vector (make_number (key), Qnil);
	      ptr = XVECTOR (retval)->contents;
	      while (key--)
		ptr [key] = perlmacs_sv_to_lisp (ary [key]);

	      return retval;
	    }
	  break;

	  /* FIXME: The following conversions probably ignore magicalness.
	     To change this, we would have to take note of what frame
	     we are called in.  If Lisp, we would have to push a Perl
	     frame for mg_get().  And we would have to gc-protect retval.  */

	case SVt_PVAV:
	  /* Convert arrayref to list.  */
	  {
	    SV **ary;
	    I32 key;

	    ary = AvARRAY ((AV *) sv);
	    key = AvFILLp ((AV *) sv) + 1;
	    retval = Qnil;
	    while (key)
	      retval = Fcons (perlmacs_sv_to_lisp (ary [--key]), retval);
	  }
	  return retval;

	case SVt_PVGV:
	  {
	    dTHR;
	    /* Check for glob in package main.  */
	    if (GvSTASH (sv) != PL_defstash)
	      break;
	    {
	      /* Convert globref to symbol.  */
	      char *pc;
	      const char *pcc;
	      const char *end;
	      char *name;

	      end = GvNAME (sv) + GvNAMELEN (sv);
	      name = alloca (1 + GvNAMELEN (sv));
	      for (pcc = GvNAME (sv), pc = name; pcc != end; ++pcc, ++pc)
		*pc = (*pcc == '_') ? '-' : (*pcc == '-') ? '_' : *pcc;
	      *pc = '\0';

	      return intern (name);
	    }
	  }
	  break;

	default:
	  break;
	}

      return perlmacs_sv_as_lisp (sv);
    }

  if (SvIOK (sv) && (IV) XINT (SvIVX (sv)) == SvIVX (sv))
    return make_number (SvIVX (sv));

#ifdef LISP_FLOAT_TYPE
  if (SvNIOK (sv))
    return make_float (SvNVX (sv));
#endif

  pc = SvPV (sv, len);
  return make_unibyte_string (pc, (int) len);
}

/* Stick a Lisp object into a Perl reference of type Emacs::Lisp::Object.  */
SV *
perlmacs_lisp_as_sv (obj)
     Lisp_Object obj;
{
  dTHR;
  struct sv_lisp *prot;
  SV *sv;

  /* Protect object from garbage collection during the SV lifetime.  */
  prot = (struct sv_lisp *) xmalloc (sizeof (struct sv_lisp));
  prot->obj = obj;
  prot->prev = 0;
  prot->next = current_perl->protlist;
  sv = Perl_sv_bless (Perl_newRV_noinc (Perl_newSViv ((IV) prot)),
		      current_perl->elo_stash);
  prot->sv = sv;

  if (current_perl->protlist)
    current_perl->protlist->prev = prot;
  current_perl->protlist = prot;

  return sv;
}

SV *
perlmacs_lisp_to_sv (obj)
     Lisp_Object obj;
{
  if (NILP (obj))
    return &PL_sv_undef;

  /* Unconvert wrapped Perl data.  */
  if (PERL_OBJECT_P (obj))
    return SvREFCNT_inc (XLISP_SV (obj));

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
  return perlmacs_lisp_as_sv (obj);
}

DEFUN ("perl-value-p", Fperl_value_p, Sperl_value_p, 1, 1, 0,
  "Return t if OBJECT is a Perl scalar value.")
     (object)
     Lisp_Object object;
{
  if (PERL_OBJECT_P (object))
    return Qt;
  return Qnil;
}

DEFUN ("perl-to-lisp", Fperl_to_lisp, Sperl_to_lisp, 1, 1, 0,
  "Return a deep copy of OBJECT replacing Perl structures with Lisp equivalents.\n\
Arrayrefs are converted to a lists.  References to arrayrefs become vectors.\n\
\n\
If the object is not Perl data or cannot be converted to Lisp (for example,\n\
a hash reference), it is returned unchanged.")
     (object)
     Lisp_Object object;
{
  if (LISP_SVP (object))
    {
      if (PERL_DEFUNCT_P (LISP_SV_INTERP (object)))
	return Qnil;
      return perlmacs_sv_to_lisp (XLISP_SV (object));
    }
  return object;
}

DEFUN ("perl-wrap", Fperl_wrap, Sperl_wrap, 1, 1, 0,
  "Return OBJECT as a Perl object of class Emacs::Lisp::Object.")
     (object)
     Lisp_Object object;
{
  INIT_PERL;
  return perlmacs_sv_as_lisp_noinc (perlmacs_lisp_as_sv (object));
}

DEFUN ("lisp-to-perl", Flisp_to_perl, Slisp_to_perl, 1, 1, 0,
  "Return a deep copy of OBJECT replacing Lisp structures with Perl equivalents.\n\
If the object is not Perl data and cannot be converted to Perl (for example,\n\
a frame object), return it as an object of class Emacs::Lisp::Object.")
     (object)
     Lisp_Object object;
{
  INIT_PERL;
  return perlmacs_sv_as_lisp_noinc (perlmacs_lisp_to_sv (object));
}

static Lisp_Object
internal_perl_eval (string, context, conv)
     Lisp_Object string, context;
     sv_to_lisp_converter_t conv;
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
  sv = Perl_sv_2mortal (perlmacs_lisp_to_sv (string));
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
  return internal_perl_eval (string, context, perlmacs_sv_as_lisp);
}

static Lisp_Object
internal_perl_call (nargs, args, do_eval, conv)
     int nargs;
     Lisp_Object *args;
     int do_eval;
     sv_to_lisp_converter_t conv;
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
      *++sp = Perl_sv_2mortal (perlmacs_lisp_to_sv (args [i]));
    PUTBACK;
  }
  if (do_eval)
    return call_perl (EVAL_AND_CALL, XSTRING (code)->data, ctx);
  else
    return call_perl (PERL_CALL_SV,
		      Perl_sv_2mortal (perlmacs_lisp_to_sv (code)), ctx, conv);
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
  return internal_perl_call (nargs, args, 0, perlmacs_sv_as_lisp);
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
  CHECK_STRING (args [0], 0);
  INIT_PERL;
  return internal_perl_call (nargs, args, 1, 0);
}

void
syms_of_perlmacs ()
{
  Qparsing = intern ("parsing");
  staticpro (&Qparsing);
  Qparsed = intern ("parsed");
  staticpro (&Qparsed);
  Qran = intern ("ran");
  staticpro (&Qran);
  Qdestructed = intern ("destructed");
  staticpro (&Qdestructed);

  Qvoid_context = intern ("void-context");
  staticpro (&Qvoid_context);
  Qscalar_context = intern ("scalar-context");
  staticpro (&Qscalar_context);
  Qlist_context = intern ("list-context");
  staticpro (&Qlist_context);

  Qperl_value = intern ("perl-value");
  staticpro (&Qperl_value);

  DEFVAR_LISP ("perl-interpreter", &Vperl_interpreter,
    "The current active Perl interpreter, or `nil' if none is active.");

  Qperl_interpreter = intern ("perl-interpreter");
  staticpro (&Qperl_interpreter);

  if (! top_level_perl)
    Vperl_interpreter = Qnil;

  Qperl_error = intern ("perl-error");
  staticpro (&Qperl_error);

  Fput (Qperl_error, Qerror_conditions,
	Fcons (Qperl_error, Fcons (Qerror, Qnil)));
  Fput (Qperl_error, Qerror_message,
	build_string ("Perl error"));

  Fprovide (intern ("perl-core"));

  defsubr (&Sprimitive_make_perl);
  defsubr (&Sperl_run);
  defsubr (&Sperl_destruct);
  defsubr (&Sperl_status);
  defsubr (&Sperl_phase);
  defsubr (&Sperl_value_p);
  defsubr (&Sperl_to_lisp);
  defsubr (&Sperl_wrap);
  defsubr (&Slisp_to_perl);
  defsubr (&Sperl_eval);
  defsubr (&Sperl_eval_raw);
  defsubr (&Sperl_call);
  defsubr (&Sperl_call_raw);
  defsubr (&Sperl_eval_and_call);
}
