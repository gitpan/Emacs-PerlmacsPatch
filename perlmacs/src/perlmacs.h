#ifndef _PERLMACS_H
#define _PERLMACS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Perlmacs hook for when Emacs::Lisp is loaded.  */
extern void perlmacs_init ();

/* Do the normal data conversion, including deep copying of nested lists.  */
extern Lisp_Object perlmacs_sv_to_lisp _((SV *sv));
extern SV *perlmacs_lisp_to_sv _((Lisp_Object obj));

/* Wrap the argument in an object of the other language type.
   These functions support shallow copying.  */
extern Lisp_Object perlmacs_lisp_wrap_sv _((SV *sv));
extern SV *perlmacs_sv_wrap_lisp _((Lisp_Object obj));

/* Enter a Lisp stack frame.  */
extern int perlmacs_call_emacs_main _((int argc, char **argv, char **envp));
extern Lisp_Object perlmacs_funcall _((int nargs, Lisp_Object *args));

/* Data access macros similar to those defined in lisp.h.
   Use sv_isa instead of sv_derived_from because there is no need
   to derive classes from Emacs::Lisp::Object, because Lisp objects
   know their type.  */
#define SV_LISPP(sv) Perl_sv_isa (sv, "Emacs::Lisp::Object")
#define XSV_LISP(sv) (*((Lisp_Object *) SvIVX (SvRV (sv))))

#ifdef __cplusplus
}
#endif

#endif  /* _PERLMACS_H */
