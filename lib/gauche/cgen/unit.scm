;;;
;;; gauche.cgen.unit - cgen-unit
;;;  
;;;   Copyright (c) 2004-2007  Shiro Kawai  <shiro@acm.org>
;;;   
;;;   Redistribution and use in source and binary forms, with or without
;;;   modification, are permitted provided that the following conditions
;;;   are met:
;;;   
;;;   1. Redistributions of source code must retain the above copyright
;;;      notice, this list of conditions and the following disclaimer.
;;;  
;;;   2. Redistributions in binary form must reproduce the above copyright
;;;      notice, this list of conditions and the following disclaimer in the
;;;      documentation and/or other materials provided with the distribution.
;;;  
;;;   3. Neither the name of the authors nor the names of its contributors
;;;      may be used to endorse or promote products derived from this
;;;      software without specific prior written permission.
;;;  
;;;   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
;;;   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
;;;   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
;;;   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
;;;   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
;;;   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
;;;   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
;;;   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
;;;   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
;;;   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
;;;   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
;;;  
;;;  $Id: unit.scm,v 1.1 2007-04-23 11:18:31 shirok Exp $
;;;

(define-module gauche.cgen.unit
  (use srfi-1)
  (use srfi-13)
  (use gauche.parameter)
  (use gauche.sequence)
  (export <cgen-unit> cgen-current-unit
          cgen-unit-c-file cgen-unit-init-name cgen-unit-h-file
          cgen-add! cgen-emit-h cgen-emit-c

          <cgen-node> cgen-decl-common cgen-emit
          <cgen-raw-node> cgen-extern cgen-decl cgen-body cgen-init
          cgen-include cgen-define
          
          cgen-safe-name

          ;; semi-private routines
          cgen-emit-static-data)
  )
(select-module gauche.cgen.unit)

;;=============================================================
;; Unit
;;

;; A 'cgen-unit' is the unit of C source.  It generates one .c file,
;; and optionally one .h file.
;; During the processing, a "current unit" is kept in a parameter
;; cgen-current-unit, and most cgen APIs implicitly work to it.

(define-class <cgen-unit> ()
  ((name     :init-keyword :name   :init-value "cgen")
   (c-file   :init-keyword :c-file :init-value #f)
   (h-file   :init-keyword :h-file :init-value #f)
   (preamble :init-keyword :preamble
             :init-value '("/* Generated by gauche.cgen $Revision: 1.1 $ */"))
   (pre-decl :init-keyword :pre-decl :init-value '())
   (init-prologue :init-keyword :init-prologue :init-value #f)
   (init-epilogue :init-keyword :init-epilogue :init-value #f)
   (toplevels :init-value '())   ;; toplevel nodes to be realized
   (transients :init-value '())  ;; transient variables
   (literals  :init-form #f)     ;; literals. see gauche.cgen.literal
   (static-data-list :init-value '()) ;; static C data, see below
   ))

(define cgen-current-unit (make-parameter #f))

(define-method cgen-unit-c-file ((unit <cgen-unit>))
  (or (ref unit 'c-file)
      #`",(ref unit 'name).c"))

(define-method cgen-unit-init-name ((unit <cgen-unit>))
  (format "Scm__Init_~a"
          (or (ref unit 'init-name)
              (cgen-safe-name (ref unit 'name)))))

(define-method cgen-unit-h-file ((unit <cgen-unit>))
  (ref unit 'h-file))

(define (cgen-add! node)
  (and-let* ((unit (cgen-current-unit)))
    (slot-push! unit 'toplevels node))
  node)

(define-method cgen-emit ((unit <cgen-unit>) part)
  (let1 context (make-hash-table)
    (define (walker node)
      (unless (hash-table-get context node #f)
        (hash-table-put! context node #t)
        (cgen-emit node part walker)))
    (for-each walker (reverse (ref unit 'toplevels)))))

(define-method cgen-emit-h ((unit <cgen-unit>))
  (and-let* ((h-file (cgen-unit-h-file unit)))
    (cgen-with-output-file h-file
      (lambda ()
        (cond ((ref unit 'preamble) => emit-raw))
        (cgen-emit unit 'extern)))))

(define-method cgen-emit-c ((unit <cgen-unit>))
  (cgen-with-output-file (cgen-unit-c-file unit)
    (lambda ()
      (cond ((ref unit 'preamble) => emit-raw))
      (cond ((ref unit 'pre-decl) => emit-raw))
      (print "#include <gauche.h>")
      ;; This piece of code is required, for Win32 DLL doesn't like
      ;; structures to be const if it contains SCM_CLASS_PTR.  Doh!
      (print "#if defined(__CYGWIN__) || defined(__MINGW32__)")
      (print "#define SCM_CGEN_CONST /*empty*/")
      (print "#else")
      (print "#define SCM_CGEN_CONST const")
      (print "#endif")
      (cgen-emit unit 'decl)
      (cgen-emit-static-data unit)
      (cgen-emit unit 'body)
      (cond ((ref unit 'init-prologue) => emit-raw)
            (else
             (print "Scm__Init_"(cgen-safe-name (ref unit 'name))"(void)")
             (print "{")))
      (cgen-emit unit 'init)
      (cond ((ref unit 'init-epilogue) => emit-raw)
            (else (print "}")))
      )))

;; NB: temporary solution for inter-module dependency.
;; The real procedure is defined in gauche.cgen.literal.
(define-generic cgen-emit-static-data)

;;=============================================================
;; Base class
;;
(define-class <cgen-node> ()
  ((extern?  :init-keyword :extern? :init-value #f)))

;; fallback methods
(define-method cgen-decl-common ((node <cgen-node>)) #f)

(define-method cgen-emit ((node <cgen-node>) part walker)
  (case part
    ((extern) (when (ref node 'extern?) (cgen-decl-common node)))
    ((decl)   (unless (ref node 'extern?) (cgen-decl-common node)))
    ((body init) #f)))

;;=============================================================
;; Raw nodes - can be used to insert a raw piece of code
;;

(define-class <cgen-raw-node> (<cgen-node>)
  ((parts :init-keyword :parts :init-value '())
   (code  :init-keyword :code :init-value "")))

(define-method cgen-emit ((node <cgen-raw-node>) part walker)
  (when (memq part (ref node 'parts))
    (emit-raw (ref node 'code))))

(define (cgen-extern . code)
  (cgen-add! (make <cgen-raw-node> :parts '(extern) :code code)))

(define (cgen-decl . code)
  (cgen-add! (make <cgen-raw-node> :parts '(decl) :code code)))
   
(define (cgen-body . code)
  (cgen-add! (make <cgen-raw-node> :parts '(body) :code code)))

(define (cgen-init . code)
  (cgen-add! (make <cgen-raw-node> :parts '(init) :code code)))

;;=============================================================
;; cpp
;;

;; #include ---------------------------------------------------
(define-class <cgen-include> (<cgen-node>)
  ((path        :init-keyword :path)))

(define-method cgen-decl-common ((node <cgen-include>))
  (print "#include "
         (if (string-prefix? "<" (ref node 'path))
           (ref node 'path)
           #`"\",(ref node 'path)\"")))

(define (cgen-include path)
  (cgen-add! (make <cgen-include> :path path)))

;; #if --------------------------------------------------------
;(define-class <cgen-cpp-if> (<cgen-node>)
;  ((condition :init-keyword :condition :init-value #f)
;   (then      :init-keyword :then :init-value '())
;   (else      :init-keyword :else :init-value '())))

;(define-method cgen-emit ((node <cgen-cpp-if>) part walker)
;  (if (ref node 'condition)
;    (begin
;      (print "#if " (ref node 'condition))
;      (for-each walker (ref node 'then))
;      (if (null? (ref node 'else))
;        (print "#endif /*" (ref node 'condition) "*/")
;        (begin
;          (print "#else  /* !" (ref node 'condition) "*/")
;          (for-each walker (ref node 'else))
;          (print "#endif /* !" (ref node 'condition) "*/"))))
;    (for-each walker (ref node 'then))))

;; #define -----------------------------------------------------
(define-class <cgen-cpp-define> (<cgen-node>)
  ((name   :init-keyword :name)
   (value  :init-keyword :value)
   ))

(define-method cgen-decl-common ((node <cgen-cpp-define>))
  (print "#define " (ref node 'name) " " (ref node 'value)))

(define (cgen-define name . maybe-value)
  (cgen-add!
   (make <cgen-cpp-define> :name name :value (get-optional maybe-value ""))))


;;=============================================================
;; Utilities
;;

(define (cgen-with-output-file file thunk)
  (receive (port tmpfile) (sys-mkstemp file)
    (guard (e (else 
               (close-output-port port)
               (sys-unlink tmpfile)
               (raise e)))
      (with-output-to-port port thunk)
      (close-output-port port)
      (sys-rename tmpfile file))))

(define (emit-raw code)
  (if (list? code)
    (for-each print code)
    (print code)))

;; creates a C-safe name from Scheme string str
(define (cgen-safe-name str)
  (with-string-io str
    (lambda ()
      (let loop ((b (read-byte)))
        (cond ((eof-object? b))
              ((or (<= 48 b 57)
                   (<= 65 b 90)
                   (<= 97 b 122))
               (write-byte b) (loop (read-byte)))
              (else
               (format #t "_~2,'0x" b) (loop (read-byte))))))))

(provide "gauche/cgen/unit")

